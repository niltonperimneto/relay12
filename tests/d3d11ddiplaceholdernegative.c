/* SPDX-License-Identifier: GPL-3.0-only
 * This file must fail to compile: unpromoted slots take no arguments.
 */
#include "wine_d3d11ddi.h"
void invoke_unpromoted_slot(D3DWDDM2_6DDI_DEVICEFUNCS *funcs)
{
    funcs->pfnDraw(NULL);
}
