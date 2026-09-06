/* SPDX-License-Identifier: GPL-3.0-only
 *
 * GPU-free validation tests for the D3D11On12 core boundary.
 *
 * These tests link the core directly and drive it with mock COM objects, so
 * they exercise every documented validation and fail-closed rule without a
 * D3D12 device, a GPU, or the Apple payload.  The mock vtables use designated
 * initializers: any method the core is not expected to call is left NULL, so
 * an unexpected call faults immediately instead of passing silently.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Werror -Igptk-d3d11 \
 *            -c -o d3d11on12coretest.o tests/d3d11on12coretest.c
 */
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>

#include "d3d11on12core.h"

#define TEST_ARRAY_SIZE(array) ((UINT)(sizeof(array) / sizeof((array)[0])))

static int failures;

static void fail(const char *test, const char *detail)
{
    printf("[fail] %s: %s\n", test, detail);
    ++failures;
}

static void check_hr(const char *test, HRESULT got, HRESULT expected)
{
    if (got == expected)
    {
        printf("[ ok ] %s\n", test);
        return;
    }
    printf("[fail] %s: got 0x%08lx, expected 0x%08lx\n", test,
            (unsigned long)got, (unsigned long)expected);
    ++failures;
}

/* Mock ID3D12Device. */
struct mock_device
{
    ID3D12Device ID3D12Device_iface;
    LONG refcount;
    HRESULT feature_hr;
    D3D_FEATURE_LEVEL max_feature_level;
    unsigned int feature_calls;
    const D3D_FEATURE_LEVEL *expected_levels;
    UINT expected_level_count;
    int bad_feature_request;
};

static struct mock_device *impl_from_device(ID3D12Device *iface)
{
    return CONTAINING_RECORD(iface, struct mock_device, ID3D12Device_iface);
}

