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
 * Authored here so far:
 *
 *   - driver and runtime object handles (adapter, resource, device, and core
 *     layer);
 *   - adapter function tables and the OpenAdapter argument structure;
 *   - the version negotiation arithmetic;
 *   - the CreateDevice argument structure, its embedded DXGI base arguments,
 *     and the create-device flags.
 *
 * Still required by docs/D3D11ON12.md, each to land with its own provenance
 * block and layout assertions:
 *
 *   - context handle types;
 *   - runtime callback tables, both the core layer's and the kernel's;
 *   - device function tables;
 *   - resource, view, shader, state, query, and command structures;
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

/* D3D10DDIARG_CREATEDEVICE is completed by the device-creation group at the
 * end of this header; the adapter table below needs only its name.  The
 * tables and callback blocks its members point at stay incomplete, and each
 * is the subject of a group of its own. */
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
 */
#define D3D11_DDI_MAJOR_VERSION 11

#define WINE_D3D11_DDI_INTERFACE_VERSION(minor) \
    (((D3D11_DDI_MAJOR_VERSION) << 16) | (minor))

#define WINE_D3D11_DDI_SUPPORTED(interface_version, build_version) \
    ((((UINT64)(interface_version)) << 32) | (((UINT64)(build_version)) << 16))

/* The composition is the whole content of this group, so it is asserted
 * rather than trusted.  The operands are arbitrary and carry no claim about
 * any real DDI version. */
WINE_DDI_STATIC_ASSERT(D3D11_DDI_MAJOR_VERSION == 11,
        "the D3D11 DDI major version is 11");
WINE_DDI_STATIC_ASSERT(WINE_D3D11_DDI_INTERFACE_VERSION(3) == 0x000b0003,
        "an interface version is the major version in the high 16 bits");
WINE_DDI_STATIC_ASSERT(
        WINE_D3D11_DDI_SUPPORTED(0x000b0003, 0x0007) == 0x000b000300070000ULL,
        "a supported-version word is the interface version in the high 32 "
        "bits and the build version in the next 16");

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
 * arm is a pointer at the same offset, and scripts/gen_ddi_layout.swift models
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
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, hRTCoreLayer, 56);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, pWDDM2_6UMCallbacks, 64);
WINE_DDI_ASSERT_FIELD(D3D10DDIARG_CREATEDEVICE, Flags, 72);
/* Flags is four bytes at 72 and the next member is eight-byte aligned, so
 * there are four bytes of padding here that no member names.  The runtime
 * allocates this structure, and a driver reading uninitialized padding is a
 * bug the host cannot see, so the host must zero the whole structure rather
 * than assign member by member. */
WINE_DDI_ASSERT_FIELD_SIZE(D3D10DDIARG_CREATEDEVICE, Flags, 4);
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

#endif /* WINE_D3D11DDI_H */
