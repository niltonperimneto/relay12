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
static LONG tessellation_size_calls;
static LONG vertex_create_calls;
static LONG pixel_create_calls;
static LONG geometry_create_calls;
static LONG hull_create_calls;
static LONG domain_create_calls;
static LONG compute_create_calls;
static LONG shader_destroy_calls;
static LONG vertex_set_calls;
static LONG pixel_set_calls;
static LONG geometry_set_calls;
static LONG hull_set_calls;
static LONG domain_set_calls;
static LONG compute_set_calls;
/* Set if the host ever described a stream this mock cannot build.  Latched
 * rather than counted: one occurrence is already a failure. */
static LONG last_geometry_stream_output_requested;

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

/* GEOMETRY_SHADER_DESC, which the geometry stage takes instead.  The five
 * stream-output members are here so the mock can assert the host left them
 * zeroed: it always passes a null stream-output argument, and a host that
 * populated them would be describing a driver object nobody asked for. */
struct mock_geometry_shader_desc
{
    const BYTE *pFunction;
    UINT SizeInBytes;
    const void *pDeclaration;
    UINT NumElements;
    const UINT *pBufferStrides;
    UINT NumStrides;
    UINT RasterizedStream;
    void *pLinkage;
};

_Static_assert(sizeof(struct mock_geometry_shader_desc)
        == WINE_D3D11ON12_GEOMETRY_SHADER_DESC_SIZE,
        "the mock's GEOMETRY_SHADER_DESC must match the core's");
_Static_assert(offsetof(struct mock_geometry_shader_desc, pLinkage) == 48,
        "the mock's GEOMETRY_SHADER_DESC must match the core's");

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
    HRESULT (STDMETHODCALLTYPE *CreateGeometryShader)(
            struct mock_ddi_device *, D3D10DDI_HSHADER,
            const struct mock_geometry_shader_desc *, const void *);
    HRESULT (STDMETHODCALLTYPE *CreateHullShader)(struct mock_ddi_device *,
            D3D10DDI_HSHADER, const struct mock_shader_desc *);
    HRESULT (STDMETHODCALLTYPE *CreateDomainShader)(struct mock_ddi_device *,
            D3D10DDI_HSHADER, const struct mock_shader_desc *);
    HRESULT (STDMETHODCALLTYPE *CreateComputeShader)(struct mock_ddi_device *,
            D3D10DDI_HSHADER, const struct mock_shader_desc *);
};

_Static_assert(offsetof(struct mock_ddi_device_vtbl, CreatePixelShader)
        == WINE_D3D11ON12_DDIDEVICE_SLOT_CREATEPIXELSHADER * sizeof(void *),
        "the mock's shader slots must land where the core will call them");
_Static_assert(offsetof(struct mock_ddi_device_vtbl, CreateGeometryShader)
        == WINE_D3D11ON12_DDIDEVICE_SLOT_CREATEGEOMETRYSHADER * sizeof(void *),
        "the mock's shader slots must land where the core will call them");
_Static_assert(offsetof(struct mock_ddi_device_vtbl, CreateHullShader)
        == WINE_D3D11ON12_DDIDEVICE_SLOT_CREATEHULLSHADER * sizeof(void *),
        "the mock's shader slots must land where the core will call them");
_Static_assert(offsetof(struct mock_ddi_device_vtbl, CreateDomainShader)
        == WINE_D3D11ON12_DDIDEVICE_SLOT_CREATEDOMAINSHADER * sizeof(void *),
        "the mock's shader slots must land where the core will call them");
