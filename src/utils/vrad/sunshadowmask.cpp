//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Bakes a soft, full-map sun-visibility shadowmask in the sun's
//          orthographic projection space and writes it beside the BSP as a
//          "<mapname>.sunshadow" sidecar file (see public/sunshadowmask.h).
//
// Enabled with the -sunshadowmask command line flag. Reuses VRAD's ray-tracing
// environment and the same TestLine_DoesHitSky visibility test the lightmap
// bake uses, so the mask matches the baked sun shadows (including soft
// penumbra from the sun's angular extent).
//
//=============================================================================//

#include "vrad.h"
#include "raytrace.h"
#include "mathlib/ssemath.h"
#include "worldsize.h"
#include "sunshadowmask.h"
#include "imagepacker.h"
#include "tier1/utlbuffer.h"
#include "tier1/strtools.h"
#include "filesystem.h"
#include <float.h>

// Options set from the command line (vrad.cpp).
bool g_bBuildSunShadowMask = false;
bool g_bDumpSunShadowMask = false;
bool g_bDumpLightmaps = false;
int  g_nSunShadowMaskRes = 1024;

//-----------------------------------------------------------------------------
// Minimal uncompressed 24-bit BGR TGA writer (top-left origin) for eyeballing
// the baked mask. No dependencies so it can't drift from the bake output.
//-----------------------------------------------------------------------------
static void WriteDebugTGA( const char *pszPath, const unsigned char *pBGR, int nWidth, int nHeight )
{
	FILE *fp = fopen( pszPath, "wb" );
	if ( !fp )
	{
		Msg( "-sunshadowmask: could not open %s for the debug image.\n", pszPath );
		return;
	}

	unsigned char hdr[18];
	memset( hdr, 0, sizeof( hdr ) );
	hdr[2]  = 2;								// uncompressed true-color
	hdr[12] = (unsigned char)( nWidth  & 0xFF );
	hdr[13] = (unsigned char)( ( nWidth  >> 8 ) & 0xFF );
	hdr[14] = (unsigned char)( nHeight & 0xFF );
	hdr[15] = (unsigned char)( ( nHeight >> 8 ) & 0xFF );
	hdr[16] = 24;								// bits per pixel
	hdr[17] = 0x20;								// top-left origin

	fwrite( hdr, 1, sizeof( hdr ), fp );
	fwrite( pBGR, 1, (size_t)nWidth * nHeight * 3, fp );
	fclose( fp );
}

// From lightmap.cpp / vrad
extern void TestLine_DoesHitSky( FourVectors const& start, FourVectors const& stop,
	fltx4 *pFractionVisible, bool canRecurse, int static_prop_to_skip, bool bDoDebug );

//-----------------------------------------------------------------------------
// Trace a column ray (from near the sun, along the sun direction) and return
// the distance to the first SOLID surface, or -1 if the column only sees sky /
// empty space.
//
// The column starts at the sun-facing extreme of the world, which on an outdoor
// map is above the skybox -- so the nearest hit is usually a sky brush. Sky is
// not a shadow-casting surface, so step past each sky hit and keep going until
// a non-sky surface (the ground/props) is found. Without this every column over
// the skybox interior reports "no surface" and the whole mask comes out empty.
//-----------------------------------------------------------------------------
static float TraceColumnFirstHit( const Vector &vStart, const Vector &vDir, float flMaxLen )
{
	Vector vOrigin = vStart;
	float flTravelled = 0.0f;

	for ( int iter = 0; iter < 16; ++iter )	// bound the sky layers we punch through
	{
		FourRays rays;
		FourVectors o4;	o4.DuplicateVector( vOrigin );
		FourVectors d4;	d4.DuplicateVector( vDir );
		rays.origin = o4;
		rays.direction = d4;

		float flRemaining = flMaxLen - flTravelled;
		if ( flRemaining <= 0.0f )
			return -1.0f;

		RayTracingResult result;
		g_RtEnv.Trace4Rays( rays, Four_Zeros, ReplicateX4( flRemaining ), &result, TRACE_ID_STATICPROP, NULL );

		if ( result.HitIds[0] == -1 )
			return -1.0f;	// nothing more along this column

		float flHit = SubFloat( result.HitDistance, 0 );
		int nTriID = g_RtEnv.OptimizedTriangleList[ result.HitIds[0] ].m_Data.m_IntersectData.m_nTriangleID;
		if ( !( nTriID & TRACE_ID_SKY ) )
			return flTravelled + flHit;	// first solid surface

		// It's the sky boundary -- advance just past it and continue.
		float flAdvance = flHit + 1.0f;
		vOrigin += vDir * flAdvance;
		flTravelled += flAdvance;
	}

	return -1.0f;
}