static HRESULT STDMETHODCALLTYPE mock_device_QueryInterface(ID3D12Device *iface,
        REFIID riid, void **out)
{
    struct mock_device *device = impl_from_device(iface);

    if (!out)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown)
            || IsEqualGUID(riid, &IID_ID3D12Device))
    {
        /* One controlling identity: every accepted interface returns the same
         * pointer, which is what the core's identity comparison relies on. */
        InterlockedIncrement(&device->refcount);
        *out = &device->ID3D12Device_iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE mock_device_AddRef(ID3D12Device *iface)
{
    return InterlockedIncrement(&impl_from_device(iface)->refcount);
}

static ULONG STDMETHODCALLTYPE mock_device_Release(ID3D12Device *iface)
{
    return InterlockedDecrement(&impl_from_device(iface)->refcount);
}

static HRESULT STDMETHODCALLTYPE mock_device_CheckFeatureSupport(
        ID3D12Device *iface, D3D12_FEATURE feature, void *data, UINT data_size)
{
    struct mock_device *device = impl_from_device(iface);
    D3D12_FEATURE_DATA_FEATURE_LEVELS *levels = data;

    ++device->feature_calls;
    if (feature != D3D12_FEATURE_FEATURE_LEVELS || !data
            || data_size != sizeof(*levels))
    {
        device->bad_feature_request = 1;
        return E_INVALIDARG;
    }
    if (levels->NumFeatureLevels != device->expected_level_count
            || levels->pFeatureLevelsRequested != device->expected_levels)
        device->bad_feature_request = 1;

    if (FAILED(device->feature_hr))
        return device->feature_hr;
    levels->MaxSupportedFeatureLevel = device->max_feature_level;
    return device->feature_hr;
}

/* Not const: the widl C interface declares lpVtbl as a pointer to
 * non-const. */
static ID3D12DeviceVtbl mock_device_vtbl =
{
    .QueryInterface = mock_device_QueryInterface,
    .AddRef = mock_device_AddRef,
    .Release = mock_device_Release,
    .CheckFeatureSupport = mock_device_CheckFeatureSupport,
};

static void mock_device_init(struct mock_device *device)
{
    memset(device, 0, sizeof(*device));
    device->ID3D12Device_iface.lpVtbl = &mock_device_vtbl;
    device->refcount = 1;
    device->feature_hr = S_OK;
    device->max_feature_level = D3D_FEATURE_LEVEL_11_0;
}

/* Mock ID3D12CommandQueue. */
struct mock_queue
{
    ID3D12CommandQueue ID3D12CommandQueue_iface;
    LONG refcount;
    D3D12_COMMAND_QUEUE_DESC desc;
    struct mock_device *device;
    int lie_about_device;
};

static struct mock_queue *impl_from_queue(ID3D12CommandQueue *iface)
{
    return CONTAINING_RECORD(iface, struct mock_queue,
            ID3D12CommandQueue_iface);
}

static HRESULT STDMETHODCALLTYPE mock_queue_QueryInterface(
        ID3D12CommandQueue *iface, REFIID riid, void **out)
{
    struct mock_queue *queue = impl_from_queue(iface);

    if (!out)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown)
            || IsEqualGUID(riid, &IID_ID3D12CommandQueue))
    {
        InterlockedIncrement(&queue->refcount);
        *out = &queue->ID3D12CommandQueue_iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE mock_queue_AddRef(ID3D12CommandQueue *iface)
{
    return InterlockedIncrement(&impl_from_queue(iface)->refcount);
}

static ULONG STDMETHODCALLTYPE mock_queue_Release(ID3D12CommandQueue *iface)
{
    return InterlockedDecrement(&impl_from_queue(iface)->refcount);
}

static HRESULT STDMETHODCALLTYPE mock_queue_GetDevice(ID3D12CommandQueue *iface,
        REFIID riid, void **out)
{
    struct mock_queue *queue = impl_from_queue(iface);

    if (queue->lie_about_device)
    {
        /* Success without an interface: the core must not dereference it. */
        if (out)
            *out = NULL;
        return S_OK;
    }
    if (!queue->device)
    {
        if (out)
            *out = NULL;
        return E_FAIL;
    }
    return mock_device_QueryInterface(&queue->device->ID3D12Device_iface, riid,
            out);
}

static D3D12_COMMAND_QUEUE_DESC * STDMETHODCALLTYPE mock_queue_GetDesc(
        ID3D12CommandQueue *iface, D3D12_COMMAND_QUEUE_DESC *ret)
{
    *ret = impl_from_queue(iface)->desc;
    return ret;
}

/* Not const: the widl C interface declares lpVtbl as a pointer to
 * non-const. */
static ID3D12CommandQueueVtbl mock_queue_vtbl =
{
    .QueryInterface = mock_queue_QueryInterface,
    .AddRef = mock_queue_AddRef,
    .Release = mock_queue_Release,
    .GetDevice = mock_queue_GetDevice,
    .GetDesc = mock_queue_GetDesc,
};

static void mock_queue_init(struct mock_queue *queue, struct mock_device *device,
        D3D12_COMMAND_LIST_TYPE type)
{
    memset(queue, 0, sizeof(*queue));
    queue->ID3D12CommandQueue_iface.lpVtbl = &mock_queue_vtbl;
    queue->refcount = 1;
    queue->desc.Type = type;
    queue->device = device;
}

/* Mock object that is not a D3D12 object at all. */
struct mock_unknown
{
    IUnknown IUnknown_iface;
    LONG refcount;
};

static struct mock_unknown *impl_from_unknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct mock_unknown, IUnknown_iface);
}

