/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Self-test for the clean-room DDI layout harness and the groups it guards.
 *
 * Two jobs, in order of importance.
 *
 * First, it proves the WINE_DDI_ASSERT_* macros compile in both C and C++ and
 * that their compile-time answers agree with the layout an actual object has
 * at run time.  A harness that silently evaluated to true would make every DDI
 * offset assertion worthless, so it is checked against a reference structure
 * whose layout follows from the Win64 C ABI alone.
 *
 * Second, it walks the declared DDI groups the same way.  Their offsets are
 * already asserted at compile time in the header; repeating them here against
 * real objects is what distinguishes an assertion that holds from one the
 * compiler agreed with in principle.  Both languages are built because the
 * Wine D3D11 frontend is C and the host will be C++, and a group that laid out
 * differently between them would be a silent ABI break at the one boundary
 * that cannot tolerate it.
 *
 * Build (both languages must succeed):
 *   x86_64-w64-mingw32-gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
 *       -Irelay12-d3d11/ddi -o d3d11ddilayout.exe tests/d3d11ddilayout.c
 *   x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror \
 *       -Irelay12-d3d11/ddi -x c++ -o d3d11ddilayoutxx.exe tests/d3d11ddilayout.c
 */
#include <stdio.h>
#include <string.h>

#include "wine_d3d11ddi.h"
#include "padding_test_helper.h"

/* A reference structure whose layout follows from the Win64 C ABI alone. */
struct layout_probe
{
    UINT32 first;
    UINT32 second;
    UINT64 third;
    void *fourth;
    UINT32 fifth;
};

WINE_DDI_ASSERT_STANDARD_LAYOUT(struct layout_probe);
WINE_DDI_ASSERT_FIELD(struct layout_probe, first, 0);
WINE_DDI_ASSERT_FIELD(struct layout_probe, second, 4);
WINE_DDI_ASSERT_FIELD(struct layout_probe, third, 8);
WINE_DDI_ASSERT_FIELD_SIZE(struct layout_probe, third, 8);
WINE_DDI_ASSERT_ALIGN(struct layout_probe, 8);
WINE_DDI_STATIC_ASSERT(sizeof(void *) != 8
        || offsetof(struct layout_probe, fourth) == 16,
        "layout_probe.fourth has an unexpected offset");
WINE_DDI_STATIC_ASSERT(sizeof(void *) != 8
        || offsetof(struct layout_probe, fifth) == 24,
        "layout_probe.fifth has an unexpected offset");
WINE_DDI_STATIC_ASSERT(sizeof(void *) != 8
        || sizeof(struct layout_probe) == 32,
        "layout_probe has an unexpected size");

static int failures;

static void check_offset(const char *type, const char *field,
        unsigned long asserted, unsigned long actual)
{
    if (asserted == actual)
    {
        printf("[ ok ] %s.%s at offset %lu\n", type, field, actual);
        return;
    }
    printf("[fail] %s.%s: the harness says %lu, the object says %lu\n",
            type, field, asserted, actual);
    ++failures;
}

static void check_size(const char *type, unsigned long asserted,
        unsigned long actual)
{
    if (asserted == actual)
    {
        printf("[ ok ] %s occupies %lu bytes\n", type, actual);
        return;
    }
    printf("[fail] %s: the harness says %lu bytes, the object spans %lu\n",
            type, asserted, actual);
    ++failures;
}

/* offsetof against the address the object actually places a field at.  The
 * cast through char * is the only portable way to ask the second question. */
#define CHECK_FIELD(object, type, field) \
    check_offset(#type, #field, (unsigned long)offsetof(type, field), \
            (unsigned long)((const char *)&(object).field \
                    - (const char *)&(object)))

/*
 * The calling-convention guard.
 *
 * Read this before reading it as a test that can fail.  On Win64 x86_64 there
 * is one calling convention: __stdcall, __cdecl and __fastcall all name it,
 * arguments arrive in registers, and the caller owns the argument area.  The
 * stack pointer therefore cannot drift across a call through a DDI slot, and
 * no declaration this header could carry would make it drift.  These checks
 * pass unconditionally on the frozen contract, and that is not a defect in
 * them.
 *
 * They exist for the case the header itself describes: an ABI other than this
 * one must re-derive the declarations, and an ABI that does distinguish
 * conventions is one where a slot declared with the wrong one unbalances the
 * stack at the call site.  On that day this is the check that says so, next to
 * the offsets, instead of the port discovering it as a corrupted frame.  It
 * also pins the assumption itself: if these ever stop holding here, the claim
 * at the top of wine_d3d11ddi.h is wrong and everything built on it needs
 * re-reading.
 *
 * The reads are volatile with a memory clobber so the compiler cannot move
 * them across the call, which also touches memory.  The frame is fixed for the
 * body of a function with no alloca, so the two reads are comparable at -O2.
 */
#if defined(__x86_64__) && defined(__GNUC__)
# define READ_STACK_POINTER(into) \
    __asm__ volatile ("movq %%rsp, %0" : "=r"(into) : : "memory")
#else
/* Nothing to read, and nothing to claim: check_stack treats a pair of zeroes
 * as "not asked". */
# define READ_STACK_POINTER(into) ((into) = 0)
#endif

#define CHECK_STACK(what, call) \
    do { \
        unsigned long long rsp_before, rsp_after; \
        READ_STACK_POINTER(rsp_before); \
        (void)(call); \
        READ_STACK_POINTER(rsp_after); \
        check_stack((what), rsp_before, rsp_after); \
    } while (0)

static void check_stack(const char *what, unsigned long long before,
        unsigned long long after)
{
    if (!before && !after)
        return;

    if (before != after)
    {
        printf("[fail] %s moved the stack pointer by %lld bytes\n", what,
                (long long)after - (long long)before);
        ++failures;
        return;
    }

    /* Reported apart from drift: a frame that was never 16-byte aligned is a
     * different fault from one the callee unbalanced, and reading which of
     * the two tripped is the whole value of the guard on the day it does. */
    if (before & 0xf)
    {
        printf("[fail] %s ran on a stack aligned to %llu, not 16\n", what,
                before & 0xf);
        ++failures;
        return;
    }

    printf("[ ok ] %s left the stack pointer where it found it\n", what);
}


static void check_dirty_memory_padding(void)
{
    D3D10DDIARG_CREATEDEVICE *create = (D3D10DDIARG_CREATEDEVICE *)malloc_dirty(sizeof(*create));
    if (!create) return;

    memset(create, 0, sizeof(*create));
    check_uninitialized_padding("D3D10DDIARG_CREATEDEVICE (zeroed)", create, sizeof(*create));
    
    struct layout_probe *probe = (struct layout_probe *)malloc_dirty(sizeof(*probe));
    if (!probe) return;
    probe->first = 1;
    probe->second = 2;
    probe->third = 3;
    probe->fourth = NULL;
    probe->fifth = 5;
    
    // Explicitly demonstrating padding trap. We expect padding to be uninitialized here.
    int leaks = check_uninitialized_padding("struct layout_probe (un-zeroed)", probe, sizeof(*probe));
    if (leaks == 0) {
        printf("[fail] expected padding leaks in layout_probe, got none\n");
        failures++;
    } else {
        printf("[ ok ] padding trap correctly identified implicit gaps\n");
    }

    free(probe);
    free(create);
}

static void check_reference_probe(void)
{
    struct layout_probe probe;

    memset(&probe, 0, sizeof(probe));

    CHECK_FIELD(probe, struct layout_probe, first);
    CHECK_FIELD(probe, struct layout_probe, second);
    CHECK_FIELD(probe, struct layout_probe, third);
    CHECK_FIELD(probe, struct layout_probe, fourth);
    CHECK_FIELD(probe, struct layout_probe, fifth);
}

static void check_object_handles(void)
{
    D3D10DDI_HADAPTER adapter;
    D3D10DDI_HRTADAPTER rt_adapter;
    D3D10DDI_HRESOURCE resource;
    D3D10DDI_HRTRESOURCE rt_resource;

    memset(&adapter, 0, sizeof(adapter));
    memset(&rt_adapter, 0, sizeof(rt_adapter));
    memset(&resource, 0, sizeof(resource));
    memset(&rt_resource, 0, sizeof(rt_resource));

    CHECK_FIELD(adapter, D3D10DDI_HADAPTER, pDrvPrivate);
    CHECK_FIELD(rt_adapter, D3D10DDI_HRTADAPTER, handle);
    CHECK_FIELD(resource, D3D10DDI_HRESOURCE, pDrvPrivate);
    CHECK_FIELD(rt_resource, D3D10DDI_HRTRESOURCE, handle);

    /* A handle is one wrapped pointer and nothing else.  If a handle ever
     * grew, the runtime would be passing a different object than the driver
     * reads, silently. */
    check_size("D3D10DDI_HADAPTER", 8, (unsigned long)sizeof(adapter));
    check_size("D3D10DDI_HRTADAPTER", 8, (unsigned long)sizeof(rt_adapter));
    check_size("D3D10DDI_HRESOURCE", 8, (unsigned long)sizeof(resource));
    check_size("D3D10DDI_HRTRESOURCE", 8, (unsigned long)sizeof(rt_resource));
}

static void check_adapter_funcs(void)
{
    D3D10DDI_ADAPTERFUNCS funcs;
    D3D10_2DDI_ADAPTERFUNCS funcs_2;

    memset(&funcs, 0, sizeof(funcs));
    memset(&funcs_2, 0, sizeof(funcs_2));

    CHECK_FIELD(funcs, D3D10DDI_ADAPTERFUNCS, pfnCalcPrivateDeviceSize);
    CHECK_FIELD(funcs, D3D10DDI_ADAPTERFUNCS, pfnCreateDevice);
    CHECK_FIELD(funcs, D3D10DDI_ADAPTERFUNCS, pfnCloseAdapter);
    check_size("D3D10DDI_ADAPTERFUNCS", 24, (unsigned long)sizeof(funcs));

    CHECK_FIELD(funcs_2, D3D10_2DDI_ADAPTERFUNCS, pfnCalcPrivateDeviceSize);
    CHECK_FIELD(funcs_2, D3D10_2DDI_ADAPTERFUNCS, pfnCreateDevice);
    CHECK_FIELD(funcs_2, D3D10_2DDI_ADAPTERFUNCS, pfnCloseAdapter);
    CHECK_FIELD(funcs_2, D3D10_2DDI_ADAPTERFUNCS, pfnGetSupportedVersions);
    CHECK_FIELD(funcs_2, D3D10_2DDI_ADAPTERFUNCS, pfnGetCaps);
    check_size("D3D10_2DDI_ADAPTERFUNCS", 40, (unsigned long)sizeof(funcs_2));
}

static void check_open_adapter(void)
{
    D3D10DDIARG_OPENADAPTER open_data;
    const char *base;

    memset(&open_data, 0, sizeof(open_data));
    base = (const char *)&open_data;

    CHECK_FIELD(open_data, D3D10DDIARG_OPENADAPTER, hRTAdapter);
    CHECK_FIELD(open_data, D3D10DDIARG_OPENADAPTER, hAdapter);
    CHECK_FIELD(open_data, D3D10DDIARG_OPENADAPTER, Interface);
    CHECK_FIELD(open_data, D3D10DDIARG_OPENADAPTER, Version);
    CHECK_FIELD(open_data, D3D10DDIARG_OPENADAPTER, pAdapterCallbacks);
    CHECK_FIELD(open_data, D3D10DDIARG_OPENADAPTER, pAdapterFuncs);
    CHECK_FIELD(open_data, D3D10DDIARG_OPENADAPTER, pAdapterFuncs_2);
    check_size("D3D10DDIARG_OPENADAPTER", 40,
            (unsigned long)sizeof(open_data));

    /* The two adapter tables share one slot.  OpenAdapter10 fills the first
     * arm and OpenAdapter10_2 the second, so if these ever stopped aliasing,
     * a version 11 driver would write its table where the runtime does not
     * read one. */
    if ((const char *)&open_data.pAdapterFuncs
            == (const char *)&open_data.pAdapterFuncs_2)
    {
        printf("[ ok ] the adapter function table arms alias\n");
    }
    else
    {
        printf("[fail] the adapter function table arms do not alias: "
                "%lu against %lu\n",
                (unsigned long)((const char *)&open_data.pAdapterFuncs - base),
                (unsigned long)((const char *)&open_data.pAdapterFuncs_2
                        - base));
        ++failures;
    }
}

