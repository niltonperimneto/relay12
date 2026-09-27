/* SPDX-License-Identifier: GPL-3.0-only
 * Minimal native test driver for the core's dynamic OpenAdapter boundary.
 */
#include <windows.h>
#include <dxgi.h>

#include "../relay12-d3d11/wine_d3d11on12_shader.h"

/* Storage the mock owns, so a readback can be compared byte for byte rather
 * than merely counted.  One image per resource and one image per view is all
 * the first frame needs: clear, draw a pixel, copy, map. */
struct mock_image
{
    const WineD3D11On12DDIResourceVtbl *lpVtbl;
    IUnknown *underlying;
    UINT width;
    UINT height;
    BYTE *pixels;
};

struct mock_rtv
{
    struct mock_image *image;
};

static LONG open_calls;
static LONG create_calls;
static LONG destroy_calls;
static LONG close_calls;
static LONG flush_calls;
static LONG draw_calls;
static LONG indexed_draw_calls;
static LONG instanced_draw_calls;
static LONG indexed_instanced_draw_calls;
static LONG topology_calls;
static INT last_topology;
static LONG resource_create_calls;
static LONG resource_destroy_calls;
static int bad_resource_description;
/* Resource creation is the most elaborate thing the core publishes -- a mip
 * array, an upload array, a subresource map table and two registry links --
 * so it is the one whose failure path is worth being able to drive. */
static volatile LONG fail_next_resource;
/* What the last Texture2D creation actually carried into the DDI.
 *
 * Recorded rather than asserted in place: the mip array is the part of
 * D3D11DDIARG_CREATERESOURCE the core builds itself, so the test has to see
 * the whole of it -- a stub that only checked the first entry would pass on
 * a core that got every later mip wrong. */
static LONG texture_create_calls;
static UINT last_texture_width;
static UINT last_texture_height;
static UINT last_texture_mip_levels;
static UINT last_texture_array_size;
static DXGI_FORMAT last_texture_format;
static UINT last_texture_bind_flags;
static int last_texture_had_initial_data;
static int bad_texture_mip_chain;
static LONG vertex_buffer_bind_calls;
static LONG index_buffer_bind_calls;
static void *last_vertex_buffer;
static void *last_index_buffer;
static UINT last_vertex_stride;
static UINT last_vertex_offset;
static UINT last_index_offset;
static DXGI_FORMAT last_index_format;
static LONG input_layout_create_calls;
static LONG input_layout_destroy_calls;
static LONG input_layout_bind_calls;
static void *last_input_layout;
static int bad_input_layout_description;
static volatile LONG fail_next_input_layout;
static LONG vertex_shader_create_calls;
static LONG pixel_shader_create_calls;
static LONG shader_destroy_calls;
static LONG vertex_shader_bind_calls;
static LONG pixel_shader_bind_calls;
static void *last_vertex_shader;
static void *last_pixel_shader;
static int bad_shader_description;
static volatile LONG fail_next_shader;
/* The frame path: views, render targets and staging readback.
 *
 * The counters go through Interlocked* like every other counter here, but the
 * bound view is a plain volatile store.  Only SetRenderTargets, Draw and
 * DestroyRenderTargetView touch it, and none of them is on the path the
 * threaded stress tests in d3d11on12openadapter.c hammer -- those bind input
 * layouts and shaders.  A test that does drive the frame path from several
 * threads has to revisit this. */
static struct mock_rtv *volatile bound_rtv;
static LONG render_target_view_create_calls;
static LONG render_target_view_destroy_calls;
static LONG map_calls;
static LONG unmap_calls;
static int bad_frame_description;
/* One injector per site rather than one shared flag.  Shared, whichever call
 * the core reached first would consume it, so a test aiming at the readback
 * path could silently end up testing view creation instead -- and would fail
 * by reporting success from the call it meant to break. */
static volatile LONG fail_next_render_target_view;
static volatile LONG fail_next_map;
/* const because the core-layer callback table belongs to the runtime and a
 * driver only reads it, which is how D3D10DDIARG_CREATEDEVICE declares it.
 * Dropping the qualifier here would let a mistake in the mock write through a
 * pointer the real runtime owns. */
static const D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS *runtime_callbacks;
static D3D10DDI_HRTCORELAYER runtime_device;
/* The immediate table, so a deferred context handed the same table -- which
 * would overwrite the immediate context's entries -- is caught. */
static D3DWDDM2_6DDI_DEVICEFUNCS *runtime_device_funcs;
static LONG amortized_processing_calls;
static unsigned char adapter_private;

/* Report a driver-side failure the way the DDI requires: through the
 * runtime's callback table, which does not exist until a device has been
 * created.  Returning whether the report landed lets a caller inject only
 * when the failure can actually be seen -- an injector armed before the
 * first CreateDevice becomes a no-op instead of a null dereference. */
static BOOL mock_report_error(HRESULT error)
{
    if (!runtime_callbacks || !runtime_callbacks->pfnSetErrorCb)
        return FALSE;
    runtime_callbacks->pfnSetErrorCb(runtime_device, error);
    return TRUE;
}

/* SIZE_T throughout: width and height are UINTs the core chose, and
 * width * height * 4 in UINT arithmetic would wrap into a short allocation
 * that every later access then runs past. */
static SIZE_T mock_image_bytes(const struct mock_image *image)
{
    return (SIZE_T)image->width * image->height * 4;
}

/* Every frame entry point resolves its handle through one of these two, so a
 * core that passes a handle it never created is recorded as a bad frame
 * description instead of corrupting the heap.  That is the whole point of
 * hand-writing a driver: a core bug has to arrive as a legible test failure,
 * not as a crash in memcpy. */
static struct mock_image *mock_image_of(void *private_resource)
{
    struct mock_image *image = private_resource;

    if (!image || !image->pixels || !image->width || !image->height)
    {
        bad_frame_description = 1;
        return NULL;
    }
    return image;
}

static struct mock_image *mock_image_of_view(void *private_view)
{
    struct mock_rtv *rtv = private_view;

    if (!rtv)
    {
        bad_frame_description = 1;
        return NULL;
    }
    return mock_image_of(rtv->image);
}

/* The DDI does not clamp a clear colour and neither does the core: the four
 * floats are the application's.  Casting one outside [0,1] -- or a NaN -- to
 * BYTE is undefined, so the clamp happens here, and the rounding is
 * round-half-up so a 0.5 clear reads back as 128 rather than 127. */