static HRESULT STDMETHODCALLTYPE mock_unknown_QueryInterface(IUnknown *iface,
        REFIID riid, void **out)
{
    struct mock_unknown *unknown = impl_from_unknown(iface);

    if (!out)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown))
    {
        InterlockedIncrement(&unknown->refcount);
        *out = &unknown->IUnknown_iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE mock_unknown_AddRef(IUnknown *iface)
{
    return InterlockedIncrement(&impl_from_unknown(iface)->refcount);
}

static ULONG STDMETHODCALLTYPE mock_unknown_Release(IUnknown *iface)
{
    return InterlockedDecrement(&impl_from_unknown(iface)->refcount);
}

/* Not const: the widl C interface declares lpVtbl as a pointer to
 * non-const. */
static IUnknownVtbl mock_unknown_vtbl =
{
    .QueryInterface = mock_unknown_QueryInterface,
    .AddRef = mock_unknown_AddRef,
    .Release = mock_unknown_Release,
};

static void mock_unknown_init(struct mock_unknown *unknown)
{
    memset(unknown, 0, sizeof(*unknown));
    unknown->IUnknown_iface.lpVtbl = &mock_unknown_vtbl;
    unknown->refcount = 1;
}

/* Mock object that breaks the COM contract by reporting success without
 * returning an interface.  The boundary faces a proprietary payload, so it
 * must survive this instead of dereferencing the null. */
static HRESULT STDMETHODCALLTYPE mock_liar_QueryInterface(IUnknown *iface,
        REFIID riid, void **out)
{
    (void)iface;
    (void)riid;
    if (out)
        *out = NULL;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE mock_liar_AddRef(IUnknown *iface)
{
    return InterlockedIncrement(&impl_from_unknown(iface)->refcount);
}

static ULONG STDMETHODCALLTYPE mock_liar_Release(IUnknown *iface)
{
    return InterlockedDecrement(&impl_from_unknown(iface)->refcount);
}

/* Not const: the widl C interface declares lpVtbl as a pointer to
 * non-const. */
static IUnknownVtbl mock_liar_vtbl =
{
    .QueryInterface = mock_liar_QueryInterface,
    .AddRef = mock_liar_AddRef,
    .Release = mock_liar_Release,
};

static void mock_liar_init(struct mock_unknown *unknown)
{
    memset(unknown, 0, sizeof(*unknown));
    unknown->IUnknown_iface.lpVtbl = &mock_liar_vtbl;
    unknown->refcount = 1;
}

/* Output parameters are poisoned before every call: the core must initialize
 * all of them even on its failure paths. */
struct outputs
{
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    D3D_FEATURE_LEVEL level;
};

static HRESULT call_create(IUnknown *device_object,
        const D3D_FEATURE_LEVEL *levels, UINT level_count,
        IUnknown *const *queues, UINT queue_count, UINT node_mask,
        struct outputs *out)
{
    out->device = (ID3D11Device *)(ULONG_PTR)0xdeadbeefdeadbeefull;
    out->context = (ID3D11DeviceContext *)(ULONG_PTR)0xfeedfacefeedfaceull;
    out->level = (D3D_FEATURE_LEVEL)0x9999;

    return WineD3D11On12CreateDeviceV1(device_object, 0, levels, level_count,
            queues, queue_count, node_mask, &out->device, &out->context,
            &out->level);
}

static void check_cleared(const char *test, const struct outputs *out)
{
    if (out->device || out->context || out->level)
        fail(test, "an output parameter survived the call uninitialized");
}

static void check_balanced(const char *test, const struct mock_device *device,
        const struct mock_queue *queue)
{
    char detail[128];

    if (device && device->refcount != 1)
    {
        snprintf(detail, sizeof(detail),
                "device refcount is %ld, expected the caller's single "
                "reference", (long)device->refcount);
        fail(test, detail);
    }
    if (queue && queue->refcount != 1)
    {
        snprintf(detail, sizeof(detail),
                "queue refcount is %ld, expected the caller's single "
                "reference", (long)queue->refcount);
        fail(test, detail);
    }
}

static void test_abi_version(void)
{
    if (WineD3D11On12GetABIVersion() != WINE_D3D11ON12_ABI_VERSION)
        fail("abi version", "the exported ABI version does not match the header");
    else
        printf("[ ok ] abi version\n");
}

static void test_get_interface(void)
{
    WineD3D11On12Interface iface;
    HRESULT hr;

    hr = WineD3D11On12GetInterface(WINE_D3D11ON12_ABI_VERSION, sizeof(iface),
            NULL);
    check_hr("get interface rejects a null table", hr, E_INVALIDARG);

    hr = WineD3D11On12GetInterface(WINE_D3D11ON12_ABI_VERSION,
            sizeof(iface) - 1, &iface);
    check_hr("get interface rejects a short table", hr, E_INVALIDARG);

    hr = WineD3D11On12GetInterface(WINE_D3D11ON12_ABI_VERSION,
            sizeof(iface) + 1, &iface);
    check_hr("get interface rejects an over-long table", hr, E_INVALIDARG);

    memset(&iface, 0xcd, sizeof(iface));
    hr = WineD3D11On12GetInterface(WINE_D3D11ON12_ABI_VERSION + 1,
            sizeof(iface), &iface);
    check_hr("get interface rejects an unknown version", hr, E_NOINTERFACE);
    if (iface.size != sizeof(iface) || iface.version != WINE_D3D11ON12_ABI_VERSION)
        fail("get interface rejects an unknown version",
                "the rejected table must still report its own size and version");
    if (iface.capabilities || iface.createDevice)
        fail("get interface rejects an unknown version",
                "a rejected table must not publish entry points");

    memset(&iface, 0xcd, sizeof(iface));
    hr = WineD3D11On12GetInterface(WINE_D3D11ON12_ABI_VERSION, sizeof(iface),
            &iface);
    check_hr("get interface publishes the current version", hr, S_OK);
    if (iface.size != sizeof(iface) || iface.version != WINE_D3D11ON12_ABI_VERSION)
        fail("get interface publishes the current version",
                "size or version is wrong");
    if (!(iface.capabilities & WINE_D3D11ON12_CAP_VALIDATION))
        fail("get interface publishes the current version",
                "the validation capability bit is missing");
    if (iface.createDevice != WineD3D11On12CreateDeviceV1)
        fail("get interface publishes the current version",
                "createDevice does not point at the V1 entry point");
}

static void test_argument_validation(void)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queues[2];
    struct outputs out;
    HRESULT hr;

    mock_device_init(&device);
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    queues[1] = (IUnknown *)&queue.ID3D12CommandQueue_iface;

    hr = call_create(NULL, NULL, 0, queues, 1, 0, &out);
    check_hr("null device object", hr, E_INVALIDARG);
    check_cleared("null device object", &out);

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, NULL, 1, 0,
            &out);
    check_hr("null queue array", hr, E_INVALIDARG);
    check_cleared("null queue array", &out);

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 0,
            0, &out);
    check_hr("zero queues", hr, E_INVALIDARG);
    check_cleared("zero queues", &out);

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 2,
            0, &out);
    check_hr("more than one queue", hr, E_INVALIDARG);
    check_cleared("more than one queue", &out);

    queues[0] = NULL;
    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            0, &out);
    check_hr("null queue entry", hr, E_INVALIDARG);
    check_cleared("null queue entry", &out);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 1, queues, 1,
            0, &out);
    check_hr("feature levels null with a nonzero count", hr, E_INVALIDARG);
    check_cleared("feature levels null with a nonzero count", &out);

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, levels, 0, queues,
            1, 0, &out);
    check_hr("feature levels present with a zero count", hr, E_INVALIDARG);
    check_cleared("feature levels present with a zero count", &out);

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            0x3, &out);
    check_hr("multi-bit node mask", hr, E_INVALIDARG);
    check_cleared("multi-bit node mask", &out);

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            0x5, &out);
    check_hr("sparse multi-bit node mask", hr, E_INVALIDARG);
    check_cleared("sparse multi-bit node mask", &out);

    if (device.feature_calls)
        fail("argument validation",
                "the device was queried before the arguments were accepted");
    check_balanced("argument validation", &device, &queue);
}