static void check_device_handles(void)
{
    D3D10DDI_HDEVICE device;
    D3D10DDI_HRTDEVICE rt_device;
    D3D10DDI_HRTCORELAYER rt_core_layer;

    memset(&device, 0, sizeof(device));
    memset(&rt_device, 0, sizeof(rt_device));
    memset(&rt_core_layer, 0, sizeof(rt_core_layer));

    CHECK_FIELD(device, D3D10DDI_HDEVICE, pDrvPrivate);
    CHECK_FIELD(rt_device, D3D10DDI_HRTDEVICE, handle);
    CHECK_FIELD(rt_core_layer, D3D10DDI_HRTCORELAYER, handle);

    check_size("D3D10DDI_HDEVICE", 8, (unsigned long)sizeof(device));
    check_size("D3D10DDI_HRTDEVICE", 8, (unsigned long)sizeof(rt_device));
    check_size("D3D10DDI_HRTCORELAYER", 8,
            (unsigned long)sizeof(rt_core_layer));
}

/* Stands in for the driver's own RetrieveSubObject.  Declaring it with the
 * declared signature is the point: it will not compile if the slot's type and
 * the callback's type ever disagree. */
static HRESULT stub_retrieve_sub_object(D3D10DDI_HDEVICE hDevice,
        UINT32 SubDeviceID, SIZE_T ParamSize, void *pParams,
        SIZE_T OutputParamSize, void *pOutputParamsBuffer)
{
    (void)hDevice;
    (void)SubDeviceID;
    (void)ParamSize;
    (void)pParams;
    (void)OutputParamSize;
    (void)pOutputParamsBuffer;
    return 0;
}

/* The runtime allocates the CreateDevice arguments, so this walk is against
 * the storage the host will really hand the driver. */
static void check_create_device(void)
{
    D3D10DDIARG_CREATEDEVICE create;
    PFND3D10DDI_RETRIEVESUBOBJECT retrieve_sub_object;
    const char *base;

    memset(&create, 0, sizeof(create));
    base = (const char *)&create;

    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, hRTDevice);
    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, Interface);
    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, Version);
    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, pKTCallbacks);
    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, pWDDM2_6DeviceFuncs);
    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, hDrvDevice);
    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, DXGIBaseDDI);
    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, hRTCoreLayer);
    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, pWDDM2_6UMCallbacks);
    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, Flags);
    CHECK_FIELD(create, D3D10DDIARG_CREATEDEVICE, ppfnRetrieveSubObject);
    check_size("D3D10DDIARG_CREATEDEVICE", 88, (unsigned long)sizeof(create));

    /* DXGI_DDI_BASE_ARGS is a member, not a pointer to one.  If it ever
     * became a pointer the structure would shrink by eight bytes and every
     * member after it would move, which is why its own members are located
     * from the enclosing structure's base here. */
    check_size("DXGI_DDI_BASE_ARGS", 16,
            (unsigned long)sizeof(create.DXGIBaseDDI));
    check_offset("D3D10DDIARG_CREATEDEVICE", "DXGIBaseDDI.pDXGIBaseCallbacks",
            40, (unsigned long)((const char *)&create.DXGIBaseDDI
                    .pDXGIBaseCallbacks - base));
    check_offset("D3D10DDIARG_CREATEDEVICE",
            "DXGIBaseDDI.pDXGIDDIBaseFunctions6_1", 48,
            (unsigned long)((const char *)&create.DXGIBaseDDI
                    .pDXGIDDIBaseFunctions6_1 - base));

    /* ppfnRetrieveSubObject points at storage the host owns and the driver
     * writes.  A host that passed a null here, or read the slot back from the
     * wrong place, would lose the driver's video function tables silently. */
    retrieve_sub_object = NULL;
    create.ppfnRetrieveSubObject = &retrieve_sub_object;
    *create.ppfnRetrieveSubObject = stub_retrieve_sub_object;
    if (retrieve_sub_object == stub_retrieve_sub_object)
    {
        printf("[ ok ] the driver's RetrieveSubObject lands in host storage\n");
    }
    else
    {
        printf("[fail] a write through ppfnRetrieveSubObject did not reach "
                "the host's slot\n");
        ++failures;
    }
}

/* Stand-ins for the host's own core-layer callbacks.  Declaring them with the
 * declared signatures and assigning them into the table is what makes this
 * group's claim to be signature-complete mean something: a typedef that
 * disagreed with an implementable function would not compile, and neither
 * the compile-time offsets nor the run-time walk below would notice. */
static int callback_calls;

static void stub_state_refresh(D3D10DDI_HRTCORELAYER hRuntimeDevice)
{
    (void)hRuntimeDevice;
    ++callback_calls;
}

static void stub_state_range_10(D3D10DDI_HRTCORELAYER hRuntimeDevice,
        UINT Count, UINT Base)
{
    (void)hRuntimeDevice;
    (void)Count;
    (void)Base;
    ++callback_calls;
}

static void stub_state_range_11(D3D10DDI_HRTCORELAYER hRuntimeDevice,
        UINT Base, UINT Count)
{
    (void)hRuntimeDevice;
    (void)Base;
    (void)Count;
    ++callback_calls;
}

static VOID stub_set_error(D3D10DDI_HRTCORELAYER hRuntimeDevice, HRESULT hr)
{
    (void)hRuntimeDevice;
    (void)hr;
    ++callback_calls;
}

static HRESULT stub_create_context(HANDLE hDevice,
        D3DDDICB_CREATECONTEXT *pData)
{
    (void)hDevice;
    (void)pData;
    return 0;
}

static HRESULT stub_create_context_virtual(HANDLE hDevice,
        D3DDDICB_CREATECONTEXTVIRTUAL *pData)
{
    (void)hDevice;
    (void)pData;
    return 0;
}

static HRESULT stub_shader_cache_store(
        D3DWDDM2_2DDI_HRTCACHESESSION hCacheSession,
        const D3DWDDM2_2DDI_SHADERCACHE_HASH *pPrecomputedHash,
        const void *pKey, SIZE_T KeyLen, const void *pValue, SIZE_T ValueLen)
{
    (void)hCacheSession;
    (void)pPrecomputedHash;
    (void)pKey;
    (void)KeyLen;
    (void)pValue;
    (void)ValueLen;
    return 0;
}

static void stub_shader_cache_addref_release(
        D3DWDDM2_2DDI_HRTCACHESESSION hCacheSession)
{
    (void)hCacheSession;
    ++callback_calls;
}

/* The two slots whose signature the specification does not publish.  They can
 * only be filled through their declared placeholder type, which is the whole
 * reason that type exists. */
static void stub_undeclared(void)
{
    ++callback_calls;
}

/* The table the host fills and the driver calls.  Every slot is a function
 * pointer, so a member dropped anywhere above shifts every one after it and
 * the driver calls the wrong host routine with the wrong frame. */
static void check_corelayer_callbacks(void)
{
    D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS callbacks;
    D3D10DDI_HRTCORELAYER core_layer;

    memset(&callbacks, 0, sizeof(callbacks));
    memset(&core_layer, 0, sizeof(core_layer));

#define CHECK_CB(field) \
    CHECK_FIELD(callbacks, D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS, field)

    CHECK_CB(pfnSetErrorCb);
    CHECK_CB(pfnStateVsConstBufCb);
    CHECK_CB(pfnStatePsSrvCb);
    CHECK_CB(pfnStatePsShaderCb);
    CHECK_CB(pfnStatePsSamplerCb);
    CHECK_CB(pfnStateVsShaderCb);
    CHECK_CB(pfnStatePsConstBufCb);
    CHECK_CB(pfnStateIaInputLayoutCb);
    CHECK_CB(pfnStateIaVertexBufCb);
    CHECK_CB(pfnStateIaIndexBufCb);
    CHECK_CB(pfnStateGsConstBufCb);
    CHECK_CB(pfnStateGsShaderCb);
    CHECK_CB(pfnStateIaPrimitiveTopologyCb);
    CHECK_CB(pfnStateVsSrvCb);
    CHECK_CB(pfnStateVsSamplerCb);
    CHECK_CB(pfnStateGsSrvCb);
    CHECK_CB(pfnStateGsSamplerCb);
    CHECK_CB(pfnStateOmRenderTargetsCb);
    CHECK_CB(pfnStateOmBlendStateCb);
    CHECK_CB(pfnStateOmDepthStateCb);
    CHECK_CB(pfnStateRsRastStateCb);
    CHECK_CB(pfnStateSoTargetsCb);
    CHECK_CB(pfnStateRsViewportsCb);
    CHECK_CB(pfnStateRsScissorCb);
    CHECK_CB(pfnDisableDeferredStagingResourceDestruction);
    CHECK_CB(pfnStateTextFilterSizeCb);
    CHECK_CB(pfnStateHsSrvCb);
    CHECK_CB(pfnStateHsShaderCb);
    CHECK_CB(pfnStateHsSamplerCb);
    CHECK_CB(pfnStateHsConstBufCb);
    CHECK_CB(pfnStateDsSrvCb);
    CHECK_CB(pfnStateDsShaderCb);
    CHECK_CB(pfnStateDsSamplerCb);
    CHECK_CB(pfnStateDsConstBufCb);
    CHECK_CB(pfnPerformAmortizedProcessingCb);
    CHECK_CB(pfnStateCsSrvCb);
    CHECK_CB(pfnStateCsUavCb);
    CHECK_CB(pfnStateCsShaderCb);
    CHECK_CB(pfnStateCsSamplerCb);
    CHECK_CB(pfnStateCsConstBufCb);
    CHECK_CB(pfnCreateContextCb);
    CHECK_CB(pfnCreateContextVirtualCb);
    CHECK_CB(pfnShaderCacheGetValueCb);
    CHECK_CB(pfnShaderCacheStoreValueCb);
    CHECK_CB(pfnShaderCacheAddRefCb);
    CHECK_CB(pfnShaderCacheReleaseCb);
    CHECK_CB(pfnQueryScanoutCapsCb);

#undef CHECK_CB

    check_size("D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS", 376,
            (unsigned long)sizeof(callbacks));

    /* One slot of every declared shape, filled and then called through the
     * table.  Assignment alone proves the signatures are implementable;
     * calling them proves the frame the driver would build actually lands. */
    callbacks.pfnSetErrorCb = stub_set_error;
    callbacks.pfnStatePsShaderCb = stub_state_refresh;
    callbacks.pfnStatePsSrvCb = stub_state_range_10;
    callbacks.pfnStateHsShaderCb = stub_state_refresh;
    callbacks.pfnStateHsSrvCb = stub_state_range_11;
    callbacks.pfnDisableDeferredStagingResourceDestruction = stub_state_refresh;
    callbacks.pfnPerformAmortizedProcessingCb = stub_state_refresh;
    callbacks.pfnCreateContextCb = stub_create_context;
    callbacks.pfnCreateContextVirtualCb = stub_create_context_virtual;
    callbacks.pfnShaderCacheStoreValueCb = stub_shader_cache_store;
    callbacks.pfnShaderCacheAddRefCb = stub_shader_cache_addref_release;
    callbacks.pfnShaderCacheReleaseCb = stub_shader_cache_addref_release;
    callbacks.pfnShaderCacheGetValueCb = stub_undeclared;
    callbacks.pfnQueryScanoutCapsCb = stub_undeclared;

    callback_calls = 0;
    /* Every call here goes through the stack guard.  One of each declared
     * shape is what makes that worth doing: a slot taking no arguments and
     * one taking two are exactly what a convention mismatch would tell
     * apart. */
    CHECK_STACK("PFND3D10DDI_SETERROR_CB",
            callbacks.pfnSetErrorCb(core_layer, 0));
    CHECK_STACK("PFND3D10DDI_STATE_PS_SHADER_CB",
            callbacks.pfnStatePsShaderCb(core_layer));
    CHECK_STACK("PFND3D10DDI_STATE_PS_SRV_CB",
            callbacks.pfnStatePsSrvCb(core_layer, 4, 0));
    CHECK_STACK("PFND3D11DDI_STATE_HS_SRV_CB",
            callbacks.pfnStateHsSrvCb(core_layer, 0, 4));
    CHECK_STACK("PFNWINE_D3D11DDI_UNDECLARED_CB (shader cache get)",
            callbacks.pfnShaderCacheGetValueCb());
    CHECK_STACK("PFNWINE_D3D11DDI_UNDECLARED_CB (query scanout caps)",
            callbacks.pfnQueryScanoutCapsCb());

    if (callback_calls == 6)
    {
        printf("[ ok ] the core-layer callback slots are callable as "
                "declared\n");
    }
    else
    {
        printf("[fail] %d of 6 core-layer callbacks reached their "
                "implementation\n", callback_calls);
        ++failures;
    }

    /* The two shader-cache reference-count slots are one type used twice, so
     * a single implementation has to fit both.  If they ever diverged, the
     * host would need two and would not be told. */
    if (callbacks.pfnShaderCacheAddRefCb == callbacks.pfnShaderCacheReleaseCb)
    {
        printf("[ ok ] the shader cache addref and release slots share a "
                "type\n");
    }
    else
    {
        printf("[fail] the shader cache addref and release slots no longer "
                "accept one implementation\n");
        ++failures;
    }
}

