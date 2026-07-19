//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Dynamic sun shadowmap.
//
// Renders a realtime shadow depth map from the map's sun (light_environment)
// as an orthographic "flashlight" that follows the local player, so nearby
// world geometry and props receive dynamic sun shadows on top of the baked
// lightmap lighting.
//
// The sun's direction, color and lightstyle are read directly from the BSP's
// worldlights lump, so the dynamic light matches whatever VRAD baked. If the
// skylight was compiled with a nonzero lightstyle, its per-luxel contribution
// is stored separately in the BSP lighting lump (a "shadowmask" of sorts) --
// we detect and report that; with the stock engine binaries the shader cannot
// sample it, so the blend to the baked lightmap happens spatially instead: the
// projected sunlight fades out over the outer band of the shadow region, and
// beyond r_sunshadow_distance only the baked lightmap shadows remain.
//
// Cvars:
//   r_sunshadow           - enable/disable
//   r_sunshadow_distance  - radius around the player where the shadowmap ends
//   r_sunshadow_depthres  - shadow depth texture resolution (clientshadowmgr.cpp)
//
//=============================================================================//

#include "cbase.h"
#include "iclientshadowmgr.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/itexture.h"
#include "materialsystem/MaterialSystemUtil.h"
#include "vtf/vtf.h"
#include "filesystem.h"
#include "bspfile.h"
#include "c_baseplayer.h"
#include "view_shared.h"
#include "mathlib/vmatrix.h"
#include "sunshadowmask.h"		// baked mask sidecar format (public/)

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern ConVar r_flashlightdepthtexture;
extern ConVar r_sunshadow_depthres;		// lives in clientshadowmgr.cpp next to the RT allocation

static ConVar r_sunshadow( "r_sunshadow", "1", FCVAR_ARCHIVE,
	"Enable dynamic shadowmapped sunlight around the player (requires a map with a light_environment)." );
static ConVar r_sunshadow_distance( "r_sunshadow_distance", "2048", FCVAR_ARCHIVE,
	"Radius of the OUTERMOST sun shadow cascade (where dynamic sun shadows end). Finer cascades are r_sunshadow_cascade_ratio times smaller and sharper; the outer edge fades back to the baked lightmap." );
static ConVar r_sunshadow_intensity( "r_sunshadow_intensity", "1.0", FCVAR_ARCHIVE,
	"Brightness multiplier for the dynamic sunlight (color comes from the map's light_environment)." );
static ConVar r_sunshadow_casterheight( "r_sunshadow_casterheight", "8192", 0,
	"How far above the player the sun shadow frustum starts; geometry up to this height still casts." );
static ConVar r_sunshadow_filter( "r_sunshadow_filter", "1.0", FCVAR_ARCHIVE,
	"Sun shadowmap filter kernel size." );
static ConVar r_sunshadow_depthbias( "r_sunshadow_depthbias", "0.0005", 0 );
static ConVar r_sunshadow_slopescale( "r_sunshadow_slopescale", "4", 0 );
static ConVar r_sunshadow_maskbias( "r_sunshadow_maskbias", "6.0", 0,
	"Baked shadowmask depth-compare bias, in world units. A point is treated as sun-shadowed when it sits this far behind the nearest baked sun-facing surface." );
static ConVar r_sunshadow_darkness( "r_sunshadow_darkness", "0.35", FCVAR_ARCHIVE,
	"How dark a fully dynamically-shadowed but baked-lit surface goes (0 = black, 1 = no darkening). Only affects surfaces the darkening world shader touches." );
static ConVar r_sunshadow_worldshader( "r_sunshadow_worldshader", "1", FCVAR_ARCHIVE,
	"Publish sun shadow parameters to the darkening world shader (requires the mod's game_shader_dx9 override of LightmappedGeneric)." );
static ConVar r_sunshadow_cascade_ratio( "r_sunshadow_cascade_ratio", "4.0", FCVAR_ARCHIVE,
	"Radius ratio between adjacent sun shadow cascades. r_sunshadow_distance is the outermost cascade; each finer one is this many times smaller (and that much sharper)." );
static ConVar r_sunshadow_cascade_blend( "r_sunshadow_cascade_blend", "0.15", FCVAR_ARCHIVE,
	"Fraction of each cascade over which it crossfades into the next, hiding the seam between cascades (0..0.5)." );
static ConVar r_sunshadow_cascade_debug( "r_sunshadow_cascade_debug", "0", FCVAR_CHEAT,
	"Tint each sun cascade a distinct colour (red/green/blue = near/mid/far) to see coverage and the crossfade overlaps." );
static ConVar r_sunshadow_suppress_entityshadows( "r_sunshadow_suppress_entityshadows", "1", FCVAR_ARCHIVE,
	"While the sun shadowmap cascades are active, disable the legacy projected/blob entity shadows (which use a single fixed angle and double up with the cascade). Entities still cast into the cascades." );

//-----------------------------------------------------------------------------
// Per-cascade overrides. Each defaults to a sentinel meaning "use the shared/
// derived value", so the globals above still drive everything out of the box;
// set a cascade's cvar to tune just that ring. Cascade 0 is the sharp near one.
// (Resolution overrides live in clientshadowmgr, next to the RT allocation.)
//-----------------------------------------------------------------------------
#define SUN_PERCASCADE_CVAR( base, dflt, help ) \
	static ConVar r_sunshadow_c0_##base( "r_sunshadow_c0_" #base, dflt, FCVAR_ARCHIVE, "Cascade 0 (near) " help ); \
	static ConVar r_sunshadow_c1_##base( "r_sunshadow_c1_" #base, dflt, FCVAR_ARCHIVE, "Cascade 1 (mid) " help ); \
	static ConVar r_sunshadow_c2_##base( "r_sunshadow_c2_" #base, dflt, FCVAR_ARCHIVE, "Cascade 2 (far) " help ); \
	static ConVar *s_pCascade_##base[3] = { &r_sunshadow_c0_##base, &r_sunshadow_c1_##base, &r_sunshadow_c2_##base };

