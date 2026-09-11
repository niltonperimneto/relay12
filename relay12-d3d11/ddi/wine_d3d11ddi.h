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
#include <dxgitype.h>

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

/* Two slots that must hold one type, not merely two compatible ones.
 *
 * Where the specification gives one PFN name to two members, a host writes one
 * implementation and binds it to both, and the pinned driver binds one of them
 * through a pointer-to-member.  No offset or size assertion reaches that: two
 * distinct typedefs with identical signatures assert the same offsets, occupy
 * the same eight bytes, and still force the host to write the function twice
 * and break the pointer-to-member.  So the identity is asserted directly.
 *
 * C has no is_same, and comparing sizeof or a cast would pass for any two
 * function pointers.  __builtin_types_compatible_p is the only construct in C
 * that answers the actual question; GCC and Clang both have it, and this
 * header is compiled by no other C compiler. */
#ifdef __cplusplus
/* The outer parentheses are load-bearing: the comma between the template
 * arguments is invisible to the preprocessor, which would otherwise read it as
 * separating two arguments to WINE_DDI_STATIC_ASSERT. */
# define WINE_DDI_ASSERT_SAME_FIELD_TYPE(type, first, second) \
    WINE_DDI_STATIC_ASSERT( \
            (std::is_same<decltype(((type *)0)->first), \
                    decltype(((type *)0)->second)>::value), \
            #type "." #first " and ." #second " must hold one type")
#else
# define WINE_DDI_ASSERT_SAME_FIELD_TYPE(type, first, second) \
    WINE_DDI_STATIC_ASSERT(__builtin_types_compatible_p( \
                    __typeof__(((type *)0)->first), \
                    __typeof__(((type *)0)->second)), \
            #type "." #first " and ." #second " must hold one type")
#endif

/*
 * Declaration groups.
 *
 * Authored here so far:
 *
 *   - driver and runtime object handles (adapter, resource, device, and core
 *     layer);
 *   - adapter function tables and the OpenAdapter argument structure;
 *   - the version negotiation arithmetic;
 *   - the CreateDevice argument structure, its embedded DXGI base arguments,
 *     and the create-device flags;
 *   - the core-layer device callback table, signature-complete except for the
 *     two slots the public set does not document;
 *   - the command list handle, which is the whole of the context handle types:
 *     a deferred context has none of its own, and reuses D3D10DDI_HDEVICE;
 *   - the 178-slot WDDM 2.6 device function table, with the command-list,
 *     deferred-context, resource, shader/render-target view, and
 *     vertex/pixel shader creation families promoted and the remaining
 *     published PFN names held behind non-callable placeholders pending
 *     signature promotion;
 *   - the kernel device callback table, with the three slots the pinned driver
 *     invokes promoted and the rest holding their offsets only, and the two
 *     argument structures those three take.
 *
 * Still required by docs/D3D11ON12.md, each to land with its own provenance
 * block and layout assertions:
 *
 *   - shader, state, query, and command structures, which are what gate the
 *     remaining device slots; docs/DDI-REMAINING-ROADMAP.md maps each group
 *     to the slot families it unblocks;
 *   - the literal DDI version numbers, which the public specification elides;
 *   - DXGI DDI interoperability structures.
 *
 * Until those land the D3D11On12 host cannot be compiled, and the core must
 * keep returning DXGI_ERROR_UNSUPPORTED.
 */

/*
 * Group: driver and runtime object handles
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/direct3d-version-10-runtime-and-driver-handles
 * Retrieved: 2026-09-06
 *
 * The specification gives the resource pair verbatim and states the rule the
 * rest follow: these handles "are essentially pointers that are wrapped with a
 * strong type to identify the object that is being operated on".  A driver
 * handle points at the runtime-allocated private block whose size the driver
 * returned from CalcPrivate<ObjType>Size, so its member is pDrvPrivate; a
 * runtime handle carries an opaque runtime value, so its member is handle.
 *
 * D3D10DDI_HRESOURCE and D3D10DDI_HRTRESOURCE are the page's own code block.
 * The adapter pair's type names come from the D3D10DDIARG_OPENADAPTER syntax
 * block cited in the group below; their contents follow the documented
 * convention.  That is a clean-room derivation, not a quotation, and it is
 * recorded as such.  What the host actually depends on is the binary
 * contract, and that the specification does determine: one pointer, asserted
 * below.
 *
 * Context handles are deliberately absent.  They belong to the deferred
 * context group, which is not authored yet; naming their members from
 * recollection would put an unverified declaration behind a provenance block,
 * which is the one thing this header exists to prevent.  The device and
 * core-layer handles are declared with the device-creation group below, whose
 * argument structure is what names them.
 */
typedef struct D3D10DDI_HADAPTER
{
    void *pDrvPrivate;
} D3D10DDI_HADAPTER;

typedef struct D3D10DDI_HRTADAPTER
{
    void *handle;
} D3D10DDI_HRTADAPTER;

typedef struct D3D10DDI_HRESOURCE
{
    void *pDrvPrivate;
} D3D10DDI_HRESOURCE;

typedef struct D3D10DDI_HRTRESOURCE
{
    void *handle;
} D3D10DDI_HRTRESOURCE;

/* State-object handles are one wrapped driver/runtime pointer pair each. */
#define WINE_DDI_DECLARE_STATE_HANDLES(name) \
    typedef struct D3D10DDI_H##name { void *pDrvPrivate; } D3D10DDI_H##name; \
    typedef struct D3D10DDI_HRT##name { void *handle; } D3D10DDI_HRT##name

WINE_DDI_DECLARE_STATE_HANDLES(BLENDSTATE);
WINE_DDI_DECLARE_STATE_HANDLES(DEPTHSTENCILSTATE);
WINE_DDI_DECLARE_STATE_HANDLES(RASTERIZERSTATE);
WINE_DDI_DECLARE_STATE_HANDLES(SAMPLER);

#undef WINE_DDI_DECLARE_STATE_HANDLES

#define WINE_DDI_ASSERT_STATE_HANDLES(name) \
    WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_H##name); \
    WINE_DDI_ASSERT_SIZE(D3D10DDI_H##name, 8); \
    WINE_DDI_ASSERT_ALIGN(D3D10DDI_H##name, 8); \
    WINE_DDI_ASSERT_FIELD(D3D10DDI_H##name, pDrvPrivate, 0); \
    WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRT##name); \
    WINE_DDI_ASSERT_SIZE(D3D10DDI_HRT##name, 8); \
    WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRT##name, 8); \
    WINE_DDI_ASSERT_FIELD(D3D10DDI_HRT##name, handle, 0)

WINE_DDI_ASSERT_STATE_HANDLES(BLENDSTATE);
WINE_DDI_ASSERT_STATE_HANDLES(DEPTHSTENCILSTATE);
WINE_DDI_ASSERT_STATE_HANDLES(RASTERIZERSTATE);
WINE_DDI_ASSERT_STATE_HANDLES(SAMPLER);

#undef WINE_DDI_ASSERT_STATE_HANDLES

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HADAPTER);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HADAPTER, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HADAPTER, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HADAPTER, pDrvPrivate, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HADAPTER, pDrvPrivate, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRTADAPTER);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HRTADAPTER, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRTADAPTER, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HRTADAPTER, handle, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HRTADAPTER, handle, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRESOURCE);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HRESOURCE, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRESOURCE, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HRESOURCE, pDrvPrivate, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HRESOURCE, pDrvPrivate, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRTRESOURCE);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HRTRESOURCE, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRTRESOURCE, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HRTRESOURCE, handle, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HRTRESOURCE, handle, 8);

/*
 * Argument structures belonging to groups that are not authored yet.
 *
 * These stay incomplete on purpose.  Every use below is behind a pointer, so
 * an incomplete type carries the correct size and alignment and keeps the
 * function signatures honest, while making it a compile error to touch a
 * field nobody has derived from a specification yet.  Each completes in place
 * when its own group lands, under its own provenance block.
 */
typedef struct D3D10DDIARG_CALCPRIVATEDEVICESIZE D3D10DDIARG_CALCPRIVATEDEVICESIZE;
typedef struct D3D10_2DDIARG_GETCAPS D3D10_2DDIARG_GETCAPS;
typedef struct _D3DDDI_ADAPTERCALLBACKS D3DDDI_ADAPTERCALLBACKS;
typedef struct D3D11_1_DDI_BLEND_DESC D3D11_1_DDI_BLEND_DESC;
typedef struct D3D10_DDI_DEPTH_STENCIL_DESC D3D10_DDI_DEPTH_STENCIL_DESC;
typedef struct D3D11_1_DDI_RASTERIZER_DESC D3D11_1_DDI_RASTERIZER_DESC;
typedef struct D3D10_DDI_SAMPLER_DESC D3D10_DDI_SAMPLER_DESC;

/* D3D10DDIARG_CREATEDEVICE is completed by the device-creation group below,
 * and D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS by the core-layer group at the
 * end of this header; the adapter table below needs only their names.  The
 * remaining tables its members point at stay incomplete, and each is the
 * subject of a group of its own. */
typedef struct D3D10DDIARG_CREATEDEVICE D3D10DDIARG_CREATEDEVICE;
typedef struct _D3DDDI_DEVICECALLBACKS D3DDDI_DEVICECALLBACKS;
typedef struct D3DWDDM2_6DDI_DEVICEFUNCS D3DWDDM2_6DDI_DEVICEFUNCS;
typedef struct D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS;
typedef struct DXGI_DDI_BASE_CALLBACKS DXGI_DDI_BASE_CALLBACKS;
typedef struct DXGI1_6_1_DDI_BASE_FUNCTIONS DXGI1_6_1_DDI_BASE_FUNCTIONS;

/*
 * Group: adapter function tables and OpenAdapter arguments
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_openadapter
 * Retrieved: 2026-09-06
 *
 * Companion specifications, all retrieved 2026-09-06:
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddi_adapterfuncs
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10_2ddi_adapterfuncs
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_openadapter
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_calcprivatedevicesize
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createdevice
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_closeadapter
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10_2ddi_getsupportedversions
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10_2ddi_getcaps
 *
 * D3D11 is why both tables are here: a version 11 driver must implement
 * OpenAdapter10_2, which returns D3D10_2DDI_ADAPTERFUNCS through the
 * pAdapterFuncs_2 arm of the union, while OpenAdapter10 returns
 * D3D10DDI_ADAPTERFUNCS through pAdapterFuncs.  The union is what makes the
 * two entry points share one argument structure, so its offset matters as
 * much as any field's.
 *
 * No calling convention is written on these pointers.  The specification's
 * syntax blocks show none, and this header is frozen to Win64 x86_64, which
 * has a single calling convention, so APIENTRY would be a no-op that implied
 * a contract this group has not verified.  An ABI that does distinguish
 * conventions must re-derive these, as the frozen contract above requires.
 */
typedef SIZE_T (*PFND3D10DDI_CALCPRIVATEDEVICESIZE)(
        D3D10DDI_HADAPTER hAdapter,
        const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pData);

typedef HRESULT (*PFND3D10DDI_CREATEDEVICE)(
        D3D10DDI_HADAPTER hAdapter,
        D3D10DDIARG_CREATEDEVICE *pCreateData);

typedef HRESULT (*PFND3D10DDI_CLOSEADAPTER)(
        D3D10DDI_HADAPTER hAdapter);

typedef HRESULT (*PFND3D10_2DDI_GETSUPPORTEDVERSIONS)(
        D3D10DDI_HADAPTER hAdapter,
        UINT32 *puEntries,
        UINT64 *pSupportedDDIInterfaceVersions);

typedef HRESULT (*PFND3D10_2DDI_GETCAPS)(
        D3D10DDI_HADAPTER hAdapter,
        const D3D10_2DDIARG_GETCAPS *pData);

typedef struct D3D10DDI_ADAPTERFUNCS
{
    PFND3D10DDI_CALCPRIVATEDEVICESIZE pfnCalcPrivateDeviceSize;
    PFND3D10DDI_CREATEDEVICE          pfnCreateDevice;
    PFND3D10DDI_CLOSEADAPTER          pfnCloseAdapter;
} D3D10DDI_ADAPTERFUNCS;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_ADAPTERFUNCS);
WINE_DDI_ASSERT_SIZE(D3D10DDI_ADAPTERFUNCS, 24);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_ADAPTERFUNCS, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_ADAPTERFUNCS, pfnCalcPrivateDeviceSize, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDI_ADAPTERFUNCS, pfnCreateDevice, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_ADAPTERFUNCS, pfnCloseAdapter, 16);

typedef struct D3D10_2DDI_ADAPTERFUNCS
{
    PFND3D10DDI_CALCPRIVATEDEVICESIZE  pfnCalcPrivateDeviceSize;
    PFND3D10DDI_CREATEDEVICE           pfnCreateDevice;
    PFND3D10DDI_CLOSEADAPTER           pfnCloseAdapter;
    PFND3D10_2DDI_GETSUPPORTEDVERSIONS pfnGetSupportedVersions;
    PFND3D10_2DDI_GETCAPS              pfnGetCaps;
} D3D10_2DDI_ADAPTERFUNCS;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10_2DDI_ADAPTERFUNCS);
WINE_DDI_ASSERT_SIZE(D3D10_2DDI_ADAPTERFUNCS, 40);
WINE_DDI_ASSERT_ALIGN(D3D10_2DDI_ADAPTERFUNCS, 8);
WINE_DDI_ASSERT_FIELD(D3D10_2DDI_ADAPTERFUNCS, pfnCalcPrivateDeviceSize, 0);
WINE_DDI_ASSERT_FIELD(D3D10_2DDI_ADAPTERFUNCS, pfnCreateDevice, 8);
WINE_DDI_ASSERT_FIELD(D3D10_2DDI_ADAPTERFUNCS, pfnCloseAdapter, 16);
WINE_DDI_ASSERT_FIELD(D3D10_2DDI_ADAPTERFUNCS, pfnGetSupportedVersions, 24);
WINE_DDI_ASSERT_FIELD(D3D10_2DDI_ADAPTERFUNCS, pfnGetCaps, 32);

/* The first three entries are the same functions in the same order in both
 * tables.  OpenAdapter10_2 is the entry point a D3D11 driver must implement,
 * so the host will fill the _2 table; asserting the shared prefix keeps a
 * future edit from reordering one table and silently changing the other's
 * meaning. */
WINE_DDI_STATIC_ASSERT(
        offsetof(D3D10DDI_ADAPTERFUNCS, pfnCalcPrivateDeviceSize)
        == offsetof(D3D10_2DDI_ADAPTERFUNCS, pfnCalcPrivateDeviceSize)
        && offsetof(D3D10DDI_ADAPTERFUNCS, pfnCreateDevice)
        == offsetof(D3D10_2DDI_ADAPTERFUNCS, pfnCreateDevice)
        && offsetof(D3D10DDI_ADAPTERFUNCS, pfnCloseAdapter)
        == offsetof(D3D10_2DDI_ADAPTERFUNCS, pfnCloseAdapter),
        "the adapter tables must share their leading three entries");

typedef struct D3D10DDIARG_OPENADAPTER
{
    D3D10DDI_HRTADAPTER           hRTAdapter;
    D3D10DDI_HADAPTER             hAdapter;
    UINT                          Interface;
    UINT                          Version;
    const D3DDDI_ADAPTERCALLBACKS *pAdapterCallbacks;
    union
    {
        D3D10DDI_ADAPTERFUNCS   *pAdapterFuncs;
        D3D10_2DDI_ADAPTERFUNCS *pAdapterFuncs_2;
    };
} D3D10DDIARG_OPENADAPTER;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_OPENADAPTER);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_OPENADAPTER, 40);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_OPENADAPTER, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENADAPTER, hRTAdapter, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENADAPTER, hAdapter, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENADAPTER, Interface, 16);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDIARG_OPENADAPTER, Interface, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENADAPTER, Version, 20);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDIARG_OPENADAPTER, Version, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENADAPTER, pAdapterCallbacks, 24);
/* Both arms are the same storage; a divergence here would mean the union had
 * been turned into a structure, which changes the size of every argument
 * block the runtime passes. */
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENADAPTER, pAdapterFuncs, 32);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENADAPTER, pAdapterFuncs_2, 32);

typedef HRESULT (*PFND3D10DDI_OPENADAPTER)(
        D3D10DDIARG_OPENADAPTER *pOpenData);

/*
 * Group: version negotiation arithmetic
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/initializing-communication-with-the-direct3d-version-11-ddi
 * Retrieved: 2026-09-06
 *
 * The specification gives the major version as a literal and the composition
 * of an interface version and a supported-version word as code.  It does not
 * give the minor and build numbers: D3D11_0_DDI_MINOR_VERSION,
 * D3D11_0_DDI_BUILD_VERSION, D3D11_0_7_DDI_MINOR_VERSION and
 * D3D11_0_7_DDI_BUILD_VERSION all appear as literal ellipses on the page.
 *
 * So the literals are not publicly specified, and this header does not
 * define them.  They exist only in the WDK header, and rule 1 forbids both
 * transcribing that header and reconstructing one from a binary; writing the
 * numbers from recollection would be exactly that, dressed in a provenance
 * block that cites a page which does not contain them.  Whoever supplies them
 * must record where they came from, under a group of their own, and the
 * repository's authorized-WDK-comparison rule above is the only sanctioned
 * route.  A wrong version number here does not corrupt memory; it makes the
 * runtime negotiate a DDI the host does not implement, which is worse,
 * because it fails inside the driver rather than at the boundary.
 *
 * What is publicly specified is the arithmetic, so that is what is captured,
 * parameterised.  These carry the WINE_ prefix deliberately: they are not the
 * WDK's fixed-name object-like macros, and must not be mistaken for them.
 *
 * The decomposition below is the inverse of the published composition rather
 * than a quotation from it, and is recorded as derived.  It is needed because
 * the host is the runtime: it reads supported-version words out of the
 * driver's GetSupportedVersions as data and passes the high 32 bits of its
 * selection back as D3D10DDIARG_CREATEDEVICE.Interface.  Without these, every
 * call site would open-code that shift, and a shift open-coded in several
 * places is a shift that will eventually disagree with itself.
 */
#define D3D11_DDI_MAJOR_VERSION 11

#define WINE_D3D11_DDI_INTERFACE_VERSION(minor) \
    (((D3D11_DDI_MAJOR_VERSION) << 16) | (minor))

#define WINE_D3D11_DDI_SUPPORTED(interface_version, build_version) \
    ((((UINT64)(interface_version)) << 32) | (((UINT64)(build_version)) << 16))

#define WINE_D3D11_DDI_INTERFACE_FROM_SUPPORTED(supported) \
    ((UINT)(((UINT64)(supported)) >> 32))

#define WINE_D3D11_DDI_BUILD_FROM_SUPPORTED(supported) \
    ((UINT)((((UINT64)(supported)) >> 16) & 0xffffu))

#define WINE_D3D11_DDI_MAJOR_FROM_INTERFACE(interface_version) \
    ((UINT)((((UINT)(interface_version)) >> 16) & 0xffffu))

#define WINE_D3D11_DDI_MINOR_FROM_INTERFACE(interface_version) \
    ((UINT)(((UINT)(interface_version)) & 0xffffu))

/* The arithmetic is the whole content of this group, so it is asserted rather
 * than trusted.  The operands are arbitrary and carry no claim about any real
 * DDI version. */
WINE_DDI_STATIC_ASSERT(D3D11_DDI_MAJOR_VERSION == 11,
        "the D3D11 DDI major version is 11");
WINE_DDI_STATIC_ASSERT(WINE_D3D11_DDI_INTERFACE_VERSION(3) == 0x000b0003,
        "an interface version is the major version in the high 16 bits");
