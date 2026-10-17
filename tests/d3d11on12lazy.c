/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Public COM tests for an allocation-free frontend and its first-use boundary.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11_4.h>
#include <stdio.h>
#include "d3d11on12core.h"
#include "d3d11on12mocks.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("[fail] line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
#define REQUIRE(x) do { CHECK(x); if (failures) return 1; } while (0)

typedef HRESULT (WINAPI *create_fn)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT,
        IUnknown *const *, UINT, UINT, ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);
typedef void (WINAPI *counts_fn)(LONG *, LONG *, LONG *, LONG *);
struct worker { ID3D11Device *device; HANDLE start; HRESULT result; };

static DWORD WINAPI first_use(void *arg)
{
    struct worker *worker = arg;
    D3D11_BUFFER_DESC desc = {0};
    ID3D11Buffer *buffer = NULL;
    WaitForSingleObject(worker->start, INFINITE);
    desc.ByteWidth = 64;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    worker->result = ID3D11Device_CreateBuffer(worker->device, &desc, NULL, &buffer);
    if (SUCCEEDED(worker->result) && !buffer) worker->result = E_FAIL;
    if (buffer) ID3D11Buffer_Release(buffer);
    return 0;
}

int main(int argc, char **argv)
{
    const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    struct mock_device underlying;
    struct mock_queue queue;
    IUnknown *queues[1], *identity = NULL, *roundtrip = NULL;
    HMODULE core, driver;
    FARPROC address;
    create_fn create;
    counts_fn counts;
    void (WINAPI *fail_next_open)(void), (WINAPI *fail_next_frame)(void);
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGIDevice *dxgi = NULL;
    D3D_FEATURE_LEVEL selected = 0;
    LONG opened, created, destroyed, closed;
    HANDLE threads[8], start;
    struct worker workers[8];
    unsigned int i;
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    mock_device_init(&underlying);
    underlying.support_device1 = 1;
    underlying.expected_levels = &requested;
    underlying.expected_level_count = 1;
    mock_queue_init(&queue, &underlying, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    SetEnvironmentVariableW(L"RELAY12_EXPERIMENTAL_FRAME", L"1");
    core = LoadLibraryW(L"d3d11on12core.dll");
    REQUIRE(core != NULL);
    address = GetProcAddress(core, "WineD3D11On12CreateDeviceV1");
    memcpy(&create, &address, sizeof(create));
    REQUIRE(create != NULL);
    REQUIRE(!GetModuleHandleW(L"d3d11on12.dll"));

    queue.desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    device = (void *)(UINT_PTR)1;
    context = (void *)(UINT_PTR)1;
    selected = requested;
    CHECK(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
            queues, 1, 0, &device, &context, &selected) == E_INVALIDARG);
    CHECK(!device && !context && !selected);
    queue.desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    underlying.support_device1 = 0;
    CHECK(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
            queues, 1, 0, &device, &context, &selected) == E_NOINTERFACE);
    CHECK(!device && !context && !selected);
    underlying.support_device1 = 1;
    CHECK(!GetModuleHandleW(L"d3d11on12.dll"));
    CHECK(underlying.refcount == 1 && queue.refcount == 1);
    REQUIRE(!failures);

    /* Inspect and destroy unused wrappers: neither path may load DTL. */
    for (i = 0; i < 4; ++i)
    {
        REQUIRE(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
                queues, 1, 0, &device, &context, &selected) == S_OK);
        CHECK(selected == requested && device && context);
        REQUIRE(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi) == S_OK);
        REQUIRE(ID3D11Device_QueryInterface(device, &IID_IUnknown, (void **)&identity) == S_OK);
        REQUIRE(IDXGIDevice_QueryInterface(dxgi, &IID_IUnknown, (void **)&roundtrip) == S_OK);
        CHECK(identity == roundtrip);
        IUnknown_Release(roundtrip);
        IUnknown_Release(identity);
        IDXGIDevice_Release(dxgi);
        CHECK(ID3D11Device_GetFeatureLevel(device) == requested);
        ID3D11DeviceContext_ClearState(context);
        CHECK(!GetModuleHandleW(L"d3d11on12.dll"));
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        CHECK(!GetModuleHandleW(L"d3d11on12.dll"));
        CHECK(underlying.refcount == 1 && queue.refcount == 1);
    }
    REQUIRE(!failures);
    if (argc > 1)
    {
        /* Run from a directory with no driver to verify late-load failure. */
        REQUIRE(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
                queues, 1, 0, &device, &context, &selected) == S_OK);
        ID3D11DeviceContext_Flush(context);
        CHECK(ID3D11Device_GetDeviceRemovedReason(device) == DXGI_ERROR_UNSUPPORTED);
        ID3D11DeviceContext_Flush(context);
        CHECK(ID3D11Device_GetDeviceRemovedReason(device) == DXGI_ERROR_UNSUPPORTED);
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
        CHECK(underlying.refcount == 1 && queue.refcount == 1);
        printf("lazy missing-driver failures: %d\n", failures);
        return failures ? 1 : 0;
    }

    REQUIRE(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
            queues, 1, 0, &device, &context, &selected) == S_OK);
    start = CreateEventW(NULL, TRUE, FALSE, NULL);
    REQUIRE(start != NULL);
    for (i = 0; i < 8; ++i)
    {
        workers[i].device = device;
        workers[i].start = start;
        workers[i].result = E_FAIL;
        threads[i] = CreateThread(NULL, 0, first_use, &workers[i], 0, NULL);
        REQUIRE(threads[i] != NULL);
    }
    SetEvent(start);
    REQUIRE(WaitForMultipleObjects(8, threads, TRUE, 30000) == WAIT_OBJECT_0);
    for (i = 0; i < 8; ++i)
    {
        CHECK(workers[i].result == S_OK);
        CloseHandle(threads[i]);
    }
    CloseHandle(start);
    driver = GetModuleHandleW(L"d3d11on12.dll");
    REQUIRE(driver != NULL);
    address = GetProcAddress(driver, "WineD3D11On12MockDriverGetCounts");
    memcpy(&counts, &address, sizeof(counts));
    REQUIRE(counts != NULL);
    counts(&opened, &created, &destroyed, &closed);
    CHECK(opened == 1 && created == 1 && !destroyed && !closed);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    counts(&opened, &created, &destroyed, &closed);
    CHECK(opened == 1 && created == 1 && destroyed == 1 && closed == 1);
    CHECK(underlying.refcount == 1 && queue.refcount == 1);

    address = GetProcAddress(driver, "WineD3D11On12MockDriverFailNextOpen");
    memcpy(&fail_next_open, &address, sizeof(fail_next_open));
    REQUIRE(fail_next_open != NULL);
    REQUIRE(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
            queues, 1, 0, &device, &context, &selected) == S_OK);
    fail_next_open();
    ID3D11DeviceContext_Flush(context);
    CHECK(ID3D11Device_GetDeviceRemovedReason(device) == E_OUTOFMEMORY);
    {
        D3D11_BUFFER_DESC desc = {0};
        ID3D11Buffer *buffer = (void *)(UINT_PTR)1;
        desc.ByteWidth = 64;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        CHECK(ID3D11Device_CreateBuffer(device, &desc, NULL, &buffer) == E_OUTOFMEMORY);
        CHECK(!buffer);
    }
    ID3D11DeviceContext_Flush(context);
    CHECK(ID3D11Device_GetDeviceRemovedReason(device) == E_OUTOFMEMORY);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    counts(&opened, &created, &destroyed, &closed);
    CHECK(opened == 2 && created == 1 && destroyed == 1 && closed == 1);
    CHECK(underlying.refcount == 1 && queue.refcount == 1);
    address = GetProcAddress(driver, "WineD3D11On12MockDriverFailNextFrameSupport");
    memcpy(&fail_next_frame, &address, sizeof(fail_next_frame));
    REQUIRE(fail_next_frame != NULL);
    REQUIRE(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
            queues, 1, 0, &device, &context, &selected) == S_OK);
    fail_next_frame();
    ID3D11DeviceContext_Flush(context);
    CHECK(ID3D11Device_GetDeviceRemovedReason(device) == DXGI_ERROR_UNSUPPORTED);
    counts(&opened, &created, &destroyed, &closed);
    CHECK(opened == 3 && created == 2 && destroyed == 2 && closed == 2);
    ID3D11DeviceContext_Flush(context);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    counts(&opened, &created, &destroyed, &closed);
    CHECK(opened == 3 && created == 2 && destroyed == 2 && closed == 2);
    CHECK(underlying.refcount == 1 && queue.refcount == 1);
    FreeLibrary(core);
    printf("lazy frontend failures: %d\n", failures);
    return failures ? 1 : 0;
}