SUN_PERCASCADE_CVAR( dist,       "0",  "radius override (0 = auto from r_sunshadow_distance / ratio)." )
SUN_PERCASCADE_CVAR( filter,     "-1", "shadow filter size override (<0 = use r_sunshadow_filter)." )
SUN_PERCASCADE_CVAR( depthbias,  "-1", "shadow depth bias override (<0 = use r_sunshadow_depthbias)." )
SUN_PERCASCADE_CVAR( slopescale, "-1", "shadow slope-scale bias override (<0 = use r_sunshadow_slopescale)." )

// Depth-texture resolution overrides are defined in clientshadowmgr.cpp (which
// owns the render-target allocation); mirror them here for the projection state.
extern ConVar r_sunshadow_c0_res, r_sunshadow_c1_res, r_sunshadow_c2_res;
static ConVar *s_pCascade_res[3] = { &r_sunshadow_c0_res, &r_sunshadow_c1_res, &r_sunshadow_c2_res };

// The per-cascade cvar arrays above are hand-written for exactly 3 cascades.
COMPILE_TIME_ASSERT( MAX_SUN_SHADOW_CASCADES == 3 );

//-----------------------------------------------------------------------------
// Per-cascade procedural cookie (a "ring"). The flashlight pass multiplies the
// added sunlight by this, so the cookie shapes where each cascade contributes:
//   * transparent in the inner region a finer cascade already covers,
//   * ramps up over the inner handoff band,
//   * fully lit through the cascade's own annulus,
//   * ramps back down over the outer handoff band to the next-coarser cascade.
// The inner ramp of a cascade and the outer ramp of the finer one occupy the
// same WORLD band and use complementary smoothsteps, so the cookies form a
// partition of unity -- the added sun is exactly 1x everywhere, no double-
// brightening on overlap and no dark gap. Chebyshev (max-norm) distance keeps
// the bands a constant width along each side of the square ortho projection.
//-----------------------------------------------------------------------------
#define SUNSHADOW_COOKIE_RES		256

static inline float SmoothStep01( float t )
{
	t = clamp( t, 0.0f, 1.0f );
	return t * t * ( 3.0f - 2.0f * t );
}

class CSunShadowCookieRegenerator : public ITextureRegenerator
{
public:
	CSunShadowCookieRegenerator() : m_flInnerLo( 0 ), m_flInnerHi( 0 ), m_flOuterLo( 0.75f ), m_flOuterHi( 1.0f ) {}

	// Thresholds are normalized Chebyshev distance [0,1] (1 = cascade edge).
	void SetBands( float flInnerLo, float flInnerHi, float flOuterLo, float flOuterHi )
	{
		m_flInnerLo = flInnerLo; m_flInnerHi = flInnerHi;
		m_flOuterLo = flOuterLo; m_flOuterHi = flOuterHi;
	}

	virtual void RegenerateTextureBits( ITexture *pTexture, IVTFTexture *pVTFTexture, Rect_t *pRect )
	{
		int nWidth = pVTFTexture->Width();
		int nHeight = pVTFTexture->Height();
		unsigned char *pData = pVTFTexture->ImageData( 0, 0, 0 );

		for ( int y = 0; y < nHeight; ++y )
		{
			for ( int x = 0; x < nWidth; ++x )
			{
				// Normalized signed coords in [-1, 1] at texel centers
				float fx = ( ( x + 0.5f ) / nWidth ) * 2.0f - 1.0f;
				float fy = ( ( y + 0.5f ) / nHeight ) * 2.0f - 1.0f;
				float flDist = MAX( fabsf( fx ), fabsf( fy ) );

				float flIntensity;
				if ( m_flInnerHi > 0.0f && flDist < m_flInnerHi )
				{
					// Inner handoff: transparent below inner_lo, ramp up to 1.
					flIntensity = ( flDist <= m_flInnerLo ) ? 0.0f :
						SmoothStep01( ( flDist - m_flInnerLo ) / ( m_flInnerHi - m_flInnerLo ) );
				}
				else if ( flDist <= m_flOuterLo )
				{
					flIntensity = 1.0f;
				}
				else if ( flDist < m_flOuterHi )
				{
					// Outer handoff: ramp 1 -> 0 to the next-coarser cascade.
					flIntensity = 1.0f - SmoothStep01( ( flDist - m_flOuterLo ) / ( m_flOuterHi - m_flOuterLo ) );
				}
				else
				{
					flIntensity = 0.0f;
				}

				unsigned char v = (unsigned char)( flIntensity * 255.0f + 0.5f );
				unsigned char *pPixel = pData + ( y * nWidth + x ) * 4;		// BGRA8888
				pPixel[0] = v;
				pPixel[1] = v;
				pPixel[2] = v;
				pPixel[3] = v;
			}
		}
	}

	virtual void Release() {}

private:
	float m_flInnerLo, m_flInnerHi, m_flOuterLo, m_flOuterHi;
};