//-----------------------------------------------------------------------------
// Soft sun visibility at a surface point: fraction of the sun disk that is
// unoccluded, area-sampled with four jittered rays (one Trace4Rays call).
//-----------------------------------------------------------------------------
static float ComputeSunVisibility( const Vector &vSurfacePos, const Vector &vSunDir,
	const Vector &vAxisU, const Vector &vAxisV )
{
	// Offset slightly toward the sun to avoid self-intersection.
	Vector vStart = vSurfacePos - vSunDir * 2.0f;
	FourVectors start4;
	start4.DuplicateVector( vStart );

	// Four jittered destinations across the sun's angular extent.
	static const float s_flJitter[4][2] = { { 0.0f, 0.0f }, { 0.7f, -0.7f }, { -0.7f, 0.7f }, { 0.6f, 0.6f } };
	Vector vDest[4];
	for ( int i = 0; i < 4; ++i )
	{
		Vector vDir = -vSunDir;
		vDir += vAxisU * ( s_flJitter[i][0] * g_SunAngularExtent );
		vDir += vAxisV * ( s_flJitter[i][1] * g_SunAngularExtent );
		VectorNormalize( vDir );
		vDest[i] = vStart + vDir * MAX_TRACE_LENGTH;
	}

	FourVectors dest4;
	dest4.LoadAndSwizzle( vDest[0], vDest[1], vDest[2], vDest[3] );

	fltx4 fractionVisible = Four_Zeros;
	TestLine_DoesHitSky( start4, dest4, &fractionVisible, true, -1, false );

	float flSum = 0.0f;
	for ( int i = 0; i < 4; ++i )
		flSum += SubFloat( fractionVisible, i );
	return flSum * 0.25f;
}

