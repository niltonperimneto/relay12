/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Mock COM objects for the D3D11On12 core boundary tests.
 *
 * Moved here verbatim from tests/d3d11on12coretest.c so that
 * tests/ddi_thread_stress.c drives the core through the same objects.  Two
 * suites inventing their own mocks is how they come to disagree about what the
 * boundary faces, and the whole value of these is that they reproduce a
 * payload that breaks the COM contract in the specific ways the real one does.
 *
 * The vtables use designated initializers: any method the core is not expected
 * to call is left NULL, so an unexpected call faults immediately instead of
 * passing silently.
 *
 * Reference counts are maintained with InterlockedIncrement and
 * InterlockedDecrement rather than ++ and --, which is what lets the stress
 * test assert a count is back to one after concurrent use.
 *
 * The functions are static inline because not every translation unit needs
 * every mock, and an unused static function is an error under -Werror.
 *
 * Include after <windows.h> and <d3d12.h>.
 */
#ifndef WINE_D3D11ON12_MOCKS_H
#define WINE_D3D11ON12_MOCKS_H

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
    /* Answer an IID_IUnknown query with S_OK and no pointer, the way the
     * D3DMetal payload is known to.  The typed interfaces stay correct: this
     * is the object that defeats an identity comparison which only checks the
     * HRESULT, because two of them compare equal to each other. */
    int lie_about_identity;
    /* How many nodes this device claims.  One is the ordinary answer; the
     * interesting values are a larger count, which makes a high node bit
     * legitimate, and zero, which is a payload naming no node at all. */
    UINT node_count;
    unsigned int node_count_calls;
};

static inline struct mock_device *impl_from_device(ID3D12Device *iface)
{
    return CONTAINING_RECORD(iface, struct mock_device, ID3D12Device_iface);
}

static inline HRESULT STDMETHODCALLTYPE mock_device_QueryInterface(ID3D12Device *iface,
        REFIID riid, void **out)
{
    struct mock_device *device = impl_from_device(iface);

    if (!out)
        return E_POINTER;
    if (IsEqualGUID(riid, &IID_IUnknown) && device->lie_about_identity)
    {
        *out = NULL;
        return S_OK;
    }
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

static inline ULONG STDMETHODCALLTYPE mock_device_AddRef(ID3D12Device *iface)
{
    return InterlockedIncrement(&impl_from_device(iface)->refcount);
}

static inline ULONG STDMETHODCALLTYPE mock_device_Release(ID3D12Device *iface)
{
    return InterlockedDecrement(&impl_from_device(iface)->refcount);
}

static inline HRESULT STDMETHODCALLTYPE mock_device_CheckFeatureSupport(
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

static UINT STDMETHODCALLTYPE mock_device_GetNodeCount(ID3D12Device *iface)
{
    struct mock_device *device = impl_from_device(iface);

    ++device->node_count_calls;
    return device->node_count;
}

/* Not const: the widl C interface declares lpVtbl as a pointer to
 * non-const. */
static ID3D12DeviceVtbl mock_device_vtbl =
{
    .QueryInterface = mock_device_QueryInterface,
    .AddRef = mock_device_AddRef,
    .Release = mock_device_Release,
    .CheckFeatureSupport = mock_device_CheckFeatureSupport,
    .GetNodeCount = mock_device_GetNodeCount,
};

static inline void mock_device_init(struct mock_device *device)
{
    memset(device, 0, sizeof(*device));
    device->ID3D12Device_iface.lpVtbl = &mock_device_vtbl;
    device->refcount = 1;
    device->feature_hr = S_OK;
    device->max_feature_level = D3D_FEATURE_LEVEL_11_0;
    device->node_count = 1;
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

static inline struct mock_queue *impl_from_queue(ID3D12CommandQueue *iface)
{
    return CONTAINING_RECORD(iface, struct mock_queue,
            ID3D12CommandQueue_iface);
}

static inline HRESULT STDMETHODCALLTYPE mock_queue_QueryInterface(
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

static inline ULONG STDMETHODCALLTYPE mock_queue_AddRef(ID3D12CommandQueue *iface)
{
    return InterlockedIncrement(&impl_from_queue(iface)->refcount);
}

static inline ULONG STDMETHODCALLTYPE mock_queue_Release(ID3D12CommandQueue *iface)
{
    return InterlockedDecrement(&impl_from_queue(iface)->refcount);
}

static inline HRESULT STDMETHODCALLTYPE mock_queue_GetDevice(ID3D12CommandQueue *iface,
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

static inline D3D12_COMMAND_QUEUE_DESC * STDMETHODCALLTYPE mock_queue_GetDesc(
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

static inline void mock_queue_init(struct mock_queue *queue, struct mock_device *device,
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

static inline struct mock_unknown *impl_from_unknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct mock_unknown, IUnknown_iface);
}

static inline HRESULT STDMETHODCALLTYPE mock_unknown_QueryInterface(IUnknown *iface,
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

static inline ULONG STDMETHODCALLTYPE mock_unknown_AddRef(IUnknown *iface)
{
    return InterlockedIncrement(&impl_from_unknown(iface)->refcount);
}

static inline ULONG STDMETHODCALLTYPE mock_unknown_Release(IUnknown *iface)
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

static inline void mock_unknown_init(struct mock_unknown *unknown)
{
    memset(unknown, 0, sizeof(*unknown));
    unknown->IUnknown_iface.lpVtbl = &mock_unknown_vtbl;
    unknown->refcount = 1;
}

/* Mock object that breaks the COM contract by reporting success without
 * returning an interface.  The boundary faces a proprietary payload, so it
 * must survive this instead of dereferencing the null. */
static inline HRESULT STDMETHODCALLTYPE mock_liar_QueryInterface(IUnknown *iface,
        REFIID riid, void **out)
{
    (void)iface;
    (void)riid;
    if (out)
        *out = NULL;
    return S_OK;
}

static inline ULONG STDMETHODCALLTYPE mock_liar_AddRef(IUnknown *iface)
{
    return InterlockedIncrement(&impl_from_unknown(iface)->refcount);
}

static inline ULONG STDMETHODCALLTYPE mock_liar_Release(IUnknown *iface)
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

static inline void mock_liar_init(struct mock_unknown *unknown)
{
    memset(unknown, 0, sizeof(*unknown));
    unknown->IUnknown_iface.lpVtbl = &mock_liar_vtbl;
    unknown->refcount = 1;
}

#endif