//-----------------------------------------------------------------------------
// The sun shadow manager
//-----------------------------------------------------------------------------
class CSunlightShadowManager : public CAutoGameSystemPerFrame
{
public:
	CSunlightShadowManager() : CAutoGameSystemPerFrame( "CSunlightShadowManager" )
	{
		for ( int c = 0; c < MAX_SUN_SHADOW_CASCADES; ++c )
		{
			m_ShadowHandle[c] = CLIENTSHADOW_INVALID_HANDLE;
			m_flLastRadius[c] = -1.0f;	// force cookie generation on the first update
		}
		m_flLastBlend = -1.0f;
		m_bSuppressedEntityShadows = false;
		m_bHasSun = false;
		m_nSunStyle = 0;
		m_vecSunDirection.Init( 0, 0, -1 );
		m_vecSunColor.Init( 1, 1, 1 );
		m_bHasMask = false;
		m_flLastPublishedParams0X = 0.0f;
		memset( &m_MaskHeader, 0, sizeof( m_MaskHeader ) );
	}

	virtual void LevelInitPostEntity()
	{
		ReadSunFromBSP();
		LoadSunShadowMask();

		if ( m_bHasSun )
		{
			DevMsg( "Sun shadowmap: skylight found, dir (%.2f %.2f %.2f), style %d%s\n",
				m_vecSunDirection.x, m_vecSunDirection.y, m_vecSunDirection.z, m_nSunStyle,
				m_nSunStyle != 0 ? " (styled skylight: per-luxel sun data present in BSP)" : "" );
		}
	}

	virtual void LevelShutdownPreEntity()
	{
		DestroySunShadow();
		FreeSunShadowMask();
		SuppressEntityShadows( false );		// restore the legacy shadows we turned off
		m_bHasSun = false;
		m_nSunStyle = 0;
	}

	bool HasBakedMask() const { return m_bHasMask; }

	// Intermediate values of a mask sample, for the r_sunshadow_probe diagnostic.
	struct MaskSample_t
	{
		bool	bInRange;		// query projected inside the baked U/V rectangle
		bool	bHasSurface;	// a baked surface exists in this column (not open sky)
		bool	bShadowed;		// depth-compare says the point is behind the occluder
		int		x, y;			// texel sampled
		float	texU, texV;		// [0,1] projection coords
		float	depthNorm;		// query depth, normalized to the baked range
		float	storedDepth;	// nearest baked surface depth at this texel
		float	biasNorm;		// depth bias applied, in the same normalized units
		float	vis;			// R channel (soft sun visibility of the sun-facing surface)
		float	result;		// final visibility returned (0 = baked shadow, 1 = full sun)
	};

	//-----------------------------------------------------------------------------
	// CPU sample of the baked mask at a world position, returning every
	// intermediate value. GetBakedSunVisibility() wraps this; r_sunshadow_probe
	// prints it so we can see exactly why a point reads lit or shadowed.
	//-----------------------------------------------------------------------------
	void SampleBakedMask( const Vector &vecWorldPos, MaskSample_t &out ) const
	{
		memset( &out, 0, sizeof( out ) );
		out.result = 1.0f;
		out.vis = 1.0f;
		if ( !m_bHasMask )
			return;

		const SunShadowMaskHeader_t &h = m_MaskHeader;
		Vector vSunDir( h.vecSunDir[0], h.vecSunDir[1], h.vecSunDir[2] );
		Vector vOrigin( h.vecOrigin[0], h.vecOrigin[1], h.vecOrigin[2] );
		Vector vU( h.vecAxisU[0], h.vecAxisU[1], h.vecAxisU[2] );
		Vector vV( h.vecAxisV[0], h.vecAxisV[1], h.vecAxisV[2] );

		Vector vRel = vecWorldPos - vOrigin;
		float u = DotProduct( vRel, vU );
		float v = DotProduct( vRel, vV );
		float d = DotProduct( vRel, vSunDir );

		out.texU = ( u - h.flMinU ) / ( h.flMaxU - h.flMinU );
		out.texV = ( v - h.flMinV ) / ( h.flMaxV - h.flMinV );
		if ( out.texU < 0.0f || out.texU > 1.0f || out.texV < 0.0f || out.texV > 1.0f )
			return;	// outside the baked region -- assume lit
		out.bInRange = true;

		float flDepthSpan = h.flMaxDepth - h.flMinDepth;
		out.depthNorm = ( flDepthSpan > 0.0f ) ? ( d - h.flMinDepth ) / flDepthSpan : 0.0f;

		out.x = (int)( out.texU * ( h.nResolution - 1 ) + 0.5f );
		out.y = (int)( out.texV * ( h.nResolution - 1 ) + 0.5f );
		const unsigned char *pTexel = &m_MaskData[ ( out.y * h.nResolution + out.x ) * 4 ];

		out.vis = pTexel[0] / 255.0f;							// R = soft sun visibility
		unsigned int usDepth = ( pTexel[1] << 8 ) | pTexel[2];	// G,B = 16-bit nearest depth
		if ( usDepth == 0xFFFF )
			return;												// no surface recorded here -> lit
		out.bHasSurface = true;
		out.storedDepth = usDepth / 65535.0f;

		// Bias is authored in world units; convert to the normalized depth space.
		out.biasNorm = ( flDepthSpan > 0.0f ) ? r_sunshadow_maskbias.GetFloat() / flDepthSpan : 0.0f;

		// Farther from the sun than the nearest recorded surface (beyond the bias)
		// means something is between us and the sun -> baked shadow.
		if ( out.depthNorm > out.storedDepth + out.biasNorm )
		{
			out.bShadowed = true;
			out.result = 0.0f;
			return;
		}

		out.result = out.vis;	// this is (near) the sun-facing surface itself
	}

