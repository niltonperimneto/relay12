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
#include "d3d11on12mocks.h"

static int failures;

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
    WineD3D11On12AdapterDevice out;
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queue_objects[1];
    HRESULT hr;
    HMODULE mock_driver;
    get_counts_fn get_counts;
    get_flush_count_fn get_flush_count;
    get_flush_count_fn get_draw_count;
    get_extended_draw_counts_fn get_extended_draw_counts;
    get_topology_fn get_topology;
    get_resource_counts_fn get_resource_counts;
    get_ia_bindings_fn get_ia_bindings;
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
    check(get_counts != NULL, "the lifecycle mock driver is loaded");
    check(get_flush_count != NULL, "the flush counter is exported");
    check(get_draw_count != NULL, "the draw counter is exported");
    check(get_extended_draw_counts != NULL,
          "the extended draw counters are exported");
    check(get_topology != NULL, "the input-assembler topology counter is exported");
    check(get_resource_counts != NULL, "the resource counters are exported");
    check(get_ia_bindings != NULL, "the IA buffer binding recorder is exported");
    if (get_counts && get_flush_count && get_draw_count
            && get_extended_draw_counts && get_topology
            && get_resource_counts && get_ia_bindings)
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
        hr = WineD3D11On12DestroyBufferV1(&index_buffer_handle);
        check(hr == S_OK, "the bound index buffer can be destroyed safely");

        buffer_handle.size = sizeof(buffer_handle);
        hr = WineD3D11On12CreateBufferV1(&out, &buffer_desc, NULL,
                &buffer_handle);
        check(hr == S_OK, "a buffer can remain owned until device teardown");

        hr = WineD3D11On12CloseAdapterDeviceV1(&out);
        check(hr == S_OK, "the complete driver lifecycle closes successfully");
        check(!buffer_handle.hDrvResource && !buffer_handle.runtimeState,
              "device teardown invalidates every surviving buffer handle");
        get_resource_counts(&resource_created, &resource_destroyed,
                &bad_resource_description);
        check(resource_created == 3 && resource_destroyed == 3,
              "device teardown destroys each surviving DDI resource");
        get_counts(&opened, &created, &destroyed, &closed);
        check(destroyed == 1 && closed == 1,
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
        check(destroyed == 1 && closed == 1,
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
