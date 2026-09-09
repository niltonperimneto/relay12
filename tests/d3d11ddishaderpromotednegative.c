/* SPDX-License-Identifier: GPL-3.0-only
 *
 * This file must fail to compile.  The vertex and pixel shader creation
 * slots share one parameter shape (device, code pointer, driver handle,
 * runtime handle, signatures pointer) with every other shader stage, and the
 * driver and runtime handles are both one wrapped pointer with no member a
 * caller could tell apart at a glance -- the same shape of mistake the
 * command-list and resource negative tests catch on their own handle pairs.
 */
#include "wine_d3d11ddi.h"

void create_vertex_shader_with_the_wrong_runtime_handle(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice,
        const UINT *pShaderCode,
        D3D10DDI_HSHADER hShader,
        const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSignatures)
{
    funcs->pfnCreateVertexShader(hDevice, pShaderCode, hShader, hShader,
            pSignatures);
}

void destroy_shader_with_the_wrong_handle_type(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRENDERTARGETVIEW hRenderTargetView)
{
    funcs->pfnDestroyShader(hDevice, hRenderTargetView);
}
