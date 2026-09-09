/* SPDX-License-Identifier: GPL-3.0-only
 *
 * This file must fail to compile.  The deferred-context handle-size callback
 * takes a D3D11DDI_HANDLETYPE enum as its second argument.  Substituting a
 * strongly typed device handle proves this newly promoted family is no longer
 * a no-argument placeholder and that its enum parameter was preserved.
 */
#include "wine_d3d11ddi.h"

void calculate_with_the_wrong_handle_type(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice)
{
    funcs->pfnCalcDeferredContextHandleSize(hDevice, hDevice, NULL);
}
