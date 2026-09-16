/* SPDX-License-Identifier: GPL-3.0-only
 * Minimal native test driver for the core's dynamic OpenAdapter boundary.
 */
#include <windows.h>
#include <dxgi.h>

#include "../relay12-d3d11/ddi/wine_d3d11ddi.h"
#include "../relay12-d3d11/d3d11on12core.h"

static LONG open_calls;
static LONG create_calls;
static LONG destroy_calls;
static LONG close_calls;
static unsigned char adapter_private;

/* The shader half of the mock, which reproduces the one property of the
 * pinned driver the host's design turns on: the DDI function table's
 * pfnCreateVertexShader and pfnCreatePixelShader are never filled, and
 * creation is reachable only through the ID3D11On12DDIDevice sub-object whose
 * vtable pointer sits at the head of the private device block.
 *
 * Leaving those two table slots NULL here is deliberate. A mock that filled
 * them would let a host which called the table pass this suite and then fault
 * against the real driver. */
#define MOCK_SHADER_MAGIC 0x5ADE12u

static LONG shader_size_calls;
static LONG vertex_create_calls;
static LONG pixel_create_calls;
static LONG shader_destroy_calls;
static LONG vertex_set_calls;
static LONG pixel_set_calls;

/* What the driver was shown, so the test can assert the host passed the
 * caller's own container through rather than a copy of its own. */
static const void *last_bytecode;
static UINT last_bytecode_size;
static const void *last_linkage;
static void *last_bound_vertex_shader;
static void *last_bound_pixel_shader;

/* SHADER_DESC, at the layout d3d11on12core.h publishes and this file pins. */
struct mock_shader_desc
{
    const BYTE *pFunction;
    UINT SizeInBytes;
    void *pLinkage;
};

_Static_assert(sizeof(struct mock_shader_desc)
        == WINE_D3D11ON12_SHADER_DESC_SIZE,
        "the mock's SHADER_DESC must match the core's");
_Static_assert(offsetof(struct mock_shader_desc, SizeInBytes) == 8,
        "the mock's SHADER_DESC must match the core's");
_Static_assert(offsetof(struct mock_shader_desc, pLinkage) == 16,
        "the mock's SHADER_DESC must match the core's");

struct mock_ddi_device;

/* Only the two slots this mock implements are typed.
 *
 * The leading slots are an opaque array sized by the index the core
 * publishes, rather than a second transcription of the interface: one vtable
 * order in the tree is the whole point, and the core static-asserts that
 * index against its own declaration. */
struct mock_ddi_device_vtbl
{
    void *unimplemented[WINE_D3D11ON12_DDIDEVICE_SLOT_CREATEVERTEXSHADER];
    HRESULT (STDMETHODCALLTYPE *CreateVertexShader)(struct mock_ddi_device *,
            D3D10DDI_HSHADER, const struct mock_shader_desc *);
    HRESULT (STDMETHODCALLTYPE *CreatePixelShader)(struct mock_ddi_device *,
            D3D10DDI_HSHADER, const struct mock_shader_desc *);
};

_Static_assert(offsetof(struct mock_ddi_device_vtbl, CreatePixelShader)
        == WINE_D3D11ON12_DDIDEVICE_SLOT_CREATEPIXELSHADER * sizeof(void *),
        "the mock's shader slots must land where the core will call them");

struct mock_ddi_device
{
    const struct mock_ddi_device_vtbl *lpVtbl;
};

/* What the mock writes into a shader's private block, so the test can tell a
 * block the driver initialised from one the host merely allocated. */
struct mock_shader
{
    UINT magic;
    UINT stage;
};