/* Stand-ins for the host's own kernel callbacks.  Only three of the 66 slots
 * have a signature to stand in for; the rest hold an offset and nothing
 * else. */
static int kernel_calls;

static HRESULT stub_escape(HANDLE hAdapter, const D3DDDICB_ESCAPE *pData)
{
    /* The driver passes a null here and puts the real handle in the argument
     * structure, so a host that rejected a null adapter would reject every
     * call it makes. */
    (void)hAdapter;
    if (pData && pData->Flags.DeviceStatusQuery)
        ++kernel_calls;
    return 0;
}

static HRESULT stub_sync_token(HANDLE hDevice, const D3DDDICB_SYNCTOKEN *pData)
{
    (void)hDevice;
    if (pData)
        ++kernel_calls;
    return 0;
}

/* The escape flags are a bitfield, so no offset assertion reaches the thing
 * that matters about them.  Value aliases the whole word, which is what makes
 * the bit positions checkable at all. */
static void check_escape_flags(void)
{
    WINE_D3D11DDI_ESCAPEFLAGS flags;

    memset(&flags, 0, sizeof(flags));
    flags.DeviceStatusQuery = 1;
    if (flags.Value == 0x2)
    {
        printf("[ ok ] DeviceStatusQuery is bit 1\n");
    }
    else
    {
        printf("[fail] DeviceStatusQuery set bits %#lx, expected 0x2\n",
                (unsigned long)flags.Value);
        ++failures;
    }

    memset(&flags, 0, sizeof(flags));
    flags.HardwareAccess = 1;
    if (flags.Value == 0x1)
    {
        printf("[ ok ] HardwareAccess is bit 0\n");
    }
    else
    {
        printf("[fail] HardwareAccess set bits %#lx, expected 0x1\n",
                (unsigned long)flags.Value);
        ++failures;
    }

    check_size("WINE_D3D11DDI_ESCAPEFLAGS", 4, (unsigned long)sizeof(flags));
}

/* The kernel callback table and the two argument structures the promoted
 * slots take.  The host allocates all three. */
static void check_kernel_callbacks(void)
{
    D3DDDI_DEVICECALLBACKS callbacks;
    D3DDDICB_ESCAPE escape;
    D3DDDICB_SYNCTOKEN token;

    memset(&callbacks, 0, sizeof(callbacks));
    memset(&escape, 0, sizeof(escape));
    memset(&token, 0, sizeof(token));

#define CHECK_KCB(field) \
    CHECK_FIELD(callbacks, D3DDDI_DEVICECALLBACKS, field)

    CHECK_KCB(pfnAllocateCb);
    CHECK_KCB(pfnDeallocateCb);
    CHECK_KCB(pfnSetPriorityCb);
    CHECK_KCB(pfnQueryResidencyCb);
    CHECK_KCB(pfnSetDisplayModeCb);
    CHECK_KCB(pfnPresentCb);
    CHECK_KCB(pfnRenderCb);
    CHECK_KCB(pfnLockCb);
    CHECK_KCB(pfnUnlockCb);
    CHECK_KCB(pfnEscapeCb);
    CHECK_KCB(pfnCreateOverlayCb);
    CHECK_KCB(pfnUpdateOverlayCb);
    CHECK_KCB(pfnFlipOverlayCb);
    CHECK_KCB(pfnDestroyOverlayCb);
    CHECK_KCB(pfnCreateContextCb);
    CHECK_KCB(pfnDestroyContextCb);
    CHECK_KCB(pfnCreateSynchronizationObjectCb);
    CHECK_KCB(pfnDestroySynchronizationObjectCb);
    CHECK_KCB(pfnWaitForSynchronizationObjectCb);
    CHECK_KCB(pfnSignalSynchronizationObjectCb);
    CHECK_KCB(pfnSetAsyncCallbacksCb);
    CHECK_KCB(pfnSetDisplayPrivateDriverFormatCb);
    CHECK_KCB(pfnOfferAllocationsCb);
    CHECK_KCB(pfnReclaimAllocationsCb);
    CHECK_KCB(pfnCreateSynchronizationObject2Cb);
    CHECK_KCB(pfnWaitForSynchronizationObject2Cb);
    CHECK_KCB(pfnSignalSynchronizationObject2Cb);
    CHECK_KCB(pfnPresentMultiPlaneOverlayCb);
    CHECK_KCB(pfnLogUMDMarkerCb);
    CHECK_KCB(pfnMakeResidentCb);
    CHECK_KCB(pfnEvictCb);
    CHECK_KCB(pfnWaitForSynchronizationObjectFromCpuCb);
    CHECK_KCB(pfnSignalSynchronizationObjectFromCpuCb);
    CHECK_KCB(pfnWaitForSynchronizationObjectFromGpuCb);
    CHECK_KCB(pfnSignalSynchronizationObjectFromGpuCb);
    CHECK_KCB(pfnCreatePagingQueueCb);
    CHECK_KCB(pfnDestroyPagingQueueCb);
    CHECK_KCB(pfnLock2Cb);
    CHECK_KCB(pfnUnlock2Cb);
    CHECK_KCB(pfnInvalidateCacheCb);
    CHECK_KCB(pfnReserveGpuVirtualAddressCb);
    CHECK_KCB(pfnMapGpuVirtualAddressCb);
    CHECK_KCB(pfnFreeGpuVirtualAddressCb);
    CHECK_KCB(pfnUpdateGpuVirtualAddressCb);
    CHECK_KCB(pfnCreateContextVirtualCb);
    CHECK_KCB(pfnSubmitCommandCb);
    CHECK_KCB(pfnDeallocate2Cb);
    CHECK_KCB(pfnSignalSynchronizationObjectFromGpu2Cb);
    CHECK_KCB(pfnReclaimAllocations2Cb);
    CHECK_KCB(pfnGetResourcePresentPrivateDriverDataCb);
    CHECK_KCB(pfnUpdateAllocationPropertyCb);
    CHECK_KCB(pfnOfferAllocations2Cb);
    CHECK_KCB(pfnReclaimAllocations3Cb);
    CHECK_KCB(pfnAcquireResourceCb);
    CHECK_KCB(pfnReleaseResourceCb);
    CHECK_KCB(pfnCreateHwContextCb);
    CHECK_KCB(pfnDestroyHwContextCb);
    CHECK_KCB(pfnCreateHwQueueCb);
    CHECK_KCB(pfnDestroyHwQueueCb);
    CHECK_KCB(pfnSubmitCommandToHwQueueCb);
    CHECK_KCB(pfnSubmitWaitForSyncObjectsToHwQueueCb);
    CHECK_KCB(pfnSubmitSignalSyncObjectsToHwQueueCb);
    CHECK_KCB(pfnSubmitPresentBltToHwQueueCb);
    CHECK_KCB(pfnSubmitPresentToHwQueueCb);
    CHECK_KCB(pfnSubmitHistorySequenceCb);
    CHECK_KCB(pfnCreateNativeFenceCb);

#undef CHECK_KCB

    check_size("D3DDDI_DEVICECALLBACKS", 528, (unsigned long)sizeof(callbacks));

    CHECK_FIELD(escape, D3DDDICB_ESCAPE, hDevice);
    CHECK_FIELD(escape, D3DDDICB_ESCAPE, Flags);
    CHECK_FIELD(escape, D3DDDICB_ESCAPE, pPrivateDriverData);
    CHECK_FIELD(escape, D3DDDICB_ESCAPE, PrivateDriverDataSize);
    CHECK_FIELD(escape, D3DDDICB_ESCAPE, hContext);
    check_size("D3DDDICB_ESCAPE", 40, (unsigned long)sizeof(escape));

    CHECK_FIELD(token, D3DDDICB_SYNCTOKEN, hSyncToken);
    CHECK_FIELD(token, D3DDDICB_SYNCTOKEN, BroadcastContextCount);
    CHECK_FIELD(token, D3DDDICB_SYNCTOKEN, BroadcastContextArray);
    check_size("D3DDDICB_SYNCTOKEN", 24, (unsigned long)sizeof(token));

    check_escape_flags();

    /* Fill the three slots the pinned driver reads and call them the way it
     * does.  Assignment proves the signatures are implementable; calling
     * proves the frames land. */
    callbacks.pfnEscapeCb = stub_escape;
    callbacks.pfnAcquireResourceCb = stub_sync_token;
    callbacks.pfnReleaseResourceCb = stub_sync_token;

    escape.Flags.DeviceStatusQuery = 1;
    kernel_calls = 0;
    CHECK_STACK("PFND3DDDI_ESCAPECB",
            callbacks.pfnEscapeCb(NULL, &escape));
    CHECK_STACK("PFND3DDDI_SYNCTOKENCB (acquire)",
            callbacks.pfnAcquireResourceCb(NULL, &token));
    CHECK_STACK("PFND3DDDI_SYNCTOKENCB (release)",
            callbacks.pfnReleaseResourceCb(NULL, &token));

    if (kernel_calls == 3)
    {
        printf("[ ok ] the promoted kernel callback slots are callable as "
                "declared\n");
    }
    else
    {
        printf("[fail] %d of 3 kernel callbacks reached their "
                "implementation\n", kernel_calls);
        ++failures;
    }

#ifdef __cplusplus
    /* The driver does not call the sync-token slots through the table.  It
     * stores a pointer-to-member and binds it to one or the other, which
     * needs this structure to be a complete type and needs both slots to have
     * exactly PFND3DDDI_SYNCTOKENCB rather than a compatible function-pointer
     * type.  Nothing else in this file can express that, and if it ever
     * stopped holding it would break in the port instead of here. */
    {
        PFND3DDDI_SYNCTOKENCB D3DDDI_DEVICECALLBACKS::*slot =
                &D3DDDI_DEVICECALLBACKS::pfnAcquireResourceCb;

        kernel_calls = 0;
        (void)(callbacks.*slot)(NULL, &token);
        slot = &D3DDDI_DEVICECALLBACKS::pfnReleaseResourceCb;
        (void)(callbacks.*slot)(NULL, &token);

        if (kernel_calls == 2)
        {
            printf("[ ok ] one pointer-to-member binds both sync-token "
                    "slots\n");
        }
        else
        {
            printf("[fail] %d of 2 sync-token slots were reached through a "
                    "pointer-to-member\n", kernel_calls);
            ++failures;
        }
    }
#endif
}