	//-----------------------------------------------------------------------------
	// Baked sun visibility 0..1 (0 = in baked shadow) at a world position. This is
	// the reference the shader will reproduce; also used by the probe command.
	//-----------------------------------------------------------------------------
	float GetBakedSunVisibility( const Vector &vecWorldPos ) const
	{
		MaskSample_t s;
		SampleBakedMask( vecWorldPos, s );
		return s.result;
	}

	virtual void Update( float frametime )
	{
		if ( !m_bHasSun || !r_sunshadow.GetBool() || !r_flashlightdepthtexture.GetBool() ||
			 !materials->SupportsShadowDepthTextures() )
		{
			DestroySunShadow();
			PublishShaderParams( false );
			SuppressEntityShadows( false );
			return;
		}

		C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
		if ( !pPlayer )
		{
			DestroySunShadow();
			PublishShaderParams( false );
			SuppressEntityShadows( false );
			return;
		}

		UpdateSunShadow( pPlayer->EyePosition() );
		PublishShaderParams( true );
		SuppressEntityShadows( true );
	}

	// While the cascades are active, turn off the legacy render-to-texture entity
	// shadows (blobs / fixed-angle projected shadows) so they don't double up with
	// the cascade shadows entities now cast. We only ever undo our own change.
	void SuppressEntityShadows( bool bWantSuppressed )
	{
		bWantSuppressed = bWantSuppressed && r_sunshadow_suppress_entityshadows.GetBool();
		if ( bWantSuppressed == m_bSuppressedEntityShadows )
			return;
		m_bSuppressedEntityShadows = bWantSuppressed;
		g_pClientShadowMgr->SetShadowsDisabled( bWantSuppressed );
	}

	bool HasSun() const { return m_bHasSun; }
	int GetSunStyle() const { return m_nSunStyle; }
	const Vector &GetSunDirection() const { return m_vecSunDirection; }
	const Vector &GetSunColor() const { return m_vecSunColor; }

private:
	//-----------------------------------------------------------------------------
	// Reads the skylight out of the BSP worldlights lump. Prefers the HDR lump
	// when the map has one, matching what the engine renders with.
	//-----------------------------------------------------------------------------
	void ReadSunFromBSP()
	{
		m_bHasSun = false;
		m_nSunStyle = 0;

		const char *pszLevelName = engine->GetLevelName();		// "maps/foo.bsp"
		if ( !pszLevelName || !pszLevelName[0] )
			return;

		FileHandle_t hFile = filesystem->Open( pszLevelName, "rb", "GAME" );
		if ( hFile == FILESYSTEM_INVALID_HANDLE )
			return;

		dheader_t header;
		if ( filesystem->Read( &header, sizeof( header ), hFile ) != sizeof( header ) ||
			 header.ident != IDBSPHEADER )
		{
			filesystem->Close( hFile );
			return;
		}

		lump_t *pLump = &header.lumps[ LUMP_WORLDLIGHTS_HDR ];
		if ( pLump->filelen == 0 )
		{
			pLump = &header.lumps[ LUMP_WORLDLIGHTS ];
		}

		int nLightCount = pLump->filelen / sizeof( dworldlight_t );
		if ( nLightCount > 0 )
		{
			filesystem->Seek( hFile, pLump->fileofs, FILESYSTEM_SEEK_HEAD );
			for ( int i = 0; i < nLightCount; ++i )
			{
				dworldlight_t light;
				if ( filesystem->Read( &light, sizeof( light ), hFile ) != sizeof( light ) )
					break;

				if ( light.type == emit_skylight )
				{
					// VRAD stores the direction the sunlight travels in 'normal'
					// (GatherSampleSkyLight lights surfaces where -N.normal > 0).
					m_vecSunDirection = light.normal;
					VectorNormalize( m_vecSunDirection );

					// Normalize the baked intensity to a hue; overall brightness
					// is driven by r_sunshadow_intensity.
					float flMax = MAX( light.intensity.x, MAX( light.intensity.y, light.intensity.z ) );
					m_vecSunColor = ( flMax > 0.0f ) ? light.intensity / flMax : Vector( 1, 1, 1 );

					m_nSunStyle = light.style;
					m_bHasSun = true;
					break;
				}
			}
		}

		filesystem->Close( hFile );
	}

	// (Re)build cascade c's ring cookie. flInnerHi is the normalized radius of the
	// inner handoff (= R[c-1]/R[c]); pass 0 for cascade 0 (solid center, no finer
	// cascade below it). Every cascade fades out over its outer band.
	void InitCookieTexture( int c, float flInnerHi, float flBlend )
	{
		if ( !m_CookieTexture[c].IsValid() )
		{
			char szName[64];
			Q_snprintf( szName, sizeof( szName ), "sunshadow_cookie%d", c );
			m_CookieTexture[c].InitProceduralTexture( szName, TEXTURE_GROUP_CLIENT_EFFECTS,
				SUNSHADOW_COOKIE_RES, SUNSHADOW_COOKIE_RES, IMAGE_FORMAT_BGRA8888,
				TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_NOMIP |
				TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_SINGLECOPY | TEXTUREFLAGS_PROCEDURAL );
			if ( m_CookieTexture[c].IsValid() )
				m_CookieTexture[c]->SetTextureRegenerator( &m_CookieRegen[c] );
		}
		if ( !m_CookieTexture[c].IsValid() )
			return;

		// Inner handoff (to the finer cascade) sits at radius R[c-1] = flInnerHi*R[c];
		// outer handoff at the cascade's own edge. Cascade 0 passes flInnerHi 0.
		float flInnerLo = flInnerHi * ( 1.0f - flBlend );
		m_CookieRegen[c].SetBands( flInnerLo, flInnerHi, 1.0f - flBlend, 1.0f );
		m_CookieTexture[c]->Download();
	}

