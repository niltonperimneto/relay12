/* SPDX-License-Identifier: GPL-3.0-only
 *
 * This file must fail to compile.  The view promotion must keep the shader
 * resource view and render target view handles and argument structures
 * distinct: passing an SRV handle where an RTV handle belongs, or a
 * CreateShaderResourceView argument structure to CreateRenderTargetView,
 * changes no offset and no size, so only the strong types catch it.
 */
#include "wine_d3d11ddi.h"

void create_rtv_with_the_wrong_handle_type(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *create,
        D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView)
{
    funcs->pfnCreateRenderTargetView(hDevice, create, hShaderResourceView,
            hShaderResourceView);
}

void create_srv_with_the_wrong_argument_structure(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *create,
        D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView,
        D3D10DDI_HRTSHADERRESOURCEVIEW hRTShaderResourceView)
{
    funcs->pfnCreateShaderResourceView(hDevice, create, hShaderResourceView,
            hRTShaderResourceView);
}
