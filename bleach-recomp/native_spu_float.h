/* Compatibility names for generated title code; semantics live in the SDK.
 * Caller owns RTZ + DAZ + FTZ, with CPU AVX2/FMA dispatch at image selection. */
#ifndef BLEACH_NATIVE_SPU_FLOAT_H
#define BLEACH_NATIVE_SPU_FLOAT_H
#include "spu_native_float_x86.h"
#define bleach_fp_fa spu_native_fp_fa
#define bleach_fp_fs spu_native_fp_fs
#define bleach_fp_fm spu_native_fp_fm
#define bleach_fp_fma spu_native_fp_fma
#define bleach_fp_fms spu_native_fp_fms
#define bleach_fp_fnms spu_native_fp_fnms
#endif