static BYTE mock_unorm8(FLOAT value)
{
    if (!(value > 0.0f))
        return 0;
    if (value >= 1.0f)
        return 255;
    return (BYTE)(value * 255.0f + 0.5f);
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

static BOOL mock_flush(D3D10DDI_HDEVICE device, UINT context_type,
        UINT flush_flags)
{
    (void)device;
    (void)context_type;
    (void)flush_flags;
    InterlockedIncrement(&flush_calls);
    if (runtime_callbacks && runtime_callbacks->pfnPerformAmortizedProcessingCb)
    {
        runtime_callbacks->pfnPerformAmortizedProcessingCb(runtime_device);
        InterlockedIncrement(&amortized_processing_calls);
    }
    return TRUE;
}

static void mock_draw(D3D10DDI_HDEVICE device, UINT vertex_count,
        UINT start_vertex_location)
{
    /* One opaque red pixel at the centre of whatever is bound.  Enough for a
     * readback to prove the draw reached the bound target and not some other
     * one, which is all a mock can honestly claim about a draw. */
    struct mock_rtv *target = bound_rtv;

    (void)device;
    (void)vertex_count;
    (void)start_vertex_location;
    if (target)
    {
        struct mock_image *image = mock_image_of_view(target);

        if (image)
        {
            BYTE *pixel = image->pixels
                    + ((SIZE_T)(image->height / 2) * image->width
                            + image->width / 2) * 4;

            pixel[0] = 255;
            pixel[1] = 0;
            pixel[2] = 0;
            pixel[3] = 255;
        }
    }
    InterlockedIncrement(&draw_calls);
}

static void mock_draw_indexed(D3D10DDI_HDEVICE device, UINT index_count,
        UINT start_index, INT base_vertex)
{
    (void)device; (void)index_count; (void)start_index; (void)base_vertex;
    InterlockedIncrement(&indexed_draw_calls);
}

static void mock_draw_instanced(D3D10DDI_HDEVICE device, UINT vertex_count,
        UINT instance_count, UINT start_vertex, UINT start_instance)
{
    (void)device; (void)vertex_count; (void)instance_count;
    (void)start_vertex; (void)start_instance;
    InterlockedIncrement(&instanced_draw_calls);
}

static void mock_draw_indexed_instanced(D3D10DDI_HDEVICE device,
        UINT index_count, UINT instance_count, UINT start_index,
        INT base_vertex, UINT start_instance)
{
    (void)device; (void)index_count; (void)instance_count; (void)start_index;
    (void)base_vertex; (void)start_instance;
    InterlockedIncrement(&indexed_instanced_draw_calls);
}

static void mock_ia_set_topology(D3D10DDI_HDEVICE device,
        D3D10_DDI_PRIMITIVE_TOPOLOGY topology)
{
    (void)device;
    last_topology = topology;
    InterlockedIncrement(&topology_calls);
}

static SIZE_T mock_calc_private_resource_size(D3D10DDI_HDEVICE device,
        const D3D11DDIARG_CREATERESOURCE *description)
{
    (void)device;
    if (!description || !description->pMipInfoList
            || (description->ResourceDimension != D3D10DDIRESOURCE_BUFFER
                    && description->ResourceDimension
                            != D3D10DDIRESOURCE_TEXTURE2D))
        bad_resource_description = 1;
    return sizeof(struct mock_image);
}

static void mock_create_resource(D3D10DDI_HDEVICE device,
        const D3D11DDIARG_CREATERESOURCE *description,
        D3D10DDI_HRESOURCE resource, D3D10DDI_HRTRESOURCE runtime_resource)
{
    struct mock_image *image;

    (void)device;
    if (!description || !description->pMipInfoList || !resource.pDrvPrivate
            || !runtime_resource.handle)
    {
        bad_resource_description = 1;
        InterlockedIncrement(&resource_create_calls);
        return;
    }

    /* Injected before anything is committed, and deliberately so: a driver
     * that fails creation owns whatever it already allocated, because the
     * runtime does not call pfnDestroyResource for a resource it was told
     * was never created.  Allocating first and then failing would leak the
     * pixels on every injected failure, and the soak would find it. */
    if (InterlockedExchange(&fail_next_resource, 0)
            && mock_report_error(E_OUTOFMEMORY))
    {
        InterlockedIncrement(&resource_create_calls);
        return;
    }

    /* Initialised for every dimension, not just the one that grows pixels.
     * mock_destroy_resource frees through this structure, so leaving a
     * buffer's copy untouched would make the mock depend on the runtime
     * having zeroed the private block -- true of this core, but not
     * something the DDI promises. */
    image = resource.pDrvPrivate;
    memset(image, 0, sizeof(*image));
    image->width = 0;
    image->height = 0;
    image->pixels = NULL;

    if (description->ResourceDimension == D3D10DDIRESOURCE_TEXTURE2D)
    {
        UINT array_slice, mip;

        image->width = description->pMipInfoList->TexelWidth;
        image->height = description->pMipInfoList->TexelHeight;
        image->pixels = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                mock_image_bytes(image));
        if (!image->pixels)
            bad_resource_description = 1;
        last_texture_width = description->pMipInfoList->TexelWidth;
        last_texture_height = description->pMipInfoList->TexelHeight;
        last_texture_mip_levels = description->MipLevels;
        last_texture_array_size = description->ArraySize;
        last_texture_format = description->Format;
        last_texture_bind_flags = description->BindFlags;
        last_texture_had_initial_data = description->pInitialDataUP != NULL;

        /* Every slice must carry a halving chain that clamps at one.  This
         * is the arithmetic the core does on its own, so it is the part a
         * mock is actually useful for checking. */
        for (array_slice = 0; array_slice < description->ArraySize; ++array_slice)
        {
            for (mip = 0; mip < description->MipLevels; ++mip)
            {
                const D3D10DDI_MIPINFO *info = &description->pMipInfoList[
                        array_slice * description->MipLevels + mip];
                UINT want_width = description->pMipInfoList->TexelWidth >> mip;
                UINT want_height = description->pMipInfoList->TexelHeight >> mip;

                if (!want_width) want_width = 1;
                if (!want_height) want_height = 1;
                if (info->TexelWidth != want_width
                        || info->TexelHeight != want_height
                        || info->TexelDepth != 1)
                    bad_texture_mip_chain = 1;
            }
        }
        InterlockedIncrement(&texture_create_calls);
    }
    else if (description->ResourceDimension != D3D10DDIRESOURCE_BUFFER
            || description->pMipInfoList->TexelWidth != 256
            || description->pMipInfoList->TexelHeight != 1
            || description->pMipInfoList->TexelDepth != 1)
    {
        bad_resource_description = 1;
    }
    InterlockedIncrement(&resource_create_calls);
}

