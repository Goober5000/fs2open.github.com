/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/ 

#ifndef _MODELSINC_H
#define _MODELSINC_H

#include "globalincs/pstypes.h"

class polymodel;

#ifndef MODEL_LIB 
#pragma message ("This should only be used internally by the model library.  See John if you think you need to include this elsewhere.")
#endif

#define OP_EOF 			0
#define OP_DEFPOINTS 	1
#define OP_FLATPOLY		2
#define OP_TMAPPOLY		3
#define OP_SORTNORM		4
#define OP_BOUNDBOX		5
#define OP_TMAP2POLY	6
#define OP_SORTNORM2	7

// endianess will be handled by cfile and others now, little-endian should be default in all cases

// little-endian (Intel) IDs
#define POF_HEADER_ID  0x4f505350	// 'OPSP' (PSPO) POF file header

// FREESPACE1 FORMAT
#define ID_OHDR 0x5244484f			// RDHO (OHDR): POF file header
#define ID_SOBJ 0x4a424f53			// JBOS (SOBJ): Subobject header

// FREESPACE2 FORMAT
#define ID_HDR2 0x32524448			// 2RDH (HDR2): POF file header
#define ID_OBJ2 0x324a424f			// 2JBO (OBJ2): Subobject header

#define ID_TXTR 0x52545854				// RTXT (TXTR): Texture filename list
#define ID_INFO 0x464e4950				// FNIP (PINF): POF file information, like command line, etc
#define ID_GRID 0x44495247				// DIRG (GRID): Grid information
#define ID_SPCL 0x4c435053				// LCPS (SPCL): Special object -- like a gun, missile, docking point, etc.
#define ID_PATH 0x48544150				// HTAP (PATH): A spline based path
#define ID_GPNT 0x544e5047				// TNPG (GPNT): gun points
#define ID_MPNT 0x544e504d				// TNPM (MPNT): missile points
#define ID_DOCK 0x4b434f44				// KCOD (DOCK): docking points
#define ID_TGUN 0x4e554754				// NUGT (TGUN): turret gun points
#define ID_TMIS 0x53494d54				// SIMT (TMIS): turret missile points
#define ID_FUEL 0x4c455546				// LEUF (FUEL): thruster points
#define ID_SHLD 0x444c4853				// DLHS (SHLD): shield definition
#define ID_EYE  0x20455945				//  EYE (EYE ): eye information
#define ID_INSG 0x47534e49				// GSNI (INSG): insignia information
#define ID_ACEN 0x4e454341				// NECA (ACEN): autocentering information
#define ID_GLOW 0x574f4c47				// WOLG (GLOW): glow points -Bobboau
#define ID_GLOX 0x584f4c47				// experimental glow points will be gone as soon as we get a proper pof editor -Bobboau
#define ID_SLDC 0x43444c53				// CDLS (SLDC): Shield Collision Tree
#define ID_SLC2 0x32434c53				// 2CLS (SLC2): Shield Collision Tree with ints instead of char - ShivanSpS

extern const ubyte* Macro_ubyte_bounds;

#ifndef NDEBUG
#define us(p)	(AssertExpr(p < Macro_ubyte_bounds), *reinterpret_cast<ushort*>(p))
#define cus(p)  (AssertExpr(p < Macro_ubyte_bounds), *reinterpret_cast<const ushort*>(p))
#define uw(p)	(AssertExpr(p < Macro_ubyte_bounds), *reinterpret_cast<uint*>(p))
#define cuw(p)  (AssertExpr(p < Macro_ubyte_bounds), *reinterpret_cast<const uint*>(p))
#define w(p)	(AssertExpr(p < Macro_ubyte_bounds), *reinterpret_cast<int*>(p))
#define cw(p)   (AssertExpr(p < Macro_ubyte_bounds), *reinterpret_cast<const int*>(p))
#define wp(p)	(AssertExpr(p < Macro_ubyte_bounds), reinterpret_cast<int*>(p)
#define vp(p)	(AssertExpr(p < Macro_ubyte_bounds), reinterpret_cast<vec3d*>(p))
#define fl(p)	(AssertExpr(p < Macro_ubyte_bounds), *reinterpret_cast<float*>(p))
#define cfl(p)  (AssertExpr(p < Macro_ubyte_bounds), *reinterpret_cast<const float*>(p))
#else
#define us(p)	(*reinterpret_cast<ushort*>(p))
#define cus(p)  (*reinterpret_cast<const ushort*>(p))
#define uw(p)	(*reinterpret_cast<uint*>(p))
#define cuw(p)  (*reinterpret_cast<const uint*>(p))
#define w(p)	(*reinterpret_cast<int*>(p))
#define cw(p)   (*reinterpret_cast<const int*>(p))
#define wp(p)	(reinterpret_cast<int*>(p)
#define vp(p)	(reinterpret_cast<vec3d*>(p))
#define fl(p)	(*reinterpret_cast<float*>(p))
#define cfl(p)  (*reinterpret_cast<const float*>(p))
#endif

void model_calc_bound_box(vec3d *box, const vec3d *big_mn, const vec3d *big_mx);

void interp_clear_instance();

// endian swapping stuff - tigital
void swap_bsp_data( polymodel *pm, void *model_ptr );

// endian swapping stuff - kaz
void swap_sldc_data(ubyte *buffer);

extern vec3d **Interp_verts;

#endif