WINE_DDI_STATIC_ASSERT(
        WINE_D3D11_DDI_SUPPORTED(0x000b0003, 0x0007) == 0x000b000300070000ULL,
        "a supported-version word is the interface version in the high 32 "
        "bits and the build version in the next 16");

/* Decomposition, asserted against the composition at the ends of each field's
 * range rather than in the middle.  A mask that is one bit too wide or a shift
 * that is one bit off reads correctly for a small minor and build number,
 * which is exactly what a real DDI version looks like, so the interesting
 * operands are 0 and 0xffff. */
WINE_DDI_STATIC_ASSERT(
        WINE_D3D11_DDI_MAJOR_FROM_INTERFACE(WINE_D3D11_DDI_INTERFACE_VERSION(0))
        == D3D11_DDI_MAJOR_VERSION
        && WINE_D3D11_DDI_MINOR_FROM_INTERFACE(
                WINE_D3D11_DDI_INTERFACE_VERSION(0)) == 0,
        "an interface version with no minor number round-trips");
WINE_DDI_STATIC_ASSERT(
        WINE_D3D11_DDI_MAJOR_FROM_INTERFACE(
                WINE_D3D11_DDI_INTERFACE_VERSION(0xffff))
        == D3D11_DDI_MAJOR_VERSION
        && WINE_D3D11_DDI_MINOR_FROM_INTERFACE(
                WINE_D3D11_DDI_INTERFACE_VERSION(0xffff)) == 0xffff,
        "the largest minor number does not reach the major number's field");
WINE_DDI_STATIC_ASSERT(
        WINE_D3D11_DDI_INTERFACE_FROM_SUPPORTED(
                WINE_D3D11_DDI_SUPPORTED(0x000b0003, 0)) == 0x000b0003
        && WINE_D3D11_DDI_BUILD_FROM_SUPPORTED(
                WINE_D3D11_DDI_SUPPORTED(0x000b0003, 0)) == 0,
        "a supported-version word with no build number round-trips");
WINE_DDI_STATIC_ASSERT(
        WINE_D3D11_DDI_INTERFACE_FROM_SUPPORTED(
                WINE_D3D11_DDI_SUPPORTED(0x000b0003, 0xffff)) == 0x000b0003
        && WINE_D3D11_DDI_BUILD_FROM_SUPPORTED(
                WINE_D3D11_DDI_SUPPORTED(0x000b0003, 0xffff)) == 0xffff,
        "the largest build number does not reach the interface version");
/* The interface version occupies the high half of a 64-bit word, so a
 * decomposition that went through a signed type would sign-extend a version
 * whose top bit is set.  No real D3D11 version does, and relying on that is
 * how the bug would survive to the day one does. */
WINE_DDI_STATIC_ASSERT(
        WINE_D3D11_DDI_INTERFACE_FROM_SUPPORTED(0xffffffff00000000ULL)
        == 0xffffffffu,
        "decomposing a supported-version word must not sign-extend");
/* The low 16 bits are the runtime's revision number.  Neither accessor claims
 * them, and neither may quietly fold them into the build number. */
WINE_DDI_STATIC_ASSERT(
        WINE_D3D11_DDI_BUILD_FROM_SUPPORTED(
                WINE_D3D11_DDI_SUPPORTED(0x000b0003, 0x0007) | 0xffffULL)
        == 0x0007,
        "the revision bits are not part of the build number");

/*
 * Group: device creation
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_createdevice
 * Retrieved: 2026-09-07
 *
 * Companion specifications, all retrieved 2026-09-07:
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/dxgiddi/ns-dxgiddi-dxgi_ddi_base_args
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_retrievesubobject
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/display/direct3d-version-10-runtime-and-driver-handles
 *
 * This is the structure the host fills and hands to the driver's CreateDevice,
 * so it is the whole device-level boundary in one declaration.  Both
 * documentation surfaces were cross-validated before it was written: the
 * rendered syntax block and the markdown mirror agree on 23 members in the
 * same order.
 *
 * Only the union arms the pinned D3D11On12 source reads are declared, as rule
 * 4 requires.  The specification prints nine device-function arms
 * (pDeviceFuncs, p10_1DeviceFuncs, p11DeviceFuncs, p11_1DeviceFuncs,
 * pWDDM1_3DeviceFuncs, pWDDM2_0DeviceFuncs, pWDDM2_1DeviceFuncs,
 * pWDDM2_2DeviceFuncs, pWDDM2_6DeviceFuncs) and five core-layer arms
 * (pUMCallbacks, p11UMCallbacks, pWDDM2_0UMCallbacks, pWDDM2_2UMCallbacks,
 * pWDDM2_6UMCallbacks).  The pinned driver's GetDeviceFuncsFromCreateArgs
 * returns pWDDM2_6DeviceFuncs and its DeviceBase constructor reads
 * pWDDM2_6UMCallbacks, unconditionally and for both versions it advertises, so
 * those are the two declared here.  The full arm lists are recorded above so
 * the omission is a declaration choice rather than a transcription loss; every
 * arm is a pointer at the same offset, and scripts/gen_ddi_layout.py models
 * both arm sets and checks they agree on every offset and on the size.
 *
 * The device handle's member name is quoted from the pinned MIT driver, which
 * constructs its device with "new (pArgs->hDrvDevice.pDrvPrivate) Device".
 * The two runtime handles' member names follow the documented convention, as
 * the adapter pair's do, and that is a derivation rather than a quotation.
 * What the host depends on is one wrapped pointer each, and that is asserted.
 *
 * DXGI_DDI_BASE_ARGS is embedded by value, so it is declared first.  Its
 * function-table union has seven published arms; the pinned driver reads
 * pDXGIDDIBaseFunctions6_1, so that is the one declared.  DXGI1_6_1_DDI_BASE_-
 * FUNCTIONS has no reference page of its own in the published set; the name is
 * quoted from the DXGI_DDI_BASE_ARGS syntax block and used only as an
 * incomplete type behind a pointer, which is all this group needs it for.
 *
 * ppfnRetrieveSubObject is a pointer to a function pointer: the runtime
 * supplies the storage and the driver writes its implementation into it, which
 * the pinned driver does as "*pArgs->ppfnRetrieveSubObject = RetrieveSubObject".
 * So the host must point it at a writable slot it owns, not at a null.
 */
typedef struct D3D10DDI_HDEVICE
{
    void *pDrvPrivate;
} D3D10DDI_HDEVICE;

typedef struct D3D10DDI_HRTDEVICE
{
    void *handle;
} D3D10DDI_HRTDEVICE;

typedef struct D3D10DDI_HRTCORELAYER
{
    void *handle;
} D3D10DDI_HRTCORELAYER;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HDEVICE);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HDEVICE, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HDEVICE, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HDEVICE, pDrvPrivate, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HDEVICE, pDrvPrivate, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRTDEVICE);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HRTDEVICE, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRTDEVICE, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HRTDEVICE, handle, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HRTDEVICE, handle, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRTCORELAYER);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HRTCORELAYER, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRTCORELAYER, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HRTCORELAYER, handle, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HRTCORELAYER, handle, 8);

/* The runtime allocates the slot and the driver writes its function pointer
 * into it, so the host owns the storage this points at. */
typedef HRESULT (*PFND3D10DDI_RETRIEVESUBOBJECT)(
        D3D10DDI_HDEVICE hDevice,
        UINT32 SubDeviceID,
        SIZE_T ParamSize,
        void *pParams,
        SIZE_T OutputParamSize,
        void *pOutputParamsBuffer);

typedef struct DXGI_DDI_BASE_ARGS
{
    DXGI_DDI_BASE_CALLBACKS *pDXGIBaseCallbacks;
    union
    {
        DXGI1_6_1_DDI_BASE_FUNCTIONS *pDXGIDDIBaseFunctions6_1;
    };
} DXGI_DDI_BASE_ARGS;

WINE_DDI_ASSERT_STANDARD_LAYOUT(DXGI_DDI_BASE_ARGS);
WINE_DDI_ASSERT_SIZE(DXGI_DDI_BASE_ARGS, 16);
WINE_DDI_ASSERT_ALIGN(DXGI_DDI_BASE_ARGS, 8);
WINE_DDI_ASSERT_FIELD(DXGI_DDI_BASE_ARGS, pDXGIBaseCallbacks, 0);
WINE_DDI_ASSERT_FIELD(DXGI_DDI_BASE_ARGS, pDXGIDDIBaseFunctions6_1, 8);

struct D3D10DDIARG_CREATEDEVICE
{
    D3D10DDI_HRTDEVICE             hRTDevice;
    UINT                           Interface;
    UINT                           Version;
    const D3DDDI_DEVICECALLBACKS   *pKTCallbacks;
    union
    {
        D3DWDDM2_6DDI_DEVICEFUNCS *pWDDM2_6DeviceFuncs;
    };
    D3D10DDI_HDEVICE               hDrvDevice;
    DXGI_DDI_BASE_ARGS             DXGIBaseDDI;
    D3D10DDI_HRTCORELAYER          hRTCoreLayer;
    union
    {
        const D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS *pWDDM2_6UMCallbacks;
    };
    UINT                           Flags;
    /* Anonymous padding in the specification, named here.  See the note below
     * the assertions. */
    UINT32                         WinePad0;
    PFND3D10DDI_RETRIEVESUBOBJECT  *ppfnRetrieveSubObject;
};

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_CREATEDEVICE);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_CREATEDEVICE, 88);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_CREATEDEVICE, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, hRTDevice, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, Interface, 8);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDIARG_CREATEDEVICE, Interface, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, Version, 12);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDIARG_CREATEDEVICE, Version, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, pKTCallbacks, 16);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, pWDDM2_6DeviceFuncs, 24);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, hDrvDevice, 32);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, DXGIBaseDDI, 40);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDIARG_CREATEDEVICE, DXGIBaseDDI, 16);
/* The embedded structure's own members, located from the enclosing
 * structure's base.  These are what stop DXGI_DDI_BASE_ARGS from being turned
 * into a pointer: that would keep its own assertions true and move every
 * member after it here. */
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE,
        DXGIBaseDDI.pDXGIBaseCallbacks, 40);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE,
        DXGIBaseDDI.pDXGIDDIBaseFunctions6_1, 48);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, hRTCoreLayer, 56);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, pWDDM2_6UMCallbacks, 64);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, Flags, 72);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDIARG_CREATEDEVICE, Flags, 4);
/*
 * Flags is four bytes at 72 and the next member is eight-byte aligned, so the
 * specification leaves four bytes here that no member of its own names.
 *
 * WinePad0 names them.  It is not a specification claim and no host may read
 * or write it as a field: it is this header's own device, prefixed like every
 * other invention here, and it exists for two reasons.  A named member can be
 * asserted, which is what docs/CLEANROOM-DDI.md requires of padding and what
 * anonymous bytes make impossible.  And a named member is zeroed by an
 * aggregate initialiser as well as by a struct-wide memset, so a host that
 * initialises this structure the ordinary way can no longer hand the driver
 * four bytes it never wrote.  The runtime allocates this structure, and a
 * driver reading uninitialized padding is a bug the host cannot see.
 *
 * The size and every following offset are asserted unchanged above and below,
 * which is what makes naming the bytes a declaration change and not a layout
 * one.
 */
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, WinePad0, 76);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDIARG_CREATEDEVICE, WinePad0, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, ppfnRetrieveSubObject, 80);

/*
 * The create-device flags.
 *
 * Quoted from the Flags member's table and remarks on the group's
 * specification page.  Only these are published: the pinned driver also tests
 * D3D11DDI_CREATEDEVICE_FLAG_IS_XBOX, whose value appears in no public
 * document, so it is not defined here.  Nothing is lost by that — the host
 * would never set it — but a host that needs to must record where the value
 * came from, under a group of its own.
 *
 * The 3-D pipeline level occupies the three bits the mask covers.  Extracting
 * it needs the D3D11DDI_3DPIPELINELEVEL enumeration, which belongs to the
 * GetCaps group and is not authored; the pinned driver never reads those bits
 * on this path, so the mask is declared and the extraction is not.  What the
 * mask is for here is knowing that bits 1 through 3 of Flags are not free.
 */
#define D3D10DDI_CREATEDEVICE_FLAG_DISABLE_EXTRA_THREAD_CREATION 0x1
#define D3D11DDI_CREATEDEVICE_FLAG_SINGLETHREADED 0x10

#define D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_SHIFT (0x1)
#define D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_MASK \
    (0x7 << D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_SHIFT)

/* The page states the pipeline level occupies the 0xE mask and separately
 * gives the shift and mask as code.  Asserting that the two agree, and that
 * neither named flag lands inside the mask, is the only check available
 * against a transcription error in a set of literals. */
WINE_DDI_STATIC_ASSERT(D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_MASK
        == 0xe, "the 3-D pipeline level occupies the 0xE mask");
WINE_DDI_STATIC_ASSERT(
        (D3D10DDI_CREATEDEVICE_FLAG_DISABLE_EXTRA_THREAD_CREATION
                & D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_MASK) == 0
        && (D3D11DDI_CREATEDEVICE_FLAG_SINGLETHREADED
                & D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_MASK) == 0,
        "no create-device flag may overlap the 3-D pipeline level mask");

/*
 * Group: core-layer device callbacks
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3dwddm2_6ddi_corelayer_devicecallbacks
 * Retrieved: 2026-09-07
 *
 * This is the table the host fills and the driver calls, which is why it is
 * authored signature-complete rather than as bare slots.  Every other table so
 * far is filled by the driver and called by the host, so an unpromoted slot
 * there is simply never invoked; here the driver invokes the host on its own
 * schedule and with its own arguments, and a slot whose signature is wrong is
 * a corrupted call frame rather than a missing feature.
 *
 * Both documentation surfaces were cross-validated before it was written: the
 * rendered syntax block and the markdown mirror agree on 47 members in the
 * same order.  46 of the member types are distinct;
 * PFND3DWDDM2_2DDI_SHADERCACHE_ADDREF_RELEASE_CB is the one type used twice,
 * for pfnShaderCacheAddRefCb and pfnShaderCacheReleaseCb.
 *
 * Note that pfnDisableDeferredStagingResourceDestruction is the one member
 * with no Cb suffix.  Both surfaces spell it that way; it is not a typo here.
 *
 * Companion specifications, all retrieved 2026-09-07, each the page
 * https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/<slug>
 * for the slug listed:
 *
 *   nc-d3d10umddi-pfnd3d10ddi_seterror_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_vs_constbuf_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_ps_srv_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_ps_shader_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_ps_sampler_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_vs_shader_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_ps_constbuf_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_ia_inputlayout_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_ia_vertexbuf_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_ia_indexbuf_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_gs_constbuf_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_gs_shader_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_ia_primitive_topology_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_vs_srv_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_vs_sampler_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_gs_srv_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_gs_sampler_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_om_rendertargets_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_om_blendstate_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_om_depthstate_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_rs_raststate_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_so_targets_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_rs_viewports_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_rs_scissor_cb
 *   nc-d3d10umddi-pfnd3d10ddi_disable_deferred_staging_resource_destruction_cb
 *   nc-d3d10umddi-pfnd3d10ddi_state_textfiltersize_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_hs_srv_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_hs_shader_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_hs_sampler_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_hs_constbuf_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_ds_srv_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_ds_shader_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_ds_sampler_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_ds_constbuf_cb
 *   nc-d3d10umddi-pfnd3d11ddi_perform_amortized_processing_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_cs_srv_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_cs_uav_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_cs_shader_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_cs_sampler_cb
 *   nc-d3d10umddi-pfnd3d11ddi_state_cs_constbuf_cb
 *   nc-d3d10umddi-pfnd3dwddm2_2ddi_shadercache_store_value_cb
 *   nc-d3d10umddi-pfnd3dwddm2_2ddi_shadercache_addref_release_cb
 *
 * and, for the two context-creation slots, whose own typedef names have no
 * page and whose documentation the structure page points at instead:
 *
 *   https://learn.microsoft.com/en-us/previous-versions/ff568895(v=vs.85)
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_createcontextvirtualcb
 *
 * Base and Count are not in the same order in both halves of this table, and
 * that is quoted, not a transcription slip.  The D3D10-era pages document
 * (hRuntimeDevice, Count, Base); the D3D11-era pages document
 * (hRuntimeDevice, Base, Count).  Both parameters are UINT, so nothing about
 * the ABI distinguishes them and no assertion here can catch a host that
 * implements one family with the other's order.  The parameter names below
 * are therefore load-bearing documentation rather than decoration.
 *
 * Two core-layer callback slots are declared without a signature.  The
 * structure page names their types but the public set contains no page for
 * either
 * PFND3DWDDM2_2DDI_SHADERCACHE_GET_VALUE_CB or
 * PFND3DWDDM2_6DDI_QUERY_SCANOUT_CAPS_CB: both surfaces return 404, and the
 * structure page prints no link for them where it links every other member.
 * Rule 1 forbids reconstructing what is not published, so they take the
 * undeclared-slot type below.  Their offsets are exact, which is what the
 * table's layout depends on; what is missing is only the ability to implement
 * them, and a host that needs to must record where the signature came from
 * under a group of its own.
 *
 * D3DWDDM2_2DDI_HRTCACHESESSION is a runtime handle, and its name is quoted
 * from the shader-cache syntax blocks above.  Its contents follow the
 * documented runtime-handle convention, exactly as the adapter pair's do, and
 * that is a derivation rather than a quotation.  The argument structures the
 * remaining slots point at stay incomplete, each the subject of a group of its
 * own: what this group needs from them is a pointer.
 *
 * No calling convention is written on these pointers, for the reason the
 * adapter tables record.  The archived pfnCreateContextCb page does print
 * APIENTRY CALLBACK, but this header is frozen to Win64 x86_64, where that
 * expands to the one calling convention there is.
 */
typedef struct D3DWDDM2_2DDI_HRTCACHESESSION
{
    void *handle;
} D3DWDDM2_2DDI_HRTCACHESESSION;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3DWDDM2_2DDI_HRTCACHESESSION);
WINE_DDI_ASSERT_SIZE(D3DWDDM2_2DDI_HRTCACHESESSION, 8);
WINE_DDI_ASSERT_ALIGN(D3DWDDM2_2DDI_HRTCACHESESSION, 8);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_2DDI_HRTCACHESESSION, handle, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3DWDDM2_2DDI_HRTCACHESESSION, handle, 8);

/*
 * Group: shader-cache hash
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3dwddm2_2ddi_shadercache_hash
 * Source mirror: https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/ns-d3d10umddi-d3dwddm2_2ddi_shadercache_hash.md
 * Retrieved: 2026-09-11
 */
typedef struct D3DWDDM2_2DDI_SHADERCACHE_HASH
{
    BYTE Hash[16];
} D3DWDDM2_2DDI_SHADERCACHE_HASH;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3DWDDM2_2DDI_SHADERCACHE_HASH);
WINE_DDI_ASSERT_SIZE(D3DWDDM2_2DDI_SHADERCACHE_HASH, 16);
WINE_DDI_ASSERT_ALIGN(D3DWDDM2_2DDI_SHADERCACHE_HASH, 1);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_2DDI_SHADERCACHE_HASH, Hash, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3DWDDM2_2DDI_SHADERCACHE_HASH, Hash, 16);
typedef struct _D3DDDICB_CREATECONTEXT D3DDDICB_CREATECONTEXT;
typedef struct _D3DDDICB_CREATECONTEXTVIRTUAL D3DDDICB_CREATECONTEXTVIRTUAL;

/* A slot whose signature the public specification does not give.  It is a
 * function pointer, so its size and every offset after it are exact, but it
 * takes no arguments and returns nothing, so calling it as though its real
 * contract were known does not compile without an explicit cast.  That is the
 * point: an undeclared contract must be impossible to invoke by accident. */
