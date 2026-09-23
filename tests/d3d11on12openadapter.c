/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Fail-closed tests for the DDI adapter entry point.
 *
 * WineD3D11On12OpenAdapterV1 is the only path to the driver's device function
 * table, and the first code in this project that will hand a live D3D12 device
 * to the pinned D3D11On12 driver. Before that can be exercised against a real
 * D3D12 -- which needs a D3D12 implementation the Ubuntu lane does not have
 * yet -- these are the properties that can be established without one:
 *
 *   * every argument rejection happens before the driver is loaded, so a
 *     malformed call cannot reach the driver at all;
 *   * the out-structure's size word is checked, because it is the only thing
 *     standing between a caller built against a different core and a write
 *     past the end of its structure;
 *   * a device that implements ID3D12Device but not ID3D12Device1 is refused
 *     with the interface error, not with a generic failure. SOpenAdapterArgs
 *     takes ID3D12Device1, so this is a real deployment case rather than a
 *     hypothetical: the mock here is the same one the core validation tests
 *     use, and it answers only the base interface.
 *
 * What this deliberately does not claim: nothing here proves the driver can be
 * opened or a device created. That is the next milestone and it needs a real
 * D3D12 underneath. See docs/PORT-QUALITY-ROADMAP.md.
 */

#include <stdio.h>
#include <string.h>

#include "d3d11on12core.h"
#include "ddi/wine_d3d11ddi.h"
#include "d3d11on12mocks.h"

static int failures;

struct input_layout_stress
{
    WineD3D11On12AdapterDevice *adapter;
    WineD3D11On12InputLayout *layout;
    volatile LONG stop;
    volatile LONG unexpected;
};

struct shader_stress
{
    WineD3D11On12AdapterDevice *adapter;
    WineD3D11On12Shader *shader;
    volatile LONG stop;
    volatile LONG unexpected;
};

static DWORD WINAPI bind_input_layout_until_stopped(void *argument)
{
    struct input_layout_stress *stress = argument;
    while (!InterlockedCompareExchange(&stress->stop, 0, 0))
    {
        HRESULT hr = WineD3D11On12SetInputLayoutV1(stress->adapter,
                stress->layout);
        if (hr != S_OK && hr != E_INVALIDARG)
            InterlockedIncrement(&stress->unexpected);
    }
    return 0;
}

static DWORD WINAPI bind_vertex_shader_until_stopped(void *argument)
{
    struct shader_stress *stress = argument;
    while (!InterlockedCompareExchange(&stress->stop, 0, 0))
    {
        HRESULT hr = WineD3D11On12SetVertexShaderV1(stress->adapter,
                stress->shader);
        if (hr != S_OK && hr != E_INVALIDARG)
            InterlockedIncrement(&stress->unexpected);
    }
    return 0;
}

static void check(int condition, const char *what)
{
    if (condition)
    {
        printf("[ ok ] %s\n", what);
    }
    else
    {
        printf("[fail] %s\n", what);
        ++failures;
    }
}

static void initialize_out(WineD3D11On12AdapterDevice *out)
{
    memset(out, 0, sizeof(*out));
    out->size = sizeof(*out);
    /* Poisoned so that a successful call is visibly a write rather than a
     * structure that happened to start out looking right. */
    out->negotiatedInterfaceVersion = 0xdeadbeefu;
}

