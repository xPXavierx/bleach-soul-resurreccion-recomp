#ifndef BLEACH_NATIVE_GRAPHICS_H
#define BLEACH_NATIVE_GRAPHICS_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Title-owned graphics policy. Program identities are hashes of decoded
 * fragment structure, independent of guest addresses and dynamic uniforms.
 * Run104's complete pass audit identifies the seven-tap final softening pass:
 * centre RGB * c8.z + six ring samples * (c0.y * c3.x).
 * It runs after depth-of-field/bloom and before the UI. No prior frame is read.
 * Off removes that legacy softening; scaled preserves its weights but measures
 * its radius in physical pixels; original preserves every original bit. */
#define BLEACH_FINAL_SOFTEN_PROGRAM UINT64_C(0x3581976ddf1ea736)
#define BLEACH_SCENE_RESOLVE_PROGRAM UINT64_C(0xd79b8553b8397edb)
enum bleach_final_filter_mode { BLEACH_FILTER_OFF, BLEACH_FILTER_SCALED, BLEACH_FILTER_ORIGINAL };
static enum bleach_final_filter_mode bleach_final_filter = BLEACH_FILTER_OFF;
static bool bleach_native_scene_resolve = true;
static float bleach_graphics_read(const uint32_t* words, unsigned i) {
    float value; memcpy(&value,words+i,sizeof(value)); return value;
}
static void bleach_graphics_write(uint32_t* words,unsigned i,float value) {
    memcpy(words+i,&value,sizeof(value));
}
static int bleach_graphics_transform(uint64_t program,float scale,uint32_t* words,
    uint32_t vectors,enum bleach_final_filter_mode mode) {
    if (program != BLEACH_FINAL_SOFTEN_PROGRAM || vectors != 9 || !scale || scale>4 ||
        mode==BLEACH_FILTER_ORIGINAL) return 0;
    if (mode==BLEACH_FILTER_SCALED) {
        if (scale==1) return 0;
        /* c0.x is the radius. The six offsets and all blend weights remain
         * the original values; changing this one physical unit is sufficient. */
        bleach_graphics_write(words,0,bleach_graphics_read(words,0)/(float)scale);
    } else {
        bleach_graphics_write(words,0,0.0f);  /* collapse ring */
        bleach_graphics_write(words,1,0.0f);  /* zero ring weight */
        bleach_graphics_write(words,34,1.0f); /* preserve centre and exposure */
    }
    return 1;
}
/* The scene surface stores two horizontal samples per output pixel. A
 * centre-aligned bilinear read averages precisely those two samples, at any
 * render scale. The legacy two-tap offsets belonged to RSX's convolution
 * kernel, not to this native grid. Keeping them displaced the taps by up to
 * 2.16 output pixels vertically at 4K, duplicating high-contrast contours.
 * Only the two colour lookups are centred. Packed depth reconstruction,
 * depth-of-field coefficients and the bloom MRT retain their original bits.
 * The production GPU test and a same-frame scene readback verify the actual
 * coverage average; an output image size alone is not that verification. */
static int bleach_graphics_resolve(uint64_t program,uint32_t* words,uint32_t vectors,bool native) {
    if (!native || program!=BLEACH_SCENE_RESOLVE_PROGRAM || vectors!=7) return 0;
    bleach_graphics_write(words,0,0.0f);
    bleach_graphics_write(words,1,0.0f);
    bleach_graphics_write(words,6,0.0f);
    return 1;
}
static void bleach_graphics_constants(uint64_t program,float scale,uint32_t* words,uint32_t vectors) {
    if (bleach_graphics_resolve(program,words,vectors,bleach_native_scene_resolve)) {
        static bool reported;
        if (!reported) {
            fprintf(stderr,"[native-graphics] scene resolve=centred two-sample grid scale=%.1f program=%016llx\n",
                scale,(unsigned long long)program);
            reported=true;
        }
    }
    if (bleach_graphics_transform(program,scale,words,vectors,bleach_final_filter)) {
        static bool reported;
        if (!reported) {
            fprintf(stderr,"[native-graphics] final filter=%s scale=%.1f program=%016llx\n",
                bleach_final_filter==BLEACH_FILTER_OFF ? "off" : "scaled",scale,
                (unsigned long long)program);
            reported=true;
        }
    }
}
static void bleach_graphics_init(void) {
    const char* resolve=getenv("BLEACH_SCENE_RESOLVE");
    bleach_native_scene_resolve=!(resolve && !strcmp(resolve,"legacy"));
    const char* mode=getenv("BLEACH_FINAL_FILTER");
    if (mode && !strcmp(mode,"original")) bleach_final_filter=BLEACH_FILTER_ORIGINAL;
    else if (mode && !strcmp(mode,"scaled")) bleach_final_filter=BLEACH_FILTER_SCALED;
    else bleach_final_filter=BLEACH_FILTER_OFF;
}
#endif