typedef void (*PFNWINE_D3D11DDI_UNDECLARED_CB)(void);

/* The one callback that reports a driver-side failure to the runtime.  Every
 * DDI entry point that returns void uses this instead. */
typedef VOID (*PFND3D10DDI_SETERROR_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice,
        HRESULT hResult);

/* The D3D10-era state refresh callbacks that name no range: the runtime
 * refreshes the whole of the state in question. */
typedef void (*PFND3D10DDI_STATE_PS_SHADER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_VS_SHADER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_GS_SHADER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_IA_INPUTLAYOUT_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_IA_INDEXBUF_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_IA_PRIMITIVE_TOPOLOGY_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_OM_RENDERTARGETS_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_OM_BLENDSTATE_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_OM_DEPTHSTATE_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_RS_RASTSTATE_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_SO_TARGETS_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_RS_VIEWPORTS_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_RS_SCISSOR_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_STATE_TEXTFILTERSIZE_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D10DDI_DISABLE_DEFERRED_STAGING_RESOURCE_DESTRUCTION_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);

/* The D3D10-era state refresh callbacks that name a range, as (Count, Base).
 * Count may be passed as -1, which asks the runtime to substitute its own
 * high-water mark, so a host must not treat it as an unsigned array length. */
typedef void (*PFND3D10DDI_STATE_VS_CONSTBUF_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Count, UINT Base);
typedef void (*PFND3D10DDI_STATE_PS_CONSTBUF_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Count, UINT Base);
typedef void (*PFND3D10DDI_STATE_GS_CONSTBUF_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Count, UINT Base);
typedef void (*PFND3D10DDI_STATE_VS_SRV_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Count, UINT Base);
typedef void (*PFND3D10DDI_STATE_PS_SRV_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Count, UINT Base);
typedef void (*PFND3D10DDI_STATE_GS_SRV_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Count, UINT Base);
typedef void (*PFND3D10DDI_STATE_VS_SAMPLER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Count, UINT Base);
typedef void (*PFND3D10DDI_STATE_PS_SAMPLER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Count, UINT Base);
typedef void (*PFND3D10DDI_STATE_GS_SAMPLER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Count, UINT Base);
typedef void (*PFND3D10DDI_STATE_IA_VERTEXBUF_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Count, UINT Base);

/* The D3D11-era state refresh callbacks.  Same two UINTs, documented in the
 * opposite order: (Base, Count). */
typedef void (*PFND3D11DDI_STATE_HS_SHADER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D11DDI_STATE_DS_SHADER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D11DDI_STATE_CS_SHADER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D11DDI_PERFORM_AMORTIZED_PROCESSING_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice);
typedef void (*PFND3D11DDI_STATE_HS_SRV_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Base, UINT Count);
typedef void (*PFND3D11DDI_STATE_HS_SAMPLER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Base, UINT Count);
typedef void (*PFND3D11DDI_STATE_HS_CONSTBUF_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Base, UINT Count);
typedef void (*PFND3D11DDI_STATE_DS_SRV_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Base, UINT Count);
typedef void (*PFND3D11DDI_STATE_DS_SAMPLER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Base, UINT Count);
typedef void (*PFND3D11DDI_STATE_DS_CONSTBUF_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Base, UINT Count);
typedef void (*PFND3D11DDI_STATE_CS_SRV_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Base, UINT Count);
typedef void (*PFND3D11DDI_STATE_CS_UAV_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Base, UINT Count);
typedef void (*PFND3D11DDI_STATE_CS_SAMPLER_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Base, UINT Count);
typedef void (*PFND3D11DDI_STATE_CS_CONSTBUF_CB)(
        D3D10DDI_HRTCORELAYER hRuntimeDevice, UINT Base, UINT Count);

/* The kernel-facing slots.  These take the display device handle rather than
 * the core-layer handle, because they reach past the runtime into the display
 * kernel; docs/D3D11ON12.md lists the display-kernel paths as disabled for
 * this port, so the host is expected to refuse rather than forward them. */
typedef HRESULT (*PFND3DWDDM2_0DDI_CREATECONTEXT_CB)(
        HANDLE hDevice,
        D3DDDICB_CREATECONTEXT *pData);
typedef HRESULT (*PFND3DWDDM2_0DDI_CREATECONTEXTVIRTUAL_CB)(
        HANDLE hDevice,
        D3DDDICB_CREATECONTEXTVIRTUAL *pData);

/* The shader cache.  Get is undeclared above; store and the shared
 * addref/release entry point are published. */
typedef HRESULT (*PFND3DWDDM2_2DDI_SHADERCACHE_STORE_VALUE_CB)(
        D3DWDDM2_2DDI_HRTCACHESESSION hCacheSession,
        const D3DWDDM2_2DDI_SHADERCACHE_HASH *pPrecomputedHash,
        const void *pKey,
        SIZE_T KeyLen,
        const void *pValue,
        SIZE_T ValueLen);
typedef void (*PFND3DWDDM2_2DDI_SHADERCACHE_ADDREF_RELEASE_CB)(
        D3DWDDM2_2DDI_HRTCACHESESSION hCacheSession);

typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DWDDM2_2DDI_SHADERCACHE_GET_VALUE_CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DWDDM2_6DDI_QUERY_SCANOUT_CAPS_CB;

struct D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS
{
    PFND3D10DDI_SETERROR_CB                    pfnSetErrorCb;
    PFND3D10DDI_STATE_VS_CONSTBUF_CB           pfnStateVsConstBufCb;
    PFND3D10DDI_STATE_PS_SRV_CB                pfnStatePsSrvCb;
    PFND3D10DDI_STATE_PS_SHADER_CB             pfnStatePsShaderCb;
    PFND3D10DDI_STATE_PS_SAMPLER_CB            pfnStatePsSamplerCb;
    PFND3D10DDI_STATE_VS_SHADER_CB             pfnStateVsShaderCb;
    PFND3D10DDI_STATE_PS_CONSTBUF_CB           pfnStatePsConstBufCb;
    PFND3D10DDI_STATE_IA_INPUTLAYOUT_CB        pfnStateIaInputLayoutCb;
    PFND3D10DDI_STATE_IA_VERTEXBUF_CB          pfnStateIaVertexBufCb;
    PFND3D10DDI_STATE_IA_INDEXBUF_CB           pfnStateIaIndexBufCb;
    PFND3D10DDI_STATE_GS_CONSTBUF_CB           pfnStateGsConstBufCb;
    PFND3D10DDI_STATE_GS_SHADER_CB             pfnStateGsShaderCb;
    PFND3D10DDI_STATE_IA_PRIMITIVE_TOPOLOGY_CB pfnStateIaPrimitiveTopologyCb;
    PFND3D10DDI_STATE_VS_SRV_CB                pfnStateVsSrvCb;
    PFND3D10DDI_STATE_VS_SAMPLER_CB            pfnStateVsSamplerCb;
    PFND3D10DDI_STATE_GS_SRV_CB                pfnStateGsSrvCb;
    PFND3D10DDI_STATE_GS_SAMPLER_CB            pfnStateGsSamplerCb;
    PFND3D10DDI_STATE_OM_RENDERTARGETS_CB      pfnStateOmRenderTargetsCb;
    PFND3D10DDI_STATE_OM_BLENDSTATE_CB         pfnStateOmBlendStateCb;
    PFND3D10DDI_STATE_OM_DEPTHSTATE_CB         pfnStateOmDepthStateCb;
    PFND3D10DDI_STATE_RS_RASTSTATE_CB          pfnStateRsRastStateCb;
    PFND3D10DDI_STATE_SO_TARGETS_CB            pfnStateSoTargetsCb;
    PFND3D10DDI_STATE_RS_VIEWPORTS_CB          pfnStateRsViewportsCb;
    PFND3D10DDI_STATE_RS_SCISSOR_CB            pfnStateRsScissorCb;
    PFND3D10DDI_DISABLE_DEFERRED_STAGING_RESOURCE_DESTRUCTION_CB
                                               pfnDisableDeferredStagingResourceDestruction;
    PFND3D10DDI_STATE_TEXTFILTERSIZE_CB        pfnStateTextFilterSizeCb;
    PFND3D11DDI_STATE_HS_SRV_CB                pfnStateHsSrvCb;
    PFND3D11DDI_STATE_HS_SHADER_CB             pfnStateHsShaderCb;
    PFND3D11DDI_STATE_HS_SAMPLER_CB            pfnStateHsSamplerCb;
    PFND3D11DDI_STATE_HS_CONSTBUF_CB           pfnStateHsConstBufCb;
    PFND3D11DDI_STATE_DS_SRV_CB                pfnStateDsSrvCb;
    PFND3D11DDI_STATE_DS_SHADER_CB             pfnStateDsShaderCb;
    PFND3D11DDI_STATE_DS_SAMPLER_CB            pfnStateDsSamplerCb;
    PFND3D11DDI_STATE_DS_CONSTBUF_CB           pfnStateDsConstBufCb;
    PFND3D11DDI_PERFORM_AMORTIZED_PROCESSING_CB pfnPerformAmortizedProcessingCb;
    PFND3D11DDI_STATE_CS_SRV_CB                pfnStateCsSrvCb;
    PFND3D11DDI_STATE_CS_UAV_CB                pfnStateCsUavCb;
    PFND3D11DDI_STATE_CS_SHADER_CB             pfnStateCsShaderCb;
    PFND3D11DDI_STATE_CS_SAMPLER_CB            pfnStateCsSamplerCb;
    PFND3D11DDI_STATE_CS_CONSTBUF_CB           pfnStateCsConstBufCb;
    PFND3DWDDM2_0DDI_CREATECONTEXT_CB          pfnCreateContextCb;
    PFND3DWDDM2_0DDI_CREATECONTEXTVIRTUAL_CB   pfnCreateContextVirtualCb;
    PFND3DWDDM2_2DDI_SHADERCACHE_GET_VALUE_CB  pfnShaderCacheGetValueCb;
    PFND3DWDDM2_2DDI_SHADERCACHE_STORE_VALUE_CB pfnShaderCacheStoreValueCb;
    PFND3DWDDM2_2DDI_SHADERCACHE_ADDREF_RELEASE_CB pfnShaderCacheAddRefCb;
    PFND3DWDDM2_2DDI_SHADERCACHE_ADDREF_RELEASE_CB pfnShaderCacheReleaseCb;
    PFND3DWDDM2_6DDI_QUERY_SCANOUT_CAPS_CB     pfnQueryScanoutCapsCb;
};

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS);
WINE_DDI_ASSERT_SIZE(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS, 376);
WINE_DDI_ASSERT_ALIGN(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS, 8);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnSetErrorCb, 0);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateVsConstBufCb, 8);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStatePsSrvCb, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStatePsShaderCb, 24);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStatePsSamplerCb, 32);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateVsShaderCb, 40);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStatePsConstBufCb, 48);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateIaInputLayoutCb, 56);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateIaVertexBufCb, 64);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateIaIndexBufCb, 72);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateGsConstBufCb, 80);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateGsShaderCb, 88);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateIaPrimitiveTopologyCb, 96);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateVsSrvCb, 104);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateVsSamplerCb, 112);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateGsSrvCb, 120);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateGsSamplerCb, 128);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateOmRenderTargetsCb, 136);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateOmBlendStateCb, 144);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateOmDepthStateCb, 152);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateRsRastStateCb, 160);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateSoTargetsCb, 168);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateRsViewportsCb, 176);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateRsScissorCb, 184);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnDisableDeferredStagingResourceDestruction, 192);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateTextFilterSizeCb, 200);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateHsSrvCb, 208);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateHsShaderCb, 216);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateHsSamplerCb, 224);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateHsConstBufCb, 232);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateDsSrvCb, 240);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateDsShaderCb, 248);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateDsSamplerCb, 256);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateDsConstBufCb, 264);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnPerformAmortizedProcessingCb, 272);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateCsSrvCb, 280);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateCsUavCb, 288);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateCsShaderCb, 296);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateCsSamplerCb, 304);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnStateCsConstBufCb, 312);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnCreateContextCb, 320);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnCreateContextVirtualCb, 328);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnShaderCacheGetValueCb, 336);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnShaderCacheStoreValueCb, 344);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnShaderCacheAddRefCb, 352);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnShaderCacheReleaseCb, 360);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnQueryScanoutCapsCb, 368);

/* The specification gives these two one PFN name, so one host implementation
 * has to fit both.  tests/d3d11ddilayout.c assigns a single function into both
 * slots and compares them, which only proves that whatever they hold today
 * happens to be assignable; this is the constraint itself. */
WINE_DDI_ASSERT_SAME_FIELD_TYPE(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,
        pfnShaderCacheAddRefCb, pfnShaderCacheReleaseCb);

/* Every slot is a function pointer, so the table is exactly its member count
 * times the pointer size.  Asserting that as arithmetic rather than as another
 * literal is what catches a member being dropped and its offsets renumbered to
 * match, which is the one mistake the per-field assertions above cannot see. */
WINE_DDI_STATIC_ASSERT(
        sizeof(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS)
        == 47 * sizeof(void (*)(void)),
        "the core-layer callback table is 47 function pointers");

/*
 * Group: command-list handles and creation arguments
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_commandlistexecute
 * Retrieved: 2026-09-08
 *
 * Companion specifications, all retrieved 2026-09-08:
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_destroycommandlist
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_recyclecommandlist
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_abandoncommandlist
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d11ddiarg_createcommandlist
 *   https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/ns-d3d10umddi-d3d11ddiarg_createcommandlist.md
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/display/introduction-to-deferred-contexts
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-command-lists
 *
 * One handle, and it is the whole of the context handle types the header's
 * roadmap asks for.  That is the finding worth recording here, because it is
 * not what the roadmap assumed: a deferred context has no handle type of its
 * own.  The runtime reuses D3D10DDI_HDEVICE for it, passed as the hDrvContext
 * member of D3D11DDIARG_CREATEDEFERREDCONTEXT and used with the subset
 * function table that structure's p11ContextFuncs member points at.  Nobody
 * may declare a D3D11DDI_HDEFERREDCONTEXT later: no specification names one,
 * so it would be an invented type behind a provenance block, which rule 1
 * forbids and which this note exists to prevent.
 *
 * The member name follows the documented convention, as the other driver
 * handles' do: this is the handle to "the driver's private data for the
 * command list", so its member is pDrvPrivate.  That is a derivation, not a
 * quotation.  What the promoted slots below depend on is one wrapped pointer,
 * and that is asserted.
 */
typedef struct D3D11DDI_HCOMMANDLIST
{
    void *pDrvPrivate;
} D3D11DDI_HCOMMANDLIST;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D11DDI_HCOMMANDLIST);
WINE_DDI_ASSERT_SIZE(D3D11DDI_HCOMMANDLIST, 8);
WINE_DDI_ASSERT_ALIGN(D3D11DDI_HCOMMANDLIST, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDI_HCOMMANDLIST, pDrvPrivate, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDI_HCOMMANDLIST, pDrvPrivate, 8);

typedef struct D3D11DDI_HRTCOMMANDLIST
{
    void *handle;
} D3D11DDI_HRTCOMMANDLIST;

typedef struct D3D11DDIARG_CREATECOMMANDLIST
{
    D3D10DDI_HDEVICE hDeferredContext;
} D3D11DDIARG_CREATECOMMANDLIST;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D11DDI_HRTCOMMANDLIST);
WINE_DDI_ASSERT_SIZE(D3D11DDI_HRTCOMMANDLIST, 8);
WINE_DDI_ASSERT_ALIGN(D3D11DDI_HRTCOMMANDLIST, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDI_HRTCOMMANDLIST, handle, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDI_HRTCOMMANDLIST, handle, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D11DDIARG_CREATECOMMANDLIST);
WINE_DDI_ASSERT_SIZE(D3D11DDIARG_CREATECOMMANDLIST, 8);
WINE_DDI_ASSERT_ALIGN(D3D11DDIARG_CREATECOMMANDLIST, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATECOMMANDLIST, hDeferredContext, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDIARG_CREATECOMMANDLIST, hDeferredContext, 8);

/*
 * Group: deferred-context creation and handle sizing
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d11ddiarg_createdeferredcontext
 * Retrieved: 2026-09-08
 * Companion specifications:
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ne-d3d10umddi-d3d11ddi_handletype
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d11ddi_handlesize
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d11ddiarg_calcprivatedeferredcontextsize
 * Source mirror:
 *   https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/ns-d3d10umddi-d3d11ddiarg_createdeferredcontext.md
 *
 * The create structure publishes unions for every interface generation.  This
 * target negotiates WDDM 2.6, so only the WDDM 2.6 arms are declared.  Every
 * published arm is one pointer; the independent model retains all of them and
 * proves that this subset has the same offsets, size, and alignment.
 * WinePad0 in each structure names padding derived from natural Win64
 * alignment.  It is not a specification member and must not be read by a
 * host.
 */
typedef enum D3D11DDI_HANDLETYPE
{
    D3D10DDI_HT_RESOURCE = 0,
    D3D10DDI_HT_SHADERRESOURCEVIEW,
    D3D10DDI_HT_RENDERTARGETVIEW,
    D3D10DDI_HT_DEPTHSTENCILVIEW,
    D3D10DDI_HT_SHADER,
    D3D10DDI_HT_ELEMENTLAYOUT,
    D3D10DDI_HT_BLENDSTATE,
    D3D10DDI_HT_DEPTHSTENCILSTATE,
    D3D10DDI_HT_RASTERIZERSTATE,
    D3D10DDI_HT_SAMPLERSTATE,
    D3D10DDI_HT_QUERY,
    D3D11DDI_HT_COMMANDLIST,
    D3D11DDI_HT_UNORDEREDACCESSVIEW,
    D3D11_1DDI_HT_DECODE,
    D3D11_1DDI_HT_VIDEOPROCESSORENUM,
    D3D11_1DDI_HT_VIDEOPROCESSOR,
    D3D11_1DDI_HT_VIDEODECODEROUTPUTVIEW,
    D3D11_1DDI_HT_VIDEOPROCESSORINPUTVIEW,
    D3D11_1DDI_HT_VIDEOPROCESSOROUTPUTVIEW,
    D3DWDDM2_2DDI_HT_CACHESESSION
} D3D11DDI_HANDLETYPE;

WINE_DDI_STATIC_ASSERT(sizeof(D3D11DDI_HANDLETYPE) == 4,
        "D3D11DDI_HANDLETYPE is a four-byte enum");
WINE_DDI_STATIC_ASSERT(D3D10DDI_HT_RESOURCE == 0,
        "the first handle type has value zero");
WINE_DDI_STATIC_ASSERT(D3DWDDM2_2DDI_HT_CACHESESSION == 19,
        "the published handle types remain contiguous");

typedef struct D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE
{
    UINT Flags;
} D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE;

typedef struct D3D11DDI_HANDLESIZE
{
    D3D11DDI_HANDLETYPE HandleType;
    UINT32              WinePad0;
    SIZE_T              DriverPrivateSize;
} D3D11DDI_HANDLESIZE;