	// Depth-texture resolution actually allocated for cascade c (override, else
	// the shared r_sunshadow_depthres). Kept in sync with clientshadowmgr.
	static int CascadeRes( int c )
	{
		int nRes = s_pCascade_res[c]->GetInt();
		return ( nRes > 0 ) ? nRes : MAX( r_sunshadow_depthres.GetInt(), 1 );
	}

	void UpdateSunShadow( const Vector &vecPlayerEyes )
	{
		const int nCascades = MAX_SUN_SHADOW_CASCADES;
		float flOuterRadius = MAX( r_sunshadow_distance.GetFloat(), 256.0f );
		float flRatio = clamp( r_sunshadow_cascade_ratio.GetFloat(), 1.5f, 16.0f );
		float flBlend = clamp( r_sunshadow_cascade_blend.GetFloat(), 0.02f, 0.5f );

		// Per-cascade radius: an explicit override, else auto from distance/ratio
		// (cascade nCascades-1 is the outermost).
		float flRadii[MAX_SUN_SHADOW_CASCADES];
		for ( int c = 0; c < nCascades; ++c )
		{
			float flOverride = s_pCascade_dist[c]->GetFloat();
			if ( flOverride > 0.0f )
			{
				flRadii[c] = flOverride;
			}
			else
			{
				flRadii[c] = flOuterRadius;
				for ( int k = c; k < nCascades - 1; ++k )
					flRadii[c] /= flRatio;
			}
			flRadii[c] = MAX( flRadii[c], 16.0f );
		}

		// Regenerate the ring cookies only when the cascade shape changed; the
		// bands come from the ACTUAL adjacent radii so arbitrary per-cascade
		// distances still tile without gaps or double-brightening.
		bool bShapeChanged = ( flBlend != m_flLastBlend );
		for ( int c = 0; c < nCascades; ++c )
			bShapeChanged = bShapeChanged || ( flRadii[c] != m_flLastRadius[c] );
		if ( bShapeChanged )
		{
			for ( int c = 0; c < nCascades; ++c )
			{
				float flInnerHi = ( c > 0 && flRadii[c] > 0.0f ) ?
					clamp( flRadii[c - 1] / flRadii[c], 0.0f, 1.0f ) : 0.0f;
				InitCookieTexture( c, flInnerHi, flBlend );
				m_flLastRadius[c] = flRadii[c];
			}
			m_flLastBlend = flBlend;
		}

		QAngle angSun;
		VectorAngles( m_vecSunDirection, angSun );

		Vector vecFwd, vecRight, vecUp;
		AngleVectors( angSun, &vecFwd, &vecRight, &vecUp );

		bool bDebug = r_sunshadow_cascade_debug.GetBool();
		float flIntensity = r_sunshadow_intensity.GetFloat();

		// ALL cascades share one projection center. If each snapped to its own
		// texel grid the centers would differ by up to half a texel, offsetting the
		// square ring cookies so their additive sum dips below 1 in a thin square
		// band -- the dark edge where cascades meet. Snapping the common center to
		// the FINEST cascade's texel keeps the sharp near cascade from shimmering;
		// the coarser cascades ride the same center (sub-texel drift, invisible at
		// their distance).
		float flFinestTexel = ( 2.0f * flRadii[0] ) / MAX( CascadeRes( 0 ), 1 );
		Vector vecCenter = vecPlayerEyes;
		{
			float flRightCoord = DotProduct( vecCenter, vecRight );
			float flUpCoord = DotProduct( vecCenter, vecUp );
			vecCenter += vecRight * ( floorf( flRightCoord / flFinestTexel ) * flFinestTexel - flRightCoord );
			vecCenter += vecUp * ( floorf( flUpCoord / flFinestTexel ) * flFinestTexel - flUpCoord );
		}

		for ( int c = 0; c < nCascades; ++c )
		{
			float flRadius = flRadii[c];
			int nDepthRes = CascadeRes( c );
			float flCasterHeight = MAX( r_sunshadow_casterheight.GetFloat(), flRadius );

			// Cookies are (re)generated above only when the cascade shape changes.
			if ( !m_CookieTexture[c].IsValid() )
				continue;

			FlashlightState_t state;
			state.m_vecLightOrigin = vecCenter - m_vecSunDirection * flCasterHeight;
			AngleQuaternion( angSun, state.m_quatOrientation );

			state.m_fQuadraticAtten = 0.0f;
			state.m_fLinearAtten = 0.0f;
			state.m_fConstantAtten = 1.0f;

			// All cascades share the same sun colour x intensity; the ring cookies
			// keep the total additive contribution at 1x across their overlaps.
			// The debug mode instead tints each cascade to reveal its coverage.
			Vector vecTint = m_vecSunColor;
			if ( bDebug )
				vecTint.Init( c == 0 ? 1.0f : 0.0f, c == 1 ? 1.0f : 0.0f, c >= 2 ? 1.0f : 0.0f );
			state.m_Color[0] = vecTint.x * flIntensity;
			state.m_Color[1] = vecTint.y * flIntensity;
			state.m_Color[2] = vecTint.z * flIntensity;
			state.m_Color[3] = 0.0f;

			state.m_NearZ = 16.0f;
			state.m_FarZ = flCasterHeight + 2.0f * flRadius;

			// FOV only matters for the initial perspective matrix build inside
			// CreateFlashlight before the ortho parameters are registered.
			state.m_fHorizontalFOVDegrees = 90.0f;
			state.m_fVerticalFOVDegrees = 90.0f;

			state.m_pSpotlightTexture = m_CookieTexture[c];
			state.m_nSpotlightTextureFrame = 0;

			// Per-cascade shadow quality: override, else the shared value. A coarse
			// far cascade usually wants a bigger bias and softer filter than the
			// sharp near one.
			float flFilter = s_pCascade_filter[c]->GetFloat();
			float flDepthBias = s_pCascade_depthbias[c]->GetFloat();
			float flSlope = s_pCascade_slopescale[c]->GetFloat();

			state.m_bEnableShadows = true;
			state.m_flShadowMapResolution = nDepthRes;
			state.m_flShadowFilterSize = ( flFilter >= 0.0f ) ? flFilter : r_sunshadow_filter.GetFloat();
			state.m_flShadowSlopeScaleDepthBias = ( flSlope >= 0.0f ) ? flSlope : r_sunshadow_slopescale.GetFloat();
			state.m_flShadowDepthBias = ( flDepthBias >= 0.0f ) ? flDepthBias : r_sunshadow_depthbias.GetFloat();
			state.m_flShadowAtten = 0.0f;

			if ( m_ShadowHandle[c] == CLIENTSHADOW_INVALID_HANDLE )
			{
				m_ShadowHandle[c] = g_pClientShadowMgr->CreateFlashlight( state );
				if ( m_ShadowHandle[c] == CLIENTSHADOW_INVALID_HANDLE )
					continue;
			}

			// Ortho params (with this cascade's dedicated depth texture) first,
			// then the state update rebuilds the world-to-shadow matrix.
			g_pClientShadowMgr->SetFlashlightOrtho( m_ShadowHandle[c], true,
				-flRadius, -flRadius, flRadius, flRadius, c );
			g_pClientShadowMgr->UpdateFlashlightState( m_ShadowHandle[c], state );
			g_pClientShadowMgr->UpdateProjectedTexture( m_ShadowHandle[c], true );
		}
	}

