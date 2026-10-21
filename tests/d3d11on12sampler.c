/* SPDX-License-Identifier: GPL-3.0-only
 * Samplers as pipeline-state kind 4 -- descriptor translation, one sampler in
 * many slots and both stages, destruction while bound -- and scissor
 * rectangles with the DDI's clear-count arithmetic. */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "d3d11on12core.h"
#include "ddi/wine_d3d11ddi.h"
#include "d3d11on12mocks.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("[fail] line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void (WINAPI *record)(LONG *, LONG *, LONG *, D3D10_DDI_SAMPLER_DESC *, int *);
static void *(WINAPI *bound)(UINT, UINT);
static void (WINAPI *scissor)(LONG *, UINT *, UINT *, D3D10_DDI_RECT *);

static HRESULT open_adapter(struct mock_device *device, struct mock_queue *queue,
        WineD3D11On12AdapterDevice *out)
{
    IUnknown *queues[1];

    mock_device_init(device);
    device->support_device1 = 1;
    mock_queue_init(queue, device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue->ID3D12CommandQueue_iface;
    out->size = sizeof(*out);
    return WineD3D11On12OpenAdapterV1((IUnknown *)&device->ID3D12Device_iface, queues, 1, 0, out);
}

static LONG destroyed_count(void)
{
    LONG destroyed = 0;
    record(NULL, &destroyed, NULL, NULL, NULL);
    return destroyed;
}

static HRESULT bind_sampler(WineD3D11On12AdapterDevice *device, UINT stage, UINT slot,
        WineD3D11On12PipelineState *state)
{
    return WineD3D11On12SetSamplersV1(device, stage, slot, 1, &state);
}

int main(void)
{
    struct mock_device device, other_device;
    struct mock_queue queue, other_queue;
    WineD3D11On12AdapterDevice owner = {0}, other = {0};
    WineD3D11On12PipelineState sampler = {.size = sizeof(sampler)}, kept = {.size = sizeof(kept)};
    WineD3D11On12PipelineState refused = {.size = sizeof(refused)}, foreign = {.size = sizeof(foreign)};
    WineD3D11On12PipelineState copy, *states[2];
    D3D11_SAMPLER_DESC desc, bad_desc;
    D3D10_DDI_SAMPLER_DESC seen;
    D3D11_RECT rects[4] = {{0, 0, 4, 4}, {1, 2, 3, 4}, {5, 6, 7, 8}, {-1, -1, 9, 9}};
    D3D10_DDI_RECT seen_rects[16];
    LONG created, destroyed, binds, binds_before, calls;
    UINT count, clear;
    int bad;

    if (FAILED(open_adapter(&device, &queue, &owner)) || FAILED(open_adapter(&other_device, &other_queue, &other)))
    {
        puts("[fail] adapters did not open");
        return 1;
    }
    {
        HMODULE driver = GetModuleHandleW(L"d3d11on12.dll");
        record = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockDriverGetSamplerRecord");
        bound = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockDriverGetBoundSampler");
        scissor = (void *)(UINT_PTR)GetProcAddress(driver, "WineD3D11On12MockDriverGetScissorRecord");
        if (!record || !bound || !scissor)
        {
            puts("[fail] the mock driver's sampler exports are missing");
            return 1;
        }
    }

    /* Translation: fields carried over, ignored fields canonicalized. */
    memset(&desc, 0, sizeof(desc));
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    desc.MipLODBias = 0.5f;
    desc.MaxAnisotropy = 9;
    desc.ComparisonFunc = 0;
    desc.BorderColor[0] = 0.25f; desc.BorderColor[3] = 1.0f;
    desc.MinLOD = 0.0f; desc.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(WineD3D11On12CreateSamplerStateV1(&owner, &desc, &sampler) == S_OK);
    CHECK(sampler.kind == WINE_D3D11ON12_STATE_SAMPLER && sampler.hDrvState && sampler.runtimeState);
    record(&created, NULL, NULL, &seen, NULL);
    CHECK(created == 1 && seen.Filter == D3D11_FILTER_MIN_MAG_MIP_POINT);
    CHECK(seen.AddressU == D3D11_TEXTURE_ADDRESS_CLAMP && seen.AddressV == D3D11_TEXTURE_ADDRESS_WRAP
            && seen.AddressW == D3D11_TEXTURE_ADDRESS_BORDER);
    CHECK(seen.MipLODBias == 0.5f && seen.MaxAnisotropy == 1 && seen.ComparisonFunc == D3D11_COMPARISON_NEVER);
    CHECK(seen.BorderColor[0] == 0.25f && seen.BorderColor[3] == 1.0f && seen.MaxLOD == D3D11_FLOAT32_MAX);
    desc.Filter = D3D11_FILTER_COMPARISON_ANISOTROPIC;
    desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    CHECK(WineD3D11On12CreateSamplerStateV1(&owner, &desc, &kept) == S_OK);
    record(NULL, NULL, NULL, &seen, NULL);
    CHECK(seen.MaxAnisotropy == 9 && seen.ComparisonFunc == D3D11_COMPARISON_LESS_EQUAL);

    /* Rejected descriptors publish nothing and reach no driver slot. */
    record(&created, NULL, NULL, NULL, NULL);
#define REFUSE(field, value) do { bad_desc = desc; bad_desc.field = value; \
        refused.hDrvState = (void *)1; \
        CHECK(WineD3D11On12CreateSamplerStateV1(&owner, &bad_desc, &refused) == E_INVALIDARG); \
        CHECK(!refused.hDrvState && !refused.runtimeState); } while (0)
    REFUSE(AddressU, 0);
    REFUSE(AddressW, 6);
    REFUSE(Filter, 0x41);
    REFUSE(Filter, 0x200);
    REFUSE(ComparisonFunc, 9);
    REFUSE(MaxAnisotropy, 17);
    REFUSE(MipLODBias, INFINITY);
    REFUSE(MinLOD, NAN);
    REFUSE(BorderColor[2], NAN);
#undef REFUSE
    CHECK(WineD3D11On12CreateSamplerStateV1(&owner, NULL, &refused) == E_INVALIDARG);
    CHECK(WineD3D11On12CreateSamplerStateV1(&owner, &desc, &sampler) == E_INVALIDARG); /* live handle */
    CHECK(WineD3D11On12CreateSamplerStateV1(&other, &desc, &foreign) == S_OK);
    record(&destroyed, NULL, NULL, NULL, NULL);
    CHECK(destroyed == created + 1);

    /* One sampler in three slots across both stages. */
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_VERTEX, 0, &sampler) == S_OK);
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_PIXEL, 3, &sampler) == S_OK);
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_PIXEL, 15, &sampler) == S_OK);
    CHECK(bound(0, 0) == sampler.hDrvState && bound(1, 3) == sampler.hDrvState
            && bound(1, 15) == sampler.hDrvState);
    /* Wrong kind, another device's, bounds, stage: rejected before dispatch,
     * and a batch with one bad entry binds none of it. */
    record(NULL, NULL, &binds_before, NULL, NULL);
    copy = sampler; copy.kind = WINE_D3D11ON12_STATE_BLEND;
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_PIXEL, 0, &copy) == E_INVALIDARG);
    copy = sampler;
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_PIXEL, 0, &copy) == E_INVALIDARG);
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_PIXEL, 0, &foreign) == E_INVALIDARG);
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_PIXEL, 16, &sampler) == E_INVALIDARG);
    CHECK(bind_sampler(&owner, 3, 0, &sampler) == E_INVALIDARG);
    states[0] = &kept; states[1] = &foreign;
    CHECK(WineD3D11On12SetSamplersV1(&owner, WINE_D3D11ON12_SHADER_PIXEL, 0, 2, states) == E_INVALIDARG);
    CHECK(WineD3D11On12SetSamplersV1(&owner, WINE_D3D11ON12_SHADER_PIXEL, 0, 1, NULL) == E_INVALIDARG);
    record(NULL, NULL, &binds, NULL, NULL);
    CHECK(binds == binds_before && !bound(1, 0));

    /* Destroyed while bound: the handle goes inert at once, the driver
     * object survives until its last slot lets go. */
    CHECK(WineD3D11On12DestroyPipelineStateV1(&sampler) == S_OK);
    CHECK(!sampler.kind && !sampler.hDrvState && !sampler.runtimeState);
    CHECK(WineD3D11On12DestroyPipelineStateV1(&sampler) == S_OK);
    destroyed = destroyed_count();
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_PIXEL, 0, &sampler) == E_INVALIDARG);
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_VERTEX, 0, NULL) == S_OK);
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_PIXEL, 3, &kept) == S_OK);
    CHECK(destroyed_count() == destroyed && bound(1, 15));
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_PIXEL, 15, NULL) == S_OK);
    CHECK(destroyed_count() == destroyed + 1);
    record(NULL, NULL, NULL, NULL, &bad);
    CHECK(!bad);

    /* Scissor: rectangles carried as RECT, and ClearRects is the previous
     * count minus the new one when it shrinks. */
    CHECK(WineD3D11On12SetScissorRectsV1(&owner, 4, rects) == S_OK);
    scissor(&calls, &count, &clear, seen_rects);
    CHECK(calls == 1 && count == 4 && clear == 0 && !memcmp(seen_rects, rects, sizeof(rects)));
    CHECK(WineD3D11On12SetScissorRectsV1(&owner, 1, rects + 1) == S_OK);
    scissor(&calls, &count, &clear, seen_rects);
    CHECK(count == 1 && clear == 3 && seen_rects[0].left == 1 && seen_rects[0].bottom == 4);
    CHECK(WineD3D11On12SetScissorRectsV1(&owner, 2, rects) == S_OK);
    scissor(NULL, &count, &clear, NULL);
    CHECK(count == 2 && clear == 0);
    CHECK(WineD3D11On12SetScissorRectsV1(&owner, 0, NULL) == S_OK);
    scissor(&calls, &count, &clear, NULL);
    CHECK(calls == 4 && count == 0 && clear == 2);
    CHECK(WineD3D11On12SetScissorRectsV1(&owner, 17, rects) == E_INVALIDARG);
    CHECK(WineD3D11On12SetScissorRectsV1(&owner, 1, NULL) == E_INVALIDARG);
    CHECK(WineD3D11On12SetScissorRectsV1(NULL, 1, rects) == E_INVALIDARG);
    scissor(&calls, NULL, NULL, NULL);
    CHECK(calls == 4);

    /* Teardown with a sampler still bound in two slots: unbound, then
     * destroyed, and later destroys are no-ops. */
    CHECK(bind_sampler(&owner, WINE_D3D11ON12_SHADER_VERTEX, 7, &kept) == S_OK);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&other) == S_OK);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&owner) == S_OK);
    CHECK(!kept.runtimeState && !foreign.runtimeState);
    CHECK(WineD3D11On12DestroyPipelineStateV1(&kept) == S_OK);
    record(&created, &destroyed, NULL, NULL, &bad);
    CHECK(created == destroyed && !bad);
    CHECK(device.refcount == 1 && other_device.refcount == 1);
    printf("[%s] samplers, multi-slot binding and scissor rectangles\n", failures ? "fail" : " ok ");
    return !!failures;
}