static void test_interface_validation(void)
{
    struct mock_device device, other_device;
    struct mock_unknown unknown;
    struct mock_queue queue, compute_queue, foreign_queue;
    IUnknown *queues[1];
    struct outputs out;
    HRESULT hr;

    mock_device_init(&device);
    mock_device_init(&other_device);
    mock_unknown_init(&unknown);
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    mock_queue_init(&compute_queue, &device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
    mock_queue_init(&foreign_queue, &other_device,
            D3D12_COMMAND_LIST_TYPE_DIRECT);

    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    hr = call_create(&unknown.IUnknown_iface, NULL, 0, queues, 1, 0, &out);
    check_hr("device object that is not an ID3D12Device", hr, E_NOINTERFACE);
    check_cleared("device object that is not an ID3D12Device", &out);
    if (unknown.refcount != 1)
        fail("device object that is not an ID3D12Device",
                "the rejected object was left with an extra reference");

    queues[0] = &unknown.IUnknown_iface;
    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            0, &out);
    check_hr("queue object that is not an ID3D12CommandQueue", hr,
            E_NOINTERFACE);
    check_cleared("queue object that is not an ID3D12CommandQueue", &out);

    queues[0] = (IUnknown *)&compute_queue.ID3D12CommandQueue_iface;
    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            0, &out);
    check_hr("queue that is not a direct queue", hr, E_INVALIDARG);
    check_cleared("queue that is not a direct queue", &out);
    check_balanced("queue that is not a direct queue", &device, &compute_queue);

    queues[0] = (IUnknown *)&foreign_queue.ID3D12CommandQueue_iface;
    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            0, &out);
    check_hr("queue belonging to another device", hr, E_INVALIDARG);
    check_cleared("queue belonging to another device", &out);
    check_balanced("queue belonging to another device", &device, &foreign_queue);
    check_balanced("queue belonging to another device", &other_device, NULL);

    if (unknown.refcount != 1)
        fail("interface validation",
                "a rejected object was left with an extra reference");
}

