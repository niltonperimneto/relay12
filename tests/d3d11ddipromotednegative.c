/* SPDX-License-Identifier: GPL-3.0-only
 *
 * This file must fail to compile.
 *
 * The companion to tests/d3d11ddiplaceholdernegative.c, and it checks the
 * opposite half of the promotion contract.  That file proves an unpromoted
 * slot rejects arguments; this one proves a promoted slot rejects the wrong
 * ones.
 *
 * Both are needed, because a promotion that got the parameter list wrong would
 * pass everything else.  The offsets are unchanged either way, the table's
 * size is unchanged, and tests/d3d11ddilayout.c would happily assign and call
 * a stub written to match whatever the typedef says.  What cannot pass is a
 * call the specification's signature does not admit.
 *
 * pfnCommandListExecute takes a device handle and a command-list handle.  Here
 * it is called with the device handle twice, which is a plausible mistake at
 * the eventual call site -- both parameters are one wrapped pointer, so
 * nothing but the strong type distinguishes them, and that is exactly what
 * makes the strong type worth having.
 */
#include "wine_d3d11ddi.h"

void execute_with_the_wrong_handle(D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice)
{
    funcs->pfnCommandListExecute(hDevice, hDevice);
}

/* pfnCreateCommandList takes a driver handle (D3D11DDI_HCOMMANDLIST) and a
 * runtime handle (D3D11DDI_HRTCOMMANDLIST) as two of its four parameters, and
 * both are one wrapped pointer with no member a caller could tell apart at a
 * glance.  Passing hCommandList a second time, where the runtime handle
 * belongs, is the same shape of mistake execute_with_the_wrong_handle catches
 * above, on the pair of parameters the strong typing exists to keep apart. */
void create_with_the_wrong_runtime_handle(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATECOMMANDLIST *create,
        D3D11DDI_HCOMMANDLIST hCommandList)
{
    funcs->pfnCreateCommandList(hDevice, create, hCommandList, hCommandList);
}

/* The final scanout argument is an output pointer.  Passing the enum value
 * itself must remain a type error after promotion. */
void query_scanout_with_a_value_instead_of_output(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE hResource)
{
    funcs->pfnQueryScanoutCaps(hDevice, hResource, 0, 0, 0,
            D3DWDDM2_6DDI_SCANOUT_FLAG_NONE);
}