typedef struct D3D11DDIARG_CREATEDEFERREDCONTEXT
{
    union
    {
        D3DWDDM2_6DDI_DEVICEFUNCS *pWDDM2_6ContextFuncs;
    };
    D3D10DDI_HDEVICE      hDrvContext;
    D3D10DDI_HRTCORELAYER hRTCoreLayer;
    union
    {
        const D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS *pWDDM2_6UMCallbacks;
    };
    UINT   Flags;
    UINT32 WinePad0;
} D3D11DDIARG_CREATEDEFERREDCONTEXT;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE);
WINE_DDI_ASSERT_SIZE(D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE, 4);
WINE_DDI_ASSERT_ALIGN(D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE, 4);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE, Flags, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE, Flags, 4);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D11DDI_HANDLESIZE);
WINE_DDI_ASSERT_SIZE(D3D11DDI_HANDLESIZE, 16);
WINE_DDI_ASSERT_ALIGN(D3D11DDI_HANDLESIZE, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDI_HANDLESIZE, HandleType, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDI_HANDLESIZE, HandleType, 4);
WINE_DDI_ASSERT_FIELD(D3D11DDI_HANDLESIZE, WinePad0, 4);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDI_HANDLESIZE, WinePad0, 4);
WINE_DDI_ASSERT_FIELD(D3D11DDI_HANDLESIZE, DriverPrivateSize, 8);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDI_HANDLESIZE, DriverPrivateSize, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D11DDIARG_CREATEDEFERREDCONTEXT);
WINE_DDI_ASSERT_SIZE(D3D11DDIARG_CREATEDEFERREDCONTEXT, 40);
WINE_DDI_ASSERT_ALIGN(D3D11DDIARG_CREATEDEFERREDCONTEXT, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATEDEFERREDCONTEXT, pWDDM2_6ContextFuncs, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDIARG_CREATEDEFERREDCONTEXT, pWDDM2_6ContextFuncs, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATEDEFERREDCONTEXT, hDrvContext, 8);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDIARG_CREATEDEFERREDCONTEXT, hDrvContext, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATEDEFERREDCONTEXT, hRTCoreLayer, 16);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDIARG_CREATEDEFERREDCONTEXT, hRTCoreLayer, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATEDEFERREDCONTEXT, pWDDM2_6UMCallbacks, 24);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDIARG_CREATEDEFERREDCONTEXT, pWDDM2_6UMCallbacks, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATEDEFERREDCONTEXT, Flags, 32);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDIARG_CREATEDEFERREDCONTEXT, Flags, 4);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATEDEFERREDCONTEXT, WinePad0, 36);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDIARG_CREATEDEFERREDCONTEXT, WinePad0, 4);

/*
 * Group: resource creation and shared-resource opening arguments
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_createresource
 * Retrieved: 2026-09-08
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d11ddiarg_createresource
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_openresource
 *
 * The resource structures contain pointers to specification-defined helper
 * structures.  Those helpers are not yet needed by the promoted callbacks, so
 * they remain incomplete here: a pointer preserves the ABI while preventing
 * unverified field access.  The scalar enum-like values are represented as
 * UINT, which records their published four-byte ABI without inventing enum
 * members.  The D3D11 form appends ByteStride, DecoderBufferType, and
 * TextureLayout to the D3D10 form.
 */
typedef struct D3D10DDI_MIPINFO D3D10DDI_MIPINFO;
typedef struct D3D10_DDIARG_SUBRESOURCE_UP D3D10_DDIARG_SUBRESOURCE_UP;
typedef struct DXGI_DDI_PRIMARY_DESC DXGI_DDI_PRIMARY_DESC;
typedef struct D3DDDI_OPENALLOCATIONINFO D3DDDI_OPENALLOCATIONINFO;
typedef struct D3DDDI_OPENALLOCATIONINFO2 D3DDDI_OPENALLOCATIONINFO2;
typedef UINT D3D10DDIRESOURCE_TYPE;
typedef UINT D3D11_1DDI_VIDEO_DECODER_BUFFER_TYPE;
typedef UINT D3DWDDM2_0DDI_TEXTURE_LAYOUT;
typedef void *D3D10DDI_HKMRESOURCE;

typedef struct D3D10DDIARG_CREATERESOURCE
{
    const D3D10DDI_MIPINFO *pMipInfoList;
    const D3D10_DDIARG_SUBRESOURCE_UP *pInitialDataUP;
    D3D10DDIRESOURCE_TYPE ResourceDimension;
    UINT Usage;
    UINT BindFlags;
    UINT MapFlags;
    UINT MiscFlags;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_DDI_PRIMARY_DESC *pPrimaryDesc;
} D3D10DDIARG_CREATERESOURCE;

typedef struct D3D11DDIARG_CREATERESOURCE
{
    const D3D10DDI_MIPINFO *pMipInfoList;
    const D3D10_DDIARG_SUBRESOURCE_UP *pInitialDataUP;
    D3D10DDIRESOURCE_TYPE ResourceDimension;
    UINT Usage;
    UINT BindFlags;
    UINT MapFlags;
    UINT MiscFlags;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT MipLevels;
    UINT ArraySize;
    DXGI_DDI_PRIMARY_DESC *pPrimaryDesc;
    UINT ByteStride;
    D3D11_1DDI_VIDEO_DECODER_BUFFER_TYPE DecoderBufferType;
    D3DWDDM2_0DDI_TEXTURE_LAYOUT TextureLayout;
    UINT WinePad0;
} D3D11DDIARG_CREATERESOURCE;

typedef struct D3D10DDIARG_OPENRESOURCE
{
    UINT NumAllocations;
    UINT WinePad0;
    union
    {
        D3DDDI_OPENALLOCATIONINFO *pOpenAllocationInfo;
        D3DDDI_OPENALLOCATIONINFO2 *pOpenAllocationInfo2;
    };
    D3D10DDI_HKMRESOURCE hKMResource;
    void *pPrivateDriverData;
    UINT PrivateDriverDataSize;
    UINT WinePad1;
} D3D10DDIARG_OPENRESOURCE;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_CREATERESOURCE);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_CREATERESOURCE, 64);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_CREATERESOURCE, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, pMipInfoList, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, pInitialDataUP, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, ResourceDimension, 16);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, Usage, 20);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, BindFlags, 24);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, MapFlags, 28);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, MiscFlags, 32);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, Format, 36);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, SampleDesc, 40);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, MipLevels, 48);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, ArraySize, 52);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATERESOURCE, pPrimaryDesc, 56);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D11DDIARG_CREATERESOURCE);
WINE_DDI_ASSERT_SIZE(D3D11DDIARG_CREATERESOURCE, 80);
WINE_DDI_ASSERT_ALIGN(D3D11DDIARG_CREATERESOURCE, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, pMipInfoList, 0);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, pInitialDataUP, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, ResourceDimension, 16);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, Usage, 20);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, BindFlags, 24);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, MapFlags, 28);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, MiscFlags, 32);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, Format, 36);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, SampleDesc, 40);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, MipLevels, 48);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, ArraySize, 52);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, pPrimaryDesc, 56);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, ByteStride, 64);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, DecoderBufferType, 68);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, TextureLayout, 72);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_CREATERESOURCE, WinePad0, 76);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_OPENRESOURCE);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_OPENRESOURCE, 40);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_OPENRESOURCE, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENRESOURCE, NumAllocations, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENRESOURCE, WinePad0, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENRESOURCE, pOpenAllocationInfo, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENRESOURCE, pOpenAllocationInfo2, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENRESOURCE, hKMResource, 16);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENRESOURCE, pPrivateDriverData, 24);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENRESOURCE, PrivateDriverDataSize, 32);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_OPENRESOURCE, WinePad1, 36);

/*
 * Group: shader resource view and render target view creation arguments
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3dwddm2_0ddiarg_createshaderresourceview
 * Retrieved: 2026-09-08
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_buffer_shaderresourceview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_tex1d_shaderresourceview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3dwddm2_0ddiarg_tex2d_shaderresourceview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_tex3d_shaderresourceview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10_1ddiarg_texcube_shaderresourceview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d11ddiarg_bufferex_shaderresourceview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_createrendertargetview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_buffer_rendertargetview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_tex1d_rendertargetview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_tex2d_rendertargetview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_tex3d_rendertargetview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddiarg_texcube_rendertargetview
 *
 * The handles follow the documented convention (see the handle group above):
 * a driver handle is pDrvPrivate, a runtime handle is handle.  Declared here,
 * with the creation arguments, because this is the first group that needs
 * them; no specification page collects the view handle pair the way the
 * direct3d-version-10-runtime-and-driver-handles page collects the resource
 * and adapter ones, so their members are a derivation from that same
 * convention, not a quotation.
 *
 * D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW never received a published WDDM
 * 2.0-named revision the way the SRV and UAV creation-argument structures
 * did: the pinned driver's own signature (src/view.cpp) names its argument
 * D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, but both documentation surfaces
 * 404 for that exact name.  Every field the driver reads through it --
 * including TexCube.ArraySize, .FirstArraySlice and .MipSlice in
 * GetTranslationDesc, which would have forced a revision had the base
 * structure lacked them -- is already present on the published
 * D3D10DDIARG_CREATERENDERTARGETVIEW and its arms.  The local type name
 * follows the ABI call site; the fields are the cross-validated public ones,
 * unchanged.
 *
 * The SRV TexCube arm has a genuine surface disagreement, investigated rather
 * than picked arbitrarily.  The rendered syntax block for
 * D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW types the arm
 * D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW (4 fields, with cube-array
 * support), but the markdown mirror's member prose links the older, 2-field
 * D3D10DDIARG_TEXCUBE_SHADERRESOURCEVIEW instead.  The pinned driver's
 * GetTranslationDesc reads pDDIDesc->TexCube.First2DArrayFace and ->NumCubes
 * for the TEXTURECUBE case, fields only the D3D10_1 arm has, so the rendered
 * surface is what the ABI actually requires and the mirror's link is stale.
 * Both arms' own pages were fetched directly to confirm this rather than
 * guessing from the parent page's prose.
 *
 * Every other arm and every Buffer-shaped structure's FirstElement/
 * ElementOffset and NumElements/ElementWidth pairs are anonymous unions of
 * two names for one four-byte member, exactly as both surfaces print them;
 * this header declares the name the pinned driver's CopyViewDimensions
 * template reads (FirstElement, NumElements).
 */
typedef struct D3D10DDI_HSHADERRESOURCEVIEW
{
    void *pDrvPrivate;
} D3D10DDI_HSHADERRESOURCEVIEW;

typedef struct D3D10DDI_HRTSHADERRESOURCEVIEW
{
    void *handle;
} D3D10DDI_HRTSHADERRESOURCEVIEW;

typedef struct D3D10DDI_HRENDERTARGETVIEW
{
    void *pDrvPrivate;
} D3D10DDI_HRENDERTARGETVIEW;

typedef struct D3D10DDI_HRTRENDERTARGETVIEW
{
    void *handle;
} D3D10DDI_HRTRENDERTARGETVIEW;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HSHADERRESOURCEVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HSHADERRESOURCEVIEW, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HSHADERRESOURCEVIEW, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HSHADERRESOURCEVIEW, pDrvPrivate, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HSHADERRESOURCEVIEW, pDrvPrivate, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRTSHADERRESOURCEVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HRTSHADERRESOURCEVIEW, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRTSHADERRESOURCEVIEW, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HRTSHADERRESOURCEVIEW, handle, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HRTSHADERRESOURCEVIEW, handle, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRENDERTARGETVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HRENDERTARGETVIEW, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRENDERTARGETVIEW, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HRENDERTARGETVIEW, pDrvPrivate, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HRENDERTARGETVIEW, pDrvPrivate, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRTRENDERTARGETVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HRTRENDERTARGETVIEW, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRTRENDERTARGETVIEW, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HRTRENDERTARGETVIEW, handle, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HRTRENDERTARGETVIEW, handle, 8);

typedef struct D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW
{
    union
    {
        UINT FirstElement;
    };
    union
    {
        UINT NumElements;
    };
} D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW;

typedef struct D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW
{
    UINT MostDetailedMip;
    UINT FirstArraySlice;
    UINT MipLevels;
    UINT ArraySize;
} D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW;

typedef struct D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW
{
    UINT MostDetailedMip;
    UINT FirstArraySlice;
    UINT MipLevels;
    UINT ArraySize;
    UINT PlaneSlice;
    UINT PlaneIndex;
} D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW;

typedef struct D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW
{
    UINT MostDetailedMip;
    UINT MipLevels;
} D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW;

typedef struct D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW
{
    UINT MostDetailedMip;
    UINT MipLevels;
    UINT First2DArrayFace;
    UINT NumCubes;
} D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW;

typedef struct D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW
{
    union
    {
        UINT FirstElement;
    };
    union
    {
        UINT NumElements;
    };
    UINT Flags;
} D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW, FirstElement, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW, NumElements, 4);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW, 16);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW, MostDetailedMip, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW, FirstArraySlice, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW, MipLevels, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW, ArraySize, 12);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW);
WINE_DDI_ASSERT_SIZE(D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, 24);
WINE_DDI_ASSERT_ALIGN(D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, MostDetailedMip, 0);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, FirstArraySlice, 4);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, MipLevels, 8);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, ArraySize, 12);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, PlaneSlice, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, PlaneIndex, 20);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW, MostDetailedMip, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW, MipLevels, 4);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW);
WINE_DDI_ASSERT_SIZE(D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW, 16);
WINE_DDI_ASSERT_ALIGN(D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW, MostDetailedMip, 0);
WINE_DDI_ASSERT_FIELD(D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW, MipLevels, 4);
WINE_DDI_ASSERT_FIELD(D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW, First2DArrayFace, 8);
WINE_DDI_ASSERT_FIELD(D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW, NumCubes, 12);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW);
WINE_DDI_ASSERT_SIZE(D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW, 12);
WINE_DDI_ASSERT_ALIGN(D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW, FirstElement, 0);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW, NumElements, 4);
WINE_DDI_ASSERT_FIELD(D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW, Flags, 8);

typedef struct D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW
{
    D3D10DDI_HRESOURCE hDrvResource;
    DXGI_FORMAT Format;
    D3D10DDIRESOURCE_TYPE ResourceDimension;
    union
    {
        D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW Buffer;
        D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW Tex1D;
        D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW Tex2D;
        D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW Tex3D;
        D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW TexCube;
        D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW BufferEx;
    };
} D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW);
WINE_DDI_ASSERT_SIZE(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, 40);
WINE_DDI_ASSERT_ALIGN(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, 8);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, hDrvResource, 0);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Format, 8);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, ResourceDimension, 12);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Buffer, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Tex1D, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Tex2D, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Tex3D, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, TexCube, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, BufferEx, 16);

typedef struct D3D10DDIARG_BUFFER_RENDERTARGETVIEW
{
    union
    {
        UINT FirstElement;
    };
    union
    {
        UINT NumElements;
    };
} D3D10DDIARG_BUFFER_RENDERTARGETVIEW;

typedef struct D3D10DDIARG_TEX1D_RENDERTARGETVIEW
{
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D10DDIARG_TEX1D_RENDERTARGETVIEW;

typedef struct D3D10DDIARG_TEX2D_RENDERTARGETVIEW
{
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D10DDIARG_TEX2D_RENDERTARGETVIEW;

typedef struct D3D10DDIARG_TEX3D_RENDERTARGETVIEW
{
    UINT MipSlice;
    UINT FirstW;
    UINT WSize;
} D3D10DDIARG_TEX3D_RENDERTARGETVIEW;

typedef struct D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW
{
    UINT MipSlice;
    UINT FirstArraySlice;
    UINT ArraySize;
} D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_BUFFER_RENDERTARGETVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_BUFFER_RENDERTARGETVIEW, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_BUFFER_RENDERTARGETVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_BUFFER_RENDERTARGETVIEW, FirstElement, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_BUFFER_RENDERTARGETVIEW, NumElements, 4);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_TEX1D_RENDERTARGETVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_TEX1D_RENDERTARGETVIEW, 12);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_TEX1D_RENDERTARGETVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX1D_RENDERTARGETVIEW, MipSlice, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX1D_RENDERTARGETVIEW, FirstArraySlice, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX1D_RENDERTARGETVIEW, ArraySize, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_TEX2D_RENDERTARGETVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_TEX2D_RENDERTARGETVIEW, 12);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_TEX2D_RENDERTARGETVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX2D_RENDERTARGETVIEW, MipSlice, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX2D_RENDERTARGETVIEW, FirstArraySlice, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX2D_RENDERTARGETVIEW, ArraySize, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_TEX3D_RENDERTARGETVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_TEX3D_RENDERTARGETVIEW, 12);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_TEX3D_RENDERTARGETVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX3D_RENDERTARGETVIEW, MipSlice, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX3D_RENDERTARGETVIEW, FirstW, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEX3D_RENDERTARGETVIEW, WSize, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW, 12);
WINE_DDI_ASSERT_ALIGN(D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW, MipSlice, 0);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW, FirstArraySlice, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW, ArraySize, 8);

typedef struct D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW
{
    D3D10DDI_HRESOURCE hDrvResource;
    DXGI_FORMAT Format;
    D3D10DDIRESOURCE_TYPE ResourceDimension;
    union
    {
        D3D10DDIARG_BUFFER_RENDERTARGETVIEW Buffer;
        D3D10DDIARG_TEX1D_RENDERTARGETVIEW Tex1D;
        D3D10DDIARG_TEX2D_RENDERTARGETVIEW Tex2D;
        D3D10DDIARG_TEX3D_RENDERTARGETVIEW Tex3D;
        D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW TexCube;
    };
    UINT32 WinePad0;
} D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW);
WINE_DDI_ASSERT_SIZE(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, 32);
WINE_DDI_ASSERT_ALIGN(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, 8);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, hDrvResource, 0);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Format, 8);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, ResourceDimension, 12);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Buffer, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Tex1D, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Tex2D, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Tex3D, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, TexCube, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, WinePad0, 28);

/*
 * Group: shader object handles
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/direct3d-version-10-runtime-and-driver-handles
 * Retrieved: 2026-09-08
 *
 * The handles follow the documented convention (see the object-handle group
 * above): a driver handle is pDrvPrivate, a runtime handle is handle. One
 * pair covers every shader stage -- vertex, pixel, geometry, hull, domain,
 * and compute all share D3D10DDI_HSHADER and D3D10DDI_HRTSHADER on every
 * documented create/destroy page -- so it is declared once here, with the
 * first two stages this header promotes.
 *
 * D3D11_1DDIARG_STAGE_IO_SIGNATURES is the other type CreateVertexShader and
 * CreatePixelShader take, but only ever behind a pointer: no promoted slot
 * dereferences it, so per the resource group's precedent (D3D10DDI_MIPINFO
 * and its neighbours) it stays an incomplete forward declaration rather than
 * an unverified layout.
 */
typedef struct D3D10DDI_HSHADER
{
    void *pDrvPrivate;
} D3D10DDI_HSHADER;

typedef struct D3D10DDI_HRTSHADER
{
    void *handle;
} D3D10DDI_HRTSHADER;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HSHADER);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HSHADER, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HSHADER, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HSHADER, pDrvPrivate, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HSHADER, pDrvPrivate, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRTSHADER);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HRTSHADER, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRTSHADER, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HRTSHADER, handle, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HRTSHADER, handle, 8);

typedef struct D3D11_1DDIARG_STAGE_IO_SIGNATURES D3D11_1DDIARG_STAGE_IO_SIGNATURES;

/*
 * Group: input-assembler and output-merger binding types
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/direct3d-version-10-runtime-and-driver-handles
 * Retrieved: 2026-09-08
 *
 * Companion callback specifications:
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createelementlayout
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_setrendertargets
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_setviewports
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ne-d3d10umddi-d3d10_ddi_primitive_topology
 *
 * The handles follow the documented convention (see the object-handle group
 * above): a driver handle is pDrvPrivate, a runtime handle is handle.
 *
 * Only the driver halves of the depth-stencil and unordered-access view
 * handles are declared.  SetRenderTargets is the only promoted slot that
 * names them and it takes no runtime half; the create-view callbacks that
 * would are still placeholders, and declaring their handles before their
 * argument structures exist would be a type nothing yet constrains.  Note
 * what this does and does not enable: a promoted SetRenderTargets can be
 * handed a depth-stencil or unordered-access view, but no promoted slot can
 * produce one, which is the state docs/D3D11ON12-SKIPPABLE-ELEMENTS.md
 * describes for the MVP.
 */