static void mock_destroy_resource(D3D10DDI_HDEVICE device,
        D3D10DDI_HRESOURCE resource)
{
    struct mock_image *image = resource.pDrvPrivate;

    (void)device;
    if (!image)
        bad_resource_description = 1;
    else if (image->pixels)
    {
        HeapFree(GetProcessHeap(), 0, image->pixels);
        image->pixels = NULL;
    }
    if (image && image->underlying)
    {
        image->underlying->lpVtbl->Release(image->underlying);
        image->underlying = NULL;
    }
    InterlockedIncrement(&resource_destroy_calls);
}

static void mock_ia_set_vertex_buffers(D3D10DDI_HDEVICE device,
        UINT start_slot, UINT count, const D3D10DDI_HRESOURCE *buffers,
        const UINT *strides, const UINT *offsets)
{
    (void)device;
    if (start_slot != 0 || count != 1 || !buffers || !strides || !offsets)
        bad_resource_description = 1;
    else
    {
        last_vertex_buffer = buffers[0].pDrvPrivate;
        last_vertex_stride = strides[0];
        last_vertex_offset = offsets[0];
    }
    InterlockedIncrement(&vertex_buffer_bind_calls);
}

static void mock_ia_set_index_buffer(D3D10DDI_HDEVICE device,
        D3D10DDI_HRESOURCE buffer, DXGI_FORMAT format, UINT offset)
{
    (void)device;
    last_index_buffer = buffer.pDrvPrivate;
    last_index_format = format;
    last_index_offset = offset;
    InterlockedIncrement(&index_buffer_bind_calls);
}

static SIZE_T mock_calc_private_element_layout_size(D3D10DDI_HDEVICE device,
        const D3D10DDIARG_CREATEELEMENTLAYOUT *description)
{
    (void)device;
    if (!description || description->NumElements != 1
            || !description->pVertexElements)
        bad_input_layout_description = 1;
    return 32;
}

static void mock_create_element_layout(D3D10DDI_HDEVICE device,
        const D3D10DDIARG_CREATEELEMENTLAYOUT *description,
        D3D10DDI_HELEMENTLAYOUT layout,
        D3D10DDI_HRTELEMENTLAYOUT runtime_layout)
{
    (void)device;
    if (!description || description->NumElements != 1
            || !description->pVertexElements
            || description->pVertexElements[0].InputSlot != 0
            || description->pVertexElements[0].AlignedByteOffset != 0
            || description->pVertexElements[0].Format != DXGI_FORMAT_R32G32_FLOAT
            || description->pVertexElements[0].InputRegister != 3
            || !layout.pDrvPrivate || !runtime_layout.handle)
        bad_input_layout_description = 1;
    InterlockedIncrement(&input_layout_create_calls);
    if (InterlockedExchange(&fail_next_input_layout, 0))
        mock_report_error(E_OUTOFMEMORY);
}

static void mock_destroy_element_layout(D3D10DDI_HDEVICE device,
        D3D10DDI_HELEMENTLAYOUT layout)
{
    (void)device;
    if (!layout.pDrvPrivate)
        bad_input_layout_description = 1;
    InterlockedIncrement(&input_layout_destroy_calls);
}

static void mock_ia_set_input_layout(D3D10DDI_HDEVICE device,
        D3D10DDI_HELEMENTLAYOUT layout)
{
    (void)device;
    last_input_layout = layout.pDrvPrivate;
    InterlockedIncrement(&input_layout_bind_calls);
}

static SIZE_T mock_calc_private_shader_size(D3D10DDI_HDEVICE device,
        const UINT *code, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    (void)device;
    /* Both arguments must be null.  Sizing takes driver tokens and creation
     * takes a DXBC container, so there is nothing legitimate the core could
     * pass here; the pinned driver's own sizing callback ignores them and
     * returns a constant.  Asserting they are null is what stops the core
     * from drifting back to handing container bytes to a token reader. */
    if (code || signatures)
        bad_shader_description = 1;
    return 32;
}

/* Shared because the two stages must be held to the same contract: a mock
 * that checked the vertex path more carefully than the pixel path would let
 * exactly one stage's bug through. */
static void mock_check_shader_desc(D3D10DDI_HSHADER shader,
        const WineD3D11On12ShaderDesc *desc)
{
    UINT container_size;

    if (!desc || !desc->pFunction || !shader.pDrvPrivate || desc->pLinkage
            || desc->SizeInBytes < 32
            || memcmp(desc->pFunction, "DXBC", 4))
    {
        bad_shader_description = 1;
        return;
    }
    /* A DXBC container records its own length at offset 24.  Checking the
     * forwarded size against it pins what the core has to get right without
     * hardcoding the length of whichever blob a test happens to use. */
    memcpy(&container_size, desc->pFunction + 24, sizeof(container_size));
    if (desc->SizeInBytes != container_size)
        bad_shader_description = 1;
}

static HRESULT STDMETHODCALLTYPE mock_create_vertex_shader(
        WineD3D11On12DDIDevice *device, D3D10DDI_HSHADER shader,
        const WineD3D11On12ShaderDesc *desc)
{
    (void)device;
    mock_check_shader_desc(shader, desc);
    InterlockedIncrement(&vertex_shader_create_calls);
    return InterlockedExchange(&fail_next_shader, 0) ? E_OUTOFMEMORY : S_OK;
}

static HRESULT STDMETHODCALLTYPE mock_create_pixel_shader(
        WineD3D11On12DDIDevice *device, D3D10DDI_HSHADER shader,
        const WineD3D11On12ShaderDesc *desc)
{
    (void)device;
    mock_check_shader_desc(shader, desc);
    InterlockedIncrement(&pixel_shader_create_calls);
    return InterlockedExchange(&fail_next_shader, 0) ? E_OUTOFMEMORY : S_OK;
}