//-----------------------------------------------------------------------------
// Main entry: build and write the sun shadowmask sidecar file.
//-----------------------------------------------------------------------------
void BuildSunShadowMask()
{
	if ( !g_bBuildSunShadowMask )
		return;

	// Find the sun.
	directlight_t *pSun = NULL;
	for ( directlight_t *dl = activelights; dl != NULL; dl = dl->next )
	{
		if ( dl->light.type == emit_skylight )
		{
			pSun = dl;
			break;
		}
	}
	if ( !pSun )
	{
		Msg( "-sunshadowmask: no light_environment (skylight) in this map; skipping.\n" );
		return;
	}

	int nRes = g_nSunShadowMaskRes;
	if ( nRes < 64 )  nRes = 64;
	if ( nRes > 8192 ) nRes = 8192;

	Vector vSunDir = pSun->light.normal;	// direction the sunlight travels
	VectorNormalize( vSunDir );

	// Orthographic basis perpendicular to the sun.
	Vector vUpRef( 0, 0, 1 );
	if ( fabs( vSunDir.z ) > 0.99f )
		vUpRef.Init( 0, 1, 0 );
	Vector vAxisU = CrossProduct( vUpRef, vSunDir );
	VectorNormalize( vAxisU );
	Vector vAxisV = CrossProduct( vSunDir, vAxisU );
	VectorNormalize( vAxisV );

	// Project the world AABB corners into sun space to size the projection.
	Vector vWorldMin, vWorldMax;
	VectorCopy( dmodels[0].mins, vWorldMin );
	VectorCopy( dmodels[0].maxs, vWorldMax );
	Vector vOrigin = ( vWorldMin + vWorldMax ) * 0.5f;

	float flMinU = FLT_MAX, flMaxU = -FLT_MAX;
	float flMinV = FLT_MAX, flMaxV = -FLT_MAX;
	float flMinD = FLT_MAX, flMaxD = -FLT_MAX;
	for ( int c = 0; c < 8; ++c )
	{
		Vector vCorner(
			( c & 1 ) ? vWorldMax.x : vWorldMin.x,
			( c & 2 ) ? vWorldMax.y : vWorldMin.y,
			( c & 4 ) ? vWorldMax.z : vWorldMin.z );
		Vector vRel = vCorner - vOrigin;
		float u = DotProduct( vRel, vAxisU );
		float v = DotProduct( vRel, vAxisV );
		float d = DotProduct( vRel, vSunDir );
		flMinU = min( flMinU, u ); flMaxU = max( flMaxU, u );
		flMinV = min( flMinV, v ); flMaxV = max( flMaxV, v );
		flMinD = min( flMinD, d ); flMaxD = max( flMaxD, d );
	}

	// Pad so surfaces on the boundary and the trace start are inside the range.
	const float flMargin = 16.0f;
	flMinU -= flMargin; flMaxU += flMargin;
	flMinV -= flMargin; flMaxV += flMargin;
	flMinD -= flMargin; flMaxD += flMargin;

	const float flDepthRange = flMaxD - flMinD;
	const float flColumnLen = flDepthRange + 2.0f * flMargin;

	Msg( "-sunshadowmask: baking %dx%d sun visibility (dir %.2f %.2f %.2f)...\n",
		nRes, nRes, vSunDir.x, vSunDir.y, vSunDir.z );
	float flStart = Plat_FloatTime();

	CUtlBuffer buf;
	SunShadowMaskHeader_t hdr;
	hdr.nMagic = SUNSHADOWMASK_MAGIC;
	hdr.nVersion = SUNSHADOWMASK_VERSION;
	hdr.nResolution = nRes;
	vSunDir.CopyToArray( hdr.vecSunDir );
	vOrigin.CopyToArray( hdr.vecOrigin );
	vAxisU.CopyToArray( hdr.vecAxisU );
	vAxisV.CopyToArray( hdr.vecAxisV );
	hdr.flMinU = flMinU; hdr.flMaxU = flMaxU;
	hdr.flMinV = flMinV; hdr.flMaxV = flMaxV;
	hdr.flMinDepth = flMinD; hdr.flMaxDepth = flMaxD;
	buf.Put( &hdr, sizeof( hdr ) );

	const float flInvDepthRange = ( flDepthRange > 0.0f ) ? 1.0f / flDepthRange : 0.0f;

	// Optional debug image (BGR). Visibility as grayscale; columns that hit no
	// surface (open sky) are tinted blue so coverage/gaps are obvious.
	unsigned char *pDebugBGR = NULL;
	if ( g_bDumpSunShadowMask )
	{
		pDebugBGR = (unsigned char *)malloc( (size_t)nRes * nRes * 3 );
	}

	for ( int y = 0; y < nRes; ++y )
	{
		for ( int x = 0; x < nRes; ++x )
		{
			float u = flMinU + ( ( x + 0.5f ) / nRes ) * ( flMaxU - flMinU );
			float v = flMinV + ( ( y + 0.5f ) / nRes ) * ( flMaxV - flMinV );

			// Start just before the near depth and trace along the sun direction.
			Vector vColStart = vOrigin + vAxisU * u + vAxisV * v + vSunDir * ( flMinD - flMargin );

			unsigned char rgba[4] = { 0, 0xFF, 0xFF, 0xFF };	// vis 0, depth = no-hit
			bool bHit = false;

			float flHitDist = TraceColumnFirstHit( vColStart, vSunDir, flColumnLen );
			if ( flHitDist >= 0.0f )
			{
				bHit = true;
				Vector vHit = vColStart + vSunDir * flHitDist;
				float d = DotProduct( vHit - vOrigin, vSunDir );
				float flDepthNorm = clamp( ( d - flMinD ) * flInvDepthRange, 0.0f, 1.0f );
				unsigned short usDepth = (unsigned short)( flDepthNorm * 65535.0f + 0.5f );

				float flVis = ComputeSunVisibility( vHit, vSunDir, vAxisU, vAxisV );
				flVis = clamp( flVis, 0.0f, 1.0f );

				rgba[0] = (unsigned char)( flVis * 255.0f + 0.5f );
				rgba[1] = (unsigned char)( usDepth >> 8 );
				rgba[2] = (unsigned char)( usDepth & 0xFF );
				rgba[3] = 0xFF;
			}

			buf.Put( rgba, 4 );

			if ( pDebugBGR )
			{
				unsigned char *pPixel = pDebugBGR + ( (size_t)y * nRes + x ) * 3;
				if ( bHit )
				{
					pPixel[0] = pPixel[1] = pPixel[2] = rgba[0];	// grayscale visibility
				}
				else
				{
					pPixel[0] = 0x80; pPixel[1] = 0x00; pPixel[2] = 0x00;	// blue: no surface
				}
			}
		}

		if ( ( y & 63 ) == 0 )
		{
			Msg( "\r-sunshadowmask: %d%%   ", ( y * 100 ) / nRes );
			fflush( stdout );
		}
	}

	// Write "<source>.sunshadow" next to the BSP.
	char szName[1024];
	Q_StripExtension( source, szName, sizeof( szName ) );
	Q_strncat( szName, ".sunshadow", sizeof( szName ), COPY_ALL_CHARACTERS );

	// Raw stdio: 'source' is an absolute path, which the game filesystem's
	// search-path/write-path rules don't handle cleanly for writes.
	FILE *fp = fopen( szName, "wb" );
	if ( !fp )
	{
		Msg( "\n-sunshadowmask: ERROR opening %s for writing.\n", szName );
		return;
	}
	fwrite( buf.Base(), 1, buf.TellPut(), fp );
	fclose( fp );

	Msg( "\r-sunshadowmask: wrote %s (%d KB) in %.1fs\n",
		szName, buf.TellPut() / 1024, Plat_FloatTime() - flStart );

	if ( pDebugBGR )
	{
		char szTGA[1024];
		Q_StripExtension( source, szTGA, sizeof( szTGA ) );
		Q_strncat( szTGA, "_sunshadow.tga", sizeof( szTGA ), COPY_ALL_CHARACTERS );
		WriteDebugTGA( szTGA, pDebugBGR, nRes, nRes );
		Msg( "-sunshadowmask: wrote debug image %s\n", szTGA );
		free( pDebugBGR );
	}
}