	void DestroySunShadow()
	{
		for ( int c = 0; c < MAX_SUN_SHADOW_CASCADES; ++c )
		{
			if ( m_ShadowHandle[c] != CLIENTSHADOW_INVALID_HANDLE )
			{
				g_pClientShadowMgr->DestroyFlashlight( m_ShadowHandle[c] );
				m_ShadowHandle[c] = CLIENTSHADOW_INVALID_HANDLE;
			}
		}
	}

	//-----------------------------------------------------------------------------
	// Publish the two affine world->space transforms (baked mask and runtime sun
	// depth map) plus scalars to the material system's render parameters, where
	// the overriding LightmappedGeneric shader reads them. Both projections are
	// orthographic, so each is affine: result = Dot( worldPos, row ) + trans.
	// When the feature is off we publish enabled=0 so the shader early-outs.
	//-----------------------------------------------------------------------------
	void PublishShaderParams( bool bActive )
	{
		CMatRenderContextPtr pRenderContext( materials );

		VMatrix worldToSun;
		bool bReady = bActive && m_bHasMask && r_sunshadow_worldshader.GetBool() &&
			g_pClientShadowMgr->GetSunShadowToTextureMatrix( worldToSun );

		if ( !bReady )
		{
			pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_PARAMS0, Vector( 0, 0, 0 ) );
			m_flLastPublishedParams0X = 0.0f;
			return;
		}

		const SunShadowMaskHeader_t &h = m_MaskHeader;
		Vector vSunDir( h.vecSunDir[0], h.vecSunDir[1], h.vecSunDir[2] );
		Vector vOrigin( h.vecOrigin[0], h.vecOrigin[1], h.vecOrigin[2] );
		Vector vU( h.vecAxisU[0], h.vecAxisU[1], h.vecAxisU[2] );
		Vector vV( h.vecAxisV[0], h.vecAxisV[1], h.vecAxisV[2] );

		float flRangeU = MAX( h.flMaxU - h.flMinU, 1e-4f );
		float flRangeV = MAX( h.flMaxV - h.flMinV, 1e-4f );
		float flRangeD = MAX( h.flMaxDepth - h.flMinDepth, 1e-4f );

		// Baked-mask transform: world -> ( maskU[0,1], maskV[0,1], depthNorm[0,1] ).
		Vector vMaskRowU = vU / flRangeU;
		Vector vMaskRowV = vV / flRangeV;
		Vector vMaskRowD = vSunDir / flRangeD;
		Vector vMaskTrans(
			-( DotProduct( vOrigin, vU ) + h.flMinU ) / flRangeU,
			-( DotProduct( vOrigin, vV ) + h.flMinV ) / flRangeV,
			-( DotProduct( vOrigin, vSunDir ) + h.flMinDepth ) / flRangeD );

		pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_MASK_ROW_U, vMaskRowU );
		pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_MASK_ROW_V, vMaskRowV );
		pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_MASK_ROW_D, vMaskRowD );
		pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_MASK_TRANS, vMaskTrans );

		// Runtime sun depth transform: world -> ( shadowU[0,1], shadowV[0,1],
		// depth[0,1] ). worldToSun is the ortho flashlight world->texture matrix,
		// so rows 0..2 are the affine outputs (w is constant 1 for ortho).
		pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_SUN_ROW_U,
			Vector( worldToSun[0][0], worldToSun[0][1], worldToSun[0][2] ) );
		pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_SUN_ROW_V,
			Vector( worldToSun[1][0], worldToSun[1][1], worldToSun[1][2] ) );
		pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_SUN_ROW_D,
			Vector( worldToSun[2][0], worldToSun[2][1], worldToSun[2][2] ) );
		pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_SUN_TRANS,
			Vector( worldToSun[0][3], worldToSun[1][3], worldToSun[2][3] ) );

		float flMaskBiasNorm = r_sunshadow_maskbias.GetFloat() / flRangeD;
		pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_PARAMS0,
			Vector( 1.0f, flMaskBiasNorm, r_sunshadow_depthbias.GetFloat() ) );
		pRenderContext->SetVectorRenderingParameter( SUNSHADOW_RP_PARAMS1,
			Vector( clamp( r_sunshadow_darkness.GetFloat(), 0.0f, 1.0f ), 0.0f, 0.0f ) );

		m_flLastPublishedParams0X = 1.0f;
	}

	//-----------------------------------------------------------------------------
	// Load the VRAD-baked mask sidecar (maps/<name>.sunshadow) for this level.
	//-----------------------------------------------------------------------------
	void LoadSunShadowMask()
	{
		FreeSunShadowMask();

		const char *pszLevelName = engine->GetLevelName();		// "maps/foo.bsp"
		if ( !pszLevelName || !pszLevelName[0] )
			return;

		char szName[MAX_PATH];
		Q_StripExtension( pszLevelName, szName, sizeof( szName ) );
		Q_strncat( szName, ".sunshadow", sizeof( szName ), COPY_ALL_CHARACTERS );

		FileHandle_t hFile = filesystem->Open( szName, "rb", "GAME" );
		if ( hFile == FILESYSTEM_INVALID_HANDLE )
		{
			DevMsg( "Sun shadowmask: no baked mask '%s' (bake with vrad -sunshadowmask).\n", szName );
			return;
		}

		SunShadowMaskHeader_t hdr;
		if ( filesystem->Read( &hdr, sizeof( hdr ), hFile ) != sizeof( hdr ) ||
			 hdr.nMagic != SUNSHADOWMASK_MAGIC || hdr.nVersion != SUNSHADOWMASK_VERSION ||
			 hdr.nResolution < 1 || hdr.nResolution > 8192 )
		{
			Warning( "Sun shadowmask: '%s' is not a valid mask (magic/version/res).\n", szName );
			filesystem->Close( hFile );
			return;
		}

		int nBytes = hdr.nResolution * hdr.nResolution * 4;
		m_MaskData.EnsureCapacity( nBytes );
		if ( filesystem->Read( m_MaskData.Base(), nBytes, hFile ) != nBytes )
		{
			Warning( "Sun shadowmask: '%s' truncated.\n", szName );
			filesystem->Close( hFile );
			m_MaskData.Purge();
			return;
		}
		filesystem->Close( hFile );

		m_MaskHeader = hdr;
		m_bHasMask = true;
		CreateMaskTexture();

		DevMsg( "Sun shadowmask: loaded %s (%dx%d).\n", szName, hdr.nResolution, hdr.nResolution );
	}

	void FreeSunShadowMask()
	{
		m_MaskTexture.Shutdown();
		m_MaskData.Purge();
		m_bHasMask = false;
		memset( &m_MaskHeader, 0, sizeof( m_MaskHeader ) );
	}

	void CreateMaskTexture();

	ClientShadowHandle_t	m_ShadowHandle[MAX_SUN_SHADOW_CASCADES];
	CTextureReference		m_CookieTexture[MAX_SUN_SHADOW_CASCADES];
	CSunShadowCookieRegenerator m_CookieRegen[MAX_SUN_SHADOW_CASCADES];
	float					m_flLastRadius[MAX_SUN_SHADOW_CASCADES];	// cascade shape the cookies were built for
	float					m_flLastBlend;
	bool					m_bSuppressedEntityShadows;	// we turned off the legacy RTT entity shadows
	Vector					m_vecSunDirection;	// direction the sunlight travels (points down)
	Vector					m_vecSunColor;		// normalized hue from the BSP skylight
	int						m_nSunStyle;		// lightstyle VRAD baked the skylight with
	bool					m_bHasSun;

	// Baked sun-visibility mask (sun-projection space), loaded from the sidecar.
	bool					m_bHasMask;
	SunShadowMaskHeader_t	m_MaskHeader;
	CUtlMemory<unsigned char> m_MaskData;		// nRes*nRes*4, BGRA-order (R=vis, GB=depth)
	CTextureReference		m_MaskTexture;		// GPU copy for the shader (Stage 3)

public:
	bool IsPublishingShaderParams() const { return m_flLastPublishedParams0X > 0.5f; }
private:
	float					m_flLastPublishedParams0X;	// last enabled flag pushed to the shader
};

static CSunlightShadowManager s_SunlightShadowManager;

//-----------------------------------------------------------------------------
// Uploads the baked mask into a GPU texture the world shader will sample in
// Stage 3. Packs so the shader reads: .r = sun visibility, .g/.b = 16-bit
// nearest depth (high/low byte). Texture is IMAGE_FORMAT_BGRA8888, whose in-
// memory order is B,G,R,A.
//-----------------------------------------------------------------------------
class CSunMaskRegenerator : public ITextureRegenerator
{
public:
	CSunMaskRegenerator() : m_pData( NULL ), m_nRes( 0 ) {}
	void Set( const unsigned char *pData, int nRes ) { m_pData = pData; m_nRes = nRes; }

	virtual void RegenerateTextureBits( ITexture *pTexture, IVTFTexture *pVTFTexture, Rect_t *pRect )
	{
		if ( !m_pData )
			return;
		int w = pVTFTexture->Width();
		int h = pVTFTexture->Height();
		unsigned char *pDst = pVTFTexture->ImageData( 0, 0, 0 );
		for ( int y = 0; y < h; ++y )
		{
			for ( int x = 0; x < w; ++x )
			{
				const unsigned char *pSrc = &m_pData[ ( y * m_nRes + x ) * 4 ];	// [vis, depthHi, depthLo, 255]
				unsigned char *p = pDst + ( y * w + x ) * 4;					// BGRA
				p[0] = pSrc[2];		// B <- depthLo
				p[1] = pSrc[1];		// G <- depthHi
				p[2] = pSrc[0];		// R <- visibility
				p[3] = 255;
			}
		}
	}
	virtual void Release() {}

private:
	const unsigned char *m_pData;
	int m_nRes;
};

