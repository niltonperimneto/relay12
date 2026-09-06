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
 *   5. Do not change the packing.  See the frozen binary contract below.
 *
 * A privately authorized WDK job may compare generated metadata against these
 * declarations, but it must never upload or echo WDK content.
 */
#ifndef WINE_D3D11DDI_H
#define WINE_D3D11DDI_H

#include <windows.h>

/*
 * The frozen binary contract.
 *
 * Every offset asserted in this header is the Win64 x86_64 ABI at natural
 * alignment, which is what MSVC's default /Zp8 produces for this DDI: no
 * field here has an alignment above 8, so MinGW-w64 GCC and MSVC agree
 * already.
 *
 * #pragma pack is prohibited in this header, and CI rejects it.  These
 * structures are not packed.  pack(1) would break every offset below, and
 * pack(8) would change nothing while hiding the real alignment from
 * WINE_DDI_ASSERT_ALIGN, whose job is to record it.  The assertions are the
 * mitigation: they are compile-time and fail the build under any ABI where a
 * layout diverges, which a pragma forcing one answer cannot do.
 *
 * An ABI other than this one must re-derive its offsets from the
 * specification and assert them.  It must not inherit these, so building for
 * one is an error rather than a silent reinterpretation.  For ARM in
 * particular, note that packing is not what differs: the offsets of integer,
 * enum, handle, and pointer fields are the same under the ARM64 Windows ABI,
 * and the real exposure is calling convention and ARM64EC thunking at the
 * exported boundary.
 */
#if !defined(_WIN64) || !(defined(__x86_64__) || defined(_M_X64))
# error "the D3D11 DDI declarations are frozen for the Win64 x86_64 ABI"
#endif

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