/* Local handles transfer a reference just like the real driver's map. */
static IUnknown *wrapped_handles[16];
static SRWLOCK wrapped_lock = SRWLOCK_INIT;
static LONG wrapped_open_count, wrapped_acquire_count, wrapped_release_count, wrapped_apply_count;
static LONG wrapped_last_input, wrapped_last_output;
static volatile LONG fail_next_wrap, fail_next_transition;

static UINT STDMETHODCALLTYPE mock_wrapped_data_size(WineD3D11On12DDIDevice *device)
{ (void)device; return sizeof(UINT); }
static HRESULT STDMETHODCALLTYPE mock_wrapping_handle(WineD3D11On12DDIDevice *device,
        IUnknown *resource, UINT reason, void *data, UINT size, UINT *out)
{
    UINT i;
    (void)device;
    if (!resource || reason != 1 || !data || size != sizeof(UINT)) return E_INVALIDARG;
    if (InterlockedExchange(&fail_next_wrap, 0)) return E_OUTOFMEMORY;
    AcquireSRWLockExclusive(&wrapped_lock);
    for (i = 0; i < 16; ++i)
        if (!wrapped_handles[i])
        {
            wrapped_handles[i] = resource;
            *(UINT *)data = 0xfeed1234;
            *out = i + 1;
            ReleaseSRWLockExclusive(&wrapped_lock);
            return S_OK;
        }
    ReleaseSRWLockExclusive(&wrapped_lock);
    return E_OUTOFMEMORY;
}
static void STDMETHODCALLTYPE mock_destroy_wrapping_handle(WineD3D11On12DDIDevice *device, UINT handle)
{
    (void)device;
    AcquireSRWLockExclusive(&wrapped_lock);
    if (handle && handle <= 16 && wrapped_handles[handle - 1])
    {
        wrapped_handles[handle - 1]->lpVtbl->Release(wrapped_handles[handle - 1]);
        wrapped_handles[handle - 1] = NULL;
    }
    ReleaseSRWLockExclusive(&wrapped_lock);
}
static void STDMETHODCALLTYPE mock_wrapped_set_state(WineD3D11On12DDIResource *resource,
        D3D12_RESOURCE_STATES state, UINT reason)
{
    (void)resource;
    InterlockedExchange(&wrapped_last_input, state);
    if (reason == 1)
    {
        InterlockedIncrement(&wrapped_acquire_count);
        if (InterlockedExchange(&fail_next_transition, 0)) mock_report_error(DXGI_ERROR_DEVICE_REMOVED);
    }
}
static const WineD3D11On12DDIResourceVtbl wrapped_resource_vtbl = {
    .SetGraphicsCurrentState = mock_wrapped_set_state,
};
static void STDMETHODCALLTYPE mock_wrapped_release(WineD3D11On12DDIDevice *device,
        WineD3D11On12DDIResource *resource, D3D12_RESOURCE_STATES state)
{
    (void)device; (void)resource;
    InterlockedExchange(&wrapped_last_output, state);
    InterlockedIncrement(&wrapped_release_count);
}
static void STDMETHODCALLTYPE mock_wrapped_apply(WineD3D11On12DDIDevice *device)
{ (void)device; InterlockedIncrement(&wrapped_apply_count); }
static SIZE_T mock_open_resource_size(D3D10DDI_HDEVICE device, const D3D10DDIARG_OPENRESOURCE *args)
{ (void)device; (void)args; return sizeof(struct mock_image); }
static void mock_open_resource(D3D10DDI_HDEVICE device, const D3D10DDIARG_OPENRESOURCE *args,
        D3D10DDI_HRESOURCE resource, D3D10DDI_HRTRESOURCE runtime_resource)
{
    struct mock_image *image = resource.pDrvPrivate;
    UINT handle = args->hKMResource.handle;
    (void)device; (void)runtime_resource;
    InterlockedIncrement(&wrapped_open_count);
    if (InterlockedExchange(&fail_next_resource, 0)) { mock_report_error(E_OUTOFMEMORY); return; }
    if (!handle || handle > 16 || !args->pPrivateDriverData
            || args->PrivateDriverDataSize != sizeof(UINT)
            || *(UINT *)args->pPrivateDriverData != 0xfeed1234)
    { mock_report_error(E_INVALIDARG); return; }
    memset(image, 0, sizeof(*image));
    image->lpVtbl = &wrapped_resource_vtbl;
    image->width = image->height = 64;
    image->pixels = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 64 * 64 * 4);
    if (!image->pixels) { mock_report_error(E_OUTOFMEMORY); return; }
    AcquireSRWLockExclusive(&wrapped_lock);
    image->underlying = wrapped_handles[handle - 1];
    wrapped_handles[handle - 1] = NULL;
    ReleaseSRWLockExclusive(&wrapped_lock);
}
__declspec(dllexport) void WINAPI WineD3D11On12MockDriverFailNextTransition(void)
{ InterlockedExchange(&fail_next_transition, 1); }
__declspec(dllexport) void WINAPI WineD3D11On12MockDriverFailNextWrap(void)
{ InterlockedExchange(&fail_next_wrap, 1); }
__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetWrappedCounts(LONG *counts)
{
    UINT i;
    counts[0] = wrapped_open_count;
    counts[1] = wrapped_acquire_count;
    counts[2] = wrapped_release_count;
    counts[3] = wrapped_apply_count;
    counts[4] = wrapped_last_input;
    counts[5] = wrapped_last_output;
    counts[6] = 0;
    AcquireSRWLockShared(&wrapped_lock);
    for (i = 0; i < 16; ++i) counts[6] += wrapped_handles[i] != NULL;
    ReleaseSRWLockShared(&wrapped_lock);
}

static const WineD3D11On12DDIDeviceVtbl shader_device_vtbl = {
    .GetResourcePrivateDataSize = mock_wrapped_data_size,
    .CreateWrappingHandle = mock_wrapping_handle,
    .DestroyKMTHandle = mock_destroy_wrapping_handle,
    .TransitionResourceForRelease = mock_wrapped_release,
    .ApplyAllResourceTransitions = mock_wrapped_apply,
    .CreateVertexShader = mock_create_vertex_shader,
    .CreatePixelShader = mock_create_pixel_shader,
};

static void mock_destroy_shader(D3D10DDI_HDEVICE device,
        D3D10DDI_HSHADER shader)
{
    (void)device;
    if (!shader.pDrvPrivate)
        bad_shader_description = 1;
    InterlockedIncrement(&shader_destroy_calls);
}

