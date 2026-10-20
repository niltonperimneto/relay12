/* SPDX-License-Identifier: GPL-3.0-only
 * Device errors and removal: the driver's escape, errors raised on the
 * translation layer's worker thread, and what a removed device still does.
 *
 * The mock driver reports errors the way D3D11On12's Device::ReportError
 * does -- a DeviceStatusQuery escape with a null adapter, then pfnSetErrorCb --
 * either on the calling thread or on a thread that never called the core,
 * which is what the worker is. */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include "d3d11on12core.h"
#include "ddi/wine_d3d11ddi.h"
#include "d3d11on12mocks.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("[fail] line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

#define WAS_STILL_DRAWING ((HRESULT)0x887b0001)

static HRESULT (WINAPI *report_worker)(HRESULT);
static HRESULT (WINAPI *report_device)(HRESULT);
static void (WINAPI *worker_during_viewport)(HRESULT);
static LONG (WINAPI *escape_count)(void);
static LONG (WINAPI *draw_count)(void);
static LONG (WINAPI *flush_count)(void);
static void (WINAPI *resource_counts)(LONG *, LONG *, int *);
static void (WINAPI *frame_counts)(LONG *, LONG *, LONG *, LONG *, int *);

/* The mock driver has no state-object slots; substitute counting ones, as
 * tests/d3d11on12pso.c does, so binds and destroys can be seen. */
static LONG blend_destroys, blend_binds;
static SIZE_T calc_blend(D3D10DDI_HDEVICE device, const D3D11_1_DDI_BLEND_DESC *desc)
{ (void)device; (void)desc; return sizeof(UINT); }
static void create_blend(D3D10DDI_HDEVICE device, const D3D11_1_DDI_BLEND_DESC *desc,
        D3D10DDI_HBLENDSTATE state, D3D10DDI_HRTBLENDSTATE runtime)
{ (void)device; (void)desc; (void)runtime; *(UINT *)state.pDrvPrivate = 1; }
static void destroy_blend(D3D10DDI_HDEVICE device, D3D10DDI_HBLENDSTATE state)
{ (void)device; (void)state; InterlockedIncrement(&blend_destroys); }
static void bind_blend(D3D10DDI_HDEVICE device, D3D10DDI_HBLENDSTATE state,
        const FLOAT factor[4], UINT mask)
{ (void)device; (void)state; (void)factor; (void)mask; InterlockedIncrement(&blend_binds); }

static int resolve(void)
{
    HMODULE driver = GetModuleHandleW(L"d3d11on12.dll");
#define RESOLVE(var, name) ((var = (void *)(UINT_PTR)GetProcAddress(driver, name)) != NULL)
    return driver
            && RESOLVE(report_worker, "WineD3D11On12MockDriverReportWorkerError")
            && RESOLVE(report_device, "WineD3D11On12MockDriverReportDeviceError")
            && RESOLVE(worker_during_viewport, "WineD3D11On12MockDriverWorkerErrorDuringNextViewport")
            && RESOLVE(escape_count, "WineD3D11On12MockDriverGetEscapeCount")
            && RESOLVE(draw_count, "WineD3D11On12MockDriverGetDrawCount")
            && RESOLVE(flush_count, "WineD3D11On12MockDriverGetFlushCount")
            && RESOLVE(resource_counts, "WineD3D11On12MockDriverGetResourceCounts")
            && RESOLVE(frame_counts, "WineD3D11On12MockDriverGetFrameCounts");
#undef RESOLVE
}

/* Callers racing worker-thread errors.  Every per-call result must be one an
 * export can legitimately return; the worker's E_OUTOFMEMORY surfacing here
 * would be a device error blamed on whichever call happened to be running. */
struct race
{
    WineD3D11On12AdapterDevice *device;
    HANDLE start;
    volatile LONG unexpected;
};

