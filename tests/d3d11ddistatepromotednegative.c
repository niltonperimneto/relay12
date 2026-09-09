/* SPDX-License-Identifier: GPL-3.0-only
 *
 * This file must fail to compile. State handles are intentionally distinct;
 * promoting pfnCreateSampler must not collapse them to a generic pointer.
 */
#include "wine_d3d11ddi.h"

void create_sampler_with_the_wrong_handle(
        D3DWDDM2_6DDI_DEVICEFUNCS *funcs,
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HBLENDSTATE wrongHandle)
{
    funcs->pfnCreateSampler(hDevice, NULL, wrongHandle,
            (D3D10DDI_HRTSAMPLER){0});
}
