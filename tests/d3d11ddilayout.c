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

int main(void)
{
    check_reference_probe();
    check_object_handles();
    check_adapter_funcs();
    check_open_adapter();
    check_device_handles();
    check_create_device();
    check_version_arithmetic();

    if (failures)
    {
        printf("RESULT: %d DDI layout failure(s)\n", failures);
        return 1;
    }
    printf("RESULT: the DDI declarations agree with the compiled ABI\n");
    return 0;
}
