/* SPDX-License-Identifier: GPL-3.0-only */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <string.h>
#include "d3d11on12core.h"
#include "ddi/wine_d3d11ddi.h"
#include "d3d11on12mocks.h"

static int failures, creates[3], destroys[3], binds[3];
static int fail_create, fail_size, fail_bind;
static LONG (WINAPI *get_flush_count)(void);
static void (WINAPI *report_error)(HRESULT);
static D3D11_1_DDI_BLEND_DESC seen_blend;
static D3D10_DDI_DEPTH_STENCIL_DESC seen_depth;
static D3DWDDM2_0DDI_RASTERIZER_DESC seen_raster;
static FLOAT seen_factor[4];
static UINT seen_mask, seen_reference;
#define CHECK(x) do { if (!(x)) { printf("[fail] %d: %s\n", __LINE__, #x); ++failures; } } while (0)

/* Each kind has a different tag. Mixing handles or calling destroy on a
 * failed construction is detected independently of the core registry. */
#define STATE_STUBS(name, DESC, H, RT, index, seen) \
static SIZE_T calc_##name(D3D10DDI_HDEVICE d, const DESC *desc) \
{ (void)d; CHECK(desc); return fail_size ? ~(SIZE_T)0 : sizeof(UINT); } \
static void create_##name(D3D10DDI_HDEVICE d, const DESC *desc, H h, RT rt) \
{ (void)d; CHECK(rt.handle); if (fail_create) { report_error(E_OUTOFMEMORY); return; } \
  seen = *desc; *(UINT *)h.pDrvPrivate = index + 1; ++creates[index]; } \