typedef struct D3D10DDI_HELEMENTLAYOUT
{
    void *pDrvPrivate;
} D3D10DDI_HELEMENTLAYOUT;

typedef struct D3D10DDI_HRTELEMENTLAYOUT
{
    void *handle;
} D3D10DDI_HRTELEMENTLAYOUT;

typedef struct D3D10DDI_HDEPTHSTENCILVIEW
{
    void *pDrvPrivate;
} D3D10DDI_HDEPTHSTENCILVIEW;

typedef struct D3D11DDI_HUNORDEREDACCESSVIEW
{
    void *pDrvPrivate;
} D3D11DDI_HUNORDEREDACCESSVIEW;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HELEMENTLAYOUT);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HELEMENTLAYOUT, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HELEMENTLAYOUT, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HELEMENTLAYOUT, pDrvPrivate, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HELEMENTLAYOUT, pDrvPrivate, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HRTELEMENTLAYOUT);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HRTELEMENTLAYOUT, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HRTELEMENTLAYOUT, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HRTELEMENTLAYOUT, handle, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HRTELEMENTLAYOUT, handle, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_HDEPTHSTENCILVIEW);
WINE_DDI_ASSERT_SIZE(D3D10DDI_HDEPTHSTENCILVIEW, 8);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_HDEPTHSTENCILVIEW, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_HDEPTHSTENCILVIEW, pDrvPrivate, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_HDEPTHSTENCILVIEW, pDrvPrivate, 8);

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D11DDI_HUNORDEREDACCESSVIEW);
WINE_DDI_ASSERT_SIZE(D3D11DDI_HUNORDEREDACCESSVIEW, 8);
WINE_DDI_ASSERT_ALIGN(D3D11DDI_HUNORDEREDACCESSVIEW, 8);
WINE_DDI_ASSERT_FIELD(D3D11DDI_HUNORDEREDACCESSVIEW, pDrvPrivate, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D11DDI_HUNORDEREDACCESSVIEW, pDrvPrivate, 8);

/* Both are named only behind a const pointer by the slots promoted here, so
 * per the resource group's precedent they stay incomplete.  A pointer to an
 * incomplete type is ABI-complete and it stops a caller reading fields this
 * header has not derived. */
typedef struct D3D10DDIARG_CREATEELEMENTLAYOUT D3D10DDIARG_CREATEELEMENTLAYOUT;
typedef struct D3D10_DDI_VIEWPORT D3D10_DDI_VIEWPORT;

/* IaSetTopology takes its topology by value, so the transport type has to be
 * declared -- but the enumeration's page publishes the enumerator names in
 * order and an initialiser for none of them.
 *
 * C's default numbering would make that list sequential from zero.  The
 * runtime's D3D_PRIMITIVE_TOPOLOGY, whose enumerator names these mirror one
 * for one, is not sequential: its adjacency and patch-list ranges sit at
 * values consecutive numbering cannot produce.  Which of the two the DDI
 * enumeration uses is exactly what the page does not say, so neither may be
 * written down here, and the authoring rule for that case is explicit --
 * leave unspecified constants undefined.
 *
 * The header therefore declares the transport and names no constant.  An
 * unscoped enumeration whose values fit in int has underlying type int in
 * both the MSVC and the MinGW model, so INT is the parameter's ABI and the
 * only part of it a caller through this slot depends on.  A host that needs a
 * particular topology takes the constant from the WDK header it compiles
 * against.
 *
 * One consequence, recorded because it is a real loss: this slot's topology
 * argument cannot be type-checked, so tests/d3d11ddidrawpromotednegative.c
 * rests on the handle parameters instead. */
typedef INT D3D10_DDI_PRIMITIVE_TOPOLOGY;

/*
 * Group: resource readback types and callbacks
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ne-d3d10umddi-d3d10_ddi_map
 * Retrieved: 2026-09-08
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddi_mapped_subresource
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_resourcemap
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_resourceunmap
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_resourcecopy
 * Source mirror: https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/ne-d3d10umddi-d3d10_ddi_map.md
 *   https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/ns-d3d10umddi-d3d10ddi_mapped_subresource.md
 *
 * D3D10_DDI_MAP is passed by value, so its transport type must be complete.
 * Its page publishes the enumerator names but gives none an initializer or a
 * numeric value.  As with D3D10_DDI_PRIMITIVE_TOPOLOGY above, this header
 * therefore records only the ABI transport and deliberately names no
 * constants: assigning values would violate the clean-room attribution rule.
 * An unscoped enumeration whose values fit in int uses int under the Win64
 * MSVC and MinGW ABI, so INT is the attributable part of the declaration.
 *
 * D3D10DDI_MAPPED_SUBRESOURCE is complete because ResourceMap writes it and
 * the readback harness reads pData.  The rendered structure page and its raw
 * MicrosoftDocs source agree on the three members and their order.  Natural
 * Win64 alignment yields no implicit gaps: pointer, UINT, UINT is 16 bytes.
 *
 * PFND3D10DDI_RESOURCEMAP is shared by seven table members and
 * PFND3D10DDI_RESOURCEUNMAP by five.  Consequently all Dynamic*Map/Unmap,
 * staging, and general resource members using those typedefs are promoted
 * together; that breadth follows from typedef-keyed promotion, not a broader
 * scope choice.  The callback pages publish unnamed Syntax parameters, so
 * the descriptive parameter names below come from their Parameters sections.
 */
typedef INT D3D10_DDI_MAP;

typedef struct D3D10DDI_MAPPED_SUBRESOURCE
{
    void *pData;
    UINT RowPitch;
    UINT DepthPitch;
} D3D10DDI_MAPPED_SUBRESOURCE;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3D10DDI_MAPPED_SUBRESOURCE);
WINE_DDI_ASSERT_SIZE(D3D10DDI_MAPPED_SUBRESOURCE, 16);
WINE_DDI_ASSERT_ALIGN(D3D10DDI_MAPPED_SUBRESOURCE, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_MAPPED_SUBRESOURCE, pData, 0);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_MAPPED_SUBRESOURCE, pData, 8);
WINE_DDI_ASSERT_FIELD(D3D10DDI_MAPPED_SUBRESOURCE, RowPitch, 8);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_MAPPED_SUBRESOURCE, RowPitch, 4);
WINE_DDI_ASSERT_FIELD(D3D10DDI_MAPPED_SUBRESOURCE, DepthPitch, 12);
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDI_MAPPED_SUBRESOURCE, DepthPitch, 4);

typedef VOID (*PFND3D10DDI_RESOURCEMAP)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE hResource,
        UINT Subresource,
        D3D10_DDI_MAP DDIMap,
        UINT Flags,
        D3D10DDI_MAPPED_SUBRESOURCE *pMappedSubResource);

typedef VOID (*PFND3D10DDI_RESOURCEUNMAP)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE hResource,
        UINT Subresource);

typedef VOID (*PFND3D10DDI_RESOURCECOPY)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE hDstResource,
        D3D10DDI_HRESOURCE hSrcResource);

/*
 * Group: WDDM 2.6 device function table
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3dwddm2_6ddi_devicefuncs
 * Source mirror: https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/ns-d3d10umddi-d3dwddm2_6ddi_devicefuncs.md
 * Retrieved: 2026-09-08
 *
 * The rendered syntax and MicrosoftDocs source mirror agree on all 178
 * members and their order. The driver fills this table; the host must not
 * invoke a slot until its published signature has been authored. Retaining
 * each published PFN name as an alias of the no-argument placeholder makes
 * accidental calls a compile error while keeping promotion local to one
 * typedef at a time.
 *
 * Twelve typedefs are promoted, covering thirteen slots. They are the
 * command-list and deferred-context creation families; each is quoted from its own
 * reference page, cited beside it.  Promotion moves no offset -- a promoted
 * function pointer is still a function pointer, and the table's size and
 * every slot's offset are asserted unchanged below, which is the point of
 * doing this family first.
 *
 * scripts/gen_ddi_layout.py holds the promoted set in PROMOTED_SLOTS and
 * requires each to have a real typedef and every other slot to remain a
 * placeholder alias, so a promotion cannot be half-landed and a slot cannot
 * quietly regress to a placeholder.
 *
 * pfnRecycleDestroyCommandList shares PFND3D11DDI_DESTROYCOMMANDLIST, which
 * is the declaration this header already carried and which the
 * DestroyCommandList page sanctions: it states that a driver may set
 * pfnRecycleDestroyCommandList to point at its DestroyCommandList, so the two
 * members take one type.
 * pfnRecycleDestroyCommandList has no reference page of its own -- the URL
 * its siblings would predict returns 404.
 */

/* The promoted command-list signatures.  Each returns nothing and reports
 * failure through pfnSetErrorCb instead, which is why none of them is
 * declared returning HRESULT.
 *
 * https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_abandoncommandlist
 * Takes the deferred context, which is a D3D10DDI_HDEVICE: this is the slot
 * that makes the absence of a deferred-context handle type concrete. */
typedef VOID (*PFND3D11DDI_ABANDONCOMMANDLIST)(
        D3D10DDI_HDEVICE hDevice);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_commandlistexecute */
typedef VOID (*PFND3D11DDI_COMMANDLISTEXECUTE)(
        D3D10DDI_HDEVICE hDevice,
        D3D11DDI_HCOMMANDLIST hCommandList);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_destroycommandlist
 * Also the type of pfnRecycleDestroyCommandList; see the group note above. */
typedef VOID (*PFND3D11DDI_DESTROYCOMMANDLIST)(
        D3D10DDI_HDEVICE hDevice,
        D3D11DDI_HCOMMANDLIST hCommandList);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_recyclecommandlist
 * The handle is an immediate-context handle, which the specification says and
 * no type distinguishes; the difference is the caller's, not the ABI's. */
typedef VOID (*PFND3D11DDI_RECYCLECOMMANDLIST)(
        D3D10DDI_HDEVICE hDevice,
        D3D11DDI_HCOMMANDLIST hCommandList);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_calcprivatecommandlistsize */
typedef SIZE_T (*PFND3D11DDI_CALCPRIVATECOMMANDLISTSIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATECOMMANDLIST *pCreateCommandList);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_createcommandlist */
typedef VOID (*PFND3D11DDI_CREATECOMMANDLIST)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATECOMMANDLIST *pCreateCommandList,
        D3D11DDI_HCOMMANDLIST hCommandList,
        D3D11DDI_HRTCOMMANDLIST hRTCommandList);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_recyclecreatecommandlist */
typedef HRESULT (*PFND3D11DDI_RECYCLECREATECOMMANDLIST)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATECOMMANDLIST *pCreateCommandList,
        D3D11DDI_HCOMMANDLIST hCommandList,
        D3D11DDI_HRTCOMMANDLIST hRTCommandList);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_checkdeferredcontexthandlesizes */
typedef VOID (*PFND3D11DDI_CHECKDEFERREDCONTEXTHANDLESIZES)(
        D3D10DDI_HDEVICE hDevice,
        UINT *pHSizes,
        D3D11DDI_HANDLESIZE *pHandleSize);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_calcdeferredcontexthandlesize */
typedef SIZE_T (*PFND3D11DDI_CALCDEFERREDCONTEXTHANDLESIZE)(
        D3D10DDI_HDEVICE hDevice,
        D3D11DDI_HANDLETYPE HandleType,
        VOID *pICObject);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_calcprivatedeferredcontextsize */
typedef SIZE_T (*PFND3D11DDI_CALCPRIVATEDEFERREDCONTEXTSIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE *pCalcPrivateDeferredContextSize);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_createdeferredcontext */
typedef VOID (*PFND3D11DDI_CREATEDEFERREDCONTEXT)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATEDEFERREDCONTEXT *pCreateDeferredContext);

/* https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_recyclecreatedeferredcontext */
typedef HRESULT (*PFND3D11DDI_RECYCLECREATEDEFERREDCONTEXT)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATEDEFERREDCONTEXT *pCreateDeferredContext);

/* Resource-family signatures, authored from the public callback pages. */
typedef SIZE_T (*PFND3D11DDI_CALCPRIVATERESOURCESIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATERESOURCE *pCreateResource);

typedef SIZE_T (*PFND3D10DDI_CALCPRIVATEOPENEDRESOURCESIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_OPENRESOURCE *pOpenResource);

typedef VOID (*PFND3D11DDI_CREATERESOURCE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATERESOURCE *pCreateResource,
        D3D10DDI_HRESOURCE hResource,
        D3D10DDI_HRTRESOURCE hRTResource);

typedef VOID (*PFND3D10DDI_OPENRESOURCE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_OPENRESOURCE *pOpenResource,
        D3D10DDI_HRESOURCE hResource,
        D3D10DDI_HRTRESOURCE hRTResource);

typedef VOID (*PFND3D10DDI_DESTROYRESOURCE)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE hResource);

/*
 * Group: base pipeline state callbacks
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddi_devicefuncs
 * Retrieved: 2026-09-08
 *
 * Companion callback specifications:
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11_1ddi_createblendstate
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createdepthstencilstate
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11_1ddi_createrasterizerstate
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createsampler
 *
 * The descriptor structures remain opaque until their nested enum and array
 * groups are authored.  A pointer to an incomplete type is ABI-complete and
 * keeps callers from reading fields that this clean-room header has not yet
 * derived.  The object handles are declared above and asserted as wrapped
 * pointers, matching the resource and command-list handle convention.
 */
typedef SIZE_T (*PFND3D11_1DDI_CALCPRIVATEBLENDSTATESIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11_1_DDI_BLEND_DESC *pBlendDesc);

typedef VOID (*PFND3D11_1DDI_CREATEBLENDSTATE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11_1_DDI_BLEND_DESC *pBlendDesc,
        D3D10DDI_HBLENDSTATE hBlendState,
        D3D10DDI_HRTBLENDSTATE hRTBlendState);

typedef VOID (*PFND3D10DDI_DESTROYBLENDSTATE)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HBLENDSTATE hBlendState);

typedef SIZE_T (*PFND3D10DDI_CALCPRIVATEDEPTHSTENCILSTATESIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D10_DDI_DEPTH_STENCIL_DESC *pDepthStencilDesc);

typedef VOID (*PFND3D10DDI_CREATEDEPTHSTENCILSTATE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D10_DDI_DEPTH_STENCIL_DESC *pDepthStencilDesc,
        D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState,
        D3D10DDI_HRTDEPTHSTENCILSTATE hRTDepthStencilState);

typedef VOID (*PFND3D10DDI_DESTROYDEPTHSTENCILSTATE)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HDEPTHSTENCILSTATE hDepthStencilState);

typedef SIZE_T (*PFND3DWDDM2_0DDI_CALCPRIVATERASTERIZERSTATESIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11_1_DDI_RASTERIZER_DESC *pRasterizerDesc);

typedef VOID (*PFND3DWDDM2_0DDI_CREATERASTERIZERSTATE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D11_1_DDI_RASTERIZER_DESC *pRasterizerDesc,
        D3D10DDI_HRASTERIZERSTATE hRasterizerState,
        D3D10DDI_HRTRASTERIZERSTATE hRTRasterizerState);

typedef VOID (*PFND3D10DDI_DESTROYRASTERIZERSTATE)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRASTERIZERSTATE hRasterizerState);

typedef SIZE_T (*PFND3D10DDI_CALCPRIVATESAMPLERSIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D10_DDI_SAMPLER_DESC *pSamplerDesc);

typedef VOID (*PFND3D10DDI_CREATESAMPLER)(
        D3D10DDI_HDEVICE hDevice,
        const D3D10_DDI_SAMPLER_DESC *pSamplerDesc,
        D3D10DDI_HSAMPLER hSampler,
        D3D10DDI_HRTSAMPLER hRTSampler);

typedef VOID (*PFND3D10DDI_DESTROYSAMPLER)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HSAMPLER hSampler);

/* View-family signatures, authored from the public callback pages.  The
 * render target pair has no published WDDM 2.0-named callback page (see the
 * group's provenance note); the base D3D10-era CalcPrivateRenderTargetView
 * and CreateRenderTargetView pages describe the identical parameter list. */
typedef SIZE_T (*PFND3DWDDM2_0DDI_CALCPRIVATESHADERRESOURCEVIEWSIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *pCreateShaderResourceView);

typedef VOID (*PFND3DWDDM2_0DDI_CREATESHADERRESOURCEVIEW)(
        D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *pCreateShaderResourceView,
        D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView,
        D3D10DDI_HRTSHADERRESOURCEVIEW hRTShaderResourceView);

typedef VOID (*PFND3D10DDI_DESTROYSHADERRESOURCEVIEW)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HSHADERRESOURCEVIEW hShaderResourceView);

typedef SIZE_T (*PFND3DWDDM2_0DDI_CALCPRIVATERENDERTARGETVIEWSIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *pCreateRenderTargetView);

typedef VOID (*PFND3DWDDM2_0DDI_CREATERENDERTARGETVIEW)(
        D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *pCreateRenderTargetView,
        D3D10DDI_HRENDERTARGETVIEW hRenderTargetView,
        D3D10DDI_HRTRENDERTARGETVIEW hRTRenderTargetView);

typedef VOID (*PFND3D10DDI_DESTROYRENDERTARGETVIEW)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRENDERTARGETVIEW hRenderTargetView);

/* Shader-family signatures, authored from the public callback pages.
 * pfnCalcPrivateShaderSize and pfnDestroyShader are shared by every shader
 * stage, not just the two promoted here; the geometry/hull/domain/compute
 * create slots stay behind placeholders until their own argument types are
 * authored. */
typedef SIZE_T (*PFND3D11_1DDI_CALCPRIVATESHADERSIZE)(
        D3D10DDI_HDEVICE hDevice,
        const UINT *pShaderCode,
        const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSignatures);

typedef VOID (*PFND3D11_1DDI_CREATEVERTEXSHADER)(
        D3D10DDI_HDEVICE hDevice,
        const UINT *pShaderCode,
        D3D10DDI_HSHADER hShader,
        D3D10DDI_HRTSHADER hRTShader,
        const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSignatures);

typedef VOID (*PFND3D11_1DDI_CREATEPIXELSHADER)(
        D3D10DDI_HDEVICE hDevice,
        const UINT *pShaderCode,
        D3D10DDI_HSHADER hShader,
        D3D10DDI_HRTSHADER hRTShader,
        const D3D11_1DDIARG_STAGE_IO_SIGNATURES *pSignatures);

typedef VOID (*PFND3D10DDI_DESTROYSHADER)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HSHADER hShader);

