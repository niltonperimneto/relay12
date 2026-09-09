/* SPDX-License-Identifier: GPL-3.0-only
 *
 * This file must fail to compile.
 *
 * The binding slots are the ones a wrongly transcribed parameter list would
 * hurt most quietly: every handle in the group is one wrapped pointer, so a
 * slot that took the wrong one would still be eight bytes at the right offset
 * and no layout assertion could see it. The element layout handle is the case
 * to prove, because IaSetInputLayout is the only promoted slot that takes it
 * and it would accept any other handle if the promotion had used void *.
 *
 * The topology slot is deliberately not tested here. Its argument is a
 * transport typedef to INT, because the enumeration's page publishes no
 * values -- see the binding-types group in wine_d3d11ddi.h -- so there is no
 * wrong integer to reject and nothing this file could assert.
 */
#include "wine_d3d11ddi.h"

void set_input_layout_with_the_wrong_handle(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HSHADER wrongHandle)
{
    funcs->pfnIaSetInputLayout(hDevice, wrongHandle);
}

void map_with_the_wrong_output_type(D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE resource,
        D3D10DDI_HSHADER *wrongOutput)
{
    funcs->pfnResourceMap(hDevice, resource, 0, 0, 0, wrongOutput);
}