static void test_contract_violating_objects(void)
{
    struct mock_device device;
    struct mock_unknown liar;
    struct mock_queue queue;
    IUnknown *queues[1];
    struct outputs out;
    HRESULT hr;

    mock_device_init(&device);
    mock_liar_init(&liar);
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);

    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    hr = call_create(&liar.IUnknown_iface, NULL, 0, queues, 1, 0, &out);
    check_hr("device object reporting success without an interface", hr,
            E_NOINTERFACE);
    check_cleared("device object reporting success without an interface", &out);

    queues[0] = &liar.IUnknown_iface;
    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            0, &out);
    check_hr("queue object reporting success without an interface", hr,
            E_NOINTERFACE);
    check_cleared("queue object reporting success without an interface", &out);

    queue.lie_about_device = 1;
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            0, &out);
    check_hr("queue reporting success without its device", hr, E_INVALIDARG);
    check_cleared("queue reporting success without its device", &out);
    queue.lie_about_device = 0;

    check_balanced("contract-violating objects", &device, &queue);
}

static void test_feature_levels(void)
{
    static const D3D_FEATURE_LEVEL levels[] =
    {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queues[1];
    struct outputs out;
    HRESULT hr;

    mock_device_init(&device);
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    device.expected_levels = levels;
    device.expected_level_count = TEST_ARRAY_SIZE(levels);

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, levels,
            TEST_ARRAY_SIZE(levels), queues, 1, 0, &out);
    check_hr("supported feature levels reach the fail-closed path", hr,
            DXGI_ERROR_UNSUPPORTED);
    check_cleared("supported feature levels reach the fail-closed path", &out);
    if (device.feature_calls != 1)
        fail("supported feature levels reach the fail-closed path",
                "the requested feature levels were not checked exactly once");
    if (device.bad_feature_request)
        fail("supported feature levels reach the fail-closed path",
                "the feature-level query did not forward the caller's array");
    check_balanced("supported feature levels reach the fail-closed path",
            &device, &queue);

    device.feature_calls = 0;
    device.max_feature_level = (D3D_FEATURE_LEVEL)0;
    hr = call_create((IUnknown *)&device.ID3D12Device_iface, levels,
            TEST_ARRAY_SIZE(levels), queues, 1, 0, &out);
    check_hr("no supported feature level", hr, E_INVALIDARG);
    check_cleared("no supported feature level", &out);
    check_balanced("no supported feature level", &device, &queue);

    device.feature_calls = 0;
    device.max_feature_level = D3D_FEATURE_LEVEL_11_0;
    device.feature_hr = E_FAIL;
    hr = call_create((IUnknown *)&device.ID3D12Device_iface, levels,
            TEST_ARRAY_SIZE(levels), queues, 1, 0, &out);
    check_hr("a failed feature-level query is propagated", hr, E_FAIL);
    check_cleared("a failed feature-level query is propagated", &out);
    check_balanced("a failed feature-level query is propagated", &device,
            &queue);
}