static void destroy_##name(D3D10DDI_HDEVICE d, H h) \
{ (void)d; CHECK(h.pDrvPrivate && *(UINT *)h.pDrvPrivate == index + 1); ++destroys[index]; }
STATE_STUBS(blend, D3D11_1_DDI_BLEND_DESC, D3D10DDI_HBLENDSTATE, D3D10DDI_HRTBLENDSTATE, 0, seen_blend)
STATE_STUBS(depth, D3D10_DDI_DEPTH_STENCIL_DESC, D3D10DDI_HDEPTHSTENCILSTATE, D3D10DDI_HRTDEPTHSTENCILSTATE, 1, seen_depth)
STATE_STUBS(raster, D3DWDDM2_0DDI_RASTERIZER_DESC, D3D10DDI_HRASTERIZERSTATE, D3D10DDI_HRTRASTERIZERSTATE, 2, seen_raster)
static void bind_blend(D3D10DDI_HDEVICE d, D3D10DDI_HBLENDSTATE h, const FLOAT factor[4], UINT mask)
{ (void)d; if (fail_bind) { report_error(E_FAIL); return; } CHECK(!h.pDrvPrivate || *(UINT *)h.pDrvPrivate == 1); memcpy(seen_factor, factor, sizeof(seen_factor)); seen_mask = mask; ++binds[0]; }
static void bind_depth(D3D10DDI_HDEVICE d, D3D10DDI_HDEPTHSTENCILSTATE h, UINT ref)
{ (void)d; CHECK(!h.pDrvPrivate || *(UINT *)h.pDrvPrivate == 2); seen_reference = ref; ++binds[1]; }
static void bind_raster(D3D10DDI_HDEVICE d, D3D10DDI_HRASTERIZERSTATE h)
{ (void)d; CHECK(!h.pDrvPrivate || *(UINT *)h.pDrvPrivate == 3); ++binds[2]; }
static void install(WineD3D11On12AdapterDevice *owner)
{
    D3DWDDM2_6DDI_DEVICEFUNCS *f = owner->deviceFuncs;
    f->pfnCalcPrivateBlendStateSize = calc_blend; f->pfnCreateBlendState = create_blend;
    f->pfnDestroyBlendState = destroy_blend; f->pfnSetBlendState = bind_blend;
    f->pfnCalcPrivateDepthStencilStateSize = calc_depth; f->pfnCreateDepthStencilState = create_depth;
    f->pfnDestroyDepthStencilState = destroy_depth; f->pfnSetDepthStencilState = bind_depth;
    f->pfnCalcPrivateRasterizerStateSize = calc_raster; f->pfnCreateRasterizerState = create_raster;
    f->pfnDestroyRasterizerState = destroy_raster; f->pfnSetRasterizerState = bind_raster;
}
int main(void)
{
    struct mock_device device;
    struct mock_queue queue;
    IUnknown *queues[1];
    WineD3D11On12AdapterDevice owner = {.size = sizeof(owner)}, other = {.size = sizeof(other)};
    WineD3D11On12PipelineState blend = {.size = sizeof(blend)}, depth = {.size = sizeof(depth)},
        raster = {.size = sizeof(raster)}, bad = {.size = sizeof(bad)}, copied;
    WineD3D11On12DeferredContext context = {.size = sizeof(context)};
    WineD3D11On12CommandList list = {.size = sizeof(list)};
    D3D11_BLEND_DESC b = {0};
    D3D11_DEPTH_STENCIL_DESC d = {0};
    D3D11_RASTERIZER_DESC r = {0};
    FLOAT factor[4] = {.25f, .5f, .75f, 1};
    int i;
    mock_device_init(&device); device.support_device1 = 1;
    mock_queue_init(&queue, &device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &owner) == S_OK);
    if (!owner.runtimeState) return 1;
    report_error = (void *)(UINT_PTR)GetProcAddress(GetModuleHandleW(L"d3d11on12.dll"), "WineD3D11On12MockDriverReportError");
    get_flush_count = (void *)(UINT_PTR)GetProcAddress(GetModuleHandleW(L"d3d11on12.dll"), "WineD3D11On12MockDriverGetFlushCount");
    if (!report_error || !get_flush_count) return 1;
    /* Missing slots must fail before allocating/dispatching. */
    CHECK(WineD3D11On12CreateBlendStateV1(&owner, &b, &bad) == DXGI_ERROR_UNSUPPORTED);
    CHECK(!bad.hDrvState && !bad.runtimeState);
    install(&owner);
    fail_create = 1;
    CHECK(WineD3D11On12CreateBlendStateV1(&owner, &b, &bad) == E_OUTOFMEMORY);
    CHECK(!bad.hDrvState && creates[0] == 0 && destroys[0] == 0);
    fail_create = 0; fail_size = 1;
    CHECK(WineD3D11On12CreateBlendStateV1(&owner, &b, &bad) == E_OUTOFMEMORY);
    fail_size = 0;
    b.RenderTarget[0].RenderTargetWriteMask = 15;
    CHECK(WineD3D11On12CreateBlendStateV1(&owner, &b, &blend) == S_OK);
    for (i = 0; i < 8; ++i)
    {
        CHECK(seen_blend.RenderTarget[i].RenderTargetWriteMask == 15);
        CHECK(seen_blend.RenderTarget[i].SrcBlend == D3D11_BLEND_ONE);
        CHECK(!seen_blend.RenderTarget[i].LogicOpEnable);
        CHECK(!memcmp(seen_blend.RenderTarget[i].WinePad0, "\0\0\0", 3));
    }
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = (D3D11_BLEND)12;
    CHECK(WineD3D11On12CreateBlendStateV1(&owner, &b, &bad) == E_INVALIDARG);
    CHECK(!bad.runtimeState);
    d.DepthEnable = TRUE; d.DepthFunc = D3D11_COMPARISON_GREATER;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    d.StencilEnable = TRUE; d.StencilReadMask = 0xab; d.StencilWriteMask = 0xcd;
    d.FrontFace = (D3D11_DEPTH_STENCILOP_DESC){D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_EQUAL};
    d.BackFace = (D3D11_DEPTH_STENCILOP_DESC){D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_DECR, D3D11_STENCIL_OP_INVERT, D3D11_COMPARISON_NEVER};
    CHECK(WineD3D11On12CreateDepthStencilStateV1(&owner, &d, &depth) == S_OK);
    CHECK(seen_depth.FrontEnable && seen_depth.BackEnable && seen_depth.StencilReadMask == 0xab);
    CHECK(seen_depth.FrontFace.StencilDepthFailOp == D3D11_STENCIL_OP_INCR);
    CHECK(seen_depth.BackFace.StencilPassOp == D3D11_STENCIL_OP_INVERT);
    CHECK(seen_depth.DepthFunc == D3D11_COMPARISON_GREATER);
    r.FillMode = D3D11_FILL_WIREFRAME; r.CullMode = D3D11_CULL_FRONT;
    r.FrontCounterClockwise = TRUE; r.DepthBias = -17; r.DepthBiasClamp = 2.5f;
    r.SlopeScaledDepthBias = 1.5f; r.ScissorEnable = TRUE;
    CHECK(WineD3D11On12CreateRasterizerStateV1(&owner, &r, &raster) == S_OK);
    CHECK(seen_raster.DepthBias == -17 && seen_raster.ScissorEnable);
    CHECK(seen_raster.ForcedSampleCount == 0 && seen_raster.ConservativeRasterizationMode == 0);
    CHECK(WineD3D11On12SetBlendStateV1(&owner, &blend, factor, 0x1234) == S_OK);
    CHECK(!memcmp(factor, seen_factor, sizeof(factor)) && seen_mask == 0x1234);
    CHECK(WineD3D11On12SetDepthStencilStateV1(&owner, &depth, 123) == S_OK && seen_reference == 123);
    CHECK(WineD3D11On12SetRasterizerStateV1(&owner, &raster) == S_OK);
    CHECK(WineD3D11On12SetRasterizerStateV1(&owner, &blend) == E_INVALIDARG);
    copied = blend;
    CHECK(WineD3D11On12SetBlendStateV1(&owner, &copied, NULL, ~0u) == E_INVALIDARG);
    CHECK(WineD3D11On12CreateBlendStateV1(&owner, &b, &blend) == E_INVALIDARG && blend.runtimeState);
    /* Execution clears tracked pipeline state, including retained objects
     * whose public handle was released while still bound. */
    CHECK(WineD3D11On12DestroyPipelineStateV1(&depth) == S_OK && destroys[1] == 0);
    CHECK(WineD3D11On12CreateDeferredContextV1(&owner, 0, &context) == S_OK);
    CHECK(WineD3D11On12CreateCommandListV1(&context, &list) == S_OK);
    CHECK(WineD3D11On12ExecuteCommandListV1(&owner, &list) == S_OK && destroys[1] == 1);
    CHECK(WineD3D11On12DestroyCommandListV1(&list) == S_OK);
    CHECK(WineD3D11On12DestroyDeferredContextV1(&context) == S_OK);
    CHECK(WineD3D11On12SetBlendStateV1(&owner, &blend, factor, 0x1234) == S_OK);
    fail_bind = 1;
    CHECK(WineD3D11On12SetBlendStateV1(&owner, NULL, NULL, ~0u) == E_FAIL);
    fail_bind = 0;
    CHECK(WineD3D11On12OpenAdapterV1((IUnknown *)&device.ID3D12Device_iface, queues, 1, 0, &other) == S_OK);
    install(&other);
    CHECK(WineD3D11On12SetBlendStateV1(&other, &blend, NULL, ~0u) == E_INVALIDARG);
    CHECK(WineD3D11On12DestroyPipelineStateV1(&blend) == S_OK);
    CHECK(WineD3D11On12DestroyPipelineStateV1(&blend) == S_OK && destroys[0] == 0);
    CHECK(WineD3D11On12SetBlendStateV1(&owner, &blend, NULL, ~0u) == E_INVALIDARG);
    CHECK(WineD3D11On12SetBlendStateV1(&owner, NULL, NULL, ~0u) == S_OK);
    CHECK(destroys[0] == 1 && seen_factor[0] == 1 && seen_factor[3] == 1);
    CHECK(get_flush_count() == 0); /* PSO state changes must not submit work. */
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&other) == S_OK);
    CHECK(WineD3D11On12CloseAdapterDeviceV1(&owner) == S_OK);
    CHECK(!depth.runtimeState && !raster.runtimeState);
    CHECK(WineD3D11On12DestroyPipelineStateV1(&depth) == S_OK);
    CHECK(WineD3D11On12DestroyPipelineStateV1(&raster) == S_OK);
    for (i = 0; i < 3; ++i) CHECK(creates[i] == 1 && destroys[i] == 1);
    printf("PSO component lifecycle: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
