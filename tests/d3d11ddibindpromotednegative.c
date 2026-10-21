/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Every case in this file must fail to compile, and each is compiled on its
 * own with -DCASE=n: in one translation unit the first failure would hide
 * whether the others fail too.
 *
 * The constant-buffer, update, sampler and scissor promotion changes no
 * offset and no size, so only the strong types can catch a parameter list
 * transcribed wrongly -- a sampler handle where a resource belongs, a
 * viewport where the update box or scissor rectangle belongs.
 */
#include "wine_d3d11ddi.h"

/* Not #error: CI's clean-room dependency pass preprocesses every
 * tests/d3d11ddi*.c without -DCASE, and must succeed.  A compile without a
 * case still fails, at the declaration below. */
#ifndef CASE
#define CASE 0
#endif

#if CASE == 1
void bind_constant_buffers_with_sampler_handles(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs, D3D10DDI_HDEVICE hDevice,
        const D3D10DDI_HSAMPLER *samplers)
{
    funcs->pfnVsSetConstantBuffers(hDevice, 0, 1, samplers, NULL, NULL);
}
#elif CASE == 2
void update_with_a_viewport_for_a_box(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs, D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE resource, const D3D10_DDI_VIEWPORT *viewport)
{
    funcs->pfnResourceUpdateSubresourceUP(hDevice, resource, 0, viewport,
            NULL, 0, 0, 0);
}
#elif CASE == 3
void bind_samplers_with_resource_handles(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs, D3D10DDI_HDEVICE hDevice,
        const D3D10DDI_HRESOURCE *resources)
{
    funcs->pfnPsSetSamplers(hDevice, 0, 1, resources);
}
#elif CASE == 4
void scissor_with_a_viewport(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs, D3D10DDI_HDEVICE hDevice,
        const D3D10_DDI_VIEWPORT *viewport)
{
    funcs->pfnSetScissorRects(hDevice, 1, 0, viewport);
}
#else
typedef char compile_once_per_case_with_CASE_1_to_4[-1];
#endif
