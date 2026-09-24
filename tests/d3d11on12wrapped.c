/* SPDX-License-Identifier: GPL-3.0-only
 * Wrapped-resource reference transfer, ownership validation and partial failure.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include "d3d11on12core.h"
#include "d3d11on12mocks.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("[fail] line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
struct resource12
{
    ID3D12Resource iface;
    LONG refs;
    struct mock_device *device;
    D3D12_RESOURCE_DESC desc;
    BOOL null_query, null_device;
};
static struct resource12 *impl(ID3D12Resource *iface)
{ return CONTAINING_RECORD(iface, struct resource12, iface); }
static HRESULT STDMETHODCALLTYPE query(ID3D12Resource *iface, REFIID iid, void **out)
{
    *out = NULL;
    if (impl(iface)->null_query) return S_OK;
    if (!IsEqualGUID(iid, &IID_IUnknown) && !IsEqualGUID(iid, &IID_ID3D12Resource)) return E_NOINTERFACE;
    InterlockedIncrement(&impl(iface)->refs); *out = iface; return S_OK;
}
static ULONG STDMETHODCALLTYPE addref(ID3D12Resource *iface) { return InterlockedIncrement(&impl(iface)->refs); }
static ULONG STDMETHODCALLTYPE release(ID3D12Resource *iface) { return InterlockedDecrement(&impl(iface)->refs); }
static HRESULT STDMETHODCALLTYPE get_device(ID3D12Resource *iface, REFIID iid, void **out)
{
    if (impl(iface)->null_device) { *out = NULL; return S_OK; }
    return ID3D12Device_QueryInterface(&impl(iface)->device->ID3D12Device_iface, iid, out);
}
static D3D12_RESOURCE_DESC *STDMETHODCALLTYPE get_desc(ID3D12Resource *iface, D3D12_RESOURCE_DESC *out)
{ *out = impl(iface)->desc; return out; }
static HRESULT STDMETHODCALLTYPE get_heap(ID3D12Resource *iface, D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS *flags)
{
    (void)iface;
    memset(props, 0, sizeof(*props)); props->Type = D3D12_HEAP_TYPE_DEFAULT;
    *flags = D3D12_HEAP_FLAG_NONE; return S_OK;
}
static ID3D12ResourceVtbl resource_vtbl = {
    .QueryInterface = query, .AddRef = addref, .Release = release,
    .GetDevice = get_device, .GetDesc = get_desc, .GetHeapProperties = get_heap,
};
struct ownership_stress
{
    WineD3D11On12AdapterDevice *owner;
    WineD3D11On12Texture2D *texture;
    HANDLE start;
    LONG unexpected;
};
static DWORD WINAPI ownership_thread(void *arg)
{
    struct ownership_stress *stress = arg;
    UINT i;
    WaitForSingleObject(stress->start, INFINITE);
    for (i = 0; i < 400; ++i)
    {
        HRESULT hr = WineD3D11On12SetWrappedOwnershipV1(stress->owner, &stress->texture, 1, TRUE);
        if (hr != S_OK && hr != E_INVALIDARG) InterlockedIncrement(&stress->unexpected);
        hr = WineD3D11On12SetWrappedOwnershipV1(stress->owner, &stress->texture, 1, FALSE);
        if (hr != S_OK && hr != E_INVALIDARG) InterlockedIncrement(&stress->unexpected);
    }
    return 0;
}
int main(void)
{
    struct mock_device device, foreign;
    struct mock_queue queue;
    struct resource12 original = {0};
    WineD3D11On12AdapterDevice owner = {.size = sizeof(owner)};
    WineD3D11On12Texture2D texture = {.size = sizeof(texture)}, other = {.size = sizeof(other)};
    WineD3D11On12Texture2D *one[] = {&texture}, *two[] = {&texture, &other}, *duplicate[] = {&texture, &texture};
    WineD3D11On12RenderTargetView view = {.size = sizeof(view)};
    D3D11_TEXTURE2D_DESC desc;
    D3D11_RESOURCE_FLAGS flags = {.BindFlags = D3D11_BIND_RENDER_TARGET};
    IUnknown *queues[1];
    HMODULE driver;
    void (WINAPI *counts)(LONG *);
    void (WINAPI *fail_wrap)(void), (WINAPI *fail_resource)(void), (WINAPI *fail_transition)(void);
    LONG before[7], after[7];
    const FLOAT color[] = {1, 0, 0, 1};
    mock_device_init(&device); device.support_device1 = 1;
    mock_device_init(&foreign); foreign.support_device1 = 1;
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    original.iface.lpVtbl = &resource_vtbl; original.refs = 1; original.device = &device;
    original.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    original.desc.Width = original.desc.Height = 64;
    original.desc.DepthOrArraySize = original.desc.MipLevels = original.desc.SampleDesc.Count = 1;
    original.desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    original.desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &owner) == S_OK);
    if (!owner.runtimeState) return 1;
    driver = GetModuleHandleW(L"d3d11on12.dll");
    counts = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockDriverGetWrappedCounts");
    fail_wrap = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockDriverFailNextWrap");
    fail_resource = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockDriverFailNextResource");
    fail_transition = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockDriverFailNextTransition");
    if (!counts || !fail_wrap || !fail_resource || !fail_transition) return 1;
#define WRAP(out) WineD3D11On12CreateWrappedTexture2DV1(&owner, (IUnknown *)&original.iface, &flags, \
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE, &desc, out)
    original.null_query = TRUE; CHECK(WRAP(&texture) == E_NOINTERFACE); original.null_query = FALSE;
    original.null_device = TRUE; CHECK(WRAP(&texture) == E_NOINTERFACE); original.null_device = FALSE;
    original.device = &foreign; CHECK(WRAP(&texture) == E_INVALIDARG); original.device = &device;
    flags.MiscFlags = D3D11_RESOURCE_MISC_SHARED; CHECK(WRAP(&texture) == DXGI_ERROR_UNSUPPORTED); flags.MiscFlags = 0;
    original.desc.SampleDesc.Count = 4; CHECK(WRAP(&texture) == DXGI_ERROR_UNSUPPORTED); original.desc.SampleDesc.Count = 1;
    CHECK(!texture.runtimeState && !texture.hDrvResource && original.refs == 1);
    fail_wrap(); CHECK(WRAP(&texture) == E_OUTOFMEMORY); CHECK(original.refs == 1);
    fail_resource(); CHECK(WRAP(&texture) == E_OUTOFMEMORY);
    counts(after); CHECK(!after[6] && original.refs == 1 && !texture.runtimeState);
    CHECK(WRAP(&texture) == S_OK);
    CHECK(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM && desc.Width == 64);
    CHECK(original.refs == 3); // caller, core, driver
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &texture, NULL, &view) == S_OK);
    CHECK(WineD3D11On12ClearRenderTargetV1(&owner, &view, color) == E_INVALIDARG);
    CHECK(WineD3D11On12SetRenderTargetV1(&owner, &view) == S_OK);
    CHECK(WineD3D11On12DrawAdapterDeviceV1(&owner, 3, 0) == E_INVALIDARG);
    counts(before);
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, duplicate, 2, TRUE) == E_INVALIDARG);
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, two, 2, TRUE) == E_INVALIDARG);
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, one, 1, FALSE) == E_INVALIDARG);
    counts(after); CHECK(after[1] == before[1] && after[2] == before[2]);
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, one, 1, TRUE) == S_OK);
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, one, 1, TRUE) == E_INVALIDARG);
    CHECK(WineD3D11On12ClearRenderTargetV1(&owner, &view, color) == S_OK);
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, one, 1, FALSE) == S_OK);
    CHECK(WineD3D11On12ClearRenderTargetV1(&owner, &view, color) == E_INVALIDARG);
    CHECK(WineD3D11On12DrawAdapterDeviceV1(&owner, 3, 0) == E_INVALIDARG);
    counts(after);
    CHECK(after[4] == D3D12_RESOURCE_STATE_RENDER_TARGET && after[5] == D3D12_RESOURCE_STATE_COPY_SOURCE);
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, one, 1, TRUE) == S_OK);
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, NULL, 0, FALSE) == S_OK);
    counts(before);
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, NULL, 0, FALSE) == S_OK);
    counts(after); CHECK(after[2] == before[2]);
    CHECK(WineD3D11On12DestroyTexture2DV1(&texture) == S_OK);
    CHECK(WineD3D11On12DestroyTexture2DV1(&texture) == S_OK);
    CHECK(original.refs == 3); // retained view pins wrapper and original
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, one, 1, TRUE) == E_INVALIDARG);
    CHECK(WineD3D11On12DestroyRenderTargetViewV1(&view) == S_OK);
    CHECK(original.refs == 1);
    CHECK(WRAP(&texture) == S_OK);
    {
        struct ownership_stress stress = {&owner, &texture, CreateEventW(NULL, TRUE, FALSE, NULL), 0};
        HANDLE threads[8];
        UINT i, started = 0;
        CHECK(stress.start != NULL);
        if (!stress.start) return 1;
        for (i = 0; i < 8; ++i)
        {
            HANDLE thread = CreateThread(NULL, 0, ownership_thread, &stress, 0, NULL);
            CHECK(thread != NULL);
            if (thread) threads[started++] = thread;
        }
        SetEvent(stress.start);
        CHECK(WineD3D11On12DestroyTexture2DV1(&texture) == S_OK);
        if (started && WaitForMultipleObjects(started, threads, TRUE, 10000) != WAIT_OBJECT_0)
        {
            puts("[fail] ownership/destruction stress timed out");
            return 1;
        }
        for (i = 0; i < started; ++i) CloseHandle(threads[i]);
        CloseHandle(stress.start);
        CHECK(stress.unexpected == 0 && original.refs == 1);
    }
    CHECK(WRAP(&texture) == S_OK);
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &texture, NULL, &view) == S_OK);
    fail_transition();
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, one, 1, TRUE) == DXGI_ERROR_DEVICE_REMOVED);
    counts(before);
    CHECK(WineD3D11On12SetWrappedOwnershipV1(&owner, one, 1, TRUE) == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(WineD3D11On12ClearRenderTargetV1(&owner, &view, color) == DXGI_ERROR_DEVICE_REMOVED);
    counts(after); CHECK(before[1] == after[1]);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&owner) == S_OK);
    CHECK(original.refs == 1 && !texture.runtimeState);
    CHECK(WineD3D11On12DestroyTexture2DV1(&texture) == S_OK);
    counts(after); CHECK(!after[6]);
    CHECK(device.refcount == 1 && queue.refcount == 1 && foreign.refcount == 1);
    printf("[%s] wrapped-resource lifecycle and ownership checks\n", failures ? "fail" : " ok ");
    return failures ? 1 : 0;
}