/* The version arithmetic is macro expansion, so the header asserts it at
 * compile time.  Recomputing it here would restate the same expansion and
 * prove nothing; what run time can still check is that the composed value
 * survives a UINT64 round trip through memory with its fields where the
 * runtime expects to read them. */
static void check_version_arithmetic(void)
{
    const UINT interface_version = WINE_D3D11_DDI_INTERFACE_VERSION(3);
    UINT64 supported;

    supported = WINE_D3D11_DDI_SUPPORTED(interface_version, 0x0007);

    if ((UINT)(supported >> 32) == interface_version)
    {
        printf("[ ok ] the interface version occupies the high 32 bits\n");
    }
    else
    {
        printf("[fail] the interface version is not in the high 32 bits: "
                "%lu\n", (unsigned long)(supported >> 32));
        ++failures;
    }

    if ((UINT)((supported >> 16) & 0xffff) == 0x0007)
    {
        printf("[ ok ] the build version occupies bits 16 through 31\n");
    }
    else
    {
        printf("[fail] the build version is not in bits 16 through 31: %lu\n",
                (unsigned long)((supported >> 16) & 0xffff));
        ++failures;
    }
}

/* Stand-ins for the driver's own command-list entry points: the first slots of
 * D3DWDDM2_6DDI_DEVICEFUNCS whose signature has been promoted from the
 * placeholder.  Declaring them with the declared signatures and assigning them
 * into the table is what makes the promotion mean something.  No offset
 * assertion can distinguish a promoted function pointer from a placeholder
 * one, so if a promoted typedef were wrong, this file is the only place that
 * would say so. */
static int command_list_calls;

static VOID stub_abandon_command_list(D3D10DDI_HDEVICE hDevice)
{
    (void)hDevice;
    ++command_list_calls;
}

static VOID stub_command_list_execute(D3D10DDI_HDEVICE hDevice,
        D3D11DDI_HCOMMANDLIST hCommandList)
{
    (void)hDevice;
    (void)hCommandList;
    ++command_list_calls;
}

static SIZE_T stub_calc_private_command_list_size(D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATECOMMANDLIST *create)
{
    (void)hDevice;
    (void)create;
    ++command_list_calls;
    return 64;
}

static VOID stub_create_command_list(D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATECOMMANDLIST *create,
        D3D11DDI_HCOMMANDLIST hCommandList,
        D3D11DDI_HRTCOMMANDLIST hRTCommandList)
{
    (void)hDevice;
    (void)create;
    (void)hCommandList;
    (void)hRTCommandList;
    ++command_list_calls;
}

static HRESULT stub_recycle_create_command_list(D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATECOMMANDLIST *create,
        D3D11DDI_HCOMMANDLIST hCommandList,
        D3D11DDI_HRTCOMMANDLIST hRTCommandList)
{
    (void)hDevice;
    (void)create;
    (void)hCommandList;
    (void)hRTCommandList;
    ++command_list_calls;
    return S_OK;
}

static VOID stub_check_deferred_context_handle_sizes(D3D10DDI_HDEVICE hDevice,
        UINT *count, D3D11DDI_HANDLESIZE *handle_size)
{
    (void)hDevice;
    (void)handle_size;
    ++command_list_calls;
    *count = 1;
}

static SIZE_T stub_calc_deferred_context_handle_size(
        D3D10DDI_HDEVICE hDevice, D3D11DDI_HANDLETYPE type, VOID *object)
{
    (void)hDevice;
    (void)type;
    (void)object;
    ++command_list_calls;
    return 32;
}

static SIZE_T stub_calc_private_deferred_context_size(
        D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE *calculate)
{
    (void)hDevice;
    (void)calculate;
    ++command_list_calls;
    return 128;
}

static VOID stub_create_deferred_context(D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATEDEFERREDCONTEXT *create)
{
    (void)hDevice;
    (void)create;
    ++command_list_calls;
}

static HRESULT stub_recycle_create_deferred_context(D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATEDEFERREDCONTEXT *create)
{
    (void)hDevice;
    (void)create;
    ++command_list_calls;
    return S_OK;
}

static SIZE_T stub_calc_private_resource_size(D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATERESOURCE *create)
{
    (void)hDevice;
    (void)create;
    ++command_list_calls;
    return 256;
}

static SIZE_T stub_calc_private_opened_resource_size(D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_OPENRESOURCE *open)
{
    (void)hDevice;
    (void)open;
    ++command_list_calls;
    return 128;
}

static VOID stub_create_resource(D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATERESOURCE *create,
        D3D10DDI_HRESOURCE resource, D3D10DDI_HRTRESOURCE rt_resource)
{
    (void)hDevice;
    (void)create;
    (void)resource;
    (void)rt_resource;
    ++command_list_calls;
}

static VOID stub_open_resource(D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_OPENRESOURCE *open,
        D3D10DDI_HRESOURCE resource, D3D10DDI_HRTRESOURCE rt_resource)
{
    (void)hDevice;
    (void)open;
    (void)resource;
    (void)rt_resource;
    ++command_list_calls;
}

static VOID stub_destroy_resource(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE resource)
{
    (void)hDevice;
    (void)resource;
    ++command_list_calls;
}

#define STATE_CALC_STUB(name, descriptor) \
static SIZE_T stub_calc_private_##name##_size(D3D10DDI_HDEVICE hDevice, \
        const descriptor *state) \
{ \
    (void)hDevice; \
    (void)state; \
    ++command_list_calls; \
    return 96; \
}

#define STATE_CREATE_STUB(name, descriptor, handle, runtime_handle) \
static VOID stub_create_##name(D3D10DDI_HDEVICE hDevice, \
        const descriptor *state, handle object, runtime_handle rt_object) \
{ \
    (void)hDevice; \
    (void)state; \
    (void)object; \
    (void)rt_object; \
    ++command_list_calls; \
}

#define STATE_DESTROY_STUB(name, handle) \
static VOID stub_destroy_##name(D3D10DDI_HDEVICE hDevice, handle object) \
{ \
    (void)hDevice; \
    (void)object; \
    ++command_list_calls; \
}

STATE_CALC_STUB(blend_state, D3D11_1_DDI_BLEND_DESC)
STATE_CREATE_STUB(blend_state, D3D11_1_DDI_BLEND_DESC,
        D3D10DDI_HBLENDSTATE, D3D10DDI_HRTBLENDSTATE)
STATE_DESTROY_STUB(blend_state, D3D10DDI_HBLENDSTATE)
STATE_CALC_STUB(depth_stencil_state, D3D10_DDI_DEPTH_STENCIL_DESC)
STATE_CREATE_STUB(depth_stencil_state, D3D10_DDI_DEPTH_STENCIL_DESC,
        D3D10DDI_HDEPTHSTENCILSTATE, D3D10DDI_HRTDEPTHSTENCILSTATE)
STATE_DESTROY_STUB(depth_stencil_state, D3D10DDI_HDEPTHSTENCILSTATE)
STATE_CALC_STUB(rasterizer_state, D3D11_1_DDI_RASTERIZER_DESC)
STATE_CREATE_STUB(rasterizer_state, D3D11_1_DDI_RASTERIZER_DESC,
        D3D10DDI_HRASTERIZERSTATE, D3D10DDI_HRTRASTERIZERSTATE)
STATE_DESTROY_STUB(rasterizer_state, D3D10DDI_HRASTERIZERSTATE)
STATE_CALC_STUB(sampler, D3D10_DDI_SAMPLER_DESC)
STATE_CREATE_STUB(sampler, D3D10_DDI_SAMPLER_DESC,
        D3D10DDI_HSAMPLER, D3D10DDI_HRTSAMPLER)
STATE_DESTROY_STUB(sampler, D3D10DDI_HSAMPLER)

#undef STATE_CALC_STUB
#undef STATE_CREATE_STUB
#undef STATE_DESTROY_STUB

static SIZE_T stub_calc_private_shader_resource_view_size(
        D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *create)
{
    (void)hDevice;
    (void)create;
    ++command_list_calls;
    return 96;
}

static VOID stub_create_shader_resource_view(D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *create,
        D3D10DDI_HSHADERRESOURCEVIEW view, D3D10DDI_HRTSHADERRESOURCEVIEW rt_view)
{
    (void)hDevice;
    (void)create;
    (void)view;
    (void)rt_view;
    ++command_list_calls;
}

static VOID stub_destroy_shader_resource_view(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HSHADERRESOURCEVIEW view)
{
    (void)hDevice;
    (void)view;
    ++command_list_calls;
}

static SIZE_T stub_calc_private_render_target_view_size(
        D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *create)
{
    (void)hDevice;
    (void)create;
    ++command_list_calls;
    return 96;
}

static VOID stub_create_render_target_view(D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *create,
        D3D10DDI_HRENDERTARGETVIEW view, D3D10DDI_HRTRENDERTARGETVIEW rt_view)
{
    (void)hDevice;
    (void)create;
    (void)view;
    (void)rt_view;
    ++command_list_calls;
}

static VOID stub_destroy_render_target_view(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRENDERTARGETVIEW view)
{
    (void)hDevice;
    (void)view;
    ++command_list_calls;
}

static SIZE_T stub_calc_private_shader_size(D3D10DDI_HDEVICE hDevice,
        const UINT *shader_code,
        const D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    (void)hDevice;
    (void)shader_code;
    (void)signatures;
    ++command_list_calls;
    return 64;
}

static VOID stub_create_vertex_shader(D3D10DDI_HDEVICE hDevice,
        const UINT *shader_code, D3D10DDI_HSHADER shader,
        D3D10DDI_HRTSHADER rt_shader,
        const D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    (void)hDevice;
    (void)shader_code;
    (void)shader;
    (void)rt_shader;
    (void)signatures;
    ++command_list_calls;
}

static VOID stub_create_pixel_shader(D3D10DDI_HDEVICE hDevice,
        const UINT *shader_code, D3D10DDI_HSHADER shader,
        D3D10DDI_HRTSHADER rt_shader,
        const D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    (void)hDevice;
    (void)shader_code;
    (void)shader;
    (void)rt_shader;
    (void)signatures;
    ++command_list_calls;
}

static VOID stub_destroy_shader(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HSHADER shader)
{
    (void)hDevice;
    (void)shader;
    ++command_list_calls;
}

static SIZE_T stub_calc_private_element_layout_size(D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_CREATEELEMENTLAYOUT *create)
{
    (void)hDevice;
    (void)create;
    ++command_list_calls;
    return 48;
}

static VOID stub_create_element_layout(D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_CREATEELEMENTLAYOUT *create,
        D3D10DDI_HELEMENTLAYOUT layout, D3D10DDI_HRTELEMENTLAYOUT rt_layout)
{
    (void)hDevice;
    (void)create;
    (void)layout;
    (void)rt_layout;
    ++command_list_calls;
}

static VOID stub_destroy_element_layout(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HELEMENTLAYOUT layout)
{
    (void)hDevice;
    (void)layout;
    ++command_list_calls;
}

static VOID stub_ia_set_input_layout(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HELEMENTLAYOUT layout)
{
    (void)hDevice;
    (void)layout;
    ++command_list_calls;
}

static VOID stub_ia_set_vertex_buffers(D3D10DDI_HDEVICE hDevice,
        UINT start_slot, UINT num_buffers, const D3D10DDI_HRESOURCE *buffers,
        const UINT *strides, const UINT *offsets)
{
    (void)hDevice;
    (void)start_slot;
    (void)num_buffers;
    (void)buffers;
    (void)strides;
    (void)offsets;
    ++command_list_calls;
}

static VOID stub_ia_set_topology(D3D10DDI_HDEVICE hDevice,
        D3D10_DDI_PRIMITIVE_TOPOLOGY topology)
{
    (void)hDevice;
    (void)topology;
    ++command_list_calls;
}

/* One implementation for all six stages, which is the claim the shared
 * typedef makes.  Assigning it into six slots below is where that claim would
 * fail if a stage ever took a different list. */