static void mock_vs_set_shader(D3D10DDI_HDEVICE device,
        D3D10DDI_HSHADER shader)
{
    (void)device;
    last_vertex_shader = shader.pDrvPrivate;
    InterlockedIncrement(&vertex_shader_bind_calls);
}

static void mock_ps_set_shader(D3D10DDI_HDEVICE device,
        D3D10DDI_HSHADER shader)
{
    (void)device;
    last_pixel_shader = shader.pDrvPrivate;
    InterlockedIncrement(&pixel_shader_bind_calls);
}

static SIZE_T mock_calc_private_render_target_view_size(D3D10DDI_HDEVICE device,
        const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *description)
{
    (void)device;
    if (!description)
        bad_frame_description = 1;
    return sizeof(struct mock_rtv);
}

static void mock_create_render_target_view(D3D10DDI_HDEVICE device,
        const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *description,
        D3D10DDI_HRENDERTARGETVIEW view, D3D10DDI_HRTRENDERTARGETVIEW runtime)
{
    struct mock_rtv *rtv = view.pDrvPrivate;

    (void)device;
    (void)runtime;
    /* Injected before the description is honoured but after it is checked:
     * a core that passes a malformed argument and a core that trips the
     * injector are different bugs and must not report the same way. */
    if (!description || !rtv || !description->hDrvResource.pDrvPrivate)
    {
        bad_frame_description = 1;
        return;
    }
    if (InterlockedExchange(&fail_next_render_target_view, 0)
            && mock_report_error(E_OUTOFMEMORY))
        return;
    rtv->image = description->hDrvResource.pDrvPrivate;
    InterlockedIncrement(&render_target_view_create_calls);
}

static void mock_destroy_render_target_view(D3D10DDI_HDEVICE device,
        D3D10DDI_HRENDERTARGETVIEW view)
{
    (void)device;
    if (!view.pDrvPrivate)
    {
        bad_frame_description = 1;
        return;
    }
    /* Unbind before the storage goes away.  A destroyed view left bound
     * would be read by the next draw. */
    if (bound_rtv == view.pDrvPrivate)
        bound_rtv = NULL;
    InterlockedIncrement(&render_target_view_destroy_calls);
}

static void mock_set_render_targets(D3D10DDI_HDEVICE device,
        const D3D10DDI_HRENDERTARGETVIEW *views, UINT count, UINT clear,
        D3D10DDI_HDEPTHSTENCILVIEW depth,
        const D3D11DDI_HUNORDEREDACCESSVIEW *uavs, const UINT *counts,
        UINT start, UINT num, UINT range_start, UINT range_size)
{
    (void)device;
    (void)clear;
    (void)depth;
    (void)uavs;
    (void)counts;
    (void)start;
    (void)num;
    (void)range_start;
    (void)range_size;
    if (count && (!views || !views[0].pDrvPrivate))
    {
        bad_frame_description = 1;
        return;
    }
    bound_rtv = count ? views[0].pDrvPrivate : NULL;
}

static void mock_set_viewports(D3D10DDI_HDEVICE device, UINT count,
        UINT clear, const D3D10_DDI_VIEWPORT *viewports)
{
    (void)device;
    (void)clear;
    if (count && !viewports)
        bad_frame_description = 1;
}

static void mock_clear_render_target_view(D3D10DDI_HDEVICE device,
        D3D10DDI_HRENDERTARGETVIEW view, FLOAT color[4])
{
    struct mock_image *image = mock_image_of_view(view.pDrvPrivate);
    BYTE rgba[4];
    SIZE_T offset, bytes;
    UINT channel;

    (void)device;
    if (!image || !color)
    {
        bad_frame_description = 1;
        return;
    }
    for (channel = 0; channel < 4; ++channel)
        rgba[channel] = mock_unorm8(color[channel]);
    bytes = mock_image_bytes(image);
    for (offset = 0; offset < bytes; offset += 4)
        memcpy(image->pixels + offset, rgba, sizeof(rgba));
}

static void mock_resource_copy(D3D10DDI_HDEVICE device,
        D3D10DDI_HRESOURCE destination, D3D10DDI_HRESOURCE source)
{
    struct mock_image *to = mock_image_of(destination.pDrvPrivate);
    struct mock_image *from = mock_image_of(source.pDrvPrivate);

    (void)device;
    if (!to || !from)
        return;
    /* The core checks the two descriptions agree before it gets here, so a
     * mismatch is a core bug.  Recording it beats copying the smaller of the
     * two and beats reading past the end of the source. */
    if (to->width != from->width || to->height != from->height)
    {
        bad_frame_description = 1;
        return;
    }
    memcpy(to->pixels, from->pixels, mock_image_bytes(to));
}

static void mock_staging_resource_map(D3D10DDI_HDEVICE device,
        D3D10DDI_HRESOURCE resource, UINT subresource, D3D10_DDI_MAP mode,
        UINT flags, D3D10DDI_MAPPED_SUBRESOURCE *out)
{
    struct mock_image *image = mock_image_of(resource.pDrvPrivate);

    (void)device;
    (void)mode;
    (void)flags;
    if (!image || !out)
    {
        bad_frame_description = 1;
        return;
    }
    /* One image per resource, so subresource 0 is the only one the mock can
     * serve -- and it must not pretend otherwise.  Returning mip 0 for every
     * index would hide a core that computed the wrong subresource, which is
     * arithmetic the core does on its own and the only reason to check. */
    if (subresource)
    {
        bad_frame_description = 1;
        return;
    }
    if (InterlockedExchange(&fail_next_map, 0)
            && mock_report_error(DXGI_ERROR_DEVICE_REMOVED))
        return;
    out->pData = image->pixels;
    out->RowPitch = image->width * 4;
    out->DepthPitch = (UINT)mock_image_bytes(image);
    InterlockedIncrement(&map_calls);
}

static void mock_staging_resource_unmap(D3D10DDI_HDEVICE device,
        D3D10DDI_HRESOURCE resource, UINT subresource)
{
    (void)device;
    if (!resource.pDrvPrivate || subresource)
    {
        bad_frame_description = 1;
        return;
    }
    InterlockedIncrement(&unmap_calls);
}

/* Deferred contexts and command lists, shaped like the pinned driver's.
 *
 * A context records into its private block; finishing a command list moves
 * what was recorded into the list and leaves the context empty, the way
 * D3D11On12's CommandList::RecycleCreate takes the context's batch; executing
 * adds the list's draws to what the immediate context has run.  Every call is
 * counted, and a context draw with vertex_count MOCK_DEFERRED_FAIL reports an
 * error through the context's own callback table, which is where the pinned
 * driver sends recording errors. */