static DWORD WINAPI race_thread(void *param)
{
    struct race *race = param;
    D3D11_BUFFER_DESC desc = {.ByteWidth = 16, .BindFlags = D3D11_BIND_VERTEX_BUFFER};
    D3D11_VIEWPORT viewport = {0, 0, 4, 4, 0, 1};
    UINT i;

    WaitForSingleObject(race->start, INFINITE);
    for (i = 0; i < 300; ++i)
    {
        WineD3D11On12Buffer buffer = {.size = sizeof(buffer)};
        HRESULT hr = WineD3D11On12CreateBufferV1(race->device, &desc, NULL, &buffer);

        if (hr != S_OK && hr != DXGI_ERROR_DEVICE_REMOVED)
            InterlockedIncrement(&race->unexpected);
        if (WineD3D11On12DestroyBufferV1(&buffer) != S_OK
                || WineD3D11On12SetViewportV1(race->device, &viewport) != S_OK
                || WineD3D11On12DrawAdapterDeviceV1(race->device, 3, 0) != S_OK)
            InterlockedIncrement(&race->unexpected);
    }
    return 0;
}

static HRESULT open_adapter(struct mock_device *device, struct mock_queue *queue,
        WineD3D11On12AdapterDevice *out)
{
    IUnknown *queues[1];

    mock_device_init(device);
    device->support_device1 = 1;
    mock_queue_init(queue, device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue->ID3D12CommandQueue_iface;
    out->size = sizeof(*out);
    return WineD3D11On12OpenAdapterV1((IUnknown *)&device->ID3D12Device_iface,
            queues, 1, 0, out);
}

int main(void)
{
    struct mock_device device, second_device;
    struct mock_queue queue, second_queue;
    WineD3D11On12AdapterDevice owner = {0}, second = {0};
    WineD3D11On12Buffer buffer = {.size = sizeof(buffer)}, late = {.size = sizeof(late)};
    WineD3D11On12Texture2D target = {.size = sizeof(target)}, staging = {.size = sizeof(staging)};
    WineD3D11On12Texture2D refused = {.size = sizeof(refused)};
    WineD3D11On12RenderTargetView view = {.size = sizeof(view)}, refused_view = {.size = sizeof(refused_view)};
    WineD3D11On12PipelineState blend = {.size = sizeof(blend)}, refused_blend = {.size = sizeof(refused_blend)};
    D3D11_BUFFER_DESC buffer_desc = {0};
    D3D11_TEXTURE2D_DESC texture_desc = {0};
    D3D11_BLEND_DESC blend_desc = {0};
    D3D11_VIEWPORT viewport = {0, 0, 8, 8, 0, 1};
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    const FLOAT color[4] = {1, 0, 0, 1};
    LONG created, destroyed, views_created, views_destroyed, before;
    BOOL submitted = TRUE;

    if (FAILED(open_adapter(&device, &queue, &owner)) || !owner.runtimeState)
    {
        puts("[fail] the adapter did not open; the mock may have rejected a "
                "null pfnEscapeCb");
        return 1;
    }
    if (!resolve())
    {
        puts("[fail] the mock driver's removal exports are missing");
        return 1;
    }

    owner.deviceFuncs->pfnCalcPrivateBlendStateSize = calc_blend;
    owner.deviceFuncs->pfnCreateBlendState = create_blend;
    owner.deviceFuncs->pfnDestroyBlendState = destroy_blend;
    owner.deviceFuncs->pfnSetBlendState = bind_blend;

    buffer_desc.ByteWidth = 64;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    texture_desc.Width = texture_desc.Height = 8;
    texture_desc.MipLevels = texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    /* Objects that must outlive the removal and still be destroyable. */
    CHECK(WineD3D11On12CreateBufferV1(&owner, &buffer_desc, NULL, &buffer) == S_OK);
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &texture_desc, NULL, &target) == S_OK);
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &view) == S_OK);
    CHECK(WineD3D11On12CreateBlendStateV1(&owner, &blend_desc, &blend) == S_OK);
    texture_desc.BindFlags = 0;
    texture_desc.Usage = D3D11_USAGE_STAGING;
    texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &texture_desc, NULL, &staging) == S_OK);
    CHECK(WineD3D11On12GetDeviceRemovedReasonV1(&owner) == S_OK);
    CHECK(WineD3D11On12GetDeviceRemovedReasonV1(NULL) == E_INVALIDARG);

    /* The escape is answered, and answers from D3D12: with D3D12 healthy a
     * status query latches nothing, wherever it comes from. */
    before = escape_count();
    CHECK(report_device(E_OUTOFMEMORY) == S_OK);
    CHECK(escape_count() == before + 1);
    CHECK(WineD3D11On12GetDeviceRemovedReasonV1(&owner) == S_OK);

    /* WASSTILLDRAWING is the GPU being busy, not the device failing: no
     * escape, and nothing latched even from the worker. */
    before = escape_count();
    CHECK(report_worker(WAS_STILL_DRAWING) == S_FALSE);
    CHECK(escape_count() == before);
    CHECK(WineD3D11On12GetDeviceRemovedReasonV1(&owner) == S_OK);
    CHECK(WineD3D11On12CreateBufferV1(&owner, &buffer_desc, NULL, &late) == S_OK);
    CHECK(WineD3D11On12DestroyBufferV1(&late) == S_OK);

    /* A worker error that lands while an export is inside the driver is not
     * that export's error.  With one shared slot SetViewport returned it. */
    worker_during_viewport(E_OUTOFMEMORY);
    CHECK(WineD3D11On12SetViewportV1(&owner, &viewport) == S_OK);
    CHECK(WineD3D11On12GetDeviceRemovedReasonV1(&owner) == DXGI_ERROR_DRIVER_INTERNAL_ERROR);

    /* First error wins: a later D3D12 removal does not rewrite the reason. */
    InterlockedExchange(&device.removed_reason, DXGI_ERROR_DEVICE_HUNG);
    CHECK(report_worker(E_FAIL) == S_OK);
    CHECK(WineD3D11On12GetDeviceRemovedReasonV1(&owner) == DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    InterlockedExchange(&device.removed_reason, S_OK);

    /* Creation and map fail, with outputs cleared. */
    late.hDrvResource = late.runtimeState = (void *)1;
    CHECK(WineD3D11On12CreateBufferV1(&owner, &buffer_desc, NULL, &late) == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(!late.hDrvResource && !late.runtimeState);
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &texture_desc, NULL, &refused) == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(!refused.hDrvResource && !refused.runtimeState);
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &refused_view) == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(!refused_view.hDrvView && !refused_view.runtimeState);
    CHECK(WineD3D11On12CreateBlendStateV1(&owner, &blend_desc, &refused_blend) == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(!refused_blend.hDrvState && !refused_blend.runtimeState);
    CHECK(WineD3D11On12MapTexture2DV1(&owner, &staging, 0, D3D11_MAP_READ, 0, &mapped) == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(!mapped.pData);

    /* Binds and draws succeed without reaching the driver, as D3D11 drops
     * work on a removed device.  Bad handles are still rejected. */
    before = draw_count();
    CHECK(WineD3D11On12SetRenderTargetV1(&owner, &view) == S_OK);
    CHECK(WineD3D11On12ClearRenderTargetV1(&owner, &view, color) == S_OK);
    CHECK(WineD3D11On12SetBlendStateV1(&owner, &blend, NULL, ~0u) == S_OK);
    CHECK(WineD3D11On12DrawAdapterDeviceV1(&owner, 3, 0) == S_OK);
    CHECK(WineD3D11On12DispatchDrawV1(&owner, WINE_D3D11ON12_DRAW_INSTANCED, 3, 1, 0, 0, 0) == S_OK);
    CHECK(WineD3D11On12DispatchDrawV1(&owner, 99, 3, 1, 0, 0, 0) == E_INVALIDARG);
    CHECK(WineD3D11On12SetRenderTargetV1(&owner, &refused_view) == E_INVALIDARG);
    CHECK(draw_count() == before);
    CHECK(blend_binds == 0);
    before = flush_count();
    CHECK(WineD3D11On12FlushAdapterDeviceV1(&owner, 0, 0, &submitted) == S_OK);
    CHECK(!submitted && flush_count() == before);

    /* Destruction still reaches the driver, exactly once. */
    resource_counts(NULL, &destroyed, NULL);
    before = destroyed;
    CHECK(WineD3D11On12DestroyBufferV1(&buffer) == S_OK);
    resource_counts(NULL, &destroyed, NULL);
    CHECK(destroyed == before + 1);
    CHECK(WineD3D11On12DestroyBufferV1(&buffer) == S_OK);
    resource_counts(NULL, &destroyed, NULL);
    CHECK(destroyed == before + 1);
    CHECK(WineD3D11On12DestroyRenderTargetViewV1(&view) == S_OK);
    CHECK(WineD3D11On12DestroyPipelineStateV1(&blend) == S_OK);
    CHECK(WineD3D11On12DestroyPipelineStateV1(&blend) == S_OK);
    CHECK(blend_destroys == 1);

    /* A second device on a second D3D12 device is unaffected, and its own
     * D3D12 removal is what the escape latches -- ahead of the generic
     * reason a worker failure would otherwise produce. */
    CHECK(SUCCEEDED(open_adapter(&second_device, &second_queue, &second)));
    CHECK(WineD3D11On12GetDeviceRemovedReasonV1(&second) == S_OK);
    InterlockedExchange(&second_device.removed_reason, DXGI_ERROR_DEVICE_HUNG);
    CHECK(WineD3D11On12GetDeviceRemovedReasonV1(&second) == DXGI_ERROR_DEVICE_HUNG);
    CHECK(WineD3D11On12CreateBufferV1(&second, &buffer_desc, NULL, &late) == S_OK);
    CHECK(report_worker(E_FAIL) == S_OK);
    InterlockedExchange(&second_device.removed_reason, S_OK);
    CHECK(WineD3D11On12GetDeviceRemovedReasonV1(&second) == DXGI_ERROR_DEVICE_HUNG);
    CHECK(WineD3D11On12CreateBufferV1(&second, &buffer_desc, NULL, &buffer) == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(WineD3D11On12GetDeviceRemovedReasonV1(&owner) == DXGI_ERROR_DRIVER_INTERNAL_ERROR);

    /* Under concurrency: eight calling threads and a worker. */
    {
        struct mock_device race_device;
        struct mock_queue race_queue;
        WineD3D11On12AdapterDevice racer = {0};
        struct race race = {&racer, CreateEventW(NULL, TRUE, FALSE, NULL), 0};
        HANDLE threads[8];
        UINT i, started = 0;

        CHECK(SUCCEEDED(open_adapter(&race_device, &race_queue, &racer)) && race.start);
        if (!racer.runtimeState || !race.start)
            return 1;
        for (i = 0; i < 8; ++i)
            if ((threads[started] = CreateThread(NULL, 0, race_thread, &race, 0, NULL)))
                ++started;
        SetEvent(race.start);
        for (i = 0; i < 16; ++i)
        {
            Sleep(1);
            CHECK(report_worker(E_OUTOFMEMORY) == S_OK);
        }
        CHECK(WaitForMultipleObjects(started, threads, TRUE, 30000) == WAIT_OBJECT_0);
        for (i = 0; i < started; ++i)
            CloseHandle(threads[i]);
        CloseHandle(race.start);
        CHECK(started == 8 && race.unexpected == 0);
        CHECK(WineD3D11On12GetDeviceRemovedReasonV1(&racer) == DXGI_ERROR_DRIVER_INTERNAL_ERROR);
        CHECK(WineD3D11On12CloseAdapterDeviceV1(&racer) == S_OK);
        CHECK(race_device.refcount == 1 && race_queue.refcount == 1);
    }

    /* Teardown after removal releases everything still alive. */
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&second) == S_OK);
    CHECK(!late.runtimeState && !late.hDrvResource);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&owner) == S_OK);
    CHECK(!target.runtimeState && !staging.runtimeState);
    CHECK(WineD3D11On12DestroyTexture2DV1(&target) == S_OK);
    CHECK(WineD3D11On12DestroyBufferV1(&late) == S_OK);
    resource_counts(&created, &destroyed, NULL);
    CHECK(created == destroyed);
    frame_counts(&views_created, &views_destroyed, NULL, NULL, NULL);
    CHECK(views_created == views_destroyed);
    CHECK(device.refcount == 1 && queue.refcount == 1);
    CHECK(second_device.refcount == 1 && second_queue.refcount == 1);

    printf("[%s] device errors, removal latching and removed-device behaviour\n",
            failures ? "fail" : " ok ");
    return !!failures;
}