static VOID stub_set_shader(D3D10DDI_HDEVICE hDevice, D3D10DDI_HSHADER shader)
{
    (void)hDevice;
    (void)shader;
    ++command_list_calls;
}

static VOID stub_set_render_targets(D3D10DDI_HDEVICE hDevice,
        const D3D10DDI_HRENDERTARGETVIEW *rtvs, UINT num_rtvs,
        UINT clear_slots, D3D10DDI_HDEPTHSTENCILVIEW dsv,
        const D3D11DDI_HUNORDEREDACCESSVIEW *uavs,
        const UINT *uav_initial_counts, UINT uav_start_slot, UINT num_uavs,
        UINT uav_range_start, UINT uav_range_size)
{
    (void)hDevice;
    (void)rtvs;
    (void)num_rtvs;
    (void)clear_slots;
    (void)dsv;
    (void)uavs;
    (void)uav_initial_counts;
    (void)uav_start_slot;
    (void)num_uavs;
    (void)uav_range_start;
    (void)uav_range_size;
    ++command_list_calls;
}

static VOID stub_set_viewports(D3D10DDI_HDEVICE hDevice, UINT num_viewports,
        UINT clear_viewports, const D3D10_DDI_VIEWPORT *viewports)
{
    (void)hDevice;
    (void)num_viewports;
    (void)clear_viewports;
    (void)viewports;
    ++command_list_calls;
}

static VOID stub_clear_render_target_view(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRENDERTARGETVIEW view, FLOAT colour[4])
{
    (void)hDevice;
    (void)view;
    (void)colour;
    ++command_list_calls;
}

static VOID stub_draw(D3D10DDI_HDEVICE hDevice, UINT vertex_count,
        UINT start_vertex_location)
{
    (void)hDevice;
    (void)vertex_count;
    (void)start_vertex_location;
    ++command_list_calls;
}

static VOID stub_resource_map(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE resource, UINT subresource, D3D10_DDI_MAP map,
        UINT flags, D3D10DDI_MAPPED_SUBRESOURCE *mapped)
{
    (void)hDevice;
    (void)resource;
    (void)subresource;
    (void)map;
    (void)flags;
    (void)mapped;
    ++command_list_calls;
}

static VOID stub_resource_unmap(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE resource, UINT subresource)
{
    (void)hDevice;
    (void)resource;
    (void)subresource;
    ++command_list_calls;
}

static VOID stub_resource_copy(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE destination, D3D10DDI_HRESOURCE source)
{
    (void)hDevice;
    (void)destination;
    (void)source;
    ++command_list_calls;
}

static VOID stub_set_blend_state(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HBLENDSTATE state, const FLOAT blend_factor[4],
        UINT sample_mask)
{
    (void)hDevice;
    (void)state;
    (void)blend_factor;
    (void)sample_mask;
    ++command_list_calls;
}

static VOID stub_set_depth_stencil_state(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HDEPTHSTENCILSTATE state, UINT stencil_ref)
{
    (void)hDevice;
    (void)state;
    (void)stencil_ref;
    ++command_list_calls;
}

static VOID stub_set_rasterizer_state(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRASTERIZERSTATE state)
{
    (void)hDevice;
    (void)state;
    ++command_list_calls;
}

static void check_command_list_handle(void)
{
    D3D11DDI_HCOMMANDLIST command_list;
    D3D11DDI_HRTCOMMANDLIST rt_command_list;
    D3D11DDIARG_CREATECOMMANDLIST create;

    memset(&command_list, 0, sizeof(command_list));
    memset(&rt_command_list, 0, sizeof(rt_command_list));
    memset(&create, 0, sizeof(create));

    CHECK_FIELD(command_list, D3D11DDI_HCOMMANDLIST, pDrvPrivate);
    CHECK_FIELD(rt_command_list, D3D11DDI_HRTCOMMANDLIST, handle);
    CHECK_FIELD(create, D3D11DDIARG_CREATECOMMANDLIST, hDeferredContext);

    /* One wrapped pointer, like every other driver handle.  A handle that grew
     * would have the runtime passing a different object than the driver
     * reads. */
    check_size("D3D11DDI_HCOMMANDLIST", 8, (unsigned long)sizeof(command_list));
    check_size("D3D11DDI_HRTCOMMANDLIST", 8,
            (unsigned long)sizeof(rt_command_list));
    check_size("D3D11DDIARG_CREATECOMMANDLIST", 8,
            (unsigned long)sizeof(create));
}

static void check_deferred_context_arguments(void)
{
    D3D11DDI_HANDLESIZE handle_size;
    D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE calculate;
    D3D11DDIARG_CREATEDEFERREDCONTEXT create;

    memset(&handle_size, 0, sizeof(handle_size));
    memset(&calculate, 0, sizeof(calculate));
    memset(&create, 0, sizeof(create));

    CHECK_FIELD(handle_size, D3D11DDI_HANDLESIZE, HandleType);
    CHECK_FIELD(handle_size, D3D11DDI_HANDLESIZE, WinePad0);
    CHECK_FIELD(handle_size, D3D11DDI_HANDLESIZE, DriverPrivateSize);
    CHECK_FIELD(calculate, D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE, Flags);
    CHECK_FIELD(create, D3D11DDIARG_CREATEDEFERREDCONTEXT, pWDDM2_6ContextFuncs);
    CHECK_FIELD(create, D3D11DDIARG_CREATEDEFERREDCONTEXT, hDrvContext);
    CHECK_FIELD(create, D3D11DDIARG_CREATEDEFERREDCONTEXT, hRTCoreLayer);
    CHECK_FIELD(create, D3D11DDIARG_CREATEDEFERREDCONTEXT, pWDDM2_6UMCallbacks);
    CHECK_FIELD(create, D3D11DDIARG_CREATEDEFERREDCONTEXT, Flags);
    CHECK_FIELD(create, D3D11DDIARG_CREATEDEFERREDCONTEXT, WinePad0);

    check_size("D3D11DDI_HANDLETYPE", 4,
            (unsigned long)sizeof(D3D11DDI_HANDLETYPE));
    check_size("D3D11DDI_HANDLESIZE", 16,
            (unsigned long)sizeof(handle_size));
    check_size("D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE", 4,
            (unsigned long)sizeof(calculate));
    check_size("D3D11DDIARG_CREATEDEFERREDCONTEXT", 40,
            (unsigned long)sizeof(create));
    check_size("D3DWDDM2_2DDI_HT_CACHESESSION", 19,
            (unsigned long)D3DWDDM2_2DDI_HT_CACHESESSION);
}

static void check_resource_arguments(void)
{
    D3D10DDIARG_CREATERESOURCE create10;
    D3D11DDIARG_CREATERESOURCE create11;
    D3D10DDIARG_OPENRESOURCE open;
    D3D10DDI_MAPPED_SUBRESOURCE mapped;

    memset(&create10, 0, sizeof(create10));
    memset(&create11, 0, sizeof(create11));
    memset(&open, 0, sizeof(open));
    memset(&mapped, 0, sizeof(mapped));

    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, pMipInfoList);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, pInitialDataUP);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, ResourceDimension);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, Usage);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, BindFlags);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, MapFlags);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, MiscFlags);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, Format);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, SampleDesc);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, MipLevels);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, ArraySize);
    CHECK_FIELD(create10, D3D10DDIARG_CREATERESOURCE, pPrimaryDesc);
    CHECK_FIELD(create11, D3D11DDIARG_CREATERESOURCE, ByteStride);
    CHECK_FIELD(create11, D3D11DDIARG_CREATERESOURCE, DecoderBufferType);
    CHECK_FIELD(create11, D3D11DDIARG_CREATERESOURCE, TextureLayout);
    CHECK_FIELD(create11, D3D11DDIARG_CREATERESOURCE, WinePad0);
    CHECK_FIELD(open, D3D10DDIARG_OPENRESOURCE, NumAllocations);
    CHECK_FIELD(open, D3D10DDIARG_OPENRESOURCE, WinePad0);
    CHECK_FIELD(open, D3D10DDIARG_OPENRESOURCE, pOpenAllocationInfo);
    CHECK_FIELD(open, D3D10DDIARG_OPENRESOURCE, pOpenAllocationInfo2);
    CHECK_FIELD(open, D3D10DDIARG_OPENRESOURCE, hKMResource);
    CHECK_FIELD(open, D3D10DDIARG_OPENRESOURCE, pPrivateDriverData);
    CHECK_FIELD(open, D3D10DDIARG_OPENRESOURCE, PrivateDriverDataSize);
    CHECK_FIELD(open, D3D10DDIARG_OPENRESOURCE, WinePad1);
    CHECK_FIELD(mapped, D3D10DDI_MAPPED_SUBRESOURCE, pData);
    CHECK_FIELD(mapped, D3D10DDI_MAPPED_SUBRESOURCE, RowPitch);
    CHECK_FIELD(mapped, D3D10DDI_MAPPED_SUBRESOURCE, DepthPitch);

    check_size("D3D10DDIARG_CREATERESOURCE", 64,
            (unsigned long)sizeof(create10));
    check_size("D3D11DDIARG_CREATERESOURCE", 80,
            (unsigned long)sizeof(create11));
    check_size("D3D10DDIARG_OPENRESOURCE", 40,
            (unsigned long)sizeof(open));
    check_size("D3D10DDI_MAPPED_SUBRESOURCE", 16,
            (unsigned long)sizeof(mapped));
}