static void test_fail_closed(void)
{
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queues[1];
    struct outputs out;
    HRESULT hr;

    mock_device_init(&device);
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            0, &out);
    check_hr("valid inputs with node mask 0", hr, DXGI_ERROR_UNSUPPORTED);
    check_cleared("valid inputs with node mask 0", &out);
    if (device.feature_calls)
        fail("valid inputs with node mask 0",
                "feature levels were queried without a requested array");

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            1, &out);
    check_hr("valid inputs with node mask 1", hr, DXGI_ERROR_UNSUPPORTED);
    check_cleared("valid inputs with node mask 1", &out);

    hr = call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues, 1,
            0x8, &out);
    check_hr("valid inputs with a single high node bit", hr,
            DXGI_ERROR_UNSUPPORTED);
    check_cleared("valid inputs with a single high node bit", &out);

    check_balanced("fail-closed path", &device, &queue);
}

static void test_optional_outputs(void)
{
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queues[1];
    ID3D11Device *device11 = (ID3D11Device *)(ULONG_PTR)0xdeadbeefdeadbeefull;
    HRESULT hr;

    mock_device_init(&device);
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;

    /* Every output is optional; omitting them must not change the outcome or
     * dereference a null pointer. */
    hr = WineD3D11On12CreateDeviceV1((IUnknown *)&device.ID3D12Device_iface, 0,
            NULL, 0, queues, 1, 0, NULL, NULL, NULL);
    check_hr("all outputs omitted", hr, DXGI_ERROR_UNSUPPORTED);

    hr = WineD3D11On12CreateDeviceV1((IUnknown *)&device.ID3D12Device_iface, 0,
            NULL, 0, queues, 1, 0, &device11, NULL, NULL);
    check_hr("only the device output requested", hr, DXGI_ERROR_UNSUPPORTED);
    if (device11)
        fail("only the device output requested",
                "the device output was not cleared");

    hr = WineD3D11On12CreateDeviceV1(NULL, 0, NULL, 0, queues, 1, 0, NULL, NULL,
            NULL);
    check_hr("all outputs omitted on an invalid call", hr, E_INVALIDARG);

    check_balanced("optional outputs", &device, &queue);
}

static void test_reference_stress(void)
{
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queues[1];
    struct outputs out;
    unsigned int i;

    mock_device_init(&device);
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;

    for (i = 0; i < 4096; ++i)
    {
        if (call_create((IUnknown *)&device.ID3D12Device_iface, NULL, 0, queues,
                1, 0, &out) != DXGI_ERROR_UNSUPPORTED)
        {
            fail("reference stress", "a repeated call changed its result");
            break;
        }
        if (device.refcount != 1 || queue.refcount != 1)
        {
            fail("reference stress", "references accumulated across calls");
            break;
        }
    }
    if (i == 4096)
        printf("[ ok ] reference stress\n");
}

int main(void)
{
    test_abi_version();
    test_get_interface();
    test_argument_validation();
    test_interface_validation();
    test_contract_violating_objects();
    test_feature_levels();
    test_fail_closed();
    test_optional_outputs();
    test_reference_stress();

    if (failures)
    {
        printf("RESULT: %d D3D11On12 core validation failure(s)\n", failures);
        return 1;
    }
    printf("RESULT: D3D11On12 core validation and fail-closed rules hold\n");
    return 0;
}
