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
    WineD3D11On12AdapterDevice out;
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queue_objects[1];
    HRESULT hr;
    HMODULE mock_driver;
    get_counts_fn get_counts;
    get_flush_count_fn get_flush_count;
    get_flush_count_fn get_draw_count;
    LONG opened, created, destroyed, closed;

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
    check(get_counts != NULL, "the lifecycle mock driver is loaded");
    check(get_flush_count != NULL, "the flush counter is exported");
    check(get_draw_count != NULL, "the draw counter is exported");
    if (get_counts && get_flush_count && get_draw_count)
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

        hr = WineD3D11On12CloseAdapterDeviceV1(&out);
        check(hr == S_OK, "the complete driver lifecycle closes successfully");
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