static void check_view_arguments(void)
{
    D3D10DDI_HSHADERRESOURCEVIEW srv;
    D3D10DDI_HRTSHADERRESOURCEVIEW rt_srv;
    D3D10DDI_HRENDERTARGETVIEW rtv;
    D3D10DDI_HRTRENDERTARGETVIEW rt_rtv;
    D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW buffer_srv;
    D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW tex1d_srv;
    D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW tex2d_srv;
    D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW tex3d_srv;
    D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW texcube_srv;
    D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW bufferex_srv;
    D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW create_srv;
    D3D10DDIARG_BUFFER_RENDERTARGETVIEW buffer_rtv;
    D3D10DDIARG_TEX1D_RENDERTARGETVIEW tex1d_rtv;
    D3D10DDIARG_TEX2D_RENDERTARGETVIEW tex2d_rtv;
    D3D10DDIARG_TEX3D_RENDERTARGETVIEW tex3d_rtv;
    D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW texcube_rtv;
    D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW create_rtv;

    memset(&srv, 0, sizeof(srv));
    memset(&rt_srv, 0, sizeof(rt_srv));
    memset(&rtv, 0, sizeof(rtv));
    memset(&rt_rtv, 0, sizeof(rt_rtv));
    memset(&buffer_srv, 0, sizeof(buffer_srv));
    memset(&tex1d_srv, 0, sizeof(tex1d_srv));
    memset(&tex2d_srv, 0, sizeof(tex2d_srv));
    memset(&tex3d_srv, 0, sizeof(tex3d_srv));
    memset(&texcube_srv, 0, sizeof(texcube_srv));
    memset(&bufferex_srv, 0, sizeof(bufferex_srv));
    memset(&create_srv, 0, sizeof(create_srv));
    memset(&buffer_rtv, 0, sizeof(buffer_rtv));
    memset(&tex1d_rtv, 0, sizeof(tex1d_rtv));
    memset(&tex2d_rtv, 0, sizeof(tex2d_rtv));
    memset(&tex3d_rtv, 0, sizeof(tex3d_rtv));
    memset(&texcube_rtv, 0, sizeof(texcube_rtv));
    memset(&create_rtv, 0, sizeof(create_rtv));

    CHECK_FIELD(srv, D3D10DDI_HSHADERRESOURCEVIEW, pDrvPrivate);
    CHECK_FIELD(rt_srv, D3D10DDI_HRTSHADERRESOURCEVIEW, handle);
    CHECK_FIELD(rtv, D3D10DDI_HRENDERTARGETVIEW, pDrvPrivate);
    CHECK_FIELD(rt_rtv, D3D10DDI_HRTRENDERTARGETVIEW, handle);

    CHECK_FIELD(buffer_srv, D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW, FirstElement);
    CHECK_FIELD(buffer_srv, D3D10DDIARG_BUFFER_SHADERRESOURCEVIEW, NumElements);
    CHECK_FIELD(tex1d_srv, D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW, MostDetailedMip);
    CHECK_FIELD(tex1d_srv, D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW, FirstArraySlice);
    CHECK_FIELD(tex1d_srv, D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW, MipLevels);
    CHECK_FIELD(tex1d_srv, D3D10DDIARG_TEX1D_SHADERRESOURCEVIEW, ArraySize);
    CHECK_FIELD(tex2d_srv, D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, MostDetailedMip);
    CHECK_FIELD(tex2d_srv, D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, FirstArraySlice);
    CHECK_FIELD(tex2d_srv, D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, MipLevels);
    CHECK_FIELD(tex2d_srv, D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, ArraySize);
    CHECK_FIELD(tex2d_srv, D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, PlaneSlice);
    CHECK_FIELD(tex2d_srv, D3DWDDM2_0DDIARG_TEX2D_SHADERRESOURCEVIEW, PlaneIndex);
    CHECK_FIELD(tex3d_srv, D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW, MostDetailedMip);
    CHECK_FIELD(tex3d_srv, D3D10DDIARG_TEX3D_SHADERRESOURCEVIEW, MipLevels);
    CHECK_FIELD(texcube_srv, D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW, MostDetailedMip);
    CHECK_FIELD(texcube_srv, D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW, MipLevels);
    CHECK_FIELD(texcube_srv, D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW, First2DArrayFace);
    CHECK_FIELD(texcube_srv, D3D10_1DDIARG_TEXCUBE_SHADERRESOURCEVIEW, NumCubes);
    CHECK_FIELD(bufferex_srv, D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW, FirstElement);
    CHECK_FIELD(bufferex_srv, D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW, NumElements);
    CHECK_FIELD(bufferex_srv, D3D11DDIARG_BUFFEREX_SHADERRESOURCEVIEW, Flags);
    CHECK_FIELD(create_srv, D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, hDrvResource);
    CHECK_FIELD(create_srv, D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Format);
    CHECK_FIELD(create_srv, D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, ResourceDimension);
    CHECK_FIELD(create_srv, D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Buffer);
    CHECK_FIELD(create_srv, D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Tex1D);
    CHECK_FIELD(create_srv, D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Tex2D);
    CHECK_FIELD(create_srv, D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, Tex3D);
    CHECK_FIELD(create_srv, D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, TexCube);
    CHECK_FIELD(create_srv, D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW, BufferEx);

    CHECK_FIELD(buffer_rtv, D3D10DDIARG_BUFFER_RENDERTARGETVIEW, FirstElement);
    CHECK_FIELD(buffer_rtv, D3D10DDIARG_BUFFER_RENDERTARGETVIEW, NumElements);
    CHECK_FIELD(tex1d_rtv, D3D10DDIARG_TEX1D_RENDERTARGETVIEW, MipSlice);
    CHECK_FIELD(tex1d_rtv, D3D10DDIARG_TEX1D_RENDERTARGETVIEW, FirstArraySlice);
    CHECK_FIELD(tex1d_rtv, D3D10DDIARG_TEX1D_RENDERTARGETVIEW, ArraySize);
    CHECK_FIELD(tex2d_rtv, D3D10DDIARG_TEX2D_RENDERTARGETVIEW, MipSlice);
    CHECK_FIELD(tex2d_rtv, D3D10DDIARG_TEX2D_RENDERTARGETVIEW, FirstArraySlice);
    CHECK_FIELD(tex2d_rtv, D3D10DDIARG_TEX2D_RENDERTARGETVIEW, ArraySize);
    CHECK_FIELD(tex3d_rtv, D3D10DDIARG_TEX3D_RENDERTARGETVIEW, MipSlice);
    CHECK_FIELD(tex3d_rtv, D3D10DDIARG_TEX3D_RENDERTARGETVIEW, FirstW);
    CHECK_FIELD(tex3d_rtv, D3D10DDIARG_TEX3D_RENDERTARGETVIEW, WSize);
    CHECK_FIELD(texcube_rtv, D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW, MipSlice);
    CHECK_FIELD(texcube_rtv, D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW, FirstArraySlice);
    CHECK_FIELD(texcube_rtv, D3D10DDIARG_TEXCUBE_RENDERTARGETVIEW, ArraySize);
    CHECK_FIELD(create_rtv, D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, hDrvResource);
    CHECK_FIELD(create_rtv, D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Format);
    CHECK_FIELD(create_rtv, D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, ResourceDimension);
    CHECK_FIELD(create_rtv, D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Buffer);
    CHECK_FIELD(create_rtv, D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Tex1D);
    CHECK_FIELD(create_rtv, D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Tex2D);
    CHECK_FIELD(create_rtv, D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, Tex3D);
    CHECK_FIELD(create_rtv, D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, TexCube);
    CHECK_FIELD(create_rtv, D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW, WinePad0);

    check_size("D3D10DDI_HSHADERRESOURCEVIEW", 8, (unsigned long)sizeof(srv));
    check_size("D3D10DDI_HRTSHADERRESOURCEVIEW", 8, (unsigned long)sizeof(rt_srv));
    check_size("D3D10DDI_HRENDERTARGETVIEW", 8, (unsigned long)sizeof(rtv));
    check_size("D3D10DDI_HRTRENDERTARGETVIEW", 8, (unsigned long)sizeof(rt_rtv));
    check_size("D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW", 40,
            (unsigned long)sizeof(create_srv));
    check_size("D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW", 32,
            (unsigned long)sizeof(create_rtv));
}

static void check_shader_handles(void)
{
    D3D10DDI_HSHADER shader;
    D3D10DDI_HRTSHADER rt_shader;

    memset(&shader, 0, sizeof(shader));
    memset(&rt_shader, 0, sizeof(rt_shader));

    CHECK_FIELD(shader, D3D10DDI_HSHADER, pDrvPrivate);
    CHECK_FIELD(rt_shader, D3D10DDI_HRTSHADER, handle);

    check_size("D3D10DDI_HSHADER", 8, (unsigned long)sizeof(shader));
    check_size("D3D10DDI_HRTSHADER", 8, (unsigned long)sizeof(rt_shader));
}

/* The promoted command-list slots, filled and called through the table.
 * pfnDestroyCommandList and pfnRecycleDestroyCommandList share one type
 * outright, so what this checks beyond callability is that the documented
 * sharing still holds. */
