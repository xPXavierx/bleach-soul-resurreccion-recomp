#ifndef RSX_ASPECT_H
#define RSX_ASPECT_H

/* Display aspect ratio for ultrawide monitors: BLEACH_ASPECT=16:9|21:9|32:9|
 * <w>:<h>|<w>x<h>|<decimal>|auto (the primary monitor, which the borderless
 * window covers). "21:9" is 64:27 (2560x1080, 5120x2160), or the primary
 * monitor's own shape when it is one of that family (3440x1440, 3840x1600).
 *
 * The game keeps its 1280x720 canvas. Everyone agrees on one factor
 * k = aspect / (16/9): the game's projection widens the horizontal field of
 * view by k (also for its culling), the renderer squeezes 2D by 1/k around
 * the centre, and presentation stretches the canvas by k onto a swap chain of
 * the monitor's shape, so neither 3D nor 2D ends up distorted. Unset, 16:9 or
 * anything narrower: k = 1 and nothing changes. Shared by the renderer (C)
 * and the game's projection hook (C++); both parse the same environment. */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

static inline double rsx_aspect_parse(const char* e)
{
    if (!e || !*e) return 0;
    if (!strcmp(e, "21:9")) {
#ifdef _WIN32
        const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        if (sw > 0 && sh > 0 && (double)sw / sh >= 2.3 && (double)sw / sh <= 2.45) return (double)sw / sh;
#endif
        return 64.0 / 27.0;
    }
    if (!strcmp(e, "auto")) {
#ifdef _WIN32
        const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
        return w > 0 && h > 0 ? (double)w / h : 0;
#else
        return 0;
#endif
    }
    double w = 0, h = 0; char sep = 0, tail = 0;
    if (sscanf(e, "%lf%c%lf%c", &w, &sep, &h, &tail) == 3 && (sep == ':' || sep == 'x' || sep == 'X'))
        return w > 0 && h > 0 ? w / h : 0;
    if (sscanf(e, "%lf%c", &w, &tail) == 1) return w > 0 ? w : 0;
    return 0;
}

/* The display aspect ratio in use (16/9 when off). */
static inline double rsx_aspect_ratio(void)
{
    static double ratio = 0;
    if (ratio == 0) {
        const double a = rsx_aspect_parse(getenv("BLEACH_ASPECT"));
        ratio = a > 16.0 / 9.0 + 1e-3 && a <= 6.0 ? a : 16.0 / 9.0;
    }
    return ratio;
}

/* k: horizontal widening of the 16:9 canvas, 1 when off. */
static inline float rsx_aspect_factor(void)
{
    return (float)(rsx_aspect_ratio() / (16.0 / 9.0));
}

#endif