static CSunMaskRegenerator s_SunMaskRegen;

void CSunlightShadowManager::CreateMaskTexture()
{
	m_MaskTexture.Shutdown();
	if ( !m_bHasMask )
		return;

	int nRes = m_MaskHeader.nResolution;
	s_SunMaskRegen.Set( m_MaskData.Base(), nRes );

	m_MaskTexture.InitProceduralTexture( "sunshadowmask_baked", TEXTURE_GROUP_CLIENT_EFFECTS,
		nRes, nRes, IMAGE_FORMAT_BGRA8888,
		TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_NOMIP |
		TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_SINGLECOPY | TEXTUREFLAGS_PROCEDURAL );
	if ( m_MaskTexture.IsValid() )
	{
		m_MaskTexture->SetTextureRegenerator( &s_SunMaskRegen );
		m_MaskTexture->Download();
	}
}

//-----------------------------------------------------------------------------
// Debug info
//-----------------------------------------------------------------------------
CON_COMMAND( r_sunshadow_info, "Prints sun shadowmap status for the current map." )
{
	if ( !s_SunlightShadowManager.HasSun() )
	{
		Msg( "Sun shadowmap: no skylight (light_environment) in this map's worldlights.\n" );
		return;
	}

	const Vector &dir = s_SunlightShadowManager.GetSunDirection();
	const Vector &clr = s_SunlightShadowManager.GetSunColor();
	Msg( "Sun shadowmap: %s\n", r_sunshadow.GetBool() ? "enabled" : "disabled (r_sunshadow 0)" );
	Msg( "  direction: %.3f %.3f %.3f\n", dir.x, dir.y, dir.z );
	Msg( "  color:     %.3f %.3f %.3f (x r_sunshadow_intensity %.2f)\n", clr.x, clr.y, clr.z, r_sunshadow_intensity.GetFloat() );
	Msg( "  distance:  %.0f units, depth res %d\n", r_sunshadow_distance.GetFloat(), r_sunshadow_depthres.GetInt() );
	if ( s_SunlightShadowManager.GetSunStyle() != 0 )
	{
		Msg( "  skylight lightstyle %d: the BSP stores the sun's per-luxel lightmap contribution separately (shadowmask data present).\n",
			s_SunlightShadowManager.GetSunStyle() );
	}
	else
	{
		Msg( "  skylight lightstyle 0: sun contribution is merged into the base lightmap (no separate shadowmask data).\n" );
	}

	Msg( "  baked mask: %s\n", s_SunlightShadowManager.HasBakedMask()
		? "loaded (maps/<name>.sunshadow)" : "none (bake with vrad -sunshadowmask)" );
	Msg( "  world shader darkening: %s\n", s_SunlightShadowManager.IsPublishingShaderParams()
		? "publishing params to LightmappedGeneric override"
		: "inactive (needs baked mask + r_sunshadow_worldshader 1 + game_shader_dx9 override)" );

	if ( !materials->SupportsShadowDepthTextures() )
	{
		Msg( "  WARNING: hardware/dxlevel does not support shadow depth textures; feature inactive.\n" );
	}
	if ( !r_flashlightdepthtexture.GetBool() )
	{
		Msg( "  WARNING: r_flashlightdepthtexture is 0; feature inactive.\n" );
	}
}

//-----------------------------------------------------------------------------
// Probe the baked mask at the local player's feet (verifies load + projection
// + sampling independently of the shader). Compare against the _sunshadow.tga.
//-----------------------------------------------------------------------------
CON_COMMAND( r_sunshadow_probe, "Prints the baked sun visibility at the player's position." )
{
	if ( !s_SunlightShadowManager.HasBakedMask() )
	{
		Msg( "Sun shadowmask: no baked mask loaded for this map.\n" );
		return;
	}

	C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
	if ( !pPlayer )
		return;

	Vector vFeet = pPlayer->GetAbsOrigin();
	CSunlightShadowManager::MaskSample_t s;
	s_SunlightShadowManager.SampleBakedMask( vFeet, s );

	Msg( "Sun baked visibility at feet (%.0f %.0f %.0f): %.2f  (1 = full sun, 0 = baked shadow)\n",
		vFeet.x, vFeet.y, vFeet.z, s.result );
	if ( !s.bInRange )
	{
		Msg( "  projection: texU %.3f texV %.3f  -> OUTSIDE the baked rectangle (treated as lit).\n", s.texU, s.texV );
		return;
	}
	Msg( "  texel:    (%d,%d)  texU %.3f texV %.3f\n", s.x, s.y, s.texU, s.texV );
	if ( !s.bHasSurface )
	{
		Msg( "  no baked surface in this column (open sky) -> lit.\n" );
		return;
	}
	Msg( "  depth:    query %.4f  vs stored %.4f  (+bias %.4f)\n", s.depthNorm, s.storedDepth, s.biasNorm );
	Msg( "  compare:  query %s stored+bias  -> %s\n",
		s.bShadowed ? ">" : "<=", s.bShadowed ? "SHADOWED" : "lit (this is the sun-facing surface)" );
	Msg( "  R vis:    %.3f (soft sun visibility of the sun-facing surface)\n", s.vis );
}