/*
 * Group: element layout, binding, and draw callbacks
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-d3d10ddi_devicefuncs
 * Retrieved: 2026-09-08
 *
 * Companion callback specifications:
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_calcprivateelementlayoutsize
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_createelementlayout
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_destroyelementlayout
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_setinputlayout
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_ia_setvertexbuffers
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_ia_settopology
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_setshader
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d11ddi_setrendertargets
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_setviewports
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_clearrendertargetview
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_draw
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_setblendstate
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_setdepthstencilstate
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_setrasterizerstate
 *
 * This is the group that turns the objects the earlier groups can create into
 * a frame.  It is what docs/D3D11ON12-SKIPPABLE-ELEMENTS.md calls the
 * "Triangle on Screen" path, past creation: bind an element layout and a
 * vertex buffer, bind the two shaders and the three state objects, name a
 * render target, clear it, and draw.
 *
 * Variances recorded, in the order they were found:
 *
 * PFND3D10DDI_SETSHADER is one type for six slots.  Its page is titled
 * CsSetShader and its Remarks then state, in the same words for each, that
 * DsSetShader, VsSetShader, GsSetShader, HsSetShader and PsSetShader set the
 * shader code for their stages.  One parameter list, one typedef, and so
 * promoting it promotes all six.  That does not promote creation for those
 * stages: geometry, hull, domain and compute shader *creation* takes argument
 * types this header has not authored and stays behind placeholders.
 *
 * PFND3D10DDI_CLEARRENDERTARGETVIEW has a defect on its page.  The Syntax
 * block reads (D3D10DDI_HDEVICE, D3D10DDI_HRENDERTARGETVIEW, FLOAT[4]) while
 * the Parameters section below it labels the second parameter pColorRGBA and
 * the third hRenderTargetView -- the names are transposed against the types.
 * The Syntax block is what declares the ABI and it is the reading that
 * type-checks, so it is what is transcribed; the prose ordering would put a
 * handle where a float array is passed.  Recorded rather than silently
 * resolved, because a later reader comparing the two will find the same
 * discrepancy.
 *
 * PFND3D10DDI_SETBLENDSTATE's third parameter is published as an unnamed
 * const FLOAT[4] -- the Parameters section lists the bare token "FLOAT[4]"
 * with no description at all.  The array bound and constness are transcribed
 * as published; the name is this header's, and marked so.
 *
 * PFND3D11DDI_SETRENDERTARGETS is the widest slot promoted so far at eleven
 * parameters, and the only one to name a depth-stencil or unordered-access
 * view.  It takes them; nothing promoted can make one.  See the binding-types
 * group above for why the handles are declared anyway.
 *
 * Two slots the frame would otherwise use are deliberately not here.
 * PFND3DWDDM2_0DDI_FLUSH has no published page -- the WDDM 2.0-named URL is a
 * 404 -- and the base PFND3D10DDI_FLUSH page describes a one-parameter list
 * that cannot be attributed to the WDDM 2.0-named typedef the table actually
 * holds, so it stays a placeholder.  PFND3D10DDI_SETSCISSORRECTS would need
 * D3D10_DDI_RECT, and the frame does not require a scissor.
 */
typedef SIZE_T (*PFND3D10DDI_CALCPRIVATEELEMENTLAYOUTSIZE)(
        D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_CREATEELEMENTLAYOUT *pCreateElementLayout);

typedef VOID (*PFND3D10DDI_CREATEELEMENTLAYOUT)(
        D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_CREATEELEMENTLAYOUT *pCreateElementLayout,
        D3D10DDI_HELEMENTLAYOUT hElementLayout,
        D3D10DDI_HRTELEMENTLAYOUT hRTElementLayout);

typedef VOID (*PFND3D10DDI_DESTROYELEMENTLAYOUT)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HELEMENTLAYOUT hElementLayout);

typedef VOID (*PFND3D10DDI_SETINPUTLAYOUT)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HELEMENTLAYOUT hInputLayout);

typedef VOID (*PFND3D10DDI_IA_SETVERTEXBUFFERS)(
        D3D10DDI_HDEVICE hDevice,
        UINT StartSlot,
        UINT NumBuffers,
        const D3D10DDI_HRESOURCE *phBuffers,
        const UINT *pStrides,
        const UINT *pOffsets);

typedef VOID (*PFND3D10DDI_IA_SETTOPOLOGY)(
        D3D10DDI_HDEVICE hDevice,
        D3D10_DDI_PRIMITIVE_TOPOLOGY PrimitiveTopology);

typedef VOID (*PFND3D10DDI_SETSHADER)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HSHADER hShader);

typedef VOID (*PFND3D11DDI_SETRENDERTARGETS)(
        D3D10DDI_HDEVICE hDevice,
        const D3D10DDI_HRENDERTARGETVIEW *phRenderTargetView,
        UINT NumRTVs,
        UINT ClearSlots,
        D3D10DDI_HDEPTHSTENCILVIEW hDepthStencilView,
        const D3D11DDI_HUNORDEREDACCESSVIEW *phUnorderedAccessView,
        const UINT *pUAVInitialCounts,
        UINT UAVStartSlot,
        UINT NumUAVs,
        UINT UAVRangeStart,
        UINT UAVRangeSize);

typedef VOID (*PFND3D10DDI_SETVIEWPORTS)(
        D3D10DDI_HDEVICE hDevice,
        UINT NumViewports,
        UINT ClearViewports,
        const D3D10_DDI_VIEWPORT *pViewports);

/* Parameter order from the Syntax block; see the transposition note above. */
typedef VOID (*PFND3D10DDI_CLEARRENDERTARGETVIEW)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRENDERTARGETVIEW hRenderTargetView,
        FLOAT ColorRGBA[4]);

typedef VOID (*PFND3D10DDI_DRAW)(
        D3D10DDI_HDEVICE hDevice,
        UINT VertexCount,
        UINT StartVertexLocation);

/* BlendFactor names an argument the specification leaves unnamed. */
typedef VOID (*PFND3D10DDI_SETBLENDSTATE)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HBLENDSTATE hState,
        const FLOAT BlendFactor[4],
        UINT SampleMask);

typedef VOID (*PFND3D10DDI_SETDEPTHSTENCILSTATE)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HDEPTHSTENCILSTATE hState,
        UINT StencilRef);

typedef VOID (*PFND3D10DDI_SETRASTERIZERSTATE)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRASTERIZERSTATE hRasterizerState);

typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_RESOURCEUPDATESUBRESOURCEUP;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_SETCONSTANTBUFFERS;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_SETSHADERRESOURCES;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_SETSAMPLERS;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_DRAWINDEXED;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_IA_SETINDEXBUFFER;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_DRAWINDEXEDINSTANCED;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_DRAWINSTANCED;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_SHADERRESOURCEVIEWREADAFTERWRITEHAZARD;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_RESOURCEREADAFTERWRITEHAZARD;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_QUERYEND;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_QUERYBEGIN;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_RESOURCECOPYREGION;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_SO_SETTARGETS;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_DRAWAUTO;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_SETSCISSORRECTS;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_CLEARDEPTHSTENCILVIEW;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_SETPREDICATION;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_QUERYGETDATA;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_0DDI_FLUSH;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_GENMIPS;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_RESOURCERESOLVESUBRESOURCE;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_RESOURCEISSTAGINGBUSY;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_6DDI_RELOCATEDEVICEFUNCS;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_CALCPRIVATEDEPTHSTENCILVIEWSIZE;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_CREATEDEPTHSTENCILVIEW;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_DESTROYDEPTHSTENCILVIEW;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_CREATEGEOMETRYSHADER;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_CALCPRIVATEGEOMETRYSHADERWITHSTREAMOUTPUT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_0DDI_CALCPRIVATEQUERYSIZE;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_0DDI_CREATEQUERY;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_DESTROYQUERY;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_CHECKFORMATSUPPORT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM1_3DDI_CHECKMULTISAMPLEQUALITYLEVELS;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_CHECKCOUNTERINFO;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_CHECKCOUNTER;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_DESTROYDEVICE;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_SETTEXTFILTERSIZE;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_RESETPRIMITIVEID;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D10DDI_SETVERTEXPIPELINEOUTPUT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_DRAWINDEXEDINSTANCEDINDIRECT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_DRAWINSTANCEDINDIRECT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_CREATEHULLSHADER;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_CREATEDOMAINSHADER;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_CALCPRIVATETESSELLATIONSHADERSIZE;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_SETSHADER_WITH_IFACES;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_CREATECOMPUTESHADER;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_0DDI_CALCPRIVATEUNORDEREDACCESSVIEWSIZE;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_0DDI_CREATEUNORDEREDACCESSVIEW;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_DESTROYUNORDEREDACCESSVIEW;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_CLEARUNORDEREDACCESSVIEWUINT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_CLEARUNORDEREDACCESSVIEWFLOAT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_SETUNORDEREDACCESSVIEWS;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_DISPATCH;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_DISPATCHINDIRECT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_SETRESOURCEMINLOD;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11DDI_COPYSTRUCTURECOUNT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_DISCARD;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_ASSIGNDEBUGBINARY;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_CHECKDIRECTFLIPSUPPORT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3D11_1DDI_CLEARVIEW;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM1_3DDI_UPDATETILEMAPPINGS;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM1_3DDI_COPYTILEMAPPINGS;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM1_3DDI_COPYTILES;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM1_3DDI_UPDATETILES;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM1_3DDI_TILEDRESOURCEBARRIER;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM1_3DDI_GETMIPPACKING;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM1_3DDI_RESIZETILEPOOL;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM1_3DDI_SETMARKER;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM1_3DDI_SETMARKERMODE;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_0DDI_SETHARDWAREPROTECTION;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_0DDI_GETRESOURCELAYOUT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_0DDI_RETRIEVE_SHADER_COMMENT;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_0DDI_SETHARDWAREPROTECTIONSTATE;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_1DDI_SYNC_TOKEN;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_2DDI_CALCPRIVATE_SHADERCACHE_SESSION_SIZE;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_2DDI_CREATE_SHADERCACHE_SESSION;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_2DDI_DESTROY_SHADERCACHE_SESSION;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_2DDI_SET_SHADERCACHE_SESSION;
/*
 * Group: WDDM 2.6 scanout capability query
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3dwddm2_6ddi_query_scanout_caps
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3d10umddi/ne-d3d10umddi-d3dwddm2_6ddi_scanout_flags
 * Source mirror: https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/nc-d3d10umddi-pfnd3dwddm2_6ddi_query_scanout_caps.md
 * Source mirror: https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/ne-d3d10umddi-d3dwddm2_6ddi_scanout_flags.md
 * Retrieved: 2026-09-11
 */
typedef UINT D3DDDI_VIDEO_PRESENT_SOURCE_ID;

typedef enum D3DWDDM2_6DDI_SCANOUT_FLAGS
{
    D3DWDDM2_6DDI_SCANOUT_FLAG_NONE,
    D3DWDDM2_6DDI_SCANOUT_FLAG_TRANSFORMATION_REQUIRED,
    D3DWDDM2_6DDI_SCANOUT_FLAG_TRANSFORMATION_DESIRED,
    D3DWDDM2_6DDI_SCANOUT_FLAG_UNPREDICTABLE_TIMING
} D3DWDDM2_6DDI_SCANOUT_FLAGS;

typedef void (*PFND3DWDDM2_6DDI_QUERY_SCANOUT_CAPS)(
        D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE hResource,
        UINT Subresource,
        D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId,
        UINT PlaneIdx,
        D3DWDDM2_6DDI_SCANOUT_FLAGS *pFlags);
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DWDDM2_6DDI_PREPARE_SCANOUT_TRANSFORMATION;