static void check_promoted_device_funcs(D3DWDDM2_6DDI_DEVICEFUNCS *funcs)
{
    D3D10DDI_HDEVICE device;
    D3D11DDI_HCOMMANDLIST command_list;
    D3D11DDI_HRTCOMMANDLIST rt_command_list;
    D3D11DDIARG_CREATECOMMANDLIST create;
    D3D11DDI_HANDLESIZE handle_size;
    D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE calculate_deferred;
    D3D11DDIARG_CREATEDEFERREDCONTEXT create_deferred;
    D3D11DDIARG_CREATERESOURCE create_resource;
    D3D10DDIARG_OPENRESOURCE open_resource;
    D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW create_srv;
    D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW create_rtv;
    D3D10DDI_HSHADERRESOURCEVIEW srv;
    D3D10DDI_HRTSHADERRESOURCEVIEW rt_srv;
    D3D10DDI_HRENDERTARGETVIEW rtv;
    D3D10DDI_HRTRENDERTARGETVIEW rt_rtv;
    D3D10DDI_HSHADER shader;
    D3D10DDI_HRTSHADER rt_shader;
    D3D10DDI_HELEMENTLAYOUT element_layout;
    D3D10DDI_HRTELEMENTLAYOUT rt_element_layout;
    D3D10DDI_HRESOURCE vertex_buffer;
    D3D10DDI_HRENDERTARGETVIEW bound_rtvs[1];
    UINT strides[1] = {16};
    UINT offsets[1] = {0};
    FLOAT clear_colour[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    FLOAT blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    D3D10DDI_MAPPED_SUBRESOURCE mapped;
    UINT handle_count = 0;

    memset(&device, 0, sizeof(device));
    memset(&command_list, 0, sizeof(command_list));
    memset(&rt_command_list, 0, sizeof(rt_command_list));
    memset(&create, 0, sizeof(create));
    memset(&handle_size, 0, sizeof(handle_size));
    memset(&calculate_deferred, 0, sizeof(calculate_deferred));
    memset(&create_deferred, 0, sizeof(create_deferred));
    memset(&create_resource, 0, sizeof(create_resource));
    memset(&open_resource, 0, sizeof(open_resource));
    memset(&create_srv, 0, sizeof(create_srv));
    memset(&create_rtv, 0, sizeof(create_rtv));
    memset(&srv, 0, sizeof(srv));
    memset(&rt_srv, 0, sizeof(rt_srv));
    memset(&rtv, 0, sizeof(rtv));
    memset(&rt_rtv, 0, sizeof(rt_rtv));
    memset(&shader, 0, sizeof(shader));
    memset(&rt_shader, 0, sizeof(rt_shader));
    memset(&element_layout, 0, sizeof(element_layout));
    memset(&rt_element_layout, 0, sizeof(rt_element_layout));
    memset(&vertex_buffer, 0, sizeof(vertex_buffer));
    memset(bound_rtvs, 0, sizeof(bound_rtvs));
    memset(&mapped, 0, sizeof(mapped));
    create.hDeferredContext = device;

    funcs->pfnAbandonCommandList = stub_abandon_command_list;
    funcs->pfnCommandListExecute = stub_command_list_execute;
    funcs->pfnDestroyCommandList = stub_command_list_execute;
    funcs->pfnRecycleCommandList = stub_command_list_execute;
    funcs->pfnRecycleDestroyCommandList = stub_command_list_execute;
    funcs->pfnCalcPrivateCommandListSize = stub_calc_private_command_list_size;
    funcs->pfnCreateCommandList = stub_create_command_list;
    funcs->pfnRecycleCreateCommandList = stub_recycle_create_command_list;
    funcs->pfnCheckDeferredContextHandleSizes =
            stub_check_deferred_context_handle_sizes;
    funcs->pfnCalcDeferredContextHandleSize =
            stub_calc_deferred_context_handle_size;
    funcs->pfnCalcPrivateDeferredContextSize =
            stub_calc_private_deferred_context_size;
    funcs->pfnCreateDeferredContext = stub_create_deferred_context;
    funcs->pfnRecycleCreateDeferredContext =
            stub_recycle_create_deferred_context;
    funcs->pfnCalcPrivateResourceSize = stub_calc_private_resource_size;
    funcs->pfnCalcPrivateOpenedResourceSize =
            stub_calc_private_opened_resource_size;
    funcs->pfnCreateResource = stub_create_resource;
    funcs->pfnOpenResource = stub_open_resource;
    funcs->pfnDestroyResource = stub_destroy_resource;
    funcs->pfnCalcPrivateBlendStateSize = stub_calc_private_blend_state_size;
    funcs->pfnCreateBlendState = stub_create_blend_state;
    funcs->pfnDestroyBlendState = stub_destroy_blend_state;
    funcs->pfnCalcPrivateDepthStencilStateSize =
            stub_calc_private_depth_stencil_state_size;
    funcs->pfnCreateDepthStencilState = stub_create_depth_stencil_state;
    funcs->pfnDestroyDepthStencilState = stub_destroy_depth_stencil_state;
    funcs->pfnCalcPrivateRasterizerStateSize =
            stub_calc_private_rasterizer_state_size;
    funcs->pfnCreateRasterizerState = stub_create_rasterizer_state;
    funcs->pfnDestroyRasterizerState = stub_destroy_rasterizer_state;
    funcs->pfnCalcPrivateSamplerSize = stub_calc_private_sampler_size;
    funcs->pfnCreateSampler = stub_create_sampler;
    funcs->pfnDestroySampler = stub_destroy_sampler;
    funcs->pfnCalcPrivateShaderResourceViewSize =
            stub_calc_private_shader_resource_view_size;
    funcs->pfnCreateShaderResourceView = stub_create_shader_resource_view;
    funcs->pfnDestroyShaderResourceView = stub_destroy_shader_resource_view;
    funcs->pfnCalcPrivateRenderTargetViewSize =
            stub_calc_private_render_target_view_size;
    funcs->pfnCreateRenderTargetView = stub_create_render_target_view;
    funcs->pfnDestroyRenderTargetView = stub_destroy_render_target_view;
    funcs->pfnCalcPrivateShaderSize = stub_calc_private_shader_size;
    funcs->pfnCreateVertexShader = stub_create_vertex_shader;
    funcs->pfnCreatePixelShader = stub_create_pixel_shader;
    funcs->pfnDestroyShader = stub_destroy_shader;
    funcs->pfnCalcPrivateElementLayoutSize =
            stub_calc_private_element_layout_size;
    funcs->pfnCreateElementLayout = stub_create_element_layout;
    funcs->pfnDestroyElementLayout = stub_destroy_element_layout;
    funcs->pfnIaSetInputLayout = stub_ia_set_input_layout;
    funcs->pfnIaSetVertexBuffers = stub_ia_set_vertex_buffers;
    funcs->pfnIaSetTopology = stub_ia_set_topology;
    funcs->pfnVsSetShader = stub_set_shader;
    funcs->pfnPsSetShader = stub_set_shader;
    funcs->pfnGsSetShader = stub_set_shader;
    funcs->pfnHsSetShader = stub_set_shader;
    funcs->pfnDsSetShader = stub_set_shader;
    funcs->pfnCsSetShader = stub_set_shader;
    funcs->pfnSetRenderTargets = stub_set_render_targets;
    funcs->pfnSetViewports = stub_set_viewports;
    funcs->pfnClearRenderTargetView = stub_clear_render_target_view;
    funcs->pfnDraw = stub_draw;
    funcs->pfnSetBlendState = stub_set_blend_state;
    funcs->pfnSetDepthStencilState = stub_set_depth_stencil_state;
    funcs->pfnSetRasterizerState = stub_set_rasterizer_state;
    funcs->pfnResourceMap = stub_resource_map;
    funcs->pfnStagingResourceMap = stub_resource_map;
    funcs->pfnDynamicIABufferMapNoOverwrite = stub_resource_map;
    funcs->pfnDynamicIABufferMapDiscard = stub_resource_map;
    funcs->pfnDynamicConstantBufferMapDiscard = stub_resource_map;
    funcs->pfnDynamicConstantBufferMapNoOverwrite = stub_resource_map;
    funcs->pfnDynamicResourceMapDiscard = stub_resource_map;
    funcs->pfnResourceUnmap = stub_resource_unmap;
    funcs->pfnStagingResourceUnmap = stub_resource_unmap;
    funcs->pfnDynamicIABufferUnmap = stub_resource_unmap;
    funcs->pfnDynamicConstantBufferUnmap = stub_resource_unmap;
    funcs->pfnDynamicResourceUnmap = stub_resource_unmap;
    funcs->pfnResourceCopy = stub_resource_copy;

    command_list_calls = 0;
    CHECK_STACK("PFND3D11DDI_ABANDONCOMMANDLIST",
            funcs->pfnAbandonCommandList(device));
    CHECK_STACK("PFND3D11DDI_COMMANDLISTEXECUTE",
            funcs->pfnCommandListExecute(device, command_list));
    CHECK_STACK("PFND3D11DDI_DESTROYCOMMANDLIST",
            funcs->pfnDestroyCommandList(device, command_list));
    CHECK_STACK("PFND3D11DDI_RECYCLECOMMANDLIST",
            funcs->pfnRecycleCommandList(device, command_list));
    CHECK_STACK("PFND3D11DDI_DESTROYCOMMANDLIST (recycle)",
            funcs->pfnRecycleDestroyCommandList(device, command_list));
    CHECK_STACK("PFND3D11DDI_CALCPRIVATECOMMANDLISTSIZE",
            (void)funcs->pfnCalcPrivateCommandListSize(device, &create));
    CHECK_STACK("PFND3D11DDI_CREATECOMMANDLIST",
            funcs->pfnCreateCommandList(device, &create, command_list,
                    rt_command_list));
    CHECK_STACK("PFND3D11DDI_RECYCLECREATECOMMANDLIST",
            (void)funcs->pfnRecycleCreateCommandList(device, &create,
                    command_list, rt_command_list));
    CHECK_STACK("PFND3D11DDI_CHECKDEFERREDCONTEXTHANDLESIZES",
            funcs->pfnCheckDeferredContextHandleSizes(device, &handle_count,
                    &handle_size));
    CHECK_STACK("PFND3D11DDI_CALCDEFERREDCONTEXTHANDLESIZE",
            (void)funcs->pfnCalcDeferredContextHandleSize(device,
                    D3D10DDI_HT_RESOURCE, NULL));
    CHECK_STACK("PFND3D11DDI_CALCPRIVATEDEFERREDCONTEXTSIZE",
            (void)funcs->pfnCalcPrivateDeferredContextSize(device,
                    &calculate_deferred));
    CHECK_STACK("PFND3D11DDI_CREATEDEFERREDCONTEXT",
            funcs->pfnCreateDeferredContext(device, &create_deferred));
    CHECK_STACK("PFND3D11DDI_RECYCLECREATEDEFERREDCONTEXT",
            (void)funcs->pfnRecycleCreateDeferredContext(device,
                    &create_deferred));
    CHECK_STACK("PFND3D11DDI_CALCPRIVATERESOURCESIZE",
            (void)funcs->pfnCalcPrivateResourceSize(device, &create_resource));
    CHECK_STACK("PFND3D10DDI_CALCPRIVATEOPENEDRESOURCESIZE",
            (void)funcs->pfnCalcPrivateOpenedResourceSize(device,
                    &open_resource));
    CHECK_STACK("PFND3D11DDI_CREATERESOURCE",
            funcs->pfnCreateResource(device, &create_resource,
                    (D3D10DDI_HRESOURCE){0}, (D3D10DDI_HRTRESOURCE){0}));
    CHECK_STACK("PFND3D10DDI_OPENRESOURCE",
            funcs->pfnOpenResource(device, &open_resource,
                    (D3D10DDI_HRESOURCE){0}, (D3D10DDI_HRTRESOURCE){0}));
    CHECK_STACK("PFND3D10DDI_DESTROYRESOURCE",
            funcs->pfnDestroyResource(device, (D3D10DDI_HRESOURCE){0}));
    CHECK_STACK("PFND3D11_1DDI_CALCPRIVATEBLENDSTATESIZE",
            (void)funcs->pfnCalcPrivateBlendStateSize(device, NULL));
    CHECK_STACK("PFND3D11_1DDI_CREATEBLENDSTATE",
            funcs->pfnCreateBlendState(device, NULL,
                    (D3D10DDI_HBLENDSTATE){0},
                    (D3D10DDI_HRTBLENDSTATE){0}));
    CHECK_STACK("PFND3D10DDI_DESTROYBLENDSTATE",
            funcs->pfnDestroyBlendState(device, (D3D10DDI_HBLENDSTATE){0}));
    CHECK_STACK("PFND3D10DDI_CALCPRIVATEDEPTHSTENCILSTATESIZE",
            (void)funcs->pfnCalcPrivateDepthStencilStateSize(device, NULL));
    CHECK_STACK("PFND3D10DDI_CREATEDEPTHSTENCILSTATE",
            funcs->pfnCreateDepthStencilState(device, NULL,
                    (D3D10DDI_HDEPTHSTENCILSTATE){0},
                    (D3D10DDI_HRTDEPTHSTENCILSTATE){0}));
    CHECK_STACK("PFND3D10DDI_DESTROYDEPTHSTENCILSTATE",
            funcs->pfnDestroyDepthStencilState(device,
                    (D3D10DDI_HDEPTHSTENCILSTATE){0}));
    CHECK_STACK("PFND3DWDDM2_0DDI_CALCPRIVATERASTERIZERSTATESIZE",
            (void)funcs->pfnCalcPrivateRasterizerStateSize(device, NULL));
    CHECK_STACK("PFND3DWDDM2_0DDI_CREATERASTERIZERSTATE",
            funcs->pfnCreateRasterizerState(device, NULL,
                    (D3D10DDI_HRASTERIZERSTATE){0},
                    (D3D10DDI_HRTRASTERIZERSTATE){0}));
    CHECK_STACK("PFND3D10DDI_DESTROYRASTERIZERSTATE",
            funcs->pfnDestroyRasterizerState(device,
                    (D3D10DDI_HRASTERIZERSTATE){0}));
    CHECK_STACK("PFND3D10DDI_CALCPRIVATESAMPLERSIZE",
            (void)funcs->pfnCalcPrivateSamplerSize(device, NULL));
    CHECK_STACK("PFND3D10DDI_CREATESAMPLER",
            funcs->pfnCreateSampler(device, NULL, (D3D10DDI_HSAMPLER){0},
                    (D3D10DDI_HRTSAMPLER){0}));
    CHECK_STACK("PFND3D10DDI_DESTROYSAMPLER",
            funcs->pfnDestroySampler(device, (D3D10DDI_HSAMPLER){0}));
    CHECK_STACK("PFND3DWDDM2_0DDI_CALCPRIVATESHADERRESOURCEVIEWSIZE",
            (void)funcs->pfnCalcPrivateShaderResourceViewSize(device, &create_srv));
    CHECK_STACK("PFND3DWDDM2_0DDI_CREATESHADERRESOURCEVIEW",
            funcs->pfnCreateShaderResourceView(device, &create_srv, srv, rt_srv));
    CHECK_STACK("PFND3D10DDI_DESTROYSHADERRESOURCEVIEW",
            funcs->pfnDestroyShaderResourceView(device, srv));
    CHECK_STACK("PFND3DWDDM2_0DDI_CALCPRIVATERENDERTARGETVIEWSIZE",
            (void)funcs->pfnCalcPrivateRenderTargetViewSize(device, &create_rtv));
    CHECK_STACK("PFND3DWDDM2_0DDI_CREATERENDERTARGETVIEW",
            funcs->pfnCreateRenderTargetView(device, &create_rtv, rtv, rt_rtv));
    CHECK_STACK("PFND3D10DDI_DESTROYRENDERTARGETVIEW",
            funcs->pfnDestroyRenderTargetView(device, rtv));
    CHECK_STACK("PFND3D11_1DDI_CALCPRIVATESHADERSIZE",
            (void)funcs->pfnCalcPrivateShaderSize(device, NULL, NULL));
    CHECK_STACK("PFND3D11_1DDI_CREATEVERTEXSHADER",
            funcs->pfnCreateVertexShader(device, NULL, shader, rt_shader, NULL));
    CHECK_STACK("PFND3D11_1DDI_CREATEPIXELSHADER",
            funcs->pfnCreatePixelShader(device, NULL, shader, rt_shader, NULL));
    CHECK_STACK("PFND3D10DDI_DESTROYSHADER",
            funcs->pfnDestroyShader(device, shader));
    CHECK_STACK("PFND3D10DDI_CALCPRIVATEELEMENTLAYOUTSIZE",
            (void)funcs->pfnCalcPrivateElementLayoutSize(device, NULL));
    CHECK_STACK("PFND3D10DDI_CREATEELEMENTLAYOUT",
            funcs->pfnCreateElementLayout(device, NULL, element_layout,
                    rt_element_layout));
    CHECK_STACK("PFND3D10DDI_SETINPUTLAYOUT",
            funcs->pfnIaSetInputLayout(device, element_layout));
    CHECK_STACK("PFND3D10DDI_IA_SETVERTEXBUFFERS",
            funcs->pfnIaSetVertexBuffers(device, 0, 1, &vertex_buffer,
                    strides, offsets));
    CHECK_STACK("PFND3D10DDI_IA_SETTOPOLOGY",
            funcs->pfnIaSetTopology(device, 0));
    /* Six slots, one implementation: the shared typedef's whole claim. */
    CHECK_STACK("PFND3D10DDI_SETSHADER (vertex)",
            funcs->pfnVsSetShader(device, shader));
    CHECK_STACK("PFND3D10DDI_SETSHADER (pixel)",
            funcs->pfnPsSetShader(device, shader));
    CHECK_STACK("PFND3D10DDI_SETSHADER (geometry)",
            funcs->pfnGsSetShader(device, shader));
    CHECK_STACK("PFND3D10DDI_SETSHADER (hull)",
            funcs->pfnHsSetShader(device, shader));
    CHECK_STACK("PFND3D10DDI_SETSHADER (domain)",
            funcs->pfnDsSetShader(device, shader));
    CHECK_STACK("PFND3D10DDI_SETSHADER (compute)",
            funcs->pfnCsSetShader(device, shader));
    CHECK_STACK("PFND3D11DDI_SETRENDERTARGETS",
            funcs->pfnSetRenderTargets(device, bound_rtvs, 1, 0,
                    (D3D10DDI_HDEPTHSTENCILVIEW){0}, NULL, NULL, 1, 0, 1, 0));
    CHECK_STACK("PFND3D10DDI_SETVIEWPORTS",
            funcs->pfnSetViewports(device, 1, 0, NULL));
    CHECK_STACK("PFND3D10DDI_SETBLENDSTATE",
            funcs->pfnSetBlendState(device, (D3D10DDI_HBLENDSTATE){0},
                    blend_factor, 0xffffffffu));
    CHECK_STACK("PFND3D10DDI_SETDEPTHSTENCILSTATE",
            funcs->pfnSetDepthStencilState(device,
                    (D3D10DDI_HDEPTHSTENCILSTATE){0}, 0));
    CHECK_STACK("PFND3D10DDI_SETRASTERIZERSTATE",
            funcs->pfnSetRasterizerState(device,
                    (D3D10DDI_HRASTERIZERSTATE){0}));
    CHECK_STACK("PFND3D10DDI_CLEARRENDERTARGETVIEW",
            funcs->pfnClearRenderTargetView(device, rtv, clear_colour));
    CHECK_STACK("PFND3D10DDI_DRAW",
            funcs->pfnDraw(device, 3, 0));
    CHECK_STACK("PFND3D10DDI_DESTROYELEMENTLAYOUT",
            funcs->pfnDestroyElementLayout(device, element_layout));
    CHECK_STACK("PFND3D10DDI_RESOURCEMAP (resource)",
            funcs->pfnResourceMap(device, vertex_buffer, 0, 0, 0, &mapped));
    CHECK_STACK("PFND3D10DDI_RESOURCEMAP (staging)",
            funcs->pfnStagingResourceMap(device, vertex_buffer, 0, 0, 0, &mapped));
    CHECK_STACK("PFND3D10DDI_RESOURCEMAP (dynamic IA no-overwrite)",
            funcs->pfnDynamicIABufferMapNoOverwrite(device, vertex_buffer, 0,
                    0, 0, &mapped));
    CHECK_STACK("PFND3D10DDI_RESOURCEMAP (dynamic IA discard)",
            funcs->pfnDynamicIABufferMapDiscard(device, vertex_buffer, 0,
                    0, 0, &mapped));
    CHECK_STACK("PFND3D10DDI_RESOURCEMAP (dynamic constant discard)",
            funcs->pfnDynamicConstantBufferMapDiscard(device, vertex_buffer,
                    0, 0, 0, &mapped));
    CHECK_STACK("PFND3D10DDI_RESOURCEMAP (dynamic constant no-overwrite)",
            funcs->pfnDynamicConstantBufferMapNoOverwrite(device,
                    vertex_buffer, 0, 0, 0, &mapped));
    CHECK_STACK("PFND3D10DDI_RESOURCEMAP (dynamic resource discard)",
            funcs->pfnDynamicResourceMapDiscard(device, vertex_buffer, 0,
                    0, 0, &mapped));
    CHECK_STACK("PFND3D10DDI_RESOURCEUNMAP (resource)",
            funcs->pfnResourceUnmap(device, vertex_buffer, 0));
    CHECK_STACK("PFND3D10DDI_RESOURCEUNMAP (staging)",
            funcs->pfnStagingResourceUnmap(device, vertex_buffer, 0));
    CHECK_STACK("PFND3D10DDI_RESOURCEUNMAP (dynamic IA)",
            funcs->pfnDynamicIABufferUnmap(device, vertex_buffer, 0));
    CHECK_STACK("PFND3D10DDI_RESOURCEUNMAP (dynamic constant)",
            funcs->pfnDynamicConstantBufferUnmap(device, vertex_buffer, 0));
    CHECK_STACK("PFND3D10DDI_RESOURCEUNMAP (dynamic resource)",
            funcs->pfnDynamicResourceUnmap(device, vertex_buffer, 0));
    CHECK_STACK("PFND3D10DDI_RESOURCECOPY",
            funcs->pfnResourceCopy(device, vertex_buffer, vertex_buffer));

    if (command_list_calls == 72 && handle_count == 1)
    {
        printf("[ ok ] the promoted command-list, deferred-context, resource, state, view, shader, "
                "and binding/draw slots are callable as declared\n");
    }
    else
    {
        printf("[fail] %d of 72 promoted command/deferred/resource/state/view/shader/draw slots "
                "reached their implementation\n", command_list_calls);
        ++failures;
    }

    /* The specification says a driver may point pfnRecycleDestroyCommandList
     * at its own DestroyCommandList, so the two slots take one type.  One
     * implementation had to fit both above; this is the same claim made where
     * a divergence would show. */
    if (funcs->pfnDestroyCommandList == funcs->pfnRecycleDestroyCommandList)
    {
        printf("[ ok ] the destroy and recycle-destroy slots share a type\n");
    }
    else
    {
        printf("[fail] the destroy and recycle-destroy slots no longer accept "
                "one implementation\n");
        ++failures;
    }
}

static void check_device_funcs(void)
{
    D3DWDDM2_6DDI_DEVICEFUNCS funcs;

    memset(&funcs, 0, sizeof(funcs));

    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDefaultConstantBufferUpdateSubresourceUP);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnVsSetConstantBuffers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnPsSetShaderResources);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnPsSetShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnPsSetSamplers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnVsSetShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawIndexed);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDraw);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicIABufferMapNoOverwrite);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicIABufferUnmap);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicConstantBufferMapDiscard);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicIABufferMapDiscard);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicConstantBufferUnmap);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnPsSetConstantBuffers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnIaSetInputLayout);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnIaSetVertexBuffers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnIaSetIndexBuffer);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawIndexedInstanced);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawInstanced);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicResourceMapDiscard);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicResourceUnmap);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnGsSetConstantBuffers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnGsSetShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnIaSetTopology);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnStagingResourceMap);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnStagingResourceUnmap);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnVsSetShaderResources);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnVsSetSamplers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnGsSetShaderResources);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnGsSetSamplers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetRenderTargets);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnShaderResourceViewReadAfterWriteHazard);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceReadAfterWriteHazard);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetBlendState);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetDepthStencilState);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetRasterizerState);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnQueryEnd);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnQueryBegin);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceCopyRegion);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceUpdateSubresourceUP);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSoSetTargets);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawAuto);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetViewports);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetScissorRects);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnClearRenderTargetView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnClearDepthStencilView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetPredication);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnQueryGetData);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnFlush);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnGenMips);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceCopy);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceResolveSubresource);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceMap);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceUnmap);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceIsStagingBusy);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnRelocateDeviceFuncs);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateResourceSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateOpenedResourceSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateResource);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnOpenResource);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyResource);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateShaderResourceViewSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateShaderResourceView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyShaderResourceView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateRenderTargetViewSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateRenderTargetView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyRenderTargetView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateDepthStencilViewSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateDepthStencilView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyDepthStencilView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateElementLayoutSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateElementLayout);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyElementLayout);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateBlendStateSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateBlendState);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyBlendState);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateDepthStencilStateSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateDepthStencilState);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyDepthStencilState);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateRasterizerStateSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateRasterizerState);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyRasterizerState);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateShaderSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateVertexShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateGeometryShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreatePixelShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateGeometryShaderWithStreamOutput);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateGeometryShaderWithStreamOutput);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateSamplerSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateSampler);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroySampler);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateQuerySize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateQuery);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyQuery);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckFormatSupport);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckMultisampleQualityLevels);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckCounterInfo);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckCounter);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyDevice);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetTextFilterSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceConvert);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResourceConvertRegion);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResetPrimitiveID);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetVertexPipelineOutput);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawIndexedInstancedIndirect);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDrawInstancedIndirect);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCommandListExecute);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnHsSetShaderResources);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnHsSetShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnHsSetSamplers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnHsSetConstantBuffers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDsSetShaderResources);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDsSetShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDsSetSamplers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDsSetConstantBuffers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateHullShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateDomainShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckDeferredContextHandleSizes);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcDeferredContextHandleSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateDeferredContextSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateDeferredContext);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnAbandonCommandList);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateCommandListSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateCommandList);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyCommandList);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateTessellationShaderSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnPsSetShaderWithIfaces);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnVsSetShaderWithIfaces);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnGsSetShaderWithIfaces);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnHsSetShaderWithIfaces);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDsSetShaderWithIfaces);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetShaderWithIfaces);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateComputeShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetShader);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetShaderResources);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetSamplers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetConstantBuffers);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateUnorderedAccessViewSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateUnorderedAccessView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyUnorderedAccessView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnClearUnorderedAccessViewUint);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnClearUnorderedAccessViewFloat);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCsSetUnorderedAccessViews);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDispatch);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDispatchIndirect);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetResourceMinLOD);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCopyStructureCount);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnRecycleCommandList);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnRecycleCreateCommandList);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnRecycleCreateDeferredContext);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnRecycleDestroyCommandList);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDiscard);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnAssignDebugBinary);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDynamicConstantBufferMapNoOverwrite);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCheckDirectFlipSupport);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnClearView);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnUpdateTileMappings);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCopyTileMappings);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCopyTiles);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnUpdateTiles);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnTiledResourceBarrier);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnGetMipPacking);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnResizeTilePool);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetMarker);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetMarkerMode);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetHardwareProtection);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnGetResourceLayout);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnRetrieveShaderComment);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetHardwareProtectionState);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnAcquireResource);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnReleaseResource);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCalcPrivateShaderCacheSessionSize);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnCreateShaderCacheSession);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnDestroyShaderCacheSession);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnSetShaderCacheSession);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnQueryScanoutCaps);
    CHECK_FIELD(funcs, D3DWDDM2_6DDI_DEVICEFUNCS, pfnPrepareScanoutTransformation);
    check_size("D3DWDDM2_6DDI_DEVICEFUNCS", 1424,
            (unsigned long)sizeof(funcs));

    /* Last, and against the same object the offsets were just walked on:
     * promoting a slot must not have moved anything, and the size asserted
     * above is what says so. */
    check_promoted_device_funcs(&funcs);
}

int main(void)
{
    check_dirty_memory_padding();
    check_reference_probe();
    check_object_handles();
    check_adapter_funcs();
    check_open_adapter();
    check_device_handles();
    check_create_device();
    check_command_list_handle();
    check_deferred_context_arguments();
    check_resource_arguments();
    check_view_arguments();
    check_shader_handles();
    check_device_funcs();
    check_corelayer_callbacks();
    check_kernel_callbacks();
    check_version_arithmetic();

    if (failures)
    {
        printf("RESULT: %d DDI layout failure(s)\n", failures);
        return 1;
    }
    printf("RESULT: the DDI declarations agree with the compiled ABI\n");
    return 0;
}
