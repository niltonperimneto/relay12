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
    WineD3D11On12AdapterDevice out;
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queue_objects[1];
    HRESULT hr;

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

    if (failures)
    {
        printf("[fail] %d check(s) failed\n", failures);
        return 1;
    }
    printf("[ ok ] the adapter entry point fails closed before the driver\n");
    return 0;
}