struct D3DWDDM2_6DDI_DEVICEFUNCS
{
    PFND3D11_1DDI_RESOURCEUPDATESUBRESOURCEUP               pfnDefaultConstantBufferUpdateSubresourceUP;
    PFND3D11_1DDI_SETCONSTANTBUFFERS                        pfnVsSetConstantBuffers;
    PFND3D10DDI_SETSHADERRESOURCES                          pfnPsSetShaderResources;
    PFND3D10DDI_SETSHADER                                   pfnPsSetShader;
    PFND3D10DDI_SETSAMPLERS                                 pfnPsSetSamplers;
    PFND3D10DDI_SETSHADER                                   pfnVsSetShader;
    PFND3D10DDI_DRAWINDEXED                                 pfnDrawIndexed;
    PFND3D10DDI_DRAW                                        pfnDraw;
    PFND3D10DDI_RESOURCEMAP                                 pfnDynamicIABufferMapNoOverwrite;
    PFND3D10DDI_RESOURCEUNMAP                               pfnDynamicIABufferUnmap;
    PFND3D10DDI_RESOURCEMAP                                 pfnDynamicConstantBufferMapDiscard;
    PFND3D10DDI_RESOURCEMAP                                 pfnDynamicIABufferMapDiscard;
    PFND3D10DDI_RESOURCEUNMAP                               pfnDynamicConstantBufferUnmap;
    PFND3D11_1DDI_SETCONSTANTBUFFERS                        pfnPsSetConstantBuffers;
    PFND3D10DDI_SETINPUTLAYOUT                              pfnIaSetInputLayout;
    PFND3D10DDI_IA_SETVERTEXBUFFERS                         pfnIaSetVertexBuffers;
    PFND3D10DDI_IA_SETINDEXBUFFER                           pfnIaSetIndexBuffer;
    PFND3D10DDI_DRAWINDEXEDINSTANCED                        pfnDrawIndexedInstanced;
    PFND3D10DDI_DRAWINSTANCED                               pfnDrawInstanced;
    PFND3D10DDI_RESOURCEMAP                                 pfnDynamicResourceMapDiscard;
    PFND3D10DDI_RESOURCEUNMAP                               pfnDynamicResourceUnmap;
    PFND3D11_1DDI_SETCONSTANTBUFFERS                        pfnGsSetConstantBuffers;
    PFND3D10DDI_SETSHADER                                   pfnGsSetShader;
    PFND3D10DDI_IA_SETTOPOLOGY                              pfnIaSetTopology;
    PFND3D10DDI_RESOURCEMAP                                 pfnStagingResourceMap;
    PFND3D10DDI_RESOURCEUNMAP                               pfnStagingResourceUnmap;
    PFND3D10DDI_SETSHADERRESOURCES                          pfnVsSetShaderResources;
    PFND3D10DDI_SETSAMPLERS                                 pfnVsSetSamplers;
    PFND3D10DDI_SETSHADERRESOURCES                          pfnGsSetShaderResources;
    PFND3D10DDI_SETSAMPLERS                                 pfnGsSetSamplers;
    PFND3D11DDI_SETRENDERTARGETS                            pfnSetRenderTargets;
    PFND3D10DDI_SHADERRESOURCEVIEWREADAFTERWRITEHAZARD      pfnShaderResourceViewReadAfterWriteHazard;
    PFND3D10DDI_RESOURCEREADAFTERWRITEHAZARD                pfnResourceReadAfterWriteHazard;
    PFND3D10DDI_SETBLENDSTATE                               pfnSetBlendState;
    PFND3D10DDI_SETDEPTHSTENCILSTATE                        pfnSetDepthStencilState;
    PFND3D10DDI_SETRASTERIZERSTATE                          pfnSetRasterizerState;
    PFND3D10DDI_QUERYEND                                    pfnQueryEnd;
    PFND3D10DDI_QUERYBEGIN                                  pfnQueryBegin;
    PFND3D11_1DDI_RESOURCECOPYREGION                        pfnResourceCopyRegion;
    PFND3D11_1DDI_RESOURCEUPDATESUBRESOURCEUP               pfnResourceUpdateSubresourceUP;
    PFND3D10DDI_SO_SETTARGETS                               pfnSoSetTargets;
    PFND3D10DDI_DRAWAUTO                                    pfnDrawAuto;
    PFND3D10DDI_SETVIEWPORTS                                pfnSetViewports;
    PFND3D10DDI_SETSCISSORRECTS                             pfnSetScissorRects;
    PFND3D10DDI_CLEARRENDERTARGETVIEW                       pfnClearRenderTargetView;
    PFND3D10DDI_CLEARDEPTHSTENCILVIEW                       pfnClearDepthStencilView;
    PFND3D10DDI_SETPREDICATION                              pfnSetPredication;
    PFND3D10DDI_QUERYGETDATA                                pfnQueryGetData;
    PFND3DWDDM2_0DDI_FLUSH                                  pfnFlush;
    PFND3D10DDI_GENMIPS                                     pfnGenMips;
    PFND3D10DDI_RESOURCECOPY                                pfnResourceCopy;
    PFND3D10DDI_RESOURCERESOLVESUBRESOURCE                  pfnResourceResolveSubresource;
    PFND3D10DDI_RESOURCEMAP                                 pfnResourceMap;
    PFND3D10DDI_RESOURCEUNMAP                               pfnResourceUnmap;
    PFND3D10DDI_RESOURCEISSTAGINGBUSY                       pfnResourceIsStagingBusy;
    PFND3DWDDM2_6DDI_RELOCATEDEVICEFUNCS                    pfnRelocateDeviceFuncs;
    PFND3D11DDI_CALCPRIVATERESOURCESIZE                     pfnCalcPrivateResourceSize;
    PFND3D10DDI_CALCPRIVATEOPENEDRESOURCESIZE               pfnCalcPrivateOpenedResourceSize;
    PFND3D11DDI_CREATERESOURCE                              pfnCreateResource;
    PFND3D10DDI_OPENRESOURCE                                pfnOpenResource;
    PFND3D10DDI_DESTROYRESOURCE                             pfnDestroyResource;
    PFND3DWDDM2_0DDI_CALCPRIVATESHADERRESOURCEVIEWSIZE      pfnCalcPrivateShaderResourceViewSize;
    PFND3DWDDM2_0DDI_CREATESHADERRESOURCEVIEW               pfnCreateShaderResourceView;
    PFND3D10DDI_DESTROYSHADERRESOURCEVIEW                   pfnDestroyShaderResourceView;
    PFND3DWDDM2_0DDI_CALCPRIVATERENDERTARGETVIEWSIZE        pfnCalcPrivateRenderTargetViewSize;
    PFND3DWDDM2_0DDI_CREATERENDERTARGETVIEW                 pfnCreateRenderTargetView;
    PFND3D10DDI_DESTROYRENDERTARGETVIEW                     pfnDestroyRenderTargetView;
    PFND3D11DDI_CALCPRIVATEDEPTHSTENCILVIEWSIZE             pfnCalcPrivateDepthStencilViewSize;
    PFND3D11DDI_CREATEDEPTHSTENCILVIEW                      pfnCreateDepthStencilView;
    PFND3D10DDI_DESTROYDEPTHSTENCILVIEW                     pfnDestroyDepthStencilView;
    PFND3D10DDI_CALCPRIVATEELEMENTLAYOUTSIZE                pfnCalcPrivateElementLayoutSize;
    PFND3D10DDI_CREATEELEMENTLAYOUT                         pfnCreateElementLayout;
    PFND3D10DDI_DESTROYELEMENTLAYOUT                        pfnDestroyElementLayout;
    PFND3D11_1DDI_CALCPRIVATEBLENDSTATESIZE                 pfnCalcPrivateBlendStateSize;
    PFND3D11_1DDI_CREATEBLENDSTATE                          pfnCreateBlendState;
    PFND3D10DDI_DESTROYBLENDSTATE                           pfnDestroyBlendState;
    PFND3D10DDI_CALCPRIVATEDEPTHSTENCILSTATESIZE            pfnCalcPrivateDepthStencilStateSize;
    PFND3D10DDI_CREATEDEPTHSTENCILSTATE                     pfnCreateDepthStencilState;
    PFND3D10DDI_DESTROYDEPTHSTENCILSTATE                    pfnDestroyDepthStencilState;
    PFND3DWDDM2_0DDI_CALCPRIVATERASTERIZERSTATESIZE         pfnCalcPrivateRasterizerStateSize;
    PFND3DWDDM2_0DDI_CREATERASTERIZERSTATE                  pfnCreateRasterizerState;
    PFND3D10DDI_DESTROYRASTERIZERSTATE                      pfnDestroyRasterizerState;
    PFND3D11_1DDI_CALCPRIVATESHADERSIZE                     pfnCalcPrivateShaderSize;
    PFND3D11_1DDI_CREATEVERTEXSHADER                        pfnCreateVertexShader;
    PFND3D11_1DDI_CREATEGEOMETRYSHADER                      pfnCreateGeometryShader;
    PFND3D11_1DDI_CREATEPIXELSHADER                         pfnCreatePixelShader;
    PFND3D11_1DDI_CALCPRIVATEGEOMETRYSHADERWITHSTREAMOUTPUT pfnCalcPrivateGeometryShaderWithStreamOutput;
    PFND3D11_1DDI_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT      pfnCreateGeometryShaderWithStreamOutput;
    PFND3D10DDI_DESTROYSHADER                               pfnDestroyShader;
    PFND3D10DDI_CALCPRIVATESAMPLERSIZE                      pfnCalcPrivateSamplerSize;
    PFND3D10DDI_CREATESAMPLER                               pfnCreateSampler;
    PFND3D10DDI_DESTROYSAMPLER                              pfnDestroySampler;
    PFND3DWDDM2_0DDI_CALCPRIVATEQUERYSIZE                   pfnCalcPrivateQuerySize;
    PFND3DWDDM2_0DDI_CREATEQUERY                            pfnCreateQuery;
    PFND3D10DDI_DESTROYQUERY                                pfnDestroyQuery;
    PFND3D10DDI_CHECKFORMATSUPPORT                          pfnCheckFormatSupport;
    PFND3DWDDM1_3DDI_CHECKMULTISAMPLEQUALITYLEVELS          pfnCheckMultisampleQualityLevels;
    PFND3D10DDI_CHECKCOUNTERINFO                            pfnCheckCounterInfo;
    PFND3D10DDI_CHECKCOUNTER                                pfnCheckCounter;
    PFND3D10DDI_DESTROYDEVICE                               pfnDestroyDevice;
    PFND3D10DDI_SETTEXTFILTERSIZE                           pfnSetTextFilterSize;
    PFND3D10DDI_RESOURCECOPY                                pfnResourceConvert;
    PFND3D11_1DDI_RESOURCECOPYREGION                        pfnResourceConvertRegion;
    PFND3D10DDI_RESETPRIMITIVEID                            pfnResetPrimitiveID;
    PFND3D10DDI_SETVERTEXPIPELINEOUTPUT                     pfnSetVertexPipelineOutput;
    PFND3D11DDI_DRAWINDEXEDINSTANCEDINDIRECT                pfnDrawIndexedInstancedIndirect;
    PFND3D11DDI_DRAWINSTANCEDINDIRECT                       pfnDrawInstancedIndirect;
    PFND3D11DDI_COMMANDLISTEXECUTE                          pfnCommandListExecute;
    PFND3D10DDI_SETSHADERRESOURCES                          pfnHsSetShaderResources;
    PFND3D10DDI_SETSHADER                                   pfnHsSetShader;
    PFND3D10DDI_SETSAMPLERS                                 pfnHsSetSamplers;
    PFND3D11_1DDI_SETCONSTANTBUFFERS                        pfnHsSetConstantBuffers;
    PFND3D10DDI_SETSHADERRESOURCES                          pfnDsSetShaderResources;
    PFND3D10DDI_SETSHADER                                   pfnDsSetShader;
    PFND3D10DDI_SETSAMPLERS                                 pfnDsSetSamplers;
    PFND3D11_1DDI_SETCONSTANTBUFFERS                        pfnDsSetConstantBuffers;
    PFND3D11_1DDI_CREATEHULLSHADER                          pfnCreateHullShader;
    PFND3D11_1DDI_CREATEDOMAINSHADER                        pfnCreateDomainShader;
    PFND3D11DDI_CHECKDEFERREDCONTEXTHANDLESIZES             pfnCheckDeferredContextHandleSizes;
    PFND3D11DDI_CALCDEFERREDCONTEXTHANDLESIZE               pfnCalcDeferredContextHandleSize;
    PFND3D11DDI_CALCPRIVATEDEFERREDCONTEXTSIZE              pfnCalcPrivateDeferredContextSize;
    PFND3D11DDI_CREATEDEFERREDCONTEXT                       pfnCreateDeferredContext;
    PFND3D11DDI_ABANDONCOMMANDLIST                          pfnAbandonCommandList;
    PFND3D11DDI_CALCPRIVATECOMMANDLISTSIZE                  pfnCalcPrivateCommandListSize;
    PFND3D11DDI_CREATECOMMANDLIST                           pfnCreateCommandList;
    PFND3D11DDI_DESTROYCOMMANDLIST                          pfnDestroyCommandList;
    PFND3D11_1DDI_CALCPRIVATETESSELLATIONSHADERSIZE         pfnCalcPrivateTessellationShaderSize;
    PFND3D11DDI_SETSHADER_WITH_IFACES                       pfnPsSetShaderWithIfaces;
    PFND3D11DDI_SETSHADER_WITH_IFACES                       pfnVsSetShaderWithIfaces;
    PFND3D11DDI_SETSHADER_WITH_IFACES                       pfnGsSetShaderWithIfaces;
    PFND3D11DDI_SETSHADER_WITH_IFACES                       pfnHsSetShaderWithIfaces;
    PFND3D11DDI_SETSHADER_WITH_IFACES                       pfnDsSetShaderWithIfaces;
    PFND3D11DDI_SETSHADER_WITH_IFACES                       pfnCsSetShaderWithIfaces;
    PFND3D11DDI_CREATECOMPUTESHADER                         pfnCreateComputeShader;
    PFND3D10DDI_SETSHADER                                   pfnCsSetShader;
    PFND3D10DDI_SETSHADERRESOURCES                          pfnCsSetShaderResources;
    PFND3D10DDI_SETSAMPLERS                                 pfnCsSetSamplers;
    PFND3D11_1DDI_SETCONSTANTBUFFERS                        pfnCsSetConstantBuffers;
    PFND3DWDDM2_0DDI_CALCPRIVATEUNORDEREDACCESSVIEWSIZE     pfnCalcPrivateUnorderedAccessViewSize;
    PFND3DWDDM2_0DDI_CREATEUNORDEREDACCESSVIEW              pfnCreateUnorderedAccessView;
    PFND3D11DDI_DESTROYUNORDEREDACCESSVIEW                  pfnDestroyUnorderedAccessView;
    PFND3D11DDI_CLEARUNORDEREDACCESSVIEWUINT                pfnClearUnorderedAccessViewUint;
    PFND3D11DDI_CLEARUNORDEREDACCESSVIEWFLOAT               pfnClearUnorderedAccessViewFloat;
    PFND3D11DDI_SETUNORDEREDACCESSVIEWS                     pfnCsSetUnorderedAccessViews;
    PFND3D11DDI_DISPATCH                                    pfnDispatch;
    PFND3D11DDI_DISPATCHINDIRECT                            pfnDispatchIndirect;
    PFND3D11DDI_SETRESOURCEMINLOD                           pfnSetResourceMinLOD;
    PFND3D11DDI_COPYSTRUCTURECOUNT                          pfnCopyStructureCount;
    PFND3D11DDI_RECYCLECOMMANDLIST                          pfnRecycleCommandList;
    PFND3D11DDI_RECYCLECREATECOMMANDLIST                    pfnRecycleCreateCommandList;
    PFND3D11DDI_RECYCLECREATEDEFERREDCONTEXT                pfnRecycleCreateDeferredContext;
    PFND3D11DDI_DESTROYCOMMANDLIST                          pfnRecycleDestroyCommandList;
    PFND3D11_1DDI_DISCARD                                   pfnDiscard;
    PFND3D11_1DDI_ASSIGNDEBUGBINARY                         pfnAssignDebugBinary;
    PFND3D10DDI_RESOURCEMAP                                 pfnDynamicConstantBufferMapNoOverwrite;
    PFND3D11_1DDI_CHECKDIRECTFLIPSUPPORT                    pfnCheckDirectFlipSupport;
    PFND3D11_1DDI_CLEARVIEW                                 pfnClearView;
    PFND3DWDDM1_3DDI_UPDATETILEMAPPINGS                     pfnUpdateTileMappings;
    PFND3DWDDM1_3DDI_COPYTILEMAPPINGS                       pfnCopyTileMappings;
    PFND3DWDDM1_3DDI_COPYTILES                              pfnCopyTiles;
    PFND3DWDDM1_3DDI_UPDATETILES                            pfnUpdateTiles;
    PFND3DWDDM1_3DDI_TILEDRESOURCEBARRIER                   pfnTiledResourceBarrier;
    PFND3DWDDM1_3DDI_GETMIPPACKING                          pfnGetMipPacking;
    PFND3DWDDM1_3DDI_RESIZETILEPOOL                         pfnResizeTilePool;
    PFND3DWDDM1_3DDI_SETMARKER                              pfnSetMarker;
    PFND3DWDDM1_3DDI_SETMARKERMODE                          pfnSetMarkerMode;
    PFND3DWDDM2_0DDI_SETHARDWAREPROTECTION                  pfnSetHardwareProtection;
    PFND3DWDDM2_0DDI_GETRESOURCELAYOUT                      pfnGetResourceLayout;
    PFND3DWDDM2_0DDI_RETRIEVE_SHADER_COMMENT                pfnRetrieveShaderComment;
    PFND3DWDDM2_0DDI_SETHARDWAREPROTECTIONSTATE             pfnSetHardwareProtectionState;
    PFND3DWDDM2_1DDI_SYNC_TOKEN                             pfnAcquireResource;
    PFND3DWDDM2_1DDI_SYNC_TOKEN                             pfnReleaseResource;
    PFND3DWDDM2_2DDI_CALCPRIVATE_SHADERCACHE_SESSION_SIZE   pfnCalcPrivateShaderCacheSessionSize;
    PFND3DWDDM2_2DDI_CREATE_SHADERCACHE_SESSION             pfnCreateShaderCacheSession;
    PFND3DWDDM2_2DDI_DESTROY_SHADERCACHE_SESSION            pfnDestroyShaderCacheSession;
    PFND3DWDDM2_2DDI_SET_SHADERCACHE_SESSION                pfnSetShaderCacheSession;
    PFND3DWDDM2_6DDI_QUERY_SCANOUT_CAPS                     pfnQueryScanoutCaps;
    PFND3DWDDM2_6DDI_PREPARE_SCANOUT_TRANSFORMATION         pfnPrepareScanoutTransformation;
};

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3DWDDM2_6DDI_DEVICEFUNCS);
WINE_DDI_ASSERT_SIZE(D3DWDDM2_6DDI_DEVICEFUNCS, 1424);
WINE_DDI_ASSERT_ALIGN(D3DWDDM2_6DDI_DEVICEFUNCS, 8);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDefaultConstantBufferUpdateSubresourceUP, 0);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnVsSetConstantBuffers, 8);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnPsSetShaderResources, 16);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnPsSetShader, 24);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnPsSetSamplers, 32);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnVsSetShader, 40);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawIndexed, 48);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDraw, 56);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicIABufferMapNoOverwrite, 64);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicIABufferUnmap, 72);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicConstantBufferMapDiscard, 80);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicIABufferMapDiscard, 88);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicConstantBufferUnmap, 96);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnPsSetConstantBuffers, 104);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnIaSetInputLayout, 112);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnIaSetVertexBuffers, 120);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnIaSetIndexBuffer, 128);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawIndexedInstanced, 136);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawInstanced, 144);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicResourceMapDiscard, 152);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicResourceUnmap, 160);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnGsSetConstantBuffers, 168);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnGsSetShader, 176);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnIaSetTopology, 184);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnStagingResourceMap, 192);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnStagingResourceUnmap, 200);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnVsSetShaderResources, 208);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnVsSetSamplers, 216);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnGsSetShaderResources, 224);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnGsSetSamplers, 232);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetRenderTargets, 240);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnShaderResourceViewReadAfterWriteHazard, 248);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceReadAfterWriteHazard, 256);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetBlendState, 264);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetDepthStencilState, 272);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetRasterizerState, 280);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnQueryEnd, 288);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnQueryBegin, 296);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceCopyRegion, 304);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceUpdateSubresourceUP, 312);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSoSetTargets, 320);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawAuto, 328);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetViewports, 336);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetScissorRects, 344);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnClearRenderTargetView, 352);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnClearDepthStencilView, 360);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetPredication, 368);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnQueryGetData, 376);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnFlush, 384);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnGenMips, 392);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceCopy, 400);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceResolveSubresource, 408);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceMap, 416);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceUnmap, 424);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceIsStagingBusy, 432);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnRelocateDeviceFuncs, 440);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateResourceSize, 448);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateOpenedResourceSize, 456);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateResource, 464);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnOpenResource, 472);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyResource, 480);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateShaderResourceViewSize, 488);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateShaderResourceView, 496);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyShaderResourceView, 504);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateRenderTargetViewSize, 512);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateRenderTargetView, 520);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyRenderTargetView, 528);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateDepthStencilViewSize, 536);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateDepthStencilView, 544);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyDepthStencilView, 552);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateElementLayoutSize, 560);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateElementLayout, 568);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyElementLayout, 576);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateBlendStateSize, 584);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateBlendState, 592);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyBlendState, 600);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateDepthStencilStateSize, 608);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateDepthStencilState, 616);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyDepthStencilState, 624);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateRasterizerStateSize, 632);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateRasterizerState, 640);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyRasterizerState, 648);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateShaderSize, 656);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateVertexShader, 664);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateGeometryShader, 672);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreatePixelShader, 680);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateGeometryShaderWithStreamOutput, 688);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateGeometryShaderWithStreamOutput, 696);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyShader, 704);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateSamplerSize, 712);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateSampler, 720);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroySampler, 728);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateQuerySize, 736);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateQuery, 744);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyQuery, 752);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckFormatSupport, 760);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckMultisampleQualityLevels, 768);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckCounterInfo, 776);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckCounter, 784);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyDevice, 792);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetTextFilterSize, 800);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceConvert, 808);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceConvertRegion, 816);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResetPrimitiveID, 824);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetVertexPipelineOutput, 832);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawIndexedInstancedIndirect, 840);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawInstancedIndirect, 848);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCommandListExecute, 856);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnHsSetShaderResources, 864);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnHsSetShader, 872);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnHsSetSamplers, 880);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnHsSetConstantBuffers, 888);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDsSetShaderResources, 896);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDsSetShader, 904);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDsSetSamplers, 912);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDsSetConstantBuffers, 920);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateHullShader, 928);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateDomainShader, 936);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckDeferredContextHandleSizes, 944);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcDeferredContextHandleSize, 952);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateDeferredContextSize, 960);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateDeferredContext, 968);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnAbandonCommandList, 976);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateCommandListSize, 984);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateCommandList, 992);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyCommandList, 1000);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateTessellationShaderSize, 1008);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnPsSetShaderWithIfaces, 1016);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnVsSetShaderWithIfaces, 1024);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnGsSetShaderWithIfaces, 1032);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnHsSetShaderWithIfaces, 1040);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDsSetShaderWithIfaces, 1048);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetShaderWithIfaces, 1056);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateComputeShader, 1064);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetShader, 1072);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetShaderResources, 1080);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetSamplers, 1088);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetConstantBuffers, 1096);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateUnorderedAccessViewSize, 1104);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateUnorderedAccessView, 1112);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyUnorderedAccessView, 1120);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnClearUnorderedAccessViewUint, 1128);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnClearUnorderedAccessViewFloat, 1136);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetUnorderedAccessViews, 1144);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDispatch, 1152);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDispatchIndirect, 1160);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetResourceMinLOD, 1168);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCopyStructureCount, 1176);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnRecycleCommandList, 1184);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnRecycleCreateCommandList, 1192);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnRecycleCreateDeferredContext, 1200);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnRecycleDestroyCommandList, 1208);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDiscard, 1216);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnAssignDebugBinary, 1224);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicConstantBufferMapNoOverwrite, 1232);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckDirectFlipSupport, 1240);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnClearView, 1248);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnUpdateTileMappings, 1256);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCopyTileMappings, 1264);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCopyTiles, 1272);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnUpdateTiles, 1280);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnTiledResourceBarrier, 1288);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnGetMipPacking, 1296);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnResizeTilePool, 1304);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetMarker, 1312);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetMarkerMode, 1320);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetHardwareProtection, 1328);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnGetResourceLayout, 1336);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnRetrieveShaderComment, 1344);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetHardwareProtectionState, 1352);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnAcquireResource, 1360);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnReleaseResource, 1368);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateShaderCacheSessionSize, 1376);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateShaderCacheSession, 1384);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyShaderCacheSession, 1392);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetShaderCacheSession, 1400);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnQueryScanoutCaps, 1408);
WINE_DDI_ASSERT_FIELD(D3DWDDM2_6DDI_DEVICEFUNCS, pfnPrepareScanoutTransformation, 1416);

/*
 * Group: kernel device callbacks
 * Specification: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddi_devicecallbacks
 * Retrieved: 2026-09-07
 *
 * Companion specifications, all retrieved 2026-09-07:
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_escape
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddicb_synctoken
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_escapecb
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_synctokencb
 *   https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dukmdt/ns-d3dukmdt-_d3dddi_escapeflags
 *
 * This is D3D10DDIARG_CREATEDEVICE's pKTCallbacks, the second and last table
 * the host fills and the driver calls, and the last unauthored member of the
 * device-creation arguments.
 *
 * Unlike the core-layer table, this one is not authored signature-complete.
 * The pinned driver reads exactly three of its slots, and the roadmap in
 * docs/CLEANROOM-DDI.md asks for only the members D3D11On12 invokes:
 *
 *   pfnEscapeCb          (slot 10) src/device.cpp, in Device::ReportError
 *   pfnAcquireResourceCb (slot 54) include/device.hpp, via a pointer-to-member
 *   pfnReleaseResourceCb (slot 55) include/device.hpp, likewise
 *
 * So the promoted set and the invoked set are the same set, deliberately.
 * Every other slot keeps its published type name as an alias of the
 * undeclared-slot type, which holds the offset exactly, cannot be called
 * without a cast, and turns promoting a signature later into a one-line change
 * here rather than an edit to the structure.  scripts/gen_ddi_layout.py holds
 * the same three indices and fails if they ever name different slots.
 *
 * Note that pfnPresentCb is in this table and is *not* one of the three.  The
 * driver does call a pfnPresentCb, but it is the DXGI table's, reached through
 * m_pDXGICallbacks in src/present.cpp; that belongs to the DXGI DDI interop
 * group.  Reading the name alone would put a signature on the wrong slot.
 *
 * The two documentation surfaces disagree here, and the disagreement is
 * recorded rather than resolved by picking one: the rendered syntax block
 * gives 66 members and the markdown mirror 65, the odd one being
 * pfnCreateNativeFenceCb, a WDDM 3.1 addition the mirror has not caught up
 * with.  The superset is declared.  See docs/D3D11ON12.md, under Decisions and
 * rejected alternatives, for why over-declaring is the safe direction for a
 * structure the host allocates.  What makes the disagreement tolerable is that
 * it is confined to the tail: slots 10, 54 and 55 sit at the same offsets
 * under both surfaces.
 *
 * pfnEscapeCb's first parameter is documented as hAdapter, but the driver
 * passes a null and puts the real handle in D3DDDICB_ESCAPE.hDevice.  A host
 * that validated that argument would reject every call the driver makes.
 *
 * The two sync-token slots must share one type, and share it by name.  The
 * driver does not call them through the table directly; it stores
 * "PFND3DDDI_SYNCTOKENCB D3DDDI_DEVICECALLBACKS::* const m_pCallback" and
 * binds it to one or the other.  A pointer-to-member needs the structure to be
 * a complete type in C++ and needs both members to have exactly that type, not
 * merely a compatible function-pointer type, so this is a constraint no offset
 * assertion can express.  tests/d3d11ddilayout.c reproduces the construct.
 *
 * D3DDDI_EXECUTIONSTATEESCAPE is deliberately absent.  The driver builds one
 * by value and passes its sizeof as PrivateDriverDataSize, but it has no
 * public reference page on either surface and no definition in the pinned
 * WineCX, so rule 1 forbids authoring it and no placeholder substitutes for a
 * type used by value.  Device::ReportError therefore does not compile yet.
 * That is a port dependency rather than a gap in this table, and
 * docs/CLEANROOM-DDI.md records it as an open question.
 */