#define MOCK_DEFERRED_FAIL 0xdeadu

struct mock_context
{
    LONG recorded_draws;
    const D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS *callbacks;
    D3D10DDI_HRTCORELAYER runtime;
};

struct mock_command_list
{
    LONG draws;
    LONG live;
};

static LONG deferred_context_create_calls;
static LONG deferred_context_destroy_calls;
static LONG deferred_abandon_calls;
static LONG command_list_create_calls;
static LONG command_list_recycle_create_calls;
static LONG command_list_destroy_calls;
static LONG command_list_recycle_destroy_calls;
static LONG command_list_execute_calls;
static LONG executed_draws;
static int bad_deferred_description;
static volatile LONG fail_next_deferred_context;
static volatile LONG fail_next_command_list;

static void mock_deferred_draw(D3D10DDI_HDEVICE context, UINT vertex_count,
        UINT start_vertex_location)
{
    struct mock_context *c = context.pDrvPrivate;

    (void)start_vertex_location;
    if (vertex_count == MOCK_DEFERRED_FAIL)
    {
        c->callbacks->pfnSetErrorCb(c->runtime, E_INVALIDARG);
        return;
    }
    InterlockedIncrement(&c->recorded_draws);
}

static void mock_destroy_deferred_context(D3D10DDI_HDEVICE context)
{
    (void)context;
    InterlockedIncrement(&deferred_context_destroy_calls);
}

static void mock_abandon_command_list(D3D10DDI_HDEVICE context)
{
    struct mock_context *c = context.pDrvPrivate;

    InterlockedExchange(&c->recorded_draws, 0);
    InterlockedIncrement(&deferred_abandon_calls);
}

static SIZE_T mock_calc_private_deferred_context_size(D3D10DDI_HDEVICE device,
        const D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE *args)
{
    (void)device;
    if (!args || args->Flags)
        bad_deferred_description = 1;
    return sizeof(struct mock_context);
}

static void mock_create_deferred_context(D3D10DDI_HDEVICE device,
        const D3D11DDIARG_CREATEDEFERREDCONTEXT *args)
{
    struct mock_context *c;

    (void)device;
    if (!args || !args->pWDDM2_6ContextFuncs || !args->hDrvContext.pDrvPrivate
            || !args->pWDDM2_6UMCallbacks
            || !args->pWDDM2_6UMCallbacks->pfnSetErrorCb
            || !args->pWDDM2_6UMCallbacks->pfnPerformAmortizedProcessingCb
            || args->pWDDM2_6ContextFuncs == runtime_device_funcs)
    {
        bad_deferred_description = 1;
        mock_report_error(E_INVALIDARG);
        return;
    }
    if (InterlockedExchange(&fail_next_deferred_context, 0)
            && mock_report_error(E_OUTOFMEMORY))
        return;
    c = args->hDrvContext.pDrvPrivate;
    c->recorded_draws = 0;
    c->callbacks = args->pWDDM2_6UMCallbacks;
    c->runtime = args->hRTCoreLayer;
    args->pWDDM2_6ContextFuncs->pfnDraw = mock_deferred_draw;
    args->pWDDM2_6ContextFuncs->pfnDestroyDevice = mock_destroy_deferred_context;
    args->pWDDM2_6ContextFuncs->pfnAbandonCommandList = mock_abandon_command_list;
    InterlockedIncrement(&deferred_context_create_calls);
}

static SIZE_T mock_calc_private_command_list_size(D3D10DDI_HDEVICE device,
        const D3D11DDIARG_CREATECOMMANDLIST *args)
{
    (void)device;
    if (!args || !args->hDeferredContext.pDrvPrivate)
        bad_deferred_description = 1;
    return sizeof(struct mock_command_list);
}

static HRESULT mock_fill_command_list(const D3D11DDIARG_CREATECOMMANDLIST *args,
        D3D11DDI_HCOMMANDLIST list, D3D11DDI_HRTCOMMANDLIST runtime)
{
    struct mock_command_list *l = list.pDrvPrivate;
    struct mock_context *c;

    if (!args || !args->hDeferredContext.pDrvPrivate || !l || !runtime.handle)
    {
        bad_deferred_description = 1;
        return E_INVALIDARG;
    }
    if (InterlockedExchange(&fail_next_command_list, 0))
        return E_OUTOFMEMORY;
    c = args->hDeferredContext.pDrvPrivate;
    l->draws = InterlockedExchange(&c->recorded_draws, 0);
    l->live = 1;
    return S_OK;
}

static void mock_create_command_list(D3D10DDI_HDEVICE device,
        const D3D11DDIARG_CREATECOMMANDLIST *args, D3D11DDI_HCOMMANDLIST list,
        D3D11DDI_HRTCOMMANDLIST runtime)
{
    HRESULT hr = mock_fill_command_list(args, list, runtime);

    (void)device;
    InterlockedIncrement(&command_list_create_calls);
    if (FAILED(hr))
        mock_report_error(hr);
}

static HRESULT mock_recycle_create_command_list(D3D10DDI_HDEVICE device,
        const D3D11DDIARG_CREATECOMMANDLIST *args, D3D11DDI_HCOMMANDLIST list,
        D3D11DDI_HRTCOMMANDLIST runtime)
{
    struct mock_command_list *l = list.pDrvPrivate;

    (void)device;
    /* Recycled memory has been through RecycleDestroyCommandList, which
     * leaves it not live; anything else is a host reusing a list in use. */
    if (l && l->live)
        bad_deferred_description = 1;
    InterlockedIncrement(&command_list_recycle_create_calls);
    return mock_fill_command_list(args, list, runtime);
}

static void mock_destroy_command_list(D3D10DDI_HDEVICE device,
        D3D11DDI_HCOMMANDLIST list)
{
    struct mock_command_list *l = list.pDrvPrivate;

    (void)device;
    if (!l || !l->live)
        bad_deferred_description = 1;
    else
        l->live = 0;
    InterlockedIncrement(&command_list_destroy_calls);
}

static void mock_recycle_destroy_command_list(D3D10DDI_HDEVICE device,
        D3D11DDI_HCOMMANDLIST list)
{
    InterlockedIncrement(&command_list_recycle_destroy_calls);
    mock_destroy_command_list(device, list);
    InterlockedDecrement(&command_list_destroy_calls);
}

