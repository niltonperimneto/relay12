/* SPDX-License-Identifier: GPL-3.0-only
 *
 * This file must fail to compile.  The resource promotion must preserve the
 * distinct D3D11 create-resource and D3D10 open-resource argument types.
 */
#include "wine_d3d11ddi.h"

void create_with_the_wrong_resource_arguments(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_OPENRESOURCE *open,
        D3D10DDI_HRESOURCE resource,
        D3D10DDI_HRTRESOURCE rtResource)
{
    funcs->pfnCreateResource(hDevice, open, resource, rtResource);
}