static HRESULT mock_record_shader(D3D10DDI_HSHADER shader,
        const struct mock_shader_desc *desc, UINT stage)
{
    struct mock_shader *private_shader;

    if (!shader.pDrvPrivate || !desc)
        return E_INVALIDARG;
    /* A container with no bytes is not a shader, and the host must not have
     * reached this far with one. */
    if (!desc->pFunction || !desc->SizeInBytes)
        return E_INVALIDARG;

    last_bytecode = desc->pFunction;
    last_bytecode_size = desc->SizeInBytes;
    last_linkage = desc->pLinkage;

    private_shader = shader.pDrvPrivate;
    private_shader->magic = MOCK_SHADER_MAGIC;
    private_shader->stage = stage;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mock_create_vertex_shader(
        struct mock_ddi_device *device, D3D10DDI_HSHADER shader,
        const struct mock_shader_desc *desc)
{
    (void)device;
    InterlockedIncrement(&vertex_create_calls);
    return mock_record_shader(shader, desc, WINE_D3D11ON12_SHADER_VERTEX);
}

static HRESULT STDMETHODCALLTYPE mock_create_pixel_shader(
        struct mock_ddi_device *device, D3D10DDI_HSHADER shader,
        const struct mock_shader_desc *desc)
{
    (void)device;
    InterlockedIncrement(&pixel_create_calls);
    return mock_record_shader(shader, desc, WINE_D3D11ON12_SHADER_PIXEL);
}

static const struct mock_ddi_device_vtbl mock_ddi_device_vtable =
{
    .CreateVertexShader = mock_create_vertex_shader,
    .CreatePixelShader = mock_create_pixel_shader,
};

static SIZE_T mock_private_shader_size(D3D10DDI_HDEVICE device,
        const UINT *code, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    (void)device;
    /* Both null, and asserted rather than ignored: the host cannot offer
     * driver bytecode on a path whose creation call takes a container, and
     * the pinned driver's own sizing function reads neither argument. */
    if (code || signatures)
        return 0;
    InterlockedIncrement(&shader_size_calls);
    return sizeof(struct mock_shader);
}

static void mock_destroy_shader(D3D10DDI_HDEVICE device,
        D3D10DDI_HSHADER shader)
{
    const struct mock_shader *private_shader = shader.pDrvPrivate;

    (void)device;
    /* Destroying a block the mock never initialised would mean the host
     * handed back a shader whose creation failed. */
    if (!private_shader || private_shader->magic != MOCK_SHADER_MAGIC)
        return;
    InterlockedIncrement(&shader_destroy_calls);
}

static void mock_vs_set_shader(D3D10DDI_HDEVICE device,
        D3D10DDI_HSHADER shader)
{
    (void)device;
    InterlockedIncrement(&vertex_set_calls);
    last_bound_vertex_shader = shader.pDrvPrivate;
}

static void mock_ps_set_shader(D3D10DDI_HDEVICE device,
        D3D10DDI_HSHADER shader)
{
    (void)device;
    InterlockedIncrement(&pixel_set_calls);
    last_bound_pixel_shader = shader.pDrvPrivate;
}

static HRESULT mock_get_versions(D3D10DDI_HADAPTER adapter, UINT32 *count,
        UINT64 *versions)
{
    (void)adapter;
    if (!count)
        return E_INVALIDARG;
    *count = 1;
    if (versions)
        versions[0] = WINE_D3D11_DDI_SUPPORTED(
                WINE_D3D11_DDI_INTERFACE_VERSION(3), 1);
    return S_OK;
}

static SIZE_T mock_private_device_size(D3D10DDI_HADAPTER adapter,
        const D3D10DDIARG_CALCPRIVATEDEVICESIZE *args)
{
    (void)adapter;
    (void)args;
    return 64;
}

static void mock_destroy_device(D3D10DDI_HDEVICE device)
{
    (void)device;
    InterlockedIncrement(&destroy_calls);
}

static HRESULT mock_create_device(D3D10DDI_HADAPTER adapter,
        D3D10DDIARG_CREATEDEVICE *args)
{
    struct mock_ddi_device *ddi_device;

    (void)adapter;
    if (!args || !args->pWDDM2_6DeviceFuncs)
        return E_INVALIDARG;
    args->pWDDM2_6DeviceFuncs->pfnDestroyDevice = mock_destroy_device;
    /* The slots the pinned driver does fill for the immediate device.
     * pfnCreateVertexShader and pfnCreatePixelShader are conspicuously not
     * among them, for the reason given at the top of this file. */
    args->pWDDM2_6DeviceFuncs->pfnCalcPrivateShaderSize =
            mock_private_shader_size;
    args->pWDDM2_6DeviceFuncs->pfnDestroyShader = mock_destroy_shader;
    args->pWDDM2_6DeviceFuncs->pfnVsSetShader = mock_vs_set_shader;
    args->pWDDM2_6DeviceFuncs->pfnPsSetShader = mock_ps_set_shader;

    /* The sub-object vtable, published the way the driver publishes it: by
     * constructing an object at the head of the private device block the host
     * allocated and handed over as hDrvDevice. */
    if (!args->hDrvDevice.pDrvPrivate)
        return E_INVALIDARG;
    ddi_device = args->hDrvDevice.pDrvPrivate;
    ddi_device->lpVtbl = &mock_ddi_device_vtable;

    InterlockedIncrement(&create_calls);
    return S_OK;
}

static HRESULT mock_close_adapter(D3D10DDI_HADAPTER adapter)
{
    (void)adapter;
    InterlockedIncrement(&close_calls);
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI OpenAdapter_D3D11On12(
        D3D10DDIARG_OPENADAPTER *args, void *private_args)
{
    (void)private_args;
    if (!args || !args->pAdapterFuncs_2)
        return E_INVALIDARG;
    args->hAdapter.pDrvPrivate = &adapter_private;
    args->pAdapterFuncs_2->pfnGetSupportedVersions = mock_get_versions;
    args->pAdapterFuncs_2->pfnCalcPrivateDeviceSize =
            mock_private_device_size;
    args->pAdapterFuncs_2->pfnCreateDevice = mock_create_device;
    args->pAdapterFuncs_2->pfnCloseAdapter = mock_close_adapter;
    InterlockedIncrement(&open_calls);
    return S_OK;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetCounts(
        LONG *opened, LONG *created, LONG *destroyed, LONG *closed)
{
    if (opened)
        *opened = open_calls;
    if (created)
        *created = create_calls;
    if (destroyed)
        *destroyed = destroy_calls;
    if (closed)
        *closed = close_calls;
}

/* The shader counters, as one structure rather than a widening argument list:
 * the lifecycle export above is already at four out-parameters, and the
 * interesting assertions here are about which calls happened in what
 * proportion. */
struct WineD3D11On12MockShaderReport
{
    UINT size;
    LONG sizeCalls;
    LONG vertexCreateCalls;
    LONG pixelCreateCalls;
    LONG destroyCalls;
    LONG vertexSetCalls;
    LONG pixelSetCalls;
    const void *lastBytecode;
    UINT lastBytecodeSize;
    const void *lastLinkage;
    void *lastBoundVertexShader;
    void *lastBoundPixelShader;
};

__declspec(dllexport) HRESULT WINAPI WineD3D11On12MockDriverGetShaderReport(
        struct WineD3D11On12MockShaderReport *report)
{
    if (!report || report->size != sizeof(*report))
        return E_INVALIDARG;

    report->sizeCalls = shader_size_calls;
    report->vertexCreateCalls = vertex_create_calls;
    report->pixelCreateCalls = pixel_create_calls;
    report->destroyCalls = shader_destroy_calls;
    report->vertexSetCalls = vertex_set_calls;
    report->pixelSetCalls = pixel_set_calls;
    report->lastBytecode = last_bytecode;
    report->lastBytecodeSize = last_bytecode_size;
    report->lastLinkage = last_linkage;
    report->lastBoundVertexShader = last_bound_vertex_shader;
    report->lastBoundPixelShader = last_bound_pixel_shader;
    return S_OK;
}
