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
	"Distance from the player at which the sun shadowmap ends. The outer 25% is a fade band blending back to the baked lightmap." );
static ConVar r_sunshadow_intensity( "r_sunshadow_intensity", "1.0", FCVAR_ARCHIVE,
	"Brightness multiplier for the dynamic sunlight (color comes from the map's light_environment)." );
static ConVar r_sunshadow_casterheight( "r_sunshadow_casterheight", "8192", 0,
	"How far above the player the sun shadow frustum starts; geometry up to this height still casts." );
static ConVar r_sunshadow_filter( "r_sunshadow_filter", "1.0", FCVAR_ARCHIVE,
	"Sun shadowmap filter kernel size." );
static ConVar r_sunshadow_depthbias( "r_sunshadow_depthbias", "0.0005", 0 );
static ConVar r_sunshadow_slopescale( "r_sunshadow_slopescale", "4", 0 );

//-----------------------------------------------------------------------------
// Procedural cookie: white core with a smooth falloff to black over the outer
// band. The flashlight pass multiplies by this texture, which gives us the
// spatial blend from dynamic sunlight back to pure baked lighting at the edge
// of the shadowed region. Chebyshev (max-norm) distance keeps the fade band a
// constant width along each side of the square ortho projection.
//-----------------------------------------------------------------------------
#define SUNSHADOW_COOKIE_RES		256
#define SUNSHADOW_FADE_START		0.75f	// fade begins at 75% of r_sunshadow_distance

class CSunShadowCookieRegenerator : public ITextureRegenerator
{
public:
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
				if ( flDist <= SUNSHADOW_FADE_START )
				{
					flIntensity = 1.0f;
				}
				else
				{
					float t = ( flDist - SUNSHADOW_FADE_START ) / ( 1.0f - SUNSHADOW_FADE_START );
					t = clamp( t, 0.0f, 1.0f );
					flIntensity = 1.0f - ( t * t * ( 3.0f - 2.0f * t ) );	// smoothstep down
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
};

static CSunShadowCookieRegenerator s_SunShadowCookieRegen;

//-----------------------------------------------------------------------------
// The sun shadow manager
//-----------------------------------------------------------------------------
class CSunlightShadowManager : public CAutoGameSystemPerFrame
{
public:
	CSunlightShadowManager() : CAutoGameSystemPerFrame( "CSunlightShadowManager" )
	{
		m_ShadowHandle = CLIENTSHADOW_INVALID_HANDLE;
		m_bHasSun = false;
		m_nSunStyle = 0;
		m_vecSunDirection.Init( 0, 0, -1 );
		m_vecSunColor.Init( 1, 1, 1 );
		m_bHasMask = false;
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
		m_bHasSun = false;
		m_nSunStyle = 0;
	}

	bool HasBakedMask() const { return m_bHasMask; }