/*
 * The escape flags, deliberately not named D3DDDI_ESCAPEFLAGS.
 *
 * The pinned WineCX does define that name, in include/d3dukmdt.h, as the
 * oldest published variant: HardwareAccess and 31 reserved bits, with no
 * DeviceStatusQuery.  DeviceStatusQuery is the one bit the driver sets.  The
 * host is Wine's D3D11 frontend and will include both that header and this
 * one, so declaring our own under the same name is a duplicate typedef and
 * deferring to Wine's is a missing member.  A distinct name is neither: the
 * layout is identical, and the driver never spells the type, only
 * EscapeCB.Flags.DeviceStatusQuery.
 *
 * The specification prints this structure with the later bits behind #if
 * ellipses, so which of them exist is version-dependent and unpublished.  The
 * four leading bits and Reserved : 28 are one of the printed variants
 * verbatim, not a blend of them.  Value aliases the whole word, so the size is
 * four bytes under every variant and only the bit positions could differ;
 * tests/d3d11ddilayout.c pins the two this port depends on.
 */
typedef struct WINE_D3D11DDI_ESCAPEFLAGS
{
    union
    {
        struct
        {
            UINT HardwareAccess : 1;
            UINT DeviceStatusQuery : 1;
            UINT ChangeFrameLatency : 1;
            UINT NoAdapterSynchronization : 1;
            UINT Reserved : 28;
        };
        UINT Value;
    };
} WINE_D3D11DDI_ESCAPEFLAGS;

WINE_DDI_ASSERT_SIZE(WINE_D3D11DDI_ESCAPEFLAGS, 4);
WINE_DDI_ASSERT_ALIGN(WINE_D3D11DDI_ESCAPEFLAGS, 4);
WINE_DDI_ASSERT_FIELD(WINE_D3D11DDI_ESCAPEFLAGS, Value, 0);

/* Both argument structures carry interior padding, so a host that assigns
 * member by member leaves the driver reading uninitialized bytes.  Zero the
 * whole structure, as D3D10DDIARG_CREATEDEVICE also requires.  The padding is
 * named here for the reasons recorded with D3D10DDIARG_CREATEDEVICE.WinePad0;
 * naming it is what lets it be asserted and zeroed, and it changes no
 * offset. */
typedef struct _D3DDDICB_ESCAPE
{
    HANDLE                    hDevice;
    WINE_D3D11DDI_ESCAPEFLAGS Flags;
    UINT32                    WinePad0;
    void                      *pPrivateDriverData;
    UINT                      PrivateDriverDataSize;
    UINT32                    WinePad1;
    HANDLE                    hContext;
} D3DDDICB_ESCAPE;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3DDDICB_ESCAPE);
WINE_DDI_ASSERT_SIZE(D3DDDICB_ESCAPE, 40);
WINE_DDI_ASSERT_ALIGN(D3DDDICB_ESCAPE, 8);
WINE_DDI_ASSERT_FIELD(D3DDDICB_ESCAPE, hDevice, 0);
WINE_DDI_ASSERT_FIELD(D3DDDICB_ESCAPE, Flags, 8);
WINE_DDI_ASSERT_FIELD_SIZE(D3DDDICB_ESCAPE, Flags, 4);
WINE_DDI_ASSERT_FIELD(D3DDDICB_ESCAPE, WinePad0, 12);
WINE_DDI_ASSERT_FIELD_SIZE(D3DDDICB_ESCAPE, WinePad0, 4);
WINE_DDI_ASSERT_FIELD(D3DDDICB_ESCAPE, pPrivateDriverData, 16);
WINE_DDI_ASSERT_FIELD(D3DDDICB_ESCAPE, PrivateDriverDataSize, 24);
WINE_DDI_ASSERT_FIELD_SIZE(D3DDDICB_ESCAPE, PrivateDriverDataSize, 4);
WINE_DDI_ASSERT_FIELD(D3DDDICB_ESCAPE, WinePad1, 28);
WINE_DDI_ASSERT_FIELD_SIZE(D3DDDICB_ESCAPE, WinePad1, 4);
WINE_DDI_ASSERT_FIELD(D3DDDICB_ESCAPE, hContext, 32);

typedef struct _D3DDDICB_SYNCTOKEN
{
    HANDLE       hSyncToken;
    UINT         BroadcastContextCount;
    UINT32       WinePad0;
    const HANDLE *BroadcastContextArray;
} D3DDDICB_SYNCTOKEN;

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3DDDICB_SYNCTOKEN);
WINE_DDI_ASSERT_SIZE(D3DDDICB_SYNCTOKEN, 24);
WINE_DDI_ASSERT_ALIGN(D3DDDICB_SYNCTOKEN, 8);
WINE_DDI_ASSERT_FIELD(D3DDDICB_SYNCTOKEN, hSyncToken, 0);
WINE_DDI_ASSERT_FIELD(D3DDDICB_SYNCTOKEN, BroadcastContextCount, 8);
WINE_DDI_ASSERT_FIELD_SIZE(D3DDDICB_SYNCTOKEN, BroadcastContextCount, 4);
WINE_DDI_ASSERT_FIELD(D3DDDICB_SYNCTOKEN, WinePad0, 12);
WINE_DDI_ASSERT_FIELD_SIZE(D3DDDICB_SYNCTOKEN, WinePad0, 4);
WINE_DDI_ASSERT_FIELD(D3DDDICB_SYNCTOKEN, BroadcastContextArray, 16);

/* The three promoted signatures: the slots the pinned driver invokes. */
typedef HRESULT (*PFND3DDDI_ESCAPECB)(
        HANDLE hAdapter,
        const D3DDDICB_ESCAPE *pData);

typedef HRESULT (*PFND3DDDI_SYNCTOKENCB)(
        HANDLE hDevice,
        const D3DDDICB_SYNCTOKEN *pData);

/* The 63 slots the driver never reads.  Each keeps its published type name so
 * the structure below reads as the specification prints it, and each is an
 * alias of the undeclared-slot type so that reading one here is unambiguous
 * about what has and has not been derived from a specification. */
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_ALLOCATECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_DEALLOCATECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_SETPRIORITYCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_QUERYRESIDENCYCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_SETDISPLAYMODECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_PRESENTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_RENDERCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_LOCKCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_UNLOCKCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_CREATEOVERLAYCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_UPDATEOVERLAYCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_FLIPOVERLAYCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_DESTROYOVERLAYCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_CREATECONTEXTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_DESTROYCONTEXTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_CREATESYNCHRONIZATIONOBJECTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_DESTROYSYNCHRONIZATIONOBJECTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_SETASYNCCALLBACKSCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_SETDISPLAYPRIVATEDRIVERFORMATCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_OFFERALLOCATIONSCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_RECLAIMALLOCATIONSCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_CREATESYNCHRONIZATIONOBJECT2CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECT2CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECT2CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_PRESENTMULTIPLANEOVERLAYCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_LOGUMDMARKERCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_MAKERESIDENTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_EVICTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMCPUCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTFROMCPUCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMGPUCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTFROMGPUCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_CREATEPAGINGQUEUECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_DESTROYPAGINGQUEUECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_LOCK2CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_UNLOCK2CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_INVALIDATECACHECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_RESERVEGPUVIRTUALADDRESSCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_MAPGPUVIRTUALADDRESSCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_FREEGPUVIRTUALADDRESSCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_UPDATEGPUVIRTUALADDRESSCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_CREATECONTEXTVIRTUALCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_SUBMITCOMMANDCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_DEALLOCATE2CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_RECLAIMALLOCATIONS2CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_GETRESOURCEPRESENTPRIVATEDRIVERDATACB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_UPDATEALLOCATIONPROPERTYCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_OFFERALLOCATIONS2CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_RECLAIMALLOCATIONS3CB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_CREATEHWCONTEXTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_DESTROYHWCONTEXTCB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_CREATEHWQUEUECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_DESTROYHWQUEUECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_SUBMITCOMMANDTOHWQUEUECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_SUBMITWAITFORSYNCOBJECTSTOHWQUEUECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB
        PFND3DDDI_SUBMITSIGNALSYNCOBJECTSTOHWQUEUECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_SUBMITPRESENTBLTTOHWQUEUECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_SUBMITPRESENTTOHWQUEUECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_SUBMITHISTORYSEQUENCECB;
typedef PFNWINE_D3D11DDI_UNDECLARED_CB PFND3DDDI_CREATENATIVEFENCECB;

struct _D3DDDI_DEVICECALLBACKS
{
    PFND3DDDI_ALLOCATECB                            pfnAllocateCb;
    PFND3DDDI_DEALLOCATECB                          pfnDeallocateCb;
    PFND3DDDI_SETPRIORITYCB                         pfnSetPriorityCb;
    PFND3DDDI_QUERYRESIDENCYCB                      pfnQueryResidencyCb;
    PFND3DDDI_SETDISPLAYMODECB                      pfnSetDisplayModeCb;
    PFND3DDDI_PRESENTCB                             pfnPresentCb;
    PFND3DDDI_RENDERCB                              pfnRenderCb;
    PFND3DDDI_LOCKCB                                pfnLockCb;
    PFND3DDDI_UNLOCKCB                              pfnUnlockCb;
    PFND3DDDI_ESCAPECB                              pfnEscapeCb;
    PFND3DDDI_CREATEOVERLAYCB                       pfnCreateOverlayCb;
    PFND3DDDI_UPDATEOVERLAYCB                       pfnUpdateOverlayCb;
    PFND3DDDI_FLIPOVERLAYCB                         pfnFlipOverlayCb;
    PFND3DDDI_DESTROYOVERLAYCB                      pfnDestroyOverlayCb;
    PFND3DDDI_CREATECONTEXTCB                       pfnCreateContextCb;
    PFND3DDDI_DESTROYCONTEXTCB                      pfnDestroyContextCb;
    PFND3DDDI_CREATESYNCHRONIZATIONOBJECTCB         pfnCreateSynchronizationObjectCb;
    PFND3DDDI_DESTROYSYNCHRONIZATIONOBJECTCB        pfnDestroySynchronizationObjectCb;
    PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTCB        pfnWaitForSynchronizationObjectCb;
    PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTCB         pfnSignalSynchronizationObjectCb;
    PFND3DDDI_SETASYNCCALLBACKSCB                   pfnSetAsyncCallbacksCb;
    PFND3DDDI_SETDISPLAYPRIVATEDRIVERFORMATCB       pfnSetDisplayPrivateDriverFormatCb;
    PFND3DDDI_OFFERALLOCATIONSCB                    pfnOfferAllocationsCb;
    PFND3DDDI_RECLAIMALLOCATIONSCB                  pfnReclaimAllocationsCb;
    PFND3DDDI_CREATESYNCHRONIZATIONOBJECT2CB        pfnCreateSynchronizationObject2Cb;
    PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECT2CB       pfnWaitForSynchronizationObject2Cb;
    PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECT2CB        pfnSignalSynchronizationObject2Cb;
    PFND3DDDI_PRESENTMULTIPLANEOVERLAYCB            pfnPresentMultiPlaneOverlayCb;
    PFND3DDDI_LOGUMDMARKERCB                        pfnLogUMDMarkerCb;
    PFND3DDDI_MAKERESIDENTCB                        pfnMakeResidentCb;
    PFND3DDDI_EVICTCB                               pfnEvictCb;
    PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMCPUCB pfnWaitForSynchronizationObjectFromCpuCb;
    PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTFROMCPUCB  pfnSignalSynchronizationObjectFromCpuCb;
    PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMGPUCB pfnWaitForSynchronizationObjectFromGpuCb;
    PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTFROMGPUCB  pfnSignalSynchronizationObjectFromGpuCb;
    PFND3DDDI_CREATEPAGINGQUEUECB                   pfnCreatePagingQueueCb;
    PFND3DDDI_DESTROYPAGINGQUEUECB                  pfnDestroyPagingQueueCb;
    PFND3DDDI_LOCK2CB                               pfnLock2Cb;
    PFND3DDDI_UNLOCK2CB                             pfnUnlock2Cb;
    PFND3DDDI_INVALIDATECACHECB                     pfnInvalidateCacheCb;
    PFND3DDDI_RESERVEGPUVIRTUALADDRESSCB            pfnReserveGpuVirtualAddressCb;
    PFND3DDDI_MAPGPUVIRTUALADDRESSCB                pfnMapGpuVirtualAddressCb;
    PFND3DDDI_FREEGPUVIRTUALADDRESSCB               pfnFreeGpuVirtualAddressCb;
    PFND3DDDI_UPDATEGPUVIRTUALADDRESSCB             pfnUpdateGpuVirtualAddressCb;
    PFND3DDDI_CREATECONTEXTVIRTUALCB                pfnCreateContextVirtualCb;
    PFND3DDDI_SUBMITCOMMANDCB                       pfnSubmitCommandCb;
    PFND3DDDI_DEALLOCATE2CB                         pfnDeallocate2Cb;
    PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2CB pfnSignalSynchronizationObjectFromGpu2Cb;
    PFND3DDDI_RECLAIMALLOCATIONS2CB                 pfnReclaimAllocations2Cb;
    PFND3DDDI_GETRESOURCEPRESENTPRIVATEDRIVERDATACB pfnGetResourcePresentPrivateDriverDataCb;
    PFND3DDDI_UPDATEALLOCATIONPROPERTYCB            pfnUpdateAllocationPropertyCb;
    PFND3DDDI_OFFERALLOCATIONS2CB                   pfnOfferAllocations2Cb;
    PFND3DDDI_RECLAIMALLOCATIONS3CB                 pfnReclaimAllocations3Cb;
    PFND3DDDI_SYNCTOKENCB                           pfnAcquireResourceCb;
    PFND3DDDI_SYNCTOKENCB                           pfnReleaseResourceCb;
    PFND3DDDI_CREATEHWCONTEXTCB                     pfnCreateHwContextCb;
    PFND3DDDI_DESTROYHWCONTEXTCB                    pfnDestroyHwContextCb;
    PFND3DDDI_CREATEHWQUEUECB                       pfnCreateHwQueueCb;
    PFND3DDDI_DESTROYHWQUEUECB                      pfnDestroyHwQueueCb;
    PFND3DDDI_SUBMITCOMMANDTOHWQUEUECB              pfnSubmitCommandToHwQueueCb;
    PFND3DDDI_SUBMITWAITFORSYNCOBJECTSTOHWQUEUECB   pfnSubmitWaitForSyncObjectsToHwQueueCb;
    PFND3DDDI_SUBMITSIGNALSYNCOBJECTSTOHWQUEUECB    pfnSubmitSignalSyncObjectsToHwQueueCb;
    PFND3DDDI_SUBMITPRESENTBLTTOHWQUEUECB           pfnSubmitPresentBltToHwQueueCb;
    PFND3DDDI_SUBMITPRESENTTOHWQUEUECB              pfnSubmitPresentToHwQueueCb;
    PFND3DDDI_SUBMITHISTORYSEQUENCECB               pfnSubmitHistorySequenceCb;
    PFND3DDDI_CREATENATIVEFENCECB                   pfnCreateNativeFenceCb;
};

WINE_DDI_ASSERT_STANDARD_LAYOUT(D3DDDI_DEVICECALLBACKS);
WINE_DDI_ASSERT_SIZE(D3DDDI_DEVICECALLBACKS, 528);
WINE_DDI_ASSERT_ALIGN(D3DDDI_DEVICECALLBACKS, 8);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnAllocateCb, 0);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnDeallocateCb, 8);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSetPriorityCb, 16);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnQueryResidencyCb, 24);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSetDisplayModeCb, 32);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnPresentCb, 40);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnRenderCb, 48);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnLockCb, 56);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnUnlockCb, 64);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnEscapeCb, 72);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnCreateOverlayCb, 80);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnUpdateOverlayCb, 88);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnFlipOverlayCb, 96);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnDestroyOverlayCb, 104);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnCreateContextCb, 112);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnDestroyContextCb, 120);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnCreateSynchronizationObjectCb, 128);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnDestroySynchronizationObjectCb, 136);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnWaitForSynchronizationObjectCb, 144);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSignalSynchronizationObjectCb, 152);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSetAsyncCallbacksCb, 160);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSetDisplayPrivateDriverFormatCb, 168);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnOfferAllocationsCb, 176);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnReclaimAllocationsCb, 184);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnCreateSynchronizationObject2Cb, 192);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnWaitForSynchronizationObject2Cb, 200);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSignalSynchronizationObject2Cb, 208);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnPresentMultiPlaneOverlayCb, 216);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnLogUMDMarkerCb, 224);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnMakeResidentCb, 232);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnEvictCb, 240);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnWaitForSynchronizationObjectFromCpuCb, 248);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSignalSynchronizationObjectFromCpuCb, 256);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnWaitForSynchronizationObjectFromGpuCb, 264);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSignalSynchronizationObjectFromGpuCb, 272);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnCreatePagingQueueCb, 280);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnDestroyPagingQueueCb, 288);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnLock2Cb, 296);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnUnlock2Cb, 304);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnInvalidateCacheCb, 312);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnReserveGpuVirtualAddressCb, 320);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnMapGpuVirtualAddressCb, 328);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnFreeGpuVirtualAddressCb, 336);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnUpdateGpuVirtualAddressCb, 344);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnCreateContextVirtualCb, 352);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSubmitCommandCb, 360);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnDeallocate2Cb, 368);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSignalSynchronizationObjectFromGpu2Cb, 376);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnReclaimAllocations2Cb, 384);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnGetResourcePresentPrivateDriverDataCb, 392);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnUpdateAllocationPropertyCb, 400);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnOfferAllocations2Cb, 408);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnReclaimAllocations3Cb, 416);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnAcquireResourceCb, 424);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnReleaseResourceCb, 432);
/* The driver binds one "PFND3DDDI_SYNCTOKENCB D3DDDI_DEVICECALLBACKS::*" to
 * either of these, so they must hold that type and not merely two compatible
 * function-pointer types.  The C++ arm of tests/d3d11ddilayout.c reproduces the
 * pointer-to-member; this states the requirement in both languages and at the
 * declaration rather than in a test that only C++ runs. */
WINE_DDI_ASSERT_SAME_FIELD_TYPE(D3DDDI_DEVICECALLBACKS,
        pfnAcquireResourceCb, pfnReleaseResourceCb);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnCreateHwContextCb, 440);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnDestroyHwContextCb, 448);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnCreateHwQueueCb, 456);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnDestroyHwQueueCb, 464);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSubmitCommandToHwQueueCb, 472);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSubmitWaitForSyncObjectsToHwQueueCb, 480);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSubmitSignalSyncObjectsToHwQueueCb, 488);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSubmitPresentBltToHwQueueCb, 496);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSubmitPresentToHwQueueCb, 504);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnSubmitHistorySequenceCb, 512);
WINE_DDI_ASSERT_FIELD(D3DDDI_DEVICECALLBACKS,
        pfnCreateNativeFenceCb, 520);

/* Every slot is a function pointer, so the table is its member count times the
 * pointer size.  Asserting that as arithmetic rather than as another literal
 * catches a member being dropped and the offsets renumbered to match, which
 * the per-field assertions above cannot see. */
WINE_DDI_STATIC_ASSERT(
        sizeof(D3DDDI_DEVICECALLBACKS) == 66 * sizeof(void (*)(void)),
        "the kernel callback table is 66 function pointers");

#endif /* WINE_D3D11DDI_H */
