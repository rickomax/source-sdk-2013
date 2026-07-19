//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: On-disk format for the baked sun shadowmask.
//
// VRAD (-sunshadowmask) bakes a soft, full-map sun-visibility texture in the
// sun's orthographic projection space and writes it next to the BSP as
// "maps/<mapname>.sunshadow". The client sun shadow system loads it and the
// world shader samples it by reconstructed world position to blend baked sun
// shadows with the runtime dynamic shadowmap (proper darkening rather than the
// additive-only sunlight).
//
// The file is not part of the BSP -- it is a standalone sidecar, so the BSP
// format is untouched and the mask can be regenerated independently.
//
//=============================================================================//

#ifndef SUNSHADOWMASK_H
#define SUNSHADOWMASK_H

#ifdef _WIN32
#pragma once
#endif

#define SUNSHADOWMASK_MAGIC		( ('S'<<0) | ('S'<<8) | ('H'<<16) | ('D'<<24) )	// 'SSHD'
#define SUNSHADOWMASK_VERSION	1

// Header, followed by nResolution*nResolution texels, row-major (y*res + x),
// 4 bytes each (BGRA-order friendly for direct texture upload):
//   [0] R = soft sun visibility at the nearest-to-sun surface (0..255)
//   [1] G = depth high byte  } 16-bit normalized depth of that surface over
//   [2] B = depth low  byte  } [flMinDepth, flMaxDepth]; 0xFFFF = no surface
//   [3] A = 255 (reserved)
//
// All vectors are world space. To project a world point P into the mask:
//   d = Dot( P - vecOrigin, vecSunDir )                       // along the sun
//   u = ( Dot( P - vecOrigin, vecAxisU ) - flMinU ) / (flMaxU - flMinU)
//   v = ( Dot( P - vecOrigin, vecAxisV ) - flMinV ) / (flMaxV - flMinV)
//   depthNorm = ( d - flMinDepth ) / ( flMaxDepth - flMinDepth )
// A pixel is in baked sun if its depthNorm ~= the stored depth at (u,v); if it
// is farther than the stored depth it is behind the sun-facing surface, i.e.
// in static shadow.
struct SunShadowMaskHeader_t
{
	int		nMagic;
	int		nVersion;
	int		nResolution;		// texels per side (square)

	float	vecSunDir[3];		// unit direction the sunlight travels
	float	vecOrigin[3];		// world-space origin of the projection
	float	vecAxisU[3];		// unit world-space U (right) axis
	float	vecAxisV[3];		// unit world-space V (up) axis

	float	flMinU, flMaxU;		// projection extents along U
	float	flMinV, flMaxV;		// projection extents along V
	float	flMinDepth, flMaxDepth;	// depth extents along the sun direction
};

//-----------------------------------------------------------------------------
// Client->shader contract for the darkening world shader.
//
// The client sun system publishes two affine world->space transforms plus a
// couple of scalars through the material system's VECTOR render parameters
// (slots 10..19 are unused by the stock engine). The overriding LightmappedGeneric
// shader reads them back, forwards them as pixel-shader constants, and:
//   * projects the surface world position into the baked mask (rows M_*) to read
//     the baked sun visibility, and
//   * projects it into the runtime sun depth map (rows S_*, texture
//     "_rt_SunShadowDepth") to read the dynamic sun shadow,
// then multiplies the lightmap by lerp( 1, dynamicSunVis, bakedSunVis ) so baked
// shadows are preserved and only baked-lit surfaces take the dynamic shadow.
//
// Each transform is affine (both projections are orthographic): a value is
//   result = Dot( worldPos, row.xyz ) + translation
// so a full 3-output transform needs 3 rows + 1 translation vector = 4 slots.
enum SunShadowRenderParm_t
{
	SUNSHADOW_RP_MASK_ROW_U   = 10,	// world->baked mask U  (linear part)
	SUNSHADOW_RP_MASK_ROW_V   = 11,	// world->baked mask V  (linear part)
	SUNSHADOW_RP_MASK_ROW_D   = 12,	// world->baked mask depthNorm (linear part)
	SUNSHADOW_RP_MASK_TRANS   = 13,	// (transU, transV, transDepth) for the mask

	SUNSHADOW_RP_SUN_ROW_U    = 14,	// world->runtime depth map U (linear part)
	SUNSHADOW_RP_SUN_ROW_V    = 15,	// world->runtime depth map V (linear part)
	SUNSHADOW_RP_SUN_ROW_D    = 16,	// world->runtime depth map projected depth (linear part)
	SUNSHADOW_RP_SUN_TRANS    = 17,	// (transU, transV, transDepth) for the depth map

	SUNSHADOW_RP_PARAMS0      = 18,	// x = enabled(0/1), y = mask depth bias, z = sun depth bias
	SUNSHADOW_RP_PARAMS1      = 19,	// x = min darkening (how dark a full dynamic shadow goes), yz reserved
};

#endif // SUNSHADOWMASK_H