	//-----------------------------------------------------------------------------
	// CPU sample of the baked mask at a world position. Returns baked sun
	// visibility 0..1 (0 = in baked shadow / behind the sun-facing surface).
	// This is the reference the shader will reproduce; also used by the probe cmd.
	//-----------------------------------------------------------------------------
	float GetBakedSunVisibility( const Vector &vecWorldPos ) const
	{
		if ( !m_bHasMask )
			return 1.0f;

		const SunShadowMaskHeader_t &h = m_MaskHeader;
		Vector vSunDir( h.vecSunDir[0], h.vecSunDir[1], h.vecSunDir[2] );
		Vector vOrigin( h.vecOrigin[0], h.vecOrigin[1], h.vecOrigin[2] );
		Vector vU( h.vecAxisU[0], h.vecAxisU[1], h.vecAxisU[2] );
		Vector vV( h.vecAxisV[0], h.vecAxisV[1], h.vecAxisV[2] );

		Vector vRel = vecWorldPos - vOrigin;
		float u = DotProduct( vRel, vU );
		float v = DotProduct( vRel, vV );
		float d = DotProduct( vRel, vSunDir );

		float texU = ( u - h.flMinU ) / ( h.flMaxU - h.flMinU );
		float texV = ( v - h.flMinV ) / ( h.flMaxV - h.flMinV );
		if ( texU < 0.0f || texU > 1.0f || texV < 0.0f || texV > 1.0f )
			return 1.0f;	// outside the baked region -- assume lit

		float depthNorm = ( d - h.flMinDepth ) / ( h.flMaxDepth - h.flMinDepth );

		int x = (int)( texU * ( h.nResolution - 1 ) + 0.5f );
		int y = (int)( texV * ( h.nResolution - 1 ) + 0.5f );
		const unsigned char *pTexel = &m_MaskData[ ( y * h.nResolution + x ) * 4 ];

		float flVis = pTexel[0] / 255.0f;						// R = soft sun visibility
		unsigned int usDepth = ( pTexel[1] << 8 ) | pTexel[2];	// G,B = 16-bit nearest depth
		if ( usDepth == 0xFFFF )
			return 1.0f;										// no surface recorded here
		float flStoredDepth = usDepth / 65535.0f;

		// If we're farther from the sun than the nearest recorded surface, we're
		// behind it -> in static shadow. Otherwise this is the sun-facing surface.
		const float flDepthBias = 2.0f / 65535.0f;
		if ( depthNorm > flStoredDepth + flDepthBias )
			return 0.0f;

		return flVis;
	}

