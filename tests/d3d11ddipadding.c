/* SPDX-License-Identifier: GPL-3.0-only
 *
 * The -Wpadded gate for the clean-room DDI declarations.
 *
 * A DDI structure the host allocates and the driver reads must not contain a
 * byte the host cannot name.  Anonymous padding is where a host that assigns
 * member by member leaves whatever was on the heap, and the driver reads it:
 * that is not a compile error, not a layout error, and not visible in any
 * offset assertion, because the offsets are correct either way.  So the
 * compiler is asked instead.  Under -Wpadded -Werror, a structure here that
 * pads implicitly fails the build, and the fix is to name the bytes and assert
 * them -- see D3D10DDIARG_CREATEDEVICE.WinePad0 in the header for why that is
 * a declaration change and not a layout one.
 *
 * This translation unit exists so the gate judges the DDI declarations and
 * nothing else.  tests/d3d11ddilayout.c cannot carry it: its reference
 * structure pads deliberately, to drive the dirty-memory trap.
 *
 * windows.h is included first, with the warning off.  Whether a toolchain
 * treats the mingw headers as system headers -- and so whether it would report
 * their padding -- is not something this gate should depend on, and the DDI
 * header's own include of it is a no-op once its guard is set.
 *
 * Build (both languages must succeed):
 *   x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -Wextra -Wpadded -Werror \
 *       -Irelay12-d3d11/ddi -c tests/d3d11ddipadding.c
 *   x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Wpadded -Werror \
 *       -fno-exceptions -fno-rtti -Irelay12-d3d11/ddi -x c++ \
 *       -c tests/d3d11ddipadding.c
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpadded"
#include <windows.h>
#pragma GCC diagnostic pop

#include "wine_d3d11ddi.h"

/* Nothing else is needed.  -Wpadded is reported where a structure is defined,
 * not where one is used, so including the header is the whole test, and the
 * three structures that pad are the three the host allocates:
 * D3D10DDIARG_CREATEDEVICE, D3DDDICB_ESCAPE and D3DDDICB_SYNCTOKEN.  Their
 * gaps are named and asserted in the header; scripts/gen_ddi_layout.py derives
 * the same three sets independently and requires a name at each.
 *
 * The names are this project's own, so the assertion that matters here is that
 * they occupy padding rather than displace a member.  The header asserts every
 * following offset and each structure's size unchanged, and this line is where
 * that reading is anchored: if a pad were a member instead, one of these would
 * have grown. */
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_CREATEDEVICE, 88);
WINE_DDI_ASSERT_SIZE(D3DDDICB_ESCAPE, 40);
WINE_DDI_ASSERT_SIZE(D3DDDICB_SYNCTOKEN, 24);
