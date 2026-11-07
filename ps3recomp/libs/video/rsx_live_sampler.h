#ifndef RSX_LIVE_SAMPLER_H
#define RSX_LIVE_SAMPLER_H
#include <d3d12.h>
#include "rsx_dispatch.h"
/* Shared by the live D3D12 path and its production-state regression test. */
static D3D12_TEXTURE_ADDRESS_MODE gcm_wrap(u32 w)
{
    switch (w & 0xF) {
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 6:
    case 7:
    case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
}

static D3D12_SAMPLER_DESC decode_sampler(const rsx_dsp_texture* t)
{
    D3D12_SAMPLER_DESC sd = {0};
    const u32 minf = (t->filter >> 16) & 0x7;
    const u32 magf = (t->filter >> 24) & 0x7;
    /* The native sample-grid path reconstructs convolution requests with
     * bilinear filtering. This is deliberately not an emulation of the RSX
     * quincunx/Gaussian kernel; falling through to POINT discarded coverage
     * when reducing a two-sample scene. Convolution has no mip chain. */
    const int min_linear = (minf == 2 || minf == 4 || minf == 6 || minf == 7);
    const int mag_linear = (magf == 2 || magf == 4);
    const int mip_linear = (minf == 5 || minf == 6);
    const int mip_present = (minf >= 3 && minf <= 6);
    D3D12_FILTER_TYPE mnf = min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mgf = mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE mpf = mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_ENCODE_BASIC_FILTER(mnf, mgf, mpf, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    sd.AddressU = gcm_wrap(t->wrap);
    sd.AddressV = gcm_wrap(t->wrap >> 8);
    sd.AddressW = gcm_wrap(t->wrap >> 16);
    const u32 max_lod_fx = (t->control0 >> 7)  & 0xFFF;
    const u32 min_lod_fx = (t->control0 >> 19) & 0xFFF;
    sd.MinLOD = (float)min_lod_fx / 256.0f;
    sd.MaxLOD = mip_present ? (float)max_lod_fx / 256.0f : 0.0f;
    if (sd.MaxLOD < sd.MinLOD) sd.MaxLOD = sd.MinLOD;
    sd.MaxAnisotropy = 1;
    /* RSX's packed ARGB border must survive the descriptor cache as well as
     * descriptor creation. Shadow coordinates outside a light's map depend
     * on this value, even when ordinary textures use identical filtering. */
    sd.BorderColor[0] = ((t->border_color >> 16) & 255u) / 255.0f;
    sd.BorderColor[1] = ((t->border_color >> 8) & 255u) / 255.0f;
    sd.BorderColor[2] = (t->border_color & 255u) / 255.0f;
    sd.BorderColor[3] = ((t->border_color >> 24) & 255u) / 255.0f;
    return sd;
}

#endif
