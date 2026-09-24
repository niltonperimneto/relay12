/* SPDX-License-Identifier: GPL-3.0-only
 * Core-owned frame lifecycle, fault injection and byte-exact mock readback.
 * This is a dispatch/ownership test; the application triangle proves the GPU.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include "d3d11on12core.h"
#include "ddi/wine_d3d11ddi.h"
#include "d3d11on12mocks.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("[fail] line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
/* For the soak, which has to report and then stop rather than keep looping
 * on a broken state: CHECK names its own expression, which is no help when
 * the interesting part is which cycle gave up and why. */
#define FAIL(message) do { printf("[fail] line %d: %s\n", __LINE__, message); ++failures; } while (0)

/* Divisible by three, so the soak's three injected failure kinds each get
 * exactly a third of the iterations and the expected counts stay exact. */
#define SOAK_ITERATIONS 255

int main(void)
{
    struct mock_device device;
    struct mock_queue queue;
    WineD3D11On12AdapterDevice owner = {.size = sizeof(owner)}, other = {.size = sizeof(other)};
    WineD3D11On12Texture2D target = {.size = sizeof(target)}, staging = {.size = sizeof(staging)};
    WineD3D11On12RenderTargetView view = {.size = sizeof(view)}, failed_view = {.size = sizeof(failed_view)};
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT viewport = {0, 0, 8, 4, 0, 1};
    const FLOAT blue[4] = {0, 0, 1, 1};
    /* Out of range on two channels and fractional on a third.  Neither the
     * DDI nor the core clamps -- these four floats are the application's --
     * so saturation and rounding are the driver's to get right, and casting
     * an unclamped float to a byte is undefined rather than merely wrong. */
    const FLOAT extremes[4] = {-1.0f, 0.5f, 2.0f, 1.0f};
    IUnknown *queues[1];
    HMODULE driver;
    void (WINAPI *fail_next_view)(void);
    void (WINAPI *fail_next_map)(void);
    void (WINAPI *fail_next_resource)(void);
    void (WINAPI *counts)(LONG *, LONG *, LONG *, LONG *, int *);
    void (WINAPI *resource_counts)(LONG *, LONG *, int *);
    LONG created, destroyed, maps, unmaps;
    LONG res_created, res_destroyed;
    LONG base_created, base_destroyed, base_maps, base_unmaps;
    LONG base_res_created, base_res_destroyed;
    int bad_frame, bad_resource;
    LONG soak;
    BYTE *pixels;
    HRESULT hr;
    mock_device_init(&device);
    device.support_device1 = 1;
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &owner) == S_OK);
    if (!owner.runtimeState) return 1;
    driver = GetModuleHandleW(L"d3d11on12.dll");
    /* One injector per site: the failure has to be aimed at the call under
     * test, or a reordering elsewhere would silently retarget it. */
    fail_next_view = (void *)(UINT_PTR)GetProcAddress(driver,
            "WineD3D11On12MockDriverFailNextView");
    fail_next_map = (void *)(UINT_PTR)GetProcAddress(driver,
            "WineD3D11On12MockDriverFailNextMap");
    fail_next_resource = (void *)(UINT_PTR)GetProcAddress(driver,
            "WineD3D11On12MockDriverFailNextResource");
    counts = (void *)(UINT_PTR)GetProcAddress(driver,
            "WineD3D11On12MockDriverGetFrameCounts");
    /* Resource counts as well as frame counts: a view whose creation failed
     * must not leave its resource holding a view count, and the only way to
     * see that is that the resource never reaches DestroyResource. */
    resource_counts = (void *)(UINT_PTR)GetProcAddress(driver,
            "WineD3D11On12MockDriverGetResourceCounts");
    CHECK(fail_next_view && fail_next_map && fail_next_resource && counts
            && resource_counts);
    if (!fail_next_view || !fail_next_map || !fail_next_resource || !counts
            || !resource_counts) return 1;
    CHECK(WineD3D11On12CheckFrameSupportV1(&owner) == S_OK);
    {
        PFND3D10DDI_RESOURCEMAP saved = owner.deviceFuncs->pfnStagingResourceMap;
        owner.deviceFuncs->pfnStagingResourceMap = NULL;
        CHECK(WineD3D11On12CheckFrameSupportV1(&owner) == DXGI_ERROR_UNSUPPORTED);
        owner.deviceFuncs->pfnStagingResourceMap = saved;
    }
    desc.Width = 8; desc.Height = 4; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &target) == S_OK);
    fail_next_view();
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &failed_view) == E_OUTOFMEMORY);
    CHECK(!failed_view.runtimeState && !failed_view.hDrvView);
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &view) == S_OK);
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &other) == S_OK);
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&other, &target, NULL, &failed_view) == E_INVALIDARG);
    CHECK(WineD3D11On12SetRenderTargetV1(&other, &view) == E_INVALIDARG);
    CHECK(WineD3D11On12SetRenderTargetV1(&owner, &view) == S_OK);
    CHECK(WineD3D11On12SetViewportV1(&owner, &viewport) == S_OK);
    CHECK(WineD3D11On12ClearRenderTargetV1(&owner, &view, blue) == S_OK);
    CHECK(WineD3D11On12DrawAdapterDeviceV1(&owner, 3, 0) == S_OK);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &staging) == S_OK);
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &staging, NULL, &failed_view) == E_INVALIDARG);
    CHECK(WineD3D11On12CopyTexture2DV1(&owner, &staging, &target) == S_OK);
    CHECK(WineD3D11On12CopyTexture2DV1(&other, &staging, &target) == E_INVALIDARG);
    fail_next_map();
    memset(&mapped, 0xcc, sizeof(mapped));
    CHECK(WineD3D11On12MapTexture2DV1(&owner, &staging, 0, D3D11_MAP_READ, 0, &mapped) == DXGI_ERROR_DEVICE_REMOVED);
    CHECK(!mapped.pData && !mapped.RowPitch && !mapped.DepthPitch);
    CHECK(WineD3D11On12MapTexture2DV1(&owner, &staging, 1, D3D11_MAP_READ, 0, &mapped) == E_INVALIDARG);
    CHECK(WineD3D11On12MapTexture2DV1(&owner, &target, 0, D3D11_MAP_READ, 0, &mapped) == DXGI_ERROR_UNSUPPORTED);
    hr = WineD3D11On12MapTexture2DV1(&owner, &staging, 0, D3D11_MAP_READ, 0, &mapped);
    CHECK(hr == S_OK && mapped.pData && mapped.RowPitch == 32);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        pixels = mapped.pData;
        CHECK(!memcmp(pixels, "\x00\x00\xff\xff", 4));
        CHECK(!memcmp(pixels + 2*mapped.RowPitch + 4*4, "\xff\x00\x00\xff", 4));
        CHECK(WineD3D11On12MapTexture2DV1(&owner, &staging, 0, D3D11_MAP_READ, 0, &mapped) == DXGI_ERROR_INVALID_CALL);
        CHECK(WineD3D11On12CopyTexture2DV1(&owner, &staging, &target) == DXGI_ERROR_INVALID_CALL);
        CHECK(WineD3D11On12DestroyTexture2DV1(&staging) == DXGI_ERROR_INVALID_CALL);
        CHECK(WineD3D11On12UnmapTexture2DV1(&owner, &staging, 0) == S_OK);
        CHECK(WineD3D11On12UnmapTexture2DV1(&owner, &staging, 0) == E_INVALIDARG);
    }
    /* The two injectors are independent.  Armed against view creation, the
     * readback must still succeed -- one shared flag would have this map
     * consume the arming and fail here instead, and the view creation below
     * would then wrongly succeed. */
    fail_next_view();
    CHECK(WineD3D11On12MapTexture2DV1(&owner, &staging, 0, D3D11_MAP_READ, 0, &mapped) == S_OK);
    CHECK(WineD3D11On12UnmapTexture2DV1(&owner, &staging, 0) == S_OK);
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &failed_view) == E_OUTOFMEMORY);
    /* Channel conversion, read back through the same path as the frame. */
    CHECK(WineD3D11On12ClearRenderTargetV1(&owner, &view, extremes) == S_OK);
    CHECK(WineD3D11On12CopyTexture2DV1(&owner, &staging, &target) == S_OK);
    hr = WineD3D11On12MapTexture2DV1(&owner, &staging, 0, D3D11_MAP_READ, 0, &mapped);
    CHECK(hr == S_OK && mapped.pData);
    if (SUCCEEDED(hr) && mapped.pData)
    {
        CHECK(!memcmp(mapped.pData, "\x00\x80\xff\xff", 4));
        CHECK(WineD3D11On12UnmapTexture2DV1(&owner, &staging, 0) == S_OK);
    }
    CHECK(WineD3D11On12DestroyTexture2DV1(&target) == S_OK);
    CHECK(!target.runtimeState);
    CHECK(WineD3D11On12ClearRenderTargetV1(&owner, &view, blue) == S_OK);
    CHECK(WineD3D11On12DestroyRenderTargetViewV1(&view) == S_OK);
    CHECK(WineD3D11On12DestroyRenderTargetViewV1(&view) == S_OK);
    CHECK(WineD3D11On12SetRenderTargetV1(&owner, &view) == E_INVALIDARG);
    CHECK(WineD3D11On12DestroyTexture2DV1(&staging) == S_OK);
    CHECK(WineD3D11On12DestroyTexture2DV1(&staging) == S_OK);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&other) == S_OK);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&owner) == S_OK);
    counts(&created, &destroyed, &maps, &unmaps, &bad_frame);
    CHECK(created == 1 && destroyed == 1 && maps == 3 && unmaps == 3);
    /* The driver saw no malformed frame argument.  Without this the checks
     * above pass on a core that hands the driver a handle it never created:
     * the mock records that and carries on rather than crashing. */
    CHECK(!bad_frame);
    CHECK(device.refcount == 1 && queue.refcount == 1);
    /* Teardown owns outstanding maps and views; late public destruction is
     * inert and must never reach freed driver storage. */
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &owner) == S_OK);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &target) == S_OK);
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &view) == S_OK);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &staging) == S_OK);
    CHECK(WineD3D11On12MapTexture2DV1(&owner, &staging, 0, D3D11_MAP_READ, 0, &mapped) == S_OK);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&owner) == S_OK);
    CHECK(WineD3D11On12DestroyRenderTargetViewV1(&view) == S_OK);
    CHECK(WineD3D11On12DestroyTexture2DV1(&target) == S_OK);
    CHECK(WineD3D11On12DestroyTexture2DV1(&staging) == S_OK);
    counts(&created, &destroyed, &maps, &unmaps, &bad_frame);
    CHECK(created == 2 && destroyed == 2 && maps == 4 && unmaps == 4);
    CHECK(!bad_frame);
    CHECK(device.refcount == 1 && queue.refcount == 1);

    /* Partial publication.  Both creators here link their object into the
     * core's registries and allocate side tables before the driver is asked
     * to agree, and both write the caller's handle only once it has.  So a
     * rejected creation has to leave nothing published and nothing linked,
     * and the evidence that the registries really were restored is that the
     * same handle then creates, binds and destroys normally.
     *
     * Counter deltas rather than totals from here down: a phase that has to
     * know every earlier phase's arithmetic is a phase that gets edited into
     * agreement with whatever the code currently does. */
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &owner) == S_OK);
    counts(&base_created, &base_destroyed, &base_maps, &base_unmaps, &bad_frame);
    resource_counts(&base_res_created, &base_res_destroyed, &bad_resource);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    target.size = sizeof(target);
    fail_next_resource();
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &target) == E_OUTOFMEMORY);
    CHECK(!target.hDrvResource && !target.runtimeState);
    CHECK(target.size == sizeof(target));
    CHECK(WineD3D11On12DestroyTexture2DV1(&target) == S_OK);
    /* An unpublished texture is not a view's resource. */
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &view) == E_INVALIDARG);
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &target) == S_OK);
    /* A rejected view must release the view count it took, or the texture
     * below would never orphan and the driver would never be told to
     * destroy it -- which the resource delta at the end of this phase is
     * what actually catches. */
    failed_view.size = sizeof(failed_view);
    fail_next_view();
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &failed_view) == E_OUTOFMEMORY);
    CHECK(!failed_view.hDrvView && !failed_view.runtimeState);
    CHECK(failed_view.size == sizeof(failed_view));
    CHECK(WineD3D11On12DestroyRenderTargetViewV1(&failed_view) == S_OK);
    CHECK(WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &view) == S_OK);
    CHECK(WineD3D11On12SetRenderTargetV1(&owner, &view) == S_OK);
    CHECK(WineD3D11On12ClearRenderTargetV1(&owner, &view, blue) == S_OK);
    CHECK(WineD3D11On12DestroyRenderTargetViewV1(&view) == S_OK);
    CHECK(WineD3D11On12DestroyTexture2DV1(&target) == S_OK);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&owner) == S_OK);
    counts(&created, &destroyed, &maps, &unmaps, &bad_frame);
    resource_counts(&res_created, &res_destroyed, &bad_resource);
    /* One view created and destroyed: the rejected one is neither.  Two
     * resource creation calls for one texture -- the rejected attempt and
     * the accepted one -- and exactly one destroy, for the accepted one. */
    CHECK(created - base_created == 1 && destroyed - base_destroyed == 1);
    CHECK(res_created - base_res_created == 2
            && res_destroyed - base_res_destroyed == 1);
    CHECK(!bad_frame && !bad_resource);
    CHECK(device.refcount == 1 && queue.refcount == 1);

    /* Soak.  Every iteration builds and tears down the whole frame path --
     * two resources, a view, a binding, a clear, a draw, a copy and a
     * readback -- across four registries under one lock.  A leaked node, a
     * stale registry link or an off-by-one in the view count does not fail
     * the first cycle; it accumulates, and shows up as counter drift or as
     * a readback that stops matching.
     *
     * Every iteration is verified, not just the last: a soak that checks
     * only the end state cannot distinguish a run that was correct
     * throughout from one that was wrong and recovered.  One injected
     * failure per iteration, cycling through the three kinds, so the
     * recovery paths are soaked too -- an unbalanced failure path is where
     * a leak actually hides. */
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &owner) == S_OK);
    counts(&base_created, &base_destroyed, &base_maps, &base_unmaps, &bad_frame);
    resource_counts(&base_res_created, &base_res_destroyed, &bad_resource);
    for (soak = 0; soak < SOAK_ITERATIONS; ++soak)
    {
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = 0;
        if (soak % 3 == 0)
        {
            fail_next_resource();
            if (WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &target) != E_OUTOFMEMORY
                    || target.runtimeState)
            {
                FAIL("a rejected soak texture published a handle");
                break;
            }
        }
        if (WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &target) != S_OK)
        {
            FAIL("soak render target creation");
            break;
        }
        if (soak % 3 == 1)
        {
            fail_next_view();
            if (WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &failed_view) != E_OUTOFMEMORY
                    || failed_view.runtimeState)
            {
                FAIL("a rejected soak view published a handle");
                break;
            }
        }
        if (WineD3D11On12CreateRenderTargetViewV1(&owner, &target, NULL, &view) != S_OK
                || WineD3D11On12SetRenderTargetV1(&owner, &view) != S_OK
                || WineD3D11On12SetViewportV1(&owner, &viewport) != S_OK
                || WineD3D11On12ClearRenderTargetV1(&owner, &view, blue) != S_OK
                || WineD3D11On12DrawAdapterDeviceV1(&owner, 3, 0) != S_OK)
        {
            FAIL("soak frame submission");
            break;
        }
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &staging) != S_OK
                || WineD3D11On12CopyTexture2DV1(&owner, &staging, &target) != S_OK)
        {
            FAIL("soak readback staging");
            break;
        }
        if (soak % 3 == 2)
        {
            fail_next_map();
            if (WineD3D11On12MapTexture2DV1(&owner, &staging, 0, D3D11_MAP_READ, 0,
                        &mapped) != DXGI_ERROR_DEVICE_REMOVED
                    || mapped.pData)
            {
                FAIL("a rejected soak map published a pointer");
                break;
            }
        }
        if (WineD3D11On12MapTexture2DV1(&owner, &staging, 0, D3D11_MAP_READ, 0, &mapped) != S_OK
                || !mapped.pData || mapped.RowPitch != 32)
        {
            FAIL("soak readback map");
            break;
        }
        pixels = mapped.pData;
        /* The cleared corner and the drawn centre, every cycle: the clear
         * proves the view still resolves to this iteration's texture, and
         * the centre proves the draw went to the bound target. */
        if (memcmp(pixels, "\x00\x00\xff\xff", 4)
                || memcmp(pixels + 2 * mapped.RowPitch + 4 * 4, "\xff\x00\x00\xff", 4))
        {
            FAIL("soak readback contents");
            break;
        }
        if (WineD3D11On12UnmapTexture2DV1(&owner, &staging, 0) != S_OK
                || WineD3D11On12DestroyRenderTargetViewV1(&view) != S_OK
                || WineD3D11On12DestroyTexture2DV1(&target) != S_OK
                || WineD3D11On12DestroyTexture2DV1(&staging) != S_OK)
        {
            FAIL("soak teardown");
            break;
        }
    }
    CHECK(soak == SOAK_ITERATIONS);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&owner) == S_OK);
    counts(&created, &destroyed, &maps, &unmaps, &bad_frame);
    resource_counts(&res_created, &res_destroyed, &bad_resource);
    /* Exact, not merely balanced.  One view and one readback per iteration,
     * two resources per iteration plus the third of them that also made a
     * rejected attempt, and nothing outstanding at the close -- if teardown
     * had had anything left to clean up, these would not land on the nose. */
    CHECK(created - base_created == SOAK_ITERATIONS
            && destroyed - base_destroyed == SOAK_ITERATIONS);
    CHECK(maps - base_maps == SOAK_ITERATIONS
            && unmaps - base_unmaps == SOAK_ITERATIONS);
    CHECK(res_created - base_res_created
                    == 2 * SOAK_ITERATIONS + SOAK_ITERATIONS / 3
            && res_destroyed - base_res_destroyed == 2 * SOAK_ITERATIONS);
    CHECK(!bad_frame && !bad_resource);
    CHECK(device.refcount == 1 && queue.refcount == 1);
    printf("[%s] first-frame lifecycle: %d failures\n", failures ? "fail" : " ok ", failures);
    return !!failures;
}