//-----------------------------------------------------------------------------
// Packs the finished per-face lightmaps into TGA page(s) for inspection.
// Reads the LDR lighting lump (pdlightdata) populated by FinalLightFace; each
// face's style-0 block is decoded with VRAD's own ConvertRGBExp32ToRGBA8888
// (the same gamma/colorspace the engine shows), and packed with CImagePacker
// -- the very packer VBSP uses for lightmap pages. Enable with -dumplightmaps.
//-----------------------------------------------------------------------------
void DumpLightmapsToTGA()
{
	if ( !g_bDumpLightmaps )
		return;

	if ( !pdlightdata || pdlightdata->Count() == 0 )
	{
		Msg( "-dumplightmaps: no (LDR) lighting data in the bsp; nothing to dump.\n" );
		return;
	}

	const int nPageSize = 1024;		// atlas page dimension
	const int nPad = 1;				// gutter between faces

	unsigned char *pImage = (unsigned char *)malloc( (size_t)nPageSize * nPageSize * 3 );
	if ( !pImage )
		return;

	CImagePacker packer;
	packer.Reset( nPageSize, nPageSize );
	memset( pImage, 0, (size_t)nPageSize * nPageSize * 3 );

	char szBase[1024];
	Q_StripExtension( source, szBase, sizeof( szBase ) );

	int nPage = 0;
	int nPacked = 0;

	for ( int f = 0; f < numfaces; ++f )
	{
		dface_t *pFace = &dfaces[f];
		if ( pFace->lightofs == -1 )
			continue;

		int w = pFace->m_LightmapTextureSizeInLuxels[0] + 1;
		int h = pFace->m_LightmapTextureSizeInLuxels[1] + 1;
		if ( w <= 0 || h <= 0 || w > nPageSize || h > nPageSize )
			continue;

		int px = 0, py = 0;
		if ( !packer.AddBlock( w + nPad, h + nPad, &px, &py ) )
		{
			// Page is full: write it out and open a fresh one.
			char szName[1024];
			Q_snprintf( szName, sizeof( szName ), "%s_lightmap%d.tga", szBase, nPage );
			WriteDebugTGA( szName, pImage, nPageSize, nPageSize );
			Msg( "-dumplightmaps: wrote %s\n", szName );

			++nPage;
			packer.Reset( nPageSize, nPageSize );
			memset( pImage, 0, (size_t)nPageSize * nPageSize * 3 );
			if ( !packer.AddBlock( w + nPad, h + nPad, &px, &py ) )
				continue;
		}

		// Style 0, first w*h luxels (the flat lightmap for bumped faces too).
		const ColorRGBExp32 *pLuxels = (const ColorRGBExp32 *)&(*pdlightdata)[pFace->lightofs];
		for ( int y = 0; y < h; ++y )
		{
			for ( int x = 0; x < w; ++x )
			{
				unsigned char rgba[4];
				ConvertRGBExp32ToRGBA8888( &pLuxels[y * w + x], rgba );

				unsigned char *pPixel = pImage + ( (size_t)( py + y ) * nPageSize + ( px + x ) ) * 3;
				pPixel[0] = rgba[2];	// B
				pPixel[1] = rgba[1];	// G
				pPixel[2] = rgba[0];	// R
			}
		}
		++nPacked;
	}

	char szName[1024];
	Q_snprintf( szName, sizeof( szName ), "%s_lightmap%d.tga", szBase, nPage );
	WriteDebugTGA( szName, pImage, nPageSize, nPageSize );
	Msg( "-dumplightmaps: wrote %s (%d faces across %d page(s))\n", szName, nPacked, nPage + 1 );

	free( pImage );
}
