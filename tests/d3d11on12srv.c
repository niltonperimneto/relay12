/* SPDX-License-Identifier: GPL-3.0-only
 * SRV dispatch, atomic binding validation and texture/view ownership. */
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
    IUnknown *queues[1];
    WineD3D11On12AdapterDevice owner = {.size = sizeof(owner)}, other = {.size = sizeof(other)};
    WineD3D11On12Texture2D texture = {.size = sizeof(texture)};
    WineD3D11On12ShaderResourceView srv = {.size = sizeof(srv)}, failed = {.size = sizeof(failed)};
    WineD3D11On12ShaderResourceView *views[2] = {NULL, &srv};
    D3D11_TEXTURE2D_DESC desc = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    LONG created, destroyed, bound, before;
    UINT start, count;
    void *last;
    HMODULE driver;
    void (WINAPI *fail_next)(void);
    void (WINAPI *counts)(LONG *, LONG *, LONG *, UINT *, UINT *, void **);
    mock_device_init(&device); device.support_device1 = 1;
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &owner) == S_OK);
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &other) == S_OK);
    if (!owner.runtimeState || !other.runtimeState) return 1;
    driver = GetModuleHandleW(L"d3d11on12.dll");
    fail_next = (void *)GetProcAddress(driver, "WineD3D11On12MockDriverFailNextSRV");
    counts = (void *)GetProcAddress(driver, "WineD3D11On12MockDriverGetSRVCounts");
    if (!fail_next || !counts) return 1;
    desc.Width = 8; desc.Height = 4; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &texture) == S_OK);
    sd.Format = desc.Format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 2;
    CHECK(WineD3D11On12CreateShaderResourceViewV1(&owner, &texture, &sd, &failed) == E_INVALIDARG);
    CHECK(!failed.runtimeState && !failed.hDrvView);
    fail_next();
    CHECK(WineD3D11On12CreateShaderResourceViewV1(&owner, &texture, NULL, &failed) == E_OUTOFMEMORY);
    CHECK(!failed.runtimeState && !failed.hDrvView);
    CHECK(WineD3D11On12DestroyShaderResourceViewV1(&failed) == S_OK);
    CHECK(WineD3D11On12CreateShaderResourceViewV1(&other, &texture, NULL, &failed) == E_INVALIDARG);
    CHECK(WineD3D11On12CreateShaderResourceViewV1(&owner, &texture, NULL, &srv) == S_OK);
    CHECK(WineD3D11On12SetPixelShaderResourcesV1(&owner, 7, 2, views) == S_OK);
    counts(&created, &destroyed, &bound, &start, &count, &last);
    CHECK(created == 1 && destroyed == 0 && start == 7 && count == 2 && last == srv.hDrvView);
    before = bound;
    CHECK(WineD3D11On12SetPixelShaderResourcesV1(&other, 7, 2, views) == E_INVALIDARG);
    CHECK(WineD3D11On12SetPixelShaderResourcesV1(&owner, 128, 1, views) == E_INVALIDARG);
    CHECK(WineD3D11On12SetPixelShaderResourcesV1(&owner, 0, 1, NULL) == E_INVALIDARG);
    views[1] = &failed;
    CHECK(WineD3D11On12SetPixelShaderResourcesV1(&owner, 0, 2, views) == E_INVALIDARG);
    counts(&created, &destroyed, &bound, &start, &count, &last);
    CHECK(before == bound); /* Invalid batches cannot partially clear bindings. */
    CHECK(WineD3D11On12DestroyTexture2DV1(&texture) == S_OK);
    CHECK(!texture.runtimeState); /* The view keeps the driver's texture alive. */
    CHECK(WineD3D11On12DestroyShaderResourceViewV1(&srv) == S_OK);
    CHECK(WineD3D11On12DestroyShaderResourceViewV1(&srv) == S_OK);
    counts(&created, &destroyed, &bound, &start, &count, &last);
    CHECK(created == destroyed);
    views[0] = &srv;
    CHECK(WineD3D11On12SetPixelShaderResourcesV1(&owner, 0, 1, views) == E_INVALIDARG);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &texture) == S_OK);
    CHECK(WineD3D11On12MapTexture2DV1(&owner, &texture, 0, D3D11_MAP_READ, 0, &mapped) == S_OK);
    CHECK(mapped.pData && mapped.RowPitch >= desc.Width * 4);
    CHECK(WineD3D11On12UnmapTexture2DV1(&owner, &texture, 0) == S_OK);
    CHECK(WineD3D11On12DestroyTexture2DV1(&texture) == S_OK);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    CHECK(WineD3D11On12CreateTexture2DV1(&owner, &desc, NULL, &texture) == S_OK);
    CHECK(WineD3D11On12CreateShaderResourceViewV1(&owner, &texture, NULL, &srv) == S_OK);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&other) == S_OK);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&owner) == S_OK);
    CHECK(!srv.runtimeState && !srv.hDrvView && !texture.runtimeState);
    CHECK(WineD3D11On12DestroyShaderResourceViewV1(&srv) == S_OK);
    CHECK(WineD3D11On12DestroyTexture2DV1(&texture) == S_OK);
    counts(&created, &destroyed, &bound, &start, &count, &last);
    CHECK(created == destroyed);
    printf("[%s] SRV failures, bindings and idempotent destruction\n", failures ? "fail" : " ok ");
    return !!failures;
}