_Static_assert(offsetof(struct mock_ddi_device_vtbl, CreateComputeShader)
        == WINE_D3D11ON12_DDIDEVICE_SLOT_CREATECOMPUTESHADER * sizeof(void *),
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

/* The geometry stage, which takes its own descriptor and an optional
 * stream-output argument.
 *
 * Both halves are checked. A non-null stream-output argument would mean the
 * host asked for a StreamOutShader, which is a larger object than the private
 * block it sized -- so refusing it here is refusing a heap overflow. The five
 * stream-output descriptor members must likewise be zero, since nothing may
 * describe a stream this host cannot request. */
static HRESULT STDMETHODCALLTYPE mock_create_geometry_shader(
        struct mock_ddi_device *device, D3D10DDI_HSHADER shader,
        const struct mock_geometry_shader_desc *desc,
        const void *stream_output_args)
{
    struct mock_shader_desc plain;

    (void)device;
    InterlockedIncrement(&geometry_create_calls);
    if (!desc)
        return E_INVALIDARG;
    if (stream_output_args)
    {
        last_geometry_stream_output_requested = 1;
        return E_INVALIDARG;
    }
    if (desc->pDeclaration || desc->NumElements || desc->pBufferStrides
            || desc->NumStrides || desc->RasterizedStream)
    {
        last_geometry_stream_output_requested = 1;
        return E_INVALIDARG;
    }

    /* The three members the driver actually reads, in the shape the shared
     * recorder expects. */
    plain.pFunction = desc->pFunction;
    plain.SizeInBytes = desc->SizeInBytes;
    plain.pLinkage = desc->pLinkage;
    return mock_record_shader(shader, &plain, WINE_D3D11ON12_SHADER_GEOMETRY);
}

static HRESULT STDMETHODCALLTYPE mock_create_hull_shader(
        struct mock_ddi_device *device, D3D10DDI_HSHADER shader,
        const struct mock_shader_desc *desc)
{
    (void)device;
    InterlockedIncrement(&hull_create_calls);
    return mock_record_shader(shader, desc, WINE_D3D11ON12_SHADER_HULL);
}

static HRESULT STDMETHODCALLTYPE mock_create_domain_shader(
        struct mock_ddi_device *device, D3D10DDI_HSHADER shader,
        const struct mock_shader_desc *desc)
{
    (void)device;
    InterlockedIncrement(&domain_create_calls);
    return mock_record_shader(shader, desc, WINE_D3D11ON12_SHADER_DOMAIN);
}

static HRESULT STDMETHODCALLTYPE mock_create_compute_shader(
        struct mock_ddi_device *device, D3D10DDI_HSHADER shader,
        const struct mock_shader_desc *desc)
{
    (void)device;
    InterlockedIncrement(&compute_create_calls);
    return mock_record_shader(shader, desc, WINE_D3D11ON12_SHADER_COMPUTE);
}

static const struct mock_ddi_device_vtbl mock_ddi_device_vtable =
{
    .CreateVertexShader = mock_create_vertex_shader,
    .CreatePixelShader = mock_create_pixel_shader,
    .CreateGeometryShader = mock_create_geometry_shader,
    .CreateHullShader = mock_create_hull_shader,
    .CreateDomainShader = mock_create_domain_shader,
    .CreateComputeShader = mock_create_compute_shader,
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

/* The tessellation sizing slot, which hull and domain shaders must use.
 *
 * It returns the same size as the slot above, mirroring the pinned driver,
 * whose shader class has one layout for every pipeline stage because the only
 * stage-dependent member -- the pipeline-state cache key -- is never stored.
 * Counted separately so the test can prove the host asked the right slot per
 * stage rather than the convenient one. */
static SIZE_T mock_private_tessellation_shader_size(D3D10DDI_HDEVICE device,
        const UINT *code,
        const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *signatures)
{
    (void)device;
    if (code || signatures)
        return 0;
    InterlockedIncrement(&tessellation_size_calls);
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

static void mock_gs_set_shader(D3D10DDI_HDEVICE device,
        D3D10DDI_HSHADER shader)
{
    (void)device;
    (void)shader;
    InterlockedIncrement(&geometry_set_calls);
}

static void mock_hs_set_shader(D3D10DDI_HDEVICE device,
        D3D10DDI_HSHADER shader)
{
    (void)device;
    (void)shader;
    InterlockedIncrement(&hull_set_calls);
}

static void mock_ds_set_shader(D3D10DDI_HDEVICE device,
        D3D10DDI_HSHADER shader)
{
    (void)device;
    (void)shader;
    InterlockedIncrement(&domain_set_calls);
}

static void mock_cs_set_shader(D3D10DDI_HDEVICE device,
        D3D10DDI_HSHADER shader)
{
    (void)device;
    (void)shader;
    InterlockedIncrement(&compute_set_calls);
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
    args->pWDDM2_6DeviceFuncs->pfnCalcPrivateTessellationShaderSize =
            mock_private_tessellation_shader_size;
    args->pWDDM2_6DeviceFuncs->pfnDestroyShader = mock_destroy_shader;
    args->pWDDM2_6DeviceFuncs->pfnVsSetShader = mock_vs_set_shader;
    args->pWDDM2_6DeviceFuncs->pfnPsSetShader = mock_ps_set_shader;
    args->pWDDM2_6DeviceFuncs->pfnGsSetShader = mock_gs_set_shader;
    args->pWDDM2_6DeviceFuncs->pfnHsSetShader = mock_hs_set_shader;
    args->pWDDM2_6DeviceFuncs->pfnDsSetShader = mock_ds_set_shader;
    args->pWDDM2_6DeviceFuncs->pfnCsSetShader = mock_cs_set_shader;
    /* pfnCalcPrivateGeometryShaderWithStreamOutput stays null for the same
     * reason the table's create-shader slots do: the host must never reach
     * for it, because it sizes a stream-output object this host cannot ask
     * the driver to build. */

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
    LONG tessellationSizeCalls;
    LONG vertexCreateCalls;
    LONG pixelCreateCalls;
    LONG geometryCreateCalls;
    LONG hullCreateCalls;
    LONG domainCreateCalls;
    LONG computeCreateCalls;
    LONG destroyCalls;
    LONG vertexSetCalls;
    LONG pixelSetCalls;
    LONG geometrySetCalls;
    LONG hullSetCalls;
    LONG domainSetCalls;
    LONG computeSetCalls;
    LONG streamOutputRequested;
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
    report->tessellationSizeCalls = tessellation_size_calls;
    report->vertexCreateCalls = vertex_create_calls;
    report->pixelCreateCalls = pixel_create_calls;
    report->geometryCreateCalls = geometry_create_calls;
    report->hullCreateCalls = hull_create_calls;
    report->domainCreateCalls = domain_create_calls;
    report->computeCreateCalls = compute_create_calls;
    report->destroyCalls = shader_destroy_calls;
    report->vertexSetCalls = vertex_set_calls;
    report->pixelSetCalls = pixel_set_calls;
    report->geometrySetCalls = geometry_set_calls;
    report->hullSetCalls = hull_set_calls;
    report->domainSetCalls = domain_set_calls;
    report->computeSetCalls = compute_set_calls;
    report->streamOutputRequested = last_geometry_stream_output_requested;
    report->lastBytecode = last_bytecode;
    report->lastBytecodeSize = last_bytecode_size;
    report->lastLinkage = last_linkage;
    report->lastBoundVertexShader = last_bound_vertex_shader;
    report->lastBoundPixelShader = last_bound_pixel_shader;
    return S_OK;
}