	virtual void Update( float frametime )
	{
		if ( !m_bHasSun || !r_sunshadow.GetBool() || !r_flashlightdepthtexture.GetBool() ||
			 !materials->SupportsShadowDepthTextures() )
		{
			DestroySunShadow();
			return;
		}

		C_BasePlayer *pPlayer = C_BasePlayer::GetLocalPlayer();
		if ( !pPlayer )
		{
			DestroySunShadow();
			return;
		}

		UpdateSunShadow( pPlayer->EyePosition() );
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

	void InitCookieTexture()
	{
		if ( m_CookieTexture.IsValid() )
			return;

		m_CookieTexture.InitProceduralTexture( "sunshadow_cookie", TEXTURE_GROUP_CLIENT_EFFECTS,
			SUNSHADOW_COOKIE_RES, SUNSHADOW_COOKIE_RES, IMAGE_FORMAT_BGRA8888,
			TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_NOMIP |
			TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_SINGLECOPY | TEXTUREFLAGS_PROCEDURAL );
		if ( m_CookieTexture.IsValid() )
		{
			m_CookieTexture->SetTextureRegenerator( &s_SunShadowCookieRegen );
			m_CookieTexture->Download();
		}
	}

	void UpdateSunShadow( const Vector &vecPlayerEyes )
	{
		float flRadius = MAX( r_sunshadow_distance.GetFloat(), 256.0f );
		float flCasterHeight = MAX( r_sunshadow_casterheight.GetFloat(), flRadius );

		InitCookieTexture();
		if ( !m_CookieTexture.IsValid() )
			return;

		QAngle angSun;
		VectorAngles( m_vecSunDirection, angSun );

		// Snap the projection center to shadowmap-texel-sized world increments in
		// the light's lateral plane, so shadow edges don't shimmer as the player
		// moves ("texel snapping").
		Vector vecFwd, vecRight, vecUp;
		AngleVectors( angSun, &vecFwd, &vecRight, &vecUp );

		Vector vecCenter = vecPlayerEyes;
		int nDepthRes = MAX( r_sunshadow_depthres.GetInt(), 1 );
		float flTexelSize = ( 2.0f * flRadius ) / nDepthRes;
		float flRightCoord = DotProduct( vecCenter, vecRight );
		float flUpCoord = DotProduct( vecCenter, vecUp );
		vecCenter += vecRight * ( floorf( flRightCoord / flTexelSize ) * flTexelSize - flRightCoord );
		vecCenter += vecUp * ( floorf( flUpCoord / flTexelSize ) * flTexelSize - flUpCoord );

		FlashlightState_t state;
		state.m_vecLightOrigin = vecCenter - m_vecSunDirection * flCasterHeight;
		AngleQuaternion( angSun, state.m_quatOrientation );

		state.m_fQuadraticAtten = 0.0f;
		state.m_fLinearAtten = 0.0f;
		state.m_fConstantAtten = 1.0f;

		float flIntensity = r_sunshadow_intensity.GetFloat();
		state.m_Color[0] = m_vecSunColor.x * flIntensity;
		state.m_Color[1] = m_vecSunColor.y * flIntensity;
		state.m_Color[2] = m_vecSunColor.z * flIntensity;
		state.m_Color[3] = 0.0f;

		state.m_NearZ = 16.0f;
		state.m_FarZ = flCasterHeight + 2.0f * flRadius;

		// FOV values are only used for the initial (perspective) matrix build
		// inside CreateFlashlight before the ortho parameters are registered.
		state.m_fHorizontalFOVDegrees = 90.0f;
		state.m_fVerticalFOVDegrees = 90.0f;

		state.m_pSpotlightTexture = m_CookieTexture;
		state.m_nSpotlightTextureFrame = 0;

		state.m_bEnableShadows = true;
		state.m_flShadowMapResolution = nDepthRes;
		state.m_flShadowFilterSize = r_sunshadow_filter.GetFloat();
		state.m_flShadowSlopeScaleDepthBias = r_sunshadow_slopescale.GetFloat();
		state.m_flShadowDepthBias = r_sunshadow_depthbias.GetFloat();
		state.m_flShadowAtten = 0.0f;

		if ( m_ShadowHandle == CLIENTSHADOW_INVALID_HANDLE )
		{
			m_ShadowHandle = g_pClientShadowMgr->CreateFlashlight( state );
			if ( m_ShadowHandle == CLIENTSHADOW_INVALID_HANDLE )
				return;
		}

		// Ortho params first, then the state update rebuilds the world-to-shadow
		// matrix orthographically.
		g_pClientShadowMgr->SetFlashlightOrtho( m_ShadowHandle, true,
			-flRadius, -flRadius, flRadius, flRadius );
		g_pClientShadowMgr->UpdateFlashlightState( m_ShadowHandle, state );
		g_pClientShadowMgr->UpdateProjectedTexture( m_ShadowHandle, true );
	}

	void DestroySunShadow()
	{
		if ( m_ShadowHandle != CLIENTSHADOW_INVALID_HANDLE )
		{
			g_pClientShadowMgr->DestroyFlashlight( m_ShadowHandle );
			m_ShadowHandle = CLIENTSHADOW_INVALID_HANDLE;
		}
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

	ClientShadowHandle_t	m_ShadowHandle;
	CTextureReference		m_CookieTexture;
	Vector					m_vecSunDirection;	// direction the sunlight travels (points down)
	Vector					m_vecSunColor;		// normalized hue from the BSP skylight
	int						m_nSunStyle;		// lightstyle VRAD baked the skylight with
	bool					m_bHasSun;

	// Baked sun-visibility mask (sun-projection space), loaded from the sidecar.
	bool					m_bHasMask;
	SunShadowMaskHeader_t	m_MaskHeader;
	CUtlMemory<unsigned char> m_MaskData;		// nRes*nRes*4, BGRA-order (R=vis, GB=depth)
	CTextureReference		m_MaskTexture;		// GPU copy for the shader (Stage 3)
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
	Vector vEyes = pPlayer->EyePosition();
	Msg( "Sun baked visibility: feet %.2f, eyes %.2f  (1 = full sun, 0 = baked shadow)\n",
		s_SunlightShadowManager.GetBakedSunVisibility( vFeet ),
		s_SunlightShadowManager.GetBakedSunVisibility( vEyes ) );
}
