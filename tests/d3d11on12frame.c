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
    IUnknown *queues[1];
    HMODULE driver;
    void (WINAPI *inject)(void);
    void (WINAPI *counts)(LONG *, LONG *, LONG *, LONG *);
    LONG created, destroyed, maps, unmaps;
    BYTE *pixels;
    HRESULT hr;
    mock_device_init(&device);
    device.support_device1 = 1;
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &owner) == S_OK);
    if (!owner.runtimeState) return 1;
    driver = GetModuleHandleW(L"d3d11on12.dll");
    inject = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockFailFrame");
    counts = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockFrameCounts");
    CHECK(inject && counts);
    if (!inject || !counts) return 1;
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
    inject();
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
    inject();
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
    counts(&created, &destroyed, &maps, &unmaps);
    CHECK(created == 1 && destroyed == 1 && maps == 1 && unmaps == 1);
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
    counts(&created, &destroyed, &maps, &unmaps);
    CHECK(created == 2 && destroyed == 2 && maps == 2 && unmaps == 2);
    CHECK(device.refcount == 1 && queue.refcount == 1);
    printf("[%s] first-frame lifecycle: %d failures\n", failures ? "fail" : " ok ", failures);
    return !!failures;
}