int main(void)
{
    typedef void (WINAPI *get_counts_fn)(LONG *, LONG *, LONG *, LONG *);
    typedef LONG (WINAPI *get_flush_count_fn)(void);
    typedef void (WINAPI *get_extended_draw_counts_fn)(LONG *, LONG *, LONG *);
    typedef LONG (WINAPI *get_topology_fn)(INT *);
    typedef void (WINAPI *get_resource_counts_fn)(LONG *, LONG *, int *);
    typedef void (WINAPI *get_ia_bindings_fn)(LONG *, LONG *, void **, UINT *,
            UINT *, void **, DXGI_FORMAT *, UINT *);
    typedef void (WINAPI *get_input_layout_counts_fn)(LONG *, LONG *, LONG *,
            void **, int *);
    typedef void (WINAPI *fail_next_input_layout_fn)(void);
    typedef void (WINAPI *get_shader_counts_fn)(LONG *, LONG *, LONG *, LONG *,
            LONG *, void **, void **, int *);
    typedef void (WINAPI *fail_next_shader_fn)(void);
    typedef void (WINAPI *fail_next_resource_fn)(void);
    typedef void (WINAPI *get_texture2d_record_fn)(LONG *, UINT *, UINT *,
            UINT *, UINT *, DXGI_FORMAT *, UINT *, int *, int *);
    WineD3D11On12AdapterDevice out;
    struct mock_device device;
    struct mock_queue queue;
    struct mock_device other_device;
    struct mock_queue other_queue;
    IUnknown *queue_objects[1];
    IUnknown *other_queue_objects[1];
    WineD3D11On12AdapterDevice other_out;
    HRESULT hr;
    HMODULE mock_driver;
    get_counts_fn get_counts;
    get_flush_count_fn get_flush_count;
    get_flush_count_fn get_draw_count;
    get_flush_count_fn get_amortized_count;
    get_extended_draw_counts_fn get_extended_draw_counts;
    get_topology_fn get_topology;
    get_resource_counts_fn get_resource_counts;
    get_ia_bindings_fn get_ia_bindings;
    get_input_layout_counts_fn get_input_layout_counts;
    fail_next_input_layout_fn fail_next_input_layout;
    get_shader_counts_fn get_shader_counts;
    fail_next_shader_fn fail_next_shader;
    fail_next_resource_fn fail_next_resource;
    get_texture2d_record_fn get_texture2d_record;
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_SUBRESOURCE_DATA texture_initial_data;
    WineD3D11On12Texture2D texture_handle;
    unsigned char texture_pixels[32 * 4];
    LONG texture_created;
    UINT texture_width, texture_height, texture_mip_levels;
    UINT texture_array_size, texture_bind_flags;
    DXGI_FORMAT texture_format;
    int texture_had_initial_data, bad_texture_mip_chain;
    LONG opened, created, destroyed, closed;
    LONG indexed, instanced, indexed_instanced;
    INT topology;
    LONG resource_created, resource_destroyed;
    int bad_resource_description;
    D3D11_BUFFER_DESC buffer_desc;
    WineD3D11On12Buffer buffer_handle;
    WineD3D11On12Buffer index_buffer_handle;
    WineD3D11On12Buffer *buffers[1];
    UINT stride = 24, offset = 8;
    LONG vertex_bind_calls, index_bind_calls;
    void *bound_vertex, *bound_index;
    UINT bound_stride, bound_vertex_offset, bound_index_offset;
    DXGI_FORMAT bound_index_format;
    D3D11_INPUT_ELEMENT_DESC input_element;
    WineD3D11On12InputLayout input_layout;
    UINT input_register = 3;
    LONG layout_created, layout_destroyed, layout_bound;
    void *bound_layout;
    int bad_layout_description;
    struct input_layout_stress layout_stress;
    HANDLE layout_threads[8];
    static const UINT shader_byte_code[8] = {
        0x43425844, 0, 0, 0, 0, 0, sizeof(shader_byte_code), 0,
    };
    WineD3D11On12Shader vertex_shader, pixel_shader;
    LONG vertex_shader_created, pixel_shader_created, shader_destroyed;
    LONG vertex_shader_bound, pixel_shader_bound;
    void *bound_vertex_shader, *bound_pixel_shader;
    int bad_shader_description;
    struct shader_stress shader_stress;
    HANDLE shader_threads[8];

    hr = WineD3D11On12CloseAdapterDeviceV1(NULL);
    check(hr == E_INVALIDARG, "close rejects a null out-structure");

    initialize_out(&out);
    out.size = sizeof(out) - 1;
    hr = WineD3D11On12CloseAdapterDeviceV1(&out);
    check(hr == E_INVALIDARG, "close rejects a mismatched structure size");

    initialize_out(&out);
    out.runtimeState = NULL;
    hr = WineD3D11On12CloseAdapterDeviceV1(&out);
    check(hr == S_OK, "closing an empty lifecycle is idempotent");
    check(out.negotiatedInterfaceVersion == 0 && out.deviceFuncs == NULL
            && out.hDrvAdapter == NULL && out.hDrvDevice == NULL
            && out.runtimeState == NULL,
          "closing an empty lifecycle clears every output");

    /* The shared initialisers, so this suite cannot disagree with the core
     * validation tests about what a well-formed mock looks like. */
    mock_device_init(&device);
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queue_objects[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;

    /* A null out-structure has nowhere to report anything, so it cannot be
     * anything but an argument error. */
    hr = WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface,
            queue_objects, 1, 0, NULL);
    check(hr == E_INVALIDARG, "a null out-structure is rejected");

    /* The size word is the version handshake. A caller compiled against a
     * different core must be refused rather than written past. */
    initialize_out(&out);
    out.size = sizeof(out) - 1;
    hr = WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface,
            queue_objects, 1, 0, &out);
    check(hr == E_INVALIDARG, "a mismatched out-structure size is rejected");
    check(out.negotiatedInterfaceVersion == 0xdeadbeefu,
          "a rejected size leaves the caller's structure untouched");

    initialize_out(&out);
    hr = WineD3D11On12OpenAdapterV1(NULL, queue_objects, 1, 0, &out);
    check(hr == E_INVALIDARG, "a null device object is rejected");

    initialize_out(&out);
    hr = WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface,
            NULL, 1, 0, &out);
    check(hr == E_INVALIDARG, "a null queue array is rejected");

    initialize_out(&out);
    hr = WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface,
            queue_objects, 2, 0, &out);
    check(hr == E_INVALIDARG, "more than one queue is rejected");

    /* Two bits set is not a node; the mask names one node or none. */
    initialize_out(&out);
    hr = WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface,
            queue_objects, 1, 0x3u, &out);
    check(hr == E_INVALIDARG, "a multi-bit node mask is rejected");

    /* The interesting one. This mock answers IID_ID3D12Device and refuses
     * IID_ID3D12Device1, which is what SOpenAdapterArgs requires. The call
     * must fail with the interface error, and must do so without having
     * touched the driver -- the outputs stay cleared. */
    initialize_out(&out);
    hr = WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface,
            queue_objects, 1, 0, &out);
    check(hr == E_NOINTERFACE,
          "a device without ID3D12Device1 is refused with E_NOINTERFACE");
    check(out.deviceFuncs == NULL && out.hDrvDevice == NULL
            && out.hDrvAdapter == NULL,
          "a refused call leaves no device function table behind");
    check(out.negotiatedInterfaceVersion == 0,
          "a refused call reports no negotiated version");

    /* The native mock driver exercises the successful dynamic boundary and
     * records the exact lifecycle order/count without requiring a GPU. */
    mock_driver = LoadLibraryW(L"d3d11on12.dll");
    get_counts = mock_driver ? (get_counts_fn)(void *)GetProcAddress(
            mock_driver, "WineD3D11On12MockDriverGetCounts") : NULL;
    get_flush_count = mock_driver ? (get_flush_count_fn)(void *)GetProcAddress(
            mock_driver, "WineD3D11On12MockDriverGetFlushCount") : NULL;
    get_draw_count = mock_driver ? (get_flush_count_fn)(void *)GetProcAddress(
            mock_driver, "WineD3D11On12MockDriverGetDrawCount") : NULL;
    get_amortized_count = mock_driver ? (get_flush_count_fn)(void *)GetProcAddress(
            mock_driver, "WineD3D11On12MockDriverGetAmortizedProcessingCount") : NULL;
    get_extended_draw_counts = mock_driver
            ? (get_extended_draw_counts_fn)(void *)GetProcAddress(mock_driver,
                    "WineD3D11On12MockDriverGetExtendedDrawCounts") : NULL;
    get_topology = mock_driver ? (get_topology_fn)(void *)GetProcAddress(
            mock_driver, "WineD3D11On12MockDriverGetTopology") : NULL;
    get_resource_counts = mock_driver
            ? (get_resource_counts_fn)(void *)GetProcAddress(mock_driver,
                    "WineD3D11On12MockDriverGetResourceCounts") : NULL;
    get_ia_bindings = mock_driver
            ? (get_ia_bindings_fn)(void *)GetProcAddress(mock_driver,
            "WineD3D11On12MockDriverGetIABufferBindings") : NULL;
    get_input_layout_counts = mock_driver
            ? (get_input_layout_counts_fn)(void *)GetProcAddress(mock_driver,
                    "WineD3D11On12MockDriverGetInputLayoutCounts") : NULL;
    fail_next_input_layout = mock_driver
            ? (fail_next_input_layout_fn)(void *)GetProcAddress(mock_driver,
                    "WineD3D11On12MockDriverFailNextInputLayout") : NULL;
    get_shader_counts = mock_driver
            ? (get_shader_counts_fn)(void *)GetProcAddress(mock_driver,
                    "WineD3D11On12MockDriverGetShaderCounts") : NULL;
    fail_next_shader = mock_driver
            ? (fail_next_shader_fn)(void *)GetProcAddress(mock_driver,
                    "WineD3D11On12MockDriverFailNextShader") : NULL;
    fail_next_resource = mock_driver
            ? (fail_next_resource_fn)(void *)GetProcAddress(mock_driver,
                    "WineD3D11On12MockDriverFailNextResource") : NULL;
    get_texture2d_record = mock_driver
            ? (get_texture2d_record_fn)(void *)GetProcAddress(mock_driver,
                    "WineD3D11On12MockDriverGetTexture2DRecord") : NULL;
    check(get_counts != NULL, "the lifecycle mock driver is loaded");
    check(get_flush_count != NULL, "the flush counter is exported");
    check(get_amortized_count != NULL,
          "the amortized processing counter is exported");
    check(get_draw_count != NULL, "the draw counter is exported");
    check(get_extended_draw_counts != NULL,
          "the extended draw counters are exported");
    check(get_topology != NULL, "the input-assembler topology counter is exported");
    check(get_resource_counts != NULL, "the resource counters are exported");
    check(get_ia_bindings != NULL, "the IA buffer binding recorder is exported");
    check(get_input_layout_counts != NULL,
          "the input-layout lifecycle recorder is exported");
    check(fail_next_input_layout != NULL,
          "the input-layout failure injector is exported");
    check(get_shader_counts != NULL, "the shader lifecycle recorder is exported");
    check(fail_next_shader != NULL, "the shader failure injector is exported");
    check(fail_next_resource != NULL,
          "the resource failure injector is exported");
    check(get_texture2d_record != NULL,
          "the Texture2D creation recorder is exported");
    if (get_counts && get_flush_count && get_amortized_count
            && get_draw_count && get_extended_draw_counts && get_topology
            && get_resource_counts && get_ia_bindings
            && get_input_layout_counts && fail_next_input_layout
            && get_shader_counts && fail_next_shader && fail_next_resource
            && get_texture2d_record)
    {
        device.support_device1 = 1;
        initialize_out(&out);
        hr = WineD3D11On12OpenAdapterV1(
                (IUnknown *)&device.ID3D12Device_iface,
                queue_objects, 1, 0, &out);
        check(hr == S_OK, "a complete driver lifecycle opens successfully");
        check(out.runtimeState != NULL && out.deviceFuncs != NULL
                && out.hDrvAdapter != NULL && out.hDrvDevice != NULL,
              "a successful open publishes owned DDI state");
        check(device.refcount == 2 && queue.refcount == 2,
              "the lifecycle retains its D3D12 device and queue");

        get_counts(&opened, &created, &destroyed, &closed);
        check(opened == 1 && created == 1 && destroyed == 0 && closed == 0,
              "creation invokes only the open and create callbacks");

        {
            BOOL submitted = FALSE;
            hr = WineD3D11On12FlushAdapterDeviceV1(&out, 0, 0, &submitted);
            check(hr == S_OK && submitted,
                  "flush dispatches through the live DDI device");
            check(get_flush_count() == 1,
                  "flush invokes the driver callback exactly once");
            check(get_amortized_count() == 1,
                  "flush invokes the runtime amortized processing callback exactly once");
        }
        hr = WineD3D11On12DrawAdapterDeviceV1(&out, 3, 0);
        check(hr == S_OK && get_draw_count() == 1,
              "draw dispatches through the live DDI device exactly once");
        hr = WineD3D11On12DispatchDrawV1(&out,
                WINE_D3D11ON12_DRAW_INDEXED, 6, 0, 2, -1, 0);
        check(hr == S_OK, "indexed draw dispatch succeeds");
        hr = WineD3D11On12DispatchDrawV1(&out,
                WINE_D3D11ON12_DRAW_INSTANCED, 3, 4, 1, 0, 2);
        check(hr == S_OK, "instanced draw dispatch succeeds");
        hr = WineD3D11On12DispatchDrawV1(&out,
                WINE_D3D11ON12_DRAW_INDEXED_INSTANCED, 6, 4, 2, -1, 2);
        check(hr == S_OK, "indexed instanced draw dispatch succeeds");
        get_extended_draw_counts(&indexed, &instanced, &indexed_instanced);
        check(indexed == 1 && instanced == 1 && indexed_instanced == 1,
              "each extended draw reaches its exact DDI callback once");
        hr = WineD3D11On12SetPrimitiveTopologyV1(&out, 4);
        check(hr == S_OK && get_topology(&topology) == 1 && topology == 4,
              "primitive topology reaches the exact IA DDI callback once");

        memset(&buffer_desc, 0, sizeof(buffer_desc));
        buffer_desc.ByteWidth = 256;
        buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        memset(&buffer_handle, 0, sizeof(buffer_handle));
        buffer_handle.size = sizeof(buffer_handle);
        hr = WineD3D11On12CreateBufferV1(&out, &buffer_desc, NULL,
                &buffer_handle);
        check(hr == S_OK && buffer_handle.hDrvResource
                && buffer_handle.runtimeState,
              "buffer creation publishes an owned DDI resource handle");
        get_resource_counts(&resource_created, &resource_destroyed,
                &bad_resource_description);
        check(resource_created == 1 && resource_destroyed == 0
                && !bad_resource_description,
              "buffer creation reaches the exact DDI descriptor once");
        buffers[0] = &buffer_handle;
        hr = WineD3D11On12SetVertexBuffersV1(&out, 0, 1, buffers,
                &stride, &offset);
        check(hr == S_OK, "a live vertex buffer binds through the IA DDI");
        get_ia_bindings(&vertex_bind_calls, &index_bind_calls, &bound_vertex,
                &bound_stride, &bound_vertex_offset, &bound_index,
                &bound_index_format, &bound_index_offset);
        check(vertex_bind_calls == 1 && bound_vertex == buffer_handle.hDrvResource
                && bound_stride == stride && bound_vertex_offset == offset,
              "vertex binding preserves handle, stride, and offset");
        hr = WineD3D11On12SetIndexBufferV1(&out, &buffer_handle,
                DXGI_FORMAT_R16_UINT, 0);
        check(hr == E_INVALIDARG,
              "a vertex-only buffer cannot be rebound as an index buffer");
        hr = WineD3D11On12DestroyBufferV1(&buffer_handle);
        check(hr == S_OK && !buffer_handle.hDrvResource
                && !buffer_handle.runtimeState,
              "buffer destruction clears the public handle");
        get_resource_counts(&resource_created, &resource_destroyed,
                &bad_resource_description);
        check(resource_created == 1 && resource_destroyed == 1,
              "buffer destruction reaches the DDI callback once");
        hr = WineD3D11On12SetVertexBuffersV1(&out, 0, 1, buffers,
                &stride, &offset);
        check(hr == E_INVALIDARG,
              "a destroyed vertex buffer is rejected before DDI dispatch");

        /* Partial publication.  The core links a resource into two registries
         * and allocates three side tables before it calls the driver, and
         * only writes the caller's handle once the driver has agreed.  A
         * failure therefore has to leave nothing behind in either registry
         * and nothing published in the handle -- and the proof that the
         * registries really were restored is that the *same* handle can then
         * be created successfully and destroyed again. */
        memset(&buffer_handle, 0, sizeof(buffer_handle));
        buffer_handle.size = sizeof(buffer_handle);
        fail_next_resource();
        hr = WineD3D11On12CreateBufferV1(&out, &buffer_desc, NULL,
                &buffer_handle);
        check(hr == E_OUTOFMEMORY && !buffer_handle.hDrvResource
                && !buffer_handle.runtimeState && !buffer_handle.reserved,
              "a DDI creation error publishes no buffer handle");
        check(buffer_handle.size == sizeof(buffer_handle),
              "a rejected buffer creation leaves the handle reusable");
        get_resource_counts(&resource_created, &resource_destroyed,
                &bad_resource_description);
        check(resource_created == 2 && resource_destroyed == 1
                && !bad_resource_description,
              "a rejected resource is reclaimed without a destroy callback");
        hr = WineD3D11On12SetVertexBuffersV1(&out, 0, 1, buffers,
                &stride, &offset);
        check(hr == E_INVALIDARG,
              "an unpublished buffer cannot be bound");
        hr = WineD3D11On12DestroyBufferV1(&buffer_handle);
        check(hr == S_OK, "destroying an unpublished buffer is inert");
        get_resource_counts(&resource_created, &resource_destroyed,
                &bad_resource_description);
        check(resource_created == 2 && resource_destroyed == 1,
              "destroying an unpublished buffer reaches no DDI callback");
        hr = WineD3D11On12CreateBufferV1(&out, &buffer_desc, NULL,
                &buffer_handle);
        check(hr == S_OK && buffer_handle.hDrvResource
                && buffer_handle.runtimeState,
              "the same handle creates successfully after a rejected attempt");
        hr = WineD3D11On12DestroyBufferV1(&buffer_handle);
        check(hr == S_OK, "the recovered buffer destroys cleanly");
        get_resource_counts(&resource_created, &resource_destroyed,
                &bad_resource_description);
        check(resource_created == 3 && resource_destroyed == 2
                && !bad_resource_description,
              "failure and recovery leave the resource counts balanced");

        memset(&buffer_desc, 0, sizeof(buffer_desc));
        buffer_desc.ByteWidth = 256;
        buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        buffer_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        memset(&index_buffer_handle, 0, sizeof(index_buffer_handle));
        index_buffer_handle.size = sizeof(index_buffer_handle);
        hr = WineD3D11On12CreateBufferV1(&out, &buffer_desc, NULL,
                &index_buffer_handle);
        check(hr == S_OK, "an index buffer receives an owned DDI handle");
        hr = WineD3D11On12SetIndexBufferV1(&out, &index_buffer_handle,
                DXGI_FORMAT_R16_UINT, 12);
        check(hr == S_OK, "a live index buffer binds through the IA DDI");
        get_ia_bindings(&vertex_bind_calls, &index_bind_calls, &bound_vertex,
                &bound_stride, &bound_vertex_offset, &bound_index,
                &bound_index_format, &bound_index_offset);
        check(index_bind_calls == 1
                && bound_index == index_buffer_handle.hDrvResource
                && bound_index_format == DXGI_FORMAT_R16_UINT
                && bound_index_offset == 12,
              "index binding preserves handle, format, and offset");

        memset(&input_element, 0, sizeof(input_element));
        input_element.SemanticName = "POSITION";
        input_element.Format = DXGI_FORMAT_R32G32_FLOAT;
        input_element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        memset(&input_layout, 0, sizeof(input_layout));
        input_layout.size = sizeof(input_layout);
        hr = WineD3D11On12CreateInputLayoutV1(&out, &input_element,
                &input_register, 1, &input_layout);
        check(hr == S_OK && input_layout.hDrvElementLayout
                && input_layout.runtimeState,
              "input-layout creation publishes an owned DDI handle");
        hr = WineD3D11On12SetInputLayoutV1(&out, &input_layout);
        check(hr == S_OK, "a live input layout binds through the IA DDI");
        get_input_layout_counts(&layout_created, &layout_destroyed,
                &layout_bound, &bound_layout, &bad_layout_description);
        check(layout_created == 1 && layout_destroyed == 0
                && layout_bound == 1
                && bound_layout == input_layout.hDrvElementLayout
                && !bad_layout_description,
              "input-layout creation and binding preserve the DDI descriptor");
        mock_device_init(&other_device);
        mock_queue_init(&other_queue, &other_device,
                D3D12_COMMAND_LIST_TYPE_DIRECT);
        other_device.support_device1 = 1;
        other_queue_objects[0] =
                (IUnknown *)&other_queue.ID3D12CommandQueue_iface;
        initialize_out(&other_out);
        hr = WineD3D11On12OpenAdapterV1(
                (IUnknown *)&other_device.ID3D12Device_iface,
                other_queue_objects, 1, 0, &other_out);
        check(hr == S_OK, "a second adapter/device lifecycle opens");
        hr = WineD3D11On12SetInputLayoutV1(&other_out, &input_layout);
        check(hr == E_INVALIDARG,
              "an input layout cannot bind to a foreign device");
        hr = WineD3D11On12CloseAdapterDeviceV1(&other_out);
        check(hr == S_OK && other_device.refcount == 1
                && other_queue.refcount == 1,
              "the second lifecycle closes without taking layout ownership");
        hr = WineD3D11On12SetInputLayoutV1(&out, NULL);
        check(hr == S_OK,
              "a null input layout explicitly unbinds the IA layout");
        get_input_layout_counts(&layout_created, &layout_destroyed,
                &layout_bound, &bound_layout, &bad_layout_description);
        check(layout_bound == 2 && !bound_layout,
              "input-layout unbinding reaches the DDI with a null handle");
        hr = WineD3D11On12DestroyInputLayoutV1(&input_layout);
        check(hr == S_OK && !input_layout.hDrvElementLayout
                && !input_layout.runtimeState,
              "input-layout destruction clears the public handle");
        hr = WineD3D11On12SetInputLayoutV1(&out, &input_layout);
        check(hr == E_INVALIDARG,
              "a destroyed input layout is rejected before DDI dispatch");
        get_input_layout_counts(&layout_created, &layout_destroyed,
                &layout_bound, &bound_layout, &bad_layout_description);
        check(layout_destroyed == 1 && layout_bound == 2,
              "input-layout destruction reaches the DDI exactly once");
        input_layout.size = sizeof(input_layout);
        fail_next_input_layout();
        hr = WineD3D11On12CreateInputLayoutV1(&out, &input_element,
                &input_register, 1, &input_layout);
        check(hr == E_OUTOFMEMORY && !input_layout.hDrvElementLayout
                && !input_layout.runtimeState,
              "a DDI creation error leaves no published input-layout handle");
        get_input_layout_counts(&layout_created, &layout_destroyed,
                &layout_bound, &bound_layout, &bad_layout_description);
        check(layout_created == 2 && layout_destroyed == 1,
              "a rejected DDI handle is reclaimed without a destroy callback");
        check(input_layout.size == sizeof(input_layout),
              "a rejected input-layout creation leaves the handle reusable");
        hr = WineD3D11On12DestroyInputLayoutV1(&input_layout);
        check(hr == S_OK, "destroying an unpublished input layout is inert");
        get_input_layout_counts(&layout_created, &layout_destroyed,
                &layout_bound, &bound_layout, &bad_layout_description);
        check(layout_created == 2 && layout_destroyed == 1,
              "destroying an unpublished layout reaches no DDI callback");

        input_layout.size = sizeof(input_layout);
        hr = WineD3D11On12CreateInputLayoutV1(&out, &input_element,
                &input_register, 1, &input_layout);
        check(hr == S_OK, "an input layout is created for lifetime stress");
        memset(&layout_stress, 0, sizeof(layout_stress));
        layout_stress.adapter = &out;
        layout_stress.layout = &input_layout;
        for (UINT i = 0; i < ARRAYSIZE(layout_threads); ++i)
            layout_threads[i] = CreateThread(NULL, 0,
                    bind_input_layout_until_stopped, &layout_stress, 0, NULL);
        Sleep(10);
        hr = WineD3D11On12DestroyInputLayoutV1(&input_layout);
        Sleep(10);
        InterlockedExchange(&layout_stress.stop, 1);
        WaitForMultipleObjects(ARRAYSIZE(layout_threads), layout_threads,
                TRUE, 5000);
        for (UINT i = 0; i < ARRAYSIZE(layout_threads); ++i)
            if (layout_threads[i]) CloseHandle(layout_threads[i]);
        check(hr == S_OK && !layout_stress.unexpected,
              "concurrent input-layout binding and destruction stays bounded");

        check(!out.deviceFuncs->pfnCreateVertexShader
                && !out.deviceFuncs->pfnCreatePixelShader,
              "the immediate driver leaves shader creation table slots null");
        memset(&vertex_shader, 0, sizeof(vertex_shader));
        vertex_shader.size = sizeof(vertex_shader);
        hr = WineD3D11On12CreateVertexShaderV1(&out, shader_byte_code,
                sizeof(shader_byte_code), &vertex_shader);
        check(hr == S_OK && vertex_shader.hDrvShader
                && vertex_shader.runtimeState
                && vertex_shader.stage == WINE_D3D11ON12_SHADER_VERTEX,
              "vertex-shader creation publishes a typed owned DDI handle");
        memset(&pixel_shader, 0, sizeof(pixel_shader));
        pixel_shader.size = sizeof(pixel_shader);
        hr = WineD3D11On12CreatePixelShaderV1(&out, shader_byte_code,
                sizeof(shader_byte_code), &pixel_shader);
        check(hr == S_OK && pixel_shader.hDrvShader
                && pixel_shader.runtimeState
                && pixel_shader.stage == WINE_D3D11ON12_SHADER_PIXEL,
              "pixel-shader creation publishes a typed owned DDI handle");
        hr = WineD3D11On12SetVertexShaderV1(&out, &vertex_shader);
        check(hr == S_OK, "a live vertex shader binds through the VS DDI");
        hr = WineD3D11On12SetPixelShaderV1(&out, &pixel_shader);
        check(hr == S_OK, "a live pixel shader binds through the PS DDI");
        hr = WineD3D11On12SetVertexShaderV1(&out, &pixel_shader);
        check(hr == E_INVALIDARG,
              "a pixel shader cannot bind to the vertex stage");
        initialize_out(&other_out);
        hr = WineD3D11On12OpenAdapterV1(
                (IUnknown *)&other_device.ID3D12Device_iface,
                other_queue_objects, 1, 0, &other_out);
        check(hr == S_OK, "a second lifecycle reopens for shader isolation");
        hr = WineD3D11On12SetVertexShaderV1(&other_out, &vertex_shader);
        check(hr == E_INVALIDARG,
              "a shader cannot bind to a foreign device");
        hr = WineD3D11On12CloseAdapterDeviceV1(&other_out);
        check(hr == S_OK, "the shader-isolation lifecycle closes cleanly");
        hr = WineD3D11On12SetVertexShaderV1(&out, NULL);
        check(hr == S_OK, "a null vertex shader explicitly unbinds the stage");
        get_shader_counts(&vertex_shader_created, &pixel_shader_created,
                &shader_destroyed, &vertex_shader_bound, &pixel_shader_bound,
                &bound_vertex_shader, &bound_pixel_shader,
                &bad_shader_description);
        check(vertex_shader_created == 1 && pixel_shader_created == 1
                && shader_destroyed == 0 && vertex_shader_bound == 2
                && pixel_shader_bound == 1 && !bound_vertex_shader
                && bound_pixel_shader == pixel_shader.hDrvShader
                && !bad_shader_description,
              "shader lifecycle preserves stage, handle, and null unbinding");
        hr = WineD3D11On12DestroyShaderV1(&pixel_shader);
        check(hr == S_OK && !pixel_shader.hDrvShader
                && !pixel_shader.runtimeState && !pixel_shader.stage,
              "shader destruction clears the public handle");
        hr = WineD3D11On12SetPixelShaderV1(&out, &pixel_shader);
        check(hr == E_INVALIDARG,
              "a destroyed shader is rejected before DDI dispatch");

        memset(&pixel_shader, 0, sizeof(pixel_shader));
        pixel_shader.size = sizeof(pixel_shader);
        fail_next_shader();
        hr = WineD3D11On12CreatePixelShaderV1(&out, shader_byte_code,
                sizeof(shader_byte_code), &pixel_shader);
        check(hr == E_OUTOFMEMORY && !pixel_shader.hDrvShader
                && !pixel_shader.runtimeState && !pixel_shader.stage,
              "a DDI creation error publishes no shader handle");
        check(pixel_shader.size == sizeof(pixel_shader),
              "a rejected shader creation leaves the handle reusable");
        hr = WineD3D11On12DestroyShaderV1(&pixel_shader);
        check(hr == S_OK, "destroying an unpublished shader is inert");
        hr = WineD3D11On12CreatePixelShaderV1(&out, shader_byte_code,
                sizeof(shader_byte_code), &pixel_shader);
        check(hr == S_OK && pixel_shader.hDrvShader && pixel_shader.runtimeState,
              "the same shader handle creates after a rejected attempt");
        hr = WineD3D11On12DestroyShaderV1(&pixel_shader);
        check(hr == S_OK, "the recovered shader destroys cleanly");

        memset(&shader_stress, 0, sizeof(shader_stress));
        shader_stress.adapter = &out;
        shader_stress.shader = &vertex_shader;
        for (UINT i = 0; i < ARRAYSIZE(shader_threads); ++i)
            shader_threads[i] = CreateThread(NULL, 0,
                    bind_vertex_shader_until_stopped, &shader_stress, 0, NULL);
        Sleep(10);
        hr = WineD3D11On12DestroyShaderV1(&vertex_shader);
        Sleep(10);
        InterlockedExchange(&shader_stress.stop, 1);
        WaitForMultipleObjects(ARRAYSIZE(shader_threads), shader_threads,
                TRUE, 5000);
        for (UINT i = 0; i < ARRAYSIZE(shader_threads); ++i)
            if (shader_threads[i]) CloseHandle(shader_threads[i]);
        check(hr == S_OK && !shader_stress.unexpected,
              "concurrent shader binding and destruction stays bounded");

        hr = WineD3D11On12DestroyBufferV1(&index_buffer_handle);
        check(hr == S_OK, "the bound index buffer can be destroyed safely");

        /* Texture2D lifecycle.
         *
         * The mip chain is the part worth checking hardest: the core derives
         * it rather than receiving it, so every slice and level is compared
         * against a halving chain that clamps at one, across an array. */
        memset(&texture_desc, 0, sizeof(texture_desc));
        texture_desc.Width = 8;
        texture_desc.Height = 4;
        texture_desc.MipLevels = 0;
        texture_desc.ArraySize = 2;
        texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        memset(&texture_handle, 0, sizeof(texture_handle));
        texture_handle.size = sizeof(texture_handle);
        hr = WineD3D11On12CreateTexture2DV1(&out, &texture_desc, NULL,
                &texture_handle);
        check(hr == S_OK && texture_handle.hDrvResource
                && texture_handle.runtimeState,
              "Texture2D creation publishes an owned DDI resource handle");
        get_texture2d_record(&texture_created, &texture_width, &texture_height,
                &texture_mip_levels, &texture_array_size, &texture_format,
                &texture_bind_flags, &texture_had_initial_data,
                &bad_texture_mip_chain);
        check(texture_created == 1 && texture_width == 8 && texture_height == 4
                && texture_array_size == 2 && !bad_texture_mip_chain,
              "Texture2D creation carries an exact mip chain for every slice");
        /* 8x4 resolves to four levels: 8x4, 4x2, 2x1, 1x1. */
        check(texture_mip_levels == 4,
              "a zero mip count resolves to the full chain");
        check(texture_format == DXGI_FORMAT_R8G8B8A8_UNORM
                && texture_bind_flags == D3D11_BIND_RENDER_TARGET,
              "Texture2D creation forwards format and bind flags");
        check(!texture_had_initial_data,
              "a Texture2D created without initial data forwards none");

        hr = WineD3D11On12DestroyTexture2DV1(&texture_handle);
        check(hr == S_OK && !texture_handle.hDrvResource
                && !texture_handle.runtimeState,
              "Texture2D destruction clears the public handle");
        get_resource_counts(&resource_created, &resource_destroyed,
                &bad_resource_description);
        {
            const LONG destroyed_once = resource_destroyed;

            /* Idempotent, like every other resource kind here: destruction
             * reaches the driver once and a repeat is a no-op that leaves the
             * caller's structure inert.  A stale handle must not be rejected
             * -- that would make a late destroy after device teardown a
             * failure rather than the double-free guard it exists to be. */
            hr = WineD3D11On12DestroyTexture2DV1(&texture_handle);
            check(hr == S_OK && !texture_handle.hDrvResource
                    && !texture_handle.runtimeState,
                  "a destroyed Texture2D handle destroys again idempotently");
            get_resource_counts(&resource_created, &resource_destroyed,
                    &bad_resource_description);
            check(resource_destroyed == destroyed_once
                    && !bad_resource_description,
                  "a repeated Texture2D destroy reaches the driver no second "
                  "time");
        }

        /* Initial data, one entry per subresource. */
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        memset(texture_pixels, 0x5a, sizeof(texture_pixels));
        memset(&texture_initial_data, 0, sizeof(texture_initial_data));
        texture_initial_data.pSysMem = texture_pixels;
        texture_initial_data.SysMemPitch = 32;
        memset(&texture_handle, 0, sizeof(texture_handle));
        texture_handle.size = sizeof(texture_handle);
        hr = WineD3D11On12CreateTexture2DV1(&out, &texture_desc,
                &texture_initial_data, &texture_handle);
        check(hr == S_OK, "a Texture2D accepts initial subresource data");
        get_texture2d_record(&texture_created, NULL, NULL, NULL, NULL, NULL,
                NULL, &texture_had_initial_data, &bad_texture_mip_chain);
        check(texture_created == 2 && texture_had_initial_data
                && !bad_texture_mip_chain,
              "initial data reaches the DDI as an upload array");
        hr = WineD3D11On12DestroyTexture2DV1(&texture_handle);
        check(hr == S_OK, "an initialised Texture2D destroys cleanly");

        /* Initial data with a null pointer must be refused, and must not
         * leave a half-created resource behind. */
        memset(&texture_initial_data, 0, sizeof(texture_initial_data));
        memset(&texture_handle, 0, sizeof(texture_handle));
        texture_handle.size = sizeof(texture_handle);
        hr = WineD3D11On12CreateTexture2DV1(&out, &texture_desc,
                &texture_initial_data, &texture_handle);
        check(hr == E_INVALIDARG && !texture_handle.hDrvResource,
              "initial data without memory is refused before the DDI");
        get_texture2d_record(&texture_created, NULL, NULL, NULL, NULL, NULL,
                NULL, NULL, NULL);
        check(texture_created == 2,
              "a refused Texture2D never reaches the DDI");

        /* Argument validation, each rejected before the driver is touched. */
        memset(&texture_handle, 0, sizeof(texture_handle));
        texture_handle.size = sizeof(texture_handle);
        texture_desc.Width = 0;
        hr = WineD3D11On12CreateTexture2DV1(&out, &texture_desc, NULL,
                &texture_handle);
        check(hr == E_INVALIDARG, "a zero-width Texture2D is refused");
        texture_desc.Width = 8;
        texture_desc.Format = DXGI_FORMAT_UNKNOWN;
        hr = WineD3D11On12CreateTexture2DV1(&out, &texture_desc, NULL,
                &texture_handle);
        check(hr == E_INVALIDARG, "an unknown Texture2D format is refused");
        texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.MipLevels = 9;
        hr = WineD3D11On12CreateTexture2DV1(&out, &texture_desc, NULL,
                &texture_handle);
        check(hr == E_INVALIDARG,
              "a mip count deeper than the full chain is refused");
        texture_desc.MipLevels = 1;
        hr = WineD3D11On12CreateTexture2DV1(NULL, &texture_desc, NULL,
                &texture_handle);
        check(hr == E_INVALIDARG, "a null adapter device is refused");
        texture_handle.size = 0;
        hr = WineD3D11On12CreateTexture2DV1(&out, &texture_desc, NULL,
                &texture_handle);
        check(hr == E_INVALIDARG, "a Texture2D handle of the wrong size is refused");
        texture_handle.size = sizeof(texture_handle);
        get_texture2d_record(&texture_created, NULL, NULL, NULL, NULL, NULL,
                NULL, NULL, NULL);
        check(texture_created == 2,
              "no rejected Texture2D argument reaches the DDI");

        buffer_handle.size = sizeof(buffer_handle);
        hr = WineD3D11On12CreateBufferV1(&out, &buffer_desc, NULL,
                &buffer_handle);
        check(hr == S_OK, "a buffer can remain owned until device teardown");
        input_layout.size = sizeof(input_layout);
        hr = WineD3D11On12CreateInputLayoutV1(&out, &input_element,
                &input_register, 1, &input_layout);
        check(hr == S_OK,
              "an input layout can remain owned until device teardown");
        vertex_shader.size = sizeof(vertex_shader);
        hr = WineD3D11On12CreateVertexShaderV1(&out, shader_byte_code,
                sizeof(shader_byte_code), &vertex_shader);
        check(hr == S_OK, "a shader can remain owned until device teardown");
        memset(&texture_handle, 0, sizeof(texture_handle));
        texture_handle.size = sizeof(texture_handle);
        hr = WineD3D11On12CreateTexture2DV1(&out, &texture_desc, NULL,
                &texture_handle);
        check(hr == S_OK,
              "a Texture2D can remain owned until device teardown");

        hr = WineD3D11On12CloseAdapterDeviceV1(&out);
        check(hr == S_OK, "the complete driver lifecycle closes successfully");
        check(!buffer_handle.hDrvResource && !buffer_handle.runtimeState,
              "device teardown invalidates every surviving buffer handle");
        check(!input_layout.hDrvElementLayout && !input_layout.runtimeState,
              "device teardown invalidates every surviving input layout");
        check(!vertex_shader.hDrvShader && !vertex_shader.runtimeState,
              "device teardown invalidates every surviving shader");
        check(!texture_handle.hDrvResource && !texture_handle.runtimeState,
              "device teardown invalidates every surviving Texture2D handle");
        get_resource_counts(&resource_created, &resource_destroyed,
                &bad_resource_description);
        /* One more creation than destruction, and exactly one: the injected
         * resource failure above is a create call the driver rejected, so it
         * never earned a destroy.  Every other resource is accounted for. */
        check(resource_created == 8 && resource_destroyed == 7,
              "device teardown destroys each surviving DDI resource");
        get_input_layout_counts(&layout_created, &layout_destroyed,
                &layout_bound, &bound_layout, &bad_layout_description);
        check(layout_created == 4 && layout_destroyed == 3,
              "device teardown destroys each surviving input layout");
        get_shader_counts(&vertex_shader_created, &pixel_shader_created,
                &shader_destroyed, &vertex_shader_bound, &pixel_shader_bound,
                &bound_vertex_shader, &bound_pixel_shader,
                &bad_shader_description);
        check(vertex_shader_created == 2 && pixel_shader_created == 3
                && shader_destroyed == 4 && !bad_shader_description,
              "shader failures and teardown preserve exact callback counts");
        get_counts(&opened, &created, &destroyed, &closed);
        check(destroyed == 3 && closed == 3,
              "close destroys the device and then closes its adapter once");
        {
            BOOL submitted = TRUE;
            hr = WineD3D11On12FlushAdapterDeviceV1(&out, 0, 0, &submitted);
            check(hr == DXGI_ERROR_UNSUPPORTED && !submitted,
                  "flush fails closed after lifecycle teardown");
        }
        hr = WineD3D11On12DrawAdapterDeviceV1(&out, 3, 0);
        check(hr == DXGI_ERROR_UNSUPPORTED && get_draw_count() == 1,
              "draw fails closed after lifecycle teardown");
        hr = WineD3D11On12SetPrimitiveTopologyV1(&out, 4);
        check(hr == DXGI_ERROR_UNSUPPORTED && get_topology(&topology) == 1,
              "primitive topology fails closed after lifecycle teardown");
        check(device.refcount == 1 && queue.refcount == 1,
              "close releases the retained D3D12 device and queue");
        check(out.runtimeState == NULL && out.deviceFuncs == NULL
                && out.hDrvAdapter == NULL && out.hDrvDevice == NULL,
              "close clears every owned DDI output");

        hr = WineD3D11On12CloseAdapterDeviceV1(&out);
        check(hr == S_OK, "a repeated close is idempotent");
        get_counts(&opened, &created, &destroyed, &closed);
        check(destroyed == 3 && closed == 3,
              "a repeated close invokes no driver callback");
    }
    if (mock_driver)
        FreeLibrary(mock_driver);

    if (failures)
    {
        printf("[fail] %d check(s) failed\n", failures);
        return 1;
    }
    printf("[ ok ] the adapter entry point fails closed before the driver\n");
    return 0;
}
