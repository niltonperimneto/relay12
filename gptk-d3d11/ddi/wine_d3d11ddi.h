/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Clean-room D3D11 DDI declarations for the D3D11On12 host.
 *
 * Microsoft's D3D11On12 is a D3D11 user-mode DDI driver, so hosting it needs
 * the D3D11 DDI declarations.  Those live in the proprietary WDK headers
 * (d3d10umddi.h, d3d11umddi.h), which must never be copied into or vendored by
 * this repository.  Every declaration here is authored from public Microsoft
 * documentation and validated by layout tests.
 *
 * Rules for adding a declaration group:
 *
 *   1. Author it from the public specification.  Do not transcribe a WDK
 *      header, and do not reconstruct one from a binary.
 *   2. Precede the group with a provenance block in exactly this form:
 *
 *          /###
 *           * Group: <name>
 *           * Specification: <public URL>
 *           * Retrieved: <YYYY-MM-DD>
 *           ###/
 *
 *      (with real comment delimiters).  CI requires a Specification line for
 *      every group marker.
 *   3. Assert the size, alignment, and every field offset with the
 *      WINE_DDI_ASSERT_* macros below.  An unasserted field is not accepted:
 *      a wrong offset in a DDI structure is a silent memory-corruption bug,
 *      not a compile error.
 *   4. Declare only the single DDI interface version this project selects and
 *      freezes.  Do not add speculative versions.
 *
 * A privately authorized WDK job may compare generated metadata against these
 * declarations, but it must never upload or echo WDK content.
 */
#ifndef WINE_D3D11DDI_H
#define WINE_D3D11DDI_H

#include <windows.h>

#ifdef __cplusplus
# include <cstddef>
# define WINE_DDI_STATIC_ASSERT(condition, message) \
    static_assert(condition, message)
# define WINE_DDI_ALIGNOF(type) alignof(type)
#else
# include <stddef.h>
# define WINE_DDI_STATIC_ASSERT(condition, message) \
    _Static_assert(condition, message)
# define WINE_DDI_ALIGNOF(type) _Alignof(type)
#endif

/* Layout harness.  These are the only sanctioned way to record a DDI
 * structure's binary contract. */
#define WINE_DDI_ASSERT_SIZE(type, expected) \
    WINE_DDI_STATIC_ASSERT(sizeof(type) == (expected), \
            #type " has an unexpected size")

#define WINE_DDI_ASSERT_ALIGN(type, expected) \
    WINE_DDI_STATIC_ASSERT(WINE_DDI_ALIGNOF(type) == (expected), \
            #type " has an unexpected alignment")

#define WINE_DDI_ASSERT_FIELD(type, field, expected) \
    WINE_DDI_STATIC_ASSERT(offsetof(type, field) == (expected), \
            #type "." #field " has an unexpected offset")

#define WINE_DDI_ASSERT_FIELD_SIZE(type, field, expected) \
    WINE_DDI_STATIC_ASSERT(sizeof(((type *)0)->field) == (expected), \
            #type "." #field " has an unexpected size")

/* The DDI is a C ABI shared with a C++ implementation; a group that is not
 * standard-layout cannot have a stable offset contract. */
#ifdef __cplusplus
# include <type_traits>
# define WINE_DDI_ASSERT_STANDARD_LAYOUT(type) \
    WINE_DDI_STATIC_ASSERT(std::is_standard_layout<type>::value, \
            #type " must be standard-layout")
#else
/* Every C structure is standard-layout; keep the macro a declaration in both
 * languages so call sites read the same and still take a semicolon. */
# define WINE_DDI_ASSERT_STANDARD_LAYOUT(type) \
    WINE_DDI_STATIC_ASSERT(sizeof(type) > 0, #type " must be a complete type")
#endif

/*
 * Declaration groups.
 *
 * None are declared yet.  docs/D3D11ON12.md requires these, and each one
 * lands here with its provenance block and layout assertions:
 *
 *   - adapter, device, context, and resource handle types;
 *   - runtime callback tables;
 *   - adapter and device function tables;
 *   - resource, view, shader, state, query, and command structures;
 *   - version negotiation constants;
 *   - DXGI DDI interoperability structures.
 *
 * Until a group is declared and asserted here, the D3D11On12 host cannot be
 * compiled, and the core must keep returning DXGI_ERROR_UNSUPPORTED.
 */

#endif /* WINE_D3D11DDI_H */