static void mock_command_list_execute(D3D10DDI_HDEVICE device,
        D3D11DDI_HCOMMANDLIST list)
{
    struct mock_command_list *l = list.pDrvPrivate;

    (void)device;
    if (!l || !l->live)
    {
        bad_deferred_description = 1;
        return;
    }
    InterlockedAdd(&executed_draws, l->draws);
    InterlockedIncrement(&command_list_execute_calls);
}

static HRESULT mock_create_device(D3D10DDI_HADAPTER adapter,
        D3D10DDIARG_CREATEDEVICE *args)
{
    (void)adapter;
    if (!args || !args->pWDDM2_6DeviceFuncs)
        return E_INVALIDARG;
    /* The driver is entitled to call back through the whole core-layer table
     * without checking it, so a missing mandatory entry is rejected here
     * rather than dereferenced later.  scripts/check_core_callbacks.py makes
     * the same demand of the core by reading the source. */
    if (!args->pWDDM2_6UMCallbacks
            || !args->pWDDM2_6UMCallbacks->pfnSetErrorCb
            || !args->pWDDM2_6UMCallbacks->pfnPerformAmortizedProcessingCb)
        return E_POINTER;
    /* The vtable is written into the runtime's private device block below. */
    if (!args->hDrvDevice.pDrvPrivate)
        return E_INVALIDARG;
    if (!runtime_callbacks)
    {
        runtime_callbacks = args->pWDDM2_6UMCallbacks;
        runtime_device = args->hRTCoreLayer;
        runtime_device_funcs = args->pWDDM2_6DeviceFuncs;
    }
    args->pWDDM2_6DeviceFuncs->pfnDestroyDevice = mock_destroy_device;
    args->pWDDM2_6DeviceFuncs->pfnFlush = mock_flush;
    args->pWDDM2_6DeviceFuncs->pfnDraw = mock_draw;
    args->pWDDM2_6DeviceFuncs->pfnDrawIndexed = mock_draw_indexed;
    args->pWDDM2_6DeviceFuncs->pfnDrawInstanced = mock_draw_instanced;
    args->pWDDM2_6DeviceFuncs->pfnDrawIndexedInstanced =
            mock_draw_indexed_instanced;
    args->pWDDM2_6DeviceFuncs->pfnIaSetTopology = mock_ia_set_topology;
    args->pWDDM2_6DeviceFuncs->pfnIaSetVertexBuffers =
            mock_ia_set_vertex_buffers;
    args->pWDDM2_6DeviceFuncs->pfnIaSetIndexBuffer = mock_ia_set_index_buffer;
    args->pWDDM2_6DeviceFuncs->pfnCalcPrivateResourceSize =
            mock_calc_private_resource_size;
    args->pWDDM2_6DeviceFuncs->pfnCalcPrivateOpenedResourceSize = mock_open_resource_size;
    args->pWDDM2_6DeviceFuncs->pfnOpenResource = mock_open_resource;
    args->pWDDM2_6DeviceFuncs->pfnCreateResource = mock_create_resource;
    args->pWDDM2_6DeviceFuncs->pfnDestroyResource = mock_destroy_resource;
    args->pWDDM2_6DeviceFuncs->pfnCalcPrivateElementLayoutSize =
            mock_calc_private_element_layout_size;
    args->pWDDM2_6DeviceFuncs->pfnCreateElementLayout =
            mock_create_element_layout;
    args->pWDDM2_6DeviceFuncs->pfnDestroyElementLayout =
            mock_destroy_element_layout;
    args->pWDDM2_6DeviceFuncs->pfnIaSetInputLayout =
            mock_ia_set_input_layout;
    args->pWDDM2_6DeviceFuncs->pfnCalcPrivateShaderSize =
            mock_calc_private_shader_size;
    /* The immediate driver does not populate either shader creation slot. */
    ((WineD3D11On12DDIDevice *)args->hDrvDevice.pDrvPrivate)->lpVtbl =
            &shader_device_vtbl;
    args->pWDDM2_6DeviceFuncs->pfnDestroyShader = mock_destroy_shader;
    args->pWDDM2_6DeviceFuncs->pfnVsSetShader = mock_vs_set_shader;
    args->pWDDM2_6DeviceFuncs->pfnPsSetShader = mock_ps_set_shader;
    args->pWDDM2_6DeviceFuncs->pfnCalcPrivateRenderTargetViewSize =
            mock_calc_private_render_target_view_size;
    args->pWDDM2_6DeviceFuncs->pfnCreateRenderTargetView =
            mock_create_render_target_view;
    args->pWDDM2_6DeviceFuncs->pfnDestroyRenderTargetView =
            mock_destroy_render_target_view;
    args->pWDDM2_6DeviceFuncs->pfnSetRenderTargets = mock_set_render_targets;
    args->pWDDM2_6DeviceFuncs->pfnSetViewports = mock_set_viewports;
    args->pWDDM2_6DeviceFuncs->pfnClearRenderTargetView =
            mock_clear_render_target_view;
    args->pWDDM2_6DeviceFuncs->pfnResourceCopy = mock_resource_copy;
    args->pWDDM2_6DeviceFuncs->pfnStagingResourceMap =
            mock_staging_resource_map;
    args->pWDDM2_6DeviceFuncs->pfnStagingResourceUnmap =
            mock_staging_resource_unmap;
    args->pWDDM2_6DeviceFuncs->pfnCalcPrivateDeferredContextSize =
            mock_calc_private_deferred_context_size;
    args->pWDDM2_6DeviceFuncs->pfnCreateDeferredContext =
            mock_create_deferred_context;
    args->pWDDM2_6DeviceFuncs->pfnCalcPrivateCommandListSize =
            mock_calc_private_command_list_size;
    args->pWDDM2_6DeviceFuncs->pfnCreateCommandList = mock_create_command_list;
    args->pWDDM2_6DeviceFuncs->pfnRecycleCreateCommandList =
            mock_recycle_create_command_list;
    args->pWDDM2_6DeviceFuncs->pfnDestroyCommandList = mock_destroy_command_list;
    args->pWDDM2_6DeviceFuncs->pfnRecycleDestroyCommandList =
            mock_recycle_destroy_command_list;
    args->pWDDM2_6DeviceFuncs->pfnCommandListExecute = mock_command_list_execute;
    InterlockedIncrement(&create_calls);
    return S_OK;
}

__declspec(dllexport) LONG WINAPI WineD3D11On12MockDriverGetFlushCount(void)
{
    return flush_calls;
}

__declspec(dllexport) LONG WINAPI WineD3D11On12MockDriverGetAmortizedProcessingCount(void)
{
    return amortized_processing_calls;
}

__declspec(dllexport) LONG WINAPI WineD3D11On12MockDriverGetDrawCount(void)
{
    return draw_calls;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetExtendedDrawCounts(
        LONG *indexed, LONG *instanced, LONG *indexed_instanced)
{
    if (indexed) *indexed = indexed_draw_calls;
    if (instanced) *instanced = instanced_draw_calls;
    if (indexed_instanced) *indexed_instanced = indexed_instanced_draw_calls;
}

__declspec(dllexport) LONG WINAPI WineD3D11On12MockDriverGetTopology(
        INT *topology)
{
    if (topology)
        *topology = last_topology;
    return topology_calls;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetResourceCounts(
        LONG *created, LONG *destroyed, int *bad_description)
{
    if (created) *created = resource_create_calls;
    if (destroyed) *destroyed = resource_destroy_calls;
    if (bad_description) *bad_description = bad_resource_description;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetTexture2DRecord(
        LONG *calls, UINT *width, UINT *height, UINT *mip_levels,
        UINT *array_size, DXGI_FORMAT *format, UINT *bind_flags,
        int *had_initial_data, int *bad_mip_chain)
{
    if (calls) *calls = texture_create_calls;
    if (width) *width = last_texture_width;
    if (height) *height = last_texture_height;
    if (mip_levels) *mip_levels = last_texture_mip_levels;
    if (array_size) *array_size = last_texture_array_size;
    if (format) *format = last_texture_format;
    if (bind_flags) *bind_flags = last_texture_bind_flags;
    if (had_initial_data) *had_initial_data = last_texture_had_initial_data;
    if (bad_mip_chain) *bad_mip_chain = bad_texture_mip_chain;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetIABufferBindings(
        LONG *vertex_calls, LONG *index_calls, void **vertex_buffer,
        UINT *vertex_stride, UINT *vertex_offset, void **index_buffer,
        DXGI_FORMAT *index_format, UINT *index_offset)
{
    if (vertex_calls) *vertex_calls = vertex_buffer_bind_calls;
    if (index_calls) *index_calls = index_buffer_bind_calls;
    if (vertex_buffer) *vertex_buffer = last_vertex_buffer;
    if (vertex_stride) *vertex_stride = last_vertex_stride;
    if (vertex_offset) *vertex_offset = last_vertex_offset;
    if (index_buffer) *index_buffer = last_index_buffer;
    if (index_format) *index_format = last_index_format;
    if (index_offset) *index_offset = last_index_offset;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetInputLayoutCounts(
        LONG *created, LONG *destroyed, LONG *bound, void **last_bound,
        int *bad_description)
{
    if (created) *created = input_layout_create_calls;
    if (destroyed) *destroyed = input_layout_destroy_calls;
    if (bound) *bound = input_layout_bind_calls;
    if (last_bound) *last_bound = last_input_layout;
    if (bad_description) *bad_description = bad_input_layout_description;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverFailNextInputLayout(void)
{
    InterlockedExchange(&fail_next_input_layout, 1);
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverFailNextResource(void)
{
    InterlockedExchange(&fail_next_resource, 1);
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetShaderCounts(
        LONG *vertex_created, LONG *pixel_created, LONG *destroyed,
        LONG *vertex_bound, LONG *pixel_bound, void **last_vertex,
        void **last_pixel, int *bad_description)
{
    if (vertex_created) *vertex_created = vertex_shader_create_calls;
    if (pixel_created) *pixel_created = pixel_shader_create_calls;
    if (destroyed) *destroyed = shader_destroy_calls;
    if (vertex_bound) *vertex_bound = vertex_shader_bind_calls;
    if (pixel_bound) *pixel_bound = pixel_shader_bind_calls;
    if (last_vertex) *last_vertex = last_vertex_shader;
    if (last_pixel) *last_pixel = last_pixel_shader;
    if (bad_description) *bad_description = bad_shader_description;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverFailNextShader(void)
{
    InterlockedExchange(&fail_next_shader, 1);
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetFrameCounts(
        LONG *view_created, LONG *view_destroyed, LONG *mapped,
        LONG *unmapped, int *bad_description)
{
    if (view_created) *view_created = render_target_view_create_calls;
    if (view_destroyed) *view_destroyed = render_target_view_destroy_calls;
    if (mapped) *mapped = map_calls;
    if (unmapped) *unmapped = unmap_calls;
    if (bad_description) *bad_description = bad_frame_description;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverFailNextView(void)
{
    InterlockedExchange(&fail_next_render_target_view, 1);
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverFailNextMap(void)
{
    InterlockedExchange(&fail_next_map, 1);
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

/* Deferred contexts and command lists: lifecycle counts, then what ran. */
__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetDeferredCounts(
        LONG *contexts_created, LONG *contexts_destroyed, LONG *abandoned,
        LONG *lists_created, LONG *lists_recycled, LONG *lists_destroyed,
        LONG *lists_recycle_destroyed, int *bad_description)
{
    if (contexts_created) *contexts_created = deferred_context_create_calls;
    if (contexts_destroyed) *contexts_destroyed = deferred_context_destroy_calls;
    if (abandoned) *abandoned = deferred_abandon_calls;
    if (lists_created) *lists_created = command_list_create_calls;
    if (lists_recycled) *lists_recycled = command_list_recycle_create_calls;
    if (lists_destroyed) *lists_destroyed = command_list_destroy_calls;
    if (lists_recycle_destroyed) *lists_recycle_destroyed = command_list_recycle_destroy_calls;
    if (bad_description) *bad_description = bad_deferred_description;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverGetExecuteCounts(
        LONG *executes, LONG *draws)
{
    if (executes) *executes = command_list_execute_calls;
    if (draws) *draws = executed_draws;
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverFailNextDeferredContext(void)
{
    InterlockedExchange(&fail_next_deferred_context, 1);
}

__declspec(dllexport) void WINAPI WineD3D11On12MockDriverFailNextCommandList(void)
{
    InterlockedExchange(&fail_next_command_list, 1);
}
