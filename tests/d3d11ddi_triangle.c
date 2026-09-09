/* SPDX-License-Identifier: GPL-3.0-only
 *
 * The triangle frame, driven through the promoted device function table.
 *
 * tests/d3d11ddilayout.c proves each promoted slot is individually callable
 * with the arguments its declaration names. That is a per-slot claim, and it
 * is deliberately made in slot order rather than in any order a runtime would
 * use. This file makes the claim the other one cannot: that the slots the MVP
 * frame needs are, together, enough to express the frame -- created in the
 * order the frame creates them, bound in the order it binds them, and with
 * the handles flowing from the call that produced each one to the call that
 * consumes it.
 *
 * What it is not. There is no host behind this table and no rasteriser: the
 * stubs record and return. So this proves the frame is expressible and
 * type-correct against the declarations, not that anything renders. The
 * application-level counterpart that would prove that is
 * tests/e2e_d3d11_triangle.cpp, which cannot run yet. When a host exists the
 * two should agree, and disagreeing is a finding.
 *
 * Why the handle plumbing is checked and not just the call order. Every
 * driver handle in this DDI is one wrapped pointer. A frame that bound the
 * vertex buffer where it meant to bind the render target would call the right
 * slots in the right order with arguments of the right types, and only the
 * identity of the pointer would be wrong. That is the failure this file is
 * shaped to catch.
 *
 * Compiled in both C and C++, and run under Wine, for the reasons
 * d3d11ddilayout.c gives: the Wine frontend is C, the host will be C++, and
 * the declarations must mean the same thing in both.
 */

#include <stdio.h>
#include <string.h>

#include "wine_d3d11ddi.h"

static int failures;

/*
 * The recorded frame.
 *
 * Each stub appends its slot name and the one handle whose identity matters
 * at that step -- the object being created, bound, or drawn from. A slot with
 * no such handle records NULL.
 */
#define MAX_EVENTS 64

struct frame_event
{
    const char *slot;
    const void *handle;
};

static struct frame_event events[MAX_EVENTS];
static unsigned int event_count;

static void record(const char *slot, const void *handle)
{
    if (event_count < MAX_EVENTS)
    {
        events[event_count].slot = slot;
        events[event_count].handle = handle;
    }
    /* Counted past the end on purpose: overflowing the log has to be a
     * failure, not a silently truncated comparison that still passes. */
    ++event_count;
}

/*
 * Driver private allocations.
 *
 * The runtime, not the driver, owns the private block: it asks
 * CalcPrivate<Object>Size how large one is, allocates it, and hands the
 * address back as the handle's pDrvPrivate. Doing that here rather than
 * passing zeroed handles is what makes each handle distinct, which is what
 * makes the identity checks below mean anything.
 */
static unsigned char private_heap[4096];
static size_t private_used;

static void *allocate_private(SIZE_T size)
{
    void *block;

    if (size == 0 || private_used + size > sizeof(private_heap))
    {
        printf("[fail] private heap exhausted at %u bytes\n",
                (unsigned int)size);
        ++failures;
        return NULL;
    }
    block = &private_heap[private_used];
    private_used += size;
    return block;
}

/* Sizes the stub driver asks for. Distinct so that a mixed-up allocation
 * shows up as a different address rather than an aliasing coincidence. */
#define RESOURCE_PRIVATE_SIZE       128
#define SHADER_PRIVATE_SIZE          64
#define ELEMENT_LAYOUT_PRIVATE_SIZE  48
#define STATE_PRIVATE_SIZE           32
#define VIEW_PRIVATE_SIZE            96
#define TARGET_WIDTH                 64
#define TARGET_HEIGHT                64
#define BYTES_PER_TEXEL               4
#define TARGET_ROW_PITCH (TARGET_WIDTH * BYTES_PER_TEXEL)
#define TARGET_BYTES (TARGET_ROW_PITCH * TARGET_HEIGHT)

static unsigned char render_target_pixels[TARGET_BYTES];
static unsigned char staging_pixels[TARGET_BYTES];

static SIZE_T stub_calc_private_resource_size(D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATERESOURCE *create)
{
    (void)hDevice;
    (void)create;
    record("CalcPrivateResourceSize", NULL);
    return RESOURCE_PRIVATE_SIZE;
}

static VOID stub_create_resource(D3D10DDI_HDEVICE hDevice,
        const D3D11DDIARG_CREATERESOURCE *create, D3D10DDI_HRESOURCE resource,
        D3D10DDI_HRTRESOURCE rt_resource)
{
    (void)hDevice;
    (void)create;
    (void)rt_resource;
    record("CreateResource", resource.pDrvPrivate);
}

static VOID stub_destroy_resource(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE resource)
{
    (void)hDevice;
    record("DestroyResource", resource.pDrvPrivate);
}

static SIZE_T stub_calc_private_shader_size(D3D10DDI_HDEVICE hDevice,
        const UINT *code, const D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    (void)hDevice;
    (void)code;
    (void)signatures;
    record("CalcPrivateShaderSize", NULL);
    return SHADER_PRIVATE_SIZE;
}

static VOID stub_create_vertex_shader(D3D10DDI_HDEVICE hDevice,
        const UINT *code, D3D10DDI_HSHADER shader,
        D3D10DDI_HRTSHADER rt_shader,
        const D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    (void)hDevice;
    (void)code;
    (void)rt_shader;
    (void)signatures;
    record("CreateVertexShader", shader.pDrvPrivate);
}

static VOID stub_create_pixel_shader(D3D10DDI_HDEVICE hDevice,
        const UINT *code, D3D10DDI_HSHADER shader,
        D3D10DDI_HRTSHADER rt_shader,
        const D3D11_1DDIARG_STAGE_IO_SIGNATURES *signatures)
{
    (void)hDevice;
    (void)code;
    (void)rt_shader;
    (void)signatures;
    record("CreatePixelShader", shader.pDrvPrivate);
}

static VOID stub_destroy_shader(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HSHADER shader)
{
    (void)hDevice;
    record("DestroyShader", shader.pDrvPrivate);
}

static SIZE_T stub_calc_private_element_layout_size(D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_CREATEELEMENTLAYOUT *create)
{
    (void)hDevice;
    (void)create;
    record("CalcPrivateElementLayoutSize", NULL);
    return ELEMENT_LAYOUT_PRIVATE_SIZE;
}

static VOID stub_create_element_layout(D3D10DDI_HDEVICE hDevice,
        const D3D10DDIARG_CREATEELEMENTLAYOUT *create,
        D3D10DDI_HELEMENTLAYOUT layout, D3D10DDI_HRTELEMENTLAYOUT rt_layout)
{
    (void)hDevice;
    (void)create;
    (void)rt_layout;
    record("CreateElementLayout", layout.pDrvPrivate);
}

static VOID stub_destroy_element_layout(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HELEMENTLAYOUT layout)
{
    (void)hDevice;
    record("DestroyElementLayout", layout.pDrvPrivate);
}

static SIZE_T stub_calc_private_blend_state_size(D3D10DDI_HDEVICE hDevice,
        const D3D11_1_DDI_BLEND_DESC *desc)
{
    (void)hDevice;
    (void)desc;
    record("CalcPrivateBlendStateSize", NULL);
    return STATE_PRIVATE_SIZE;
}

static VOID stub_create_blend_state(D3D10DDI_HDEVICE hDevice,
        const D3D11_1_DDI_BLEND_DESC *desc, D3D10DDI_HBLENDSTATE state,
        D3D10DDI_HRTBLENDSTATE rt_state)
{
    (void)hDevice;
    (void)desc;
    (void)rt_state;
    record("CreateBlendState", state.pDrvPrivate);
}

static VOID stub_destroy_blend_state(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HBLENDSTATE state)
{
    (void)hDevice;
    record("DestroyBlendState", state.pDrvPrivate);
}

static SIZE_T stub_calc_private_depth_stencil_state_size(
        D3D10DDI_HDEVICE hDevice, const D3D10_DDI_DEPTH_STENCIL_DESC *desc)
{
    (void)hDevice;
    (void)desc;
    record("CalcPrivateDepthStencilStateSize", NULL);
    return STATE_PRIVATE_SIZE;
}

static VOID stub_create_depth_stencil_state(D3D10DDI_HDEVICE hDevice,
        const D3D10_DDI_DEPTH_STENCIL_DESC *desc,
        D3D10DDI_HDEPTHSTENCILSTATE state,
        D3D10DDI_HRTDEPTHSTENCILSTATE rt_state)
{
    (void)hDevice;
    (void)desc;
    (void)rt_state;
    record("CreateDepthStencilState", state.pDrvPrivate);
}

static VOID stub_destroy_depth_stencil_state(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HDEPTHSTENCILSTATE state)
{
    (void)hDevice;
    record("DestroyDepthStencilState", state.pDrvPrivate);
}

static SIZE_T stub_calc_private_rasterizer_state_size(
        D3D10DDI_HDEVICE hDevice, const D3D11_1_DDI_RASTERIZER_DESC *desc)
{
    (void)hDevice;
    (void)desc;
    record("CalcPrivateRasterizerStateSize", NULL);
    return STATE_PRIVATE_SIZE;
}

static VOID stub_create_rasterizer_state(D3D10DDI_HDEVICE hDevice,
        const D3D11_1_DDI_RASTERIZER_DESC *desc,
        D3D10DDI_HRASTERIZERSTATE state, D3D10DDI_HRTRASTERIZERSTATE rt_state)
{
    (void)hDevice;
    (void)desc;
    (void)rt_state;
    record("CreateRasterizerState", state.pDrvPrivate);
}

static VOID stub_destroy_rasterizer_state(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRASTERIZERSTATE state)
{
    (void)hDevice;
    record("DestroyRasterizerState", state.pDrvPrivate);
}

/* Records the resource the view is being created over, not the view handle:
 * this is the one step where the interesting identity is the input. The view
 * handle is checked where it is bound, below. */
static const void *rtv_source_resource;

static SIZE_T stub_calc_private_render_target_view_size(
        D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *create)
{
    (void)hDevice;
    (void)create;
    record("CalcPrivateRenderTargetViewSize", NULL);
    return VIEW_PRIVATE_SIZE;
}

static VOID stub_create_render_target_view(D3D10DDI_HDEVICE hDevice,
        const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *create,
        D3D10DDI_HRENDERTARGETVIEW view, D3D10DDI_HRTRENDERTARGETVIEW rt_view)
{
    (void)hDevice;
    (void)rt_view;
    rtv_source_resource = create ? create->hDrvResource.pDrvPrivate : NULL;
    record("CreateRenderTargetView", view.pDrvPrivate);
}

static VOID stub_destroy_render_target_view(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRENDERTARGETVIEW view)
{
    (void)hDevice;
    record("DestroyRenderTargetView", view.pDrvPrivate);
}

/* The bound state the frame's correctness turns on. */
static UINT bound_rtv_count;
static UINT drawn_vertex_count;
static UINT drawn_start_vertex;
static FLOAT recorded_clear[4];
static UINT recorded_stride;
static const void *copy_source_resource;

static VOID stub_set_render_targets(D3D10DDI_HDEVICE hDevice,
        const D3D10DDI_HRENDERTARGETVIEW *rtvs, UINT num_rtvs,
        UINT clear_slots, D3D10DDI_HDEPTHSTENCILVIEW dsv,
        const D3D11DDI_HUNORDEREDACCESSVIEW *uavs,
        const UINT *uav_initial_counts, UINT uav_start_slot, UINT num_uavs,
        UINT uav_range_start, UINT uav_range_size)
{
    (void)hDevice;
    (void)clear_slots;
    (void)dsv;
    (void)uavs;
    (void)uav_initial_counts;
    (void)uav_start_slot;
    (void)num_uavs;
    (void)uav_range_start;
    (void)uav_range_size;
    bound_rtv_count = num_rtvs;
    record("SetRenderTargets",
            (rtvs && num_rtvs) ? rtvs[0].pDrvPrivate : NULL);
}

static VOID stub_set_viewports(D3D10DDI_HDEVICE hDevice, UINT num_viewports,
        UINT clear_viewports, const D3D10_DDI_VIEWPORT *viewports)
{
    (void)hDevice;
    (void)num_viewports;
    (void)clear_viewports;
    (void)viewports;
    record("SetViewports", NULL);
}

static VOID stub_ia_set_input_layout(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HELEMENTLAYOUT layout)
{
    (void)hDevice;
    record("IaSetInputLayout", layout.pDrvPrivate);
}

static VOID stub_ia_set_vertex_buffers(D3D10DDI_HDEVICE hDevice,
        UINT start_slot, UINT num_buffers, const D3D10DDI_HRESOURCE *buffers,
        const UINT *strides, const UINT *offsets)
{
    (void)hDevice;
    (void)start_slot;
    (void)offsets;
    recorded_stride = strides ? strides[0] : 0;
    record("IaSetVertexBuffers",
            (buffers && num_buffers) ? buffers[0].pDrvPrivate : NULL);
}

static VOID stub_ia_set_topology(D3D10DDI_HDEVICE hDevice,
        D3D10_DDI_PRIMITIVE_TOPOLOGY topology)
{
    (void)hDevice;
    (void)topology;
    record("IaSetTopology", NULL);
}

static VOID stub_vs_set_shader(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HSHADER shader)
{
    (void)hDevice;
    record("VsSetShader", shader.pDrvPrivate);
}

static VOID stub_ps_set_shader(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HSHADER shader)
{
    (void)hDevice;
    record("PsSetShader", shader.pDrvPrivate);
}

static VOID stub_set_blend_state(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HBLENDSTATE state, const FLOAT blend_factor[4],
        UINT sample_mask)
{
    (void)hDevice;
    (void)blend_factor;
    (void)sample_mask;
    record("SetBlendState", state.pDrvPrivate);
}

static VOID stub_set_depth_stencil_state(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HDEPTHSTENCILSTATE state, UINT stencil_ref)
{
    (void)hDevice;
    (void)stencil_ref;
    record("SetDepthStencilState", state.pDrvPrivate);
}

static VOID stub_set_rasterizer_state(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRASTERIZERSTATE state)
{
    (void)hDevice;
    record("SetRasterizerState", state.pDrvPrivate);
}

static VOID stub_clear_render_target_view(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRENDERTARGETVIEW view, FLOAT colour[4])
{
    unsigned int texel;

    (void)hDevice;
    if (colour)
    {
        memcpy(recorded_clear, colour, sizeof(recorded_clear));
        for (texel = 0; texel < TARGET_WIDTH * TARGET_HEIGHT; ++texel)
        {
            render_target_pixels[texel * BYTES_PER_TEXEL + 0] =
                    (unsigned char)(colour[0] * 255.0f);
            render_target_pixels[texel * BYTES_PER_TEXEL + 1] =
                    (unsigned char)(colour[1] * 255.0f);
            render_target_pixels[texel * BYTES_PER_TEXEL + 2] =
                    (unsigned char)(colour[2] * 255.0f);
            render_target_pixels[texel * BYTES_PER_TEXEL + 3] =
                    (unsigned char)(colour[3] * 255.0f);
        }
    }
    record("ClearRenderTargetView", view.pDrvPrivate);
}

static VOID stub_draw(D3D10DDI_HDEVICE hDevice, UINT vertex_count,
        UINT start_vertex_location)
{
    const unsigned int centre = ((TARGET_HEIGHT / 2) * TARGET_WIDTH
            + TARGET_WIDTH / 2) * BYTES_PER_TEXEL;

    (void)hDevice;
    drawn_vertex_count = vertex_count;
    drawn_start_vertex = start_vertex_location;
    render_target_pixels[centre + 0] = 0xff;
    render_target_pixels[centre + 1] = 0x00;
    render_target_pixels[centre + 2] = 0x00;
    render_target_pixels[centre + 3] = 0xff;
    record("Draw", NULL);
}

static VOID stub_resource_copy(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE destination, D3D10DDI_HRESOURCE source)
{
    (void)hDevice;
    memcpy(staging_pixels, render_target_pixels, sizeof(staging_pixels));
    copy_source_resource = source.pDrvPrivate;
    record("ResourceCopy", destination.pDrvPrivate);
}

static VOID stub_resource_map(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE resource, UINT subresource, D3D10_DDI_MAP map,
        UINT flags, D3D10DDI_MAPPED_SUBRESOURCE *mapped)
{
    (void)hDevice;
    (void)subresource;
    (void)map;
    (void)flags;
    if (mapped)
    {
        mapped->pData = staging_pixels;
        mapped->RowPitch = TARGET_ROW_PITCH;
        mapped->DepthPitch = TARGET_BYTES;
    }
    record("ResourceMap", resource.pDrvPrivate);
}

static VOID stub_resource_unmap(D3D10DDI_HDEVICE hDevice,
        D3D10DDI_HRESOURCE resource, UINT subresource)
{
    (void)hDevice;
    (void)subresource;
    record("ResourceUnmap", resource.pDrvPrivate);
}

static void install(D3DWDDM2_6DDI_DEVICEFUNCS *funcs)
{
    memset(funcs, 0, sizeof(*funcs));

    funcs->pfnCalcPrivateResourceSize = stub_calc_private_resource_size;
    funcs->pfnCreateResource = stub_create_resource;
    funcs->pfnDestroyResource = stub_destroy_resource;
    funcs->pfnCalcPrivateShaderSize = stub_calc_private_shader_size;
    funcs->pfnCreateVertexShader = stub_create_vertex_shader;
    funcs->pfnCreatePixelShader = stub_create_pixel_shader;
    funcs->pfnDestroyShader = stub_destroy_shader;
    funcs->pfnCalcPrivateElementLayoutSize =
            stub_calc_private_element_layout_size;
    funcs->pfnCreateElementLayout = stub_create_element_layout;
    funcs->pfnDestroyElementLayout = stub_destroy_element_layout;
    funcs->pfnCalcPrivateBlendStateSize = stub_calc_private_blend_state_size;
    funcs->pfnCreateBlendState = stub_create_blend_state;
    funcs->pfnDestroyBlendState = stub_destroy_blend_state;
    funcs->pfnCalcPrivateDepthStencilStateSize =
            stub_calc_private_depth_stencil_state_size;
    funcs->pfnCreateDepthStencilState = stub_create_depth_stencil_state;
    funcs->pfnDestroyDepthStencilState = stub_destroy_depth_stencil_state;
    funcs->pfnCalcPrivateRasterizerStateSize =
            stub_calc_private_rasterizer_state_size;
    funcs->pfnCreateRasterizerState = stub_create_rasterizer_state;
    funcs->pfnDestroyRasterizerState = stub_destroy_rasterizer_state;
    funcs->pfnCalcPrivateRenderTargetViewSize =
            stub_calc_private_render_target_view_size;
    funcs->pfnCreateRenderTargetView = stub_create_render_target_view;
    funcs->pfnDestroyRenderTargetView = stub_destroy_render_target_view;
    funcs->pfnSetRenderTargets = stub_set_render_targets;
    funcs->pfnSetViewports = stub_set_viewports;
    funcs->pfnIaSetInputLayout = stub_ia_set_input_layout;
    funcs->pfnIaSetVertexBuffers = stub_ia_set_vertex_buffers;
    funcs->pfnIaSetTopology = stub_ia_set_topology;
    funcs->pfnVsSetShader = stub_vs_set_shader;
    funcs->pfnPsSetShader = stub_ps_set_shader;
    funcs->pfnSetBlendState = stub_set_blend_state;
    funcs->pfnSetDepthStencilState = stub_set_depth_stencil_state;
    funcs->pfnSetRasterizerState = stub_set_rasterizer_state;
    funcs->pfnClearRenderTargetView = stub_clear_render_target_view;
    funcs->pfnDraw = stub_draw;
    funcs->pfnResourceCopy = stub_resource_copy;
    funcs->pfnResourceMap = stub_resource_map;
    funcs->pfnResourceUnmap = stub_resource_unmap;
}

/* The order the frame must be issued in. Creation before binding, binding
 * before the clear, the clear before the draw, and teardown after. Written
 * out rather than derived so that a reordering has to be argued for here. */
static const char *const EXPECTED[] = {
    "CalcPrivateResourceSize",
    "CreateResource",
    "CalcPrivateResourceSize",
    "CreateResource",
    "CalcPrivateResourceSize",
    "CreateResource",
    "CalcPrivateShaderSize",
    "CreateVertexShader",
    "CalcPrivateShaderSize",
    "CreatePixelShader",
    "CalcPrivateElementLayoutSize",
    "CreateElementLayout",
    "CalcPrivateBlendStateSize",
    "CreateBlendState",
    "CalcPrivateDepthStencilStateSize",
    "CreateDepthStencilState",
    "CalcPrivateRasterizerStateSize",
    "CreateRasterizerState",
    "CalcPrivateRenderTargetViewSize",
    "CreateRenderTargetView",
    "SetRenderTargets",
    "SetViewports",
    "IaSetInputLayout",
    "IaSetVertexBuffers",
    "IaSetTopology",
    "VsSetShader",
    "PsSetShader",
    "SetBlendState",
    "SetDepthStencilState",
    "SetRasterizerState",
    "ClearRenderTargetView",
    "Draw",
    "ResourceCopy",
    "ResourceMap",
    "ResourceUnmap",
    "DestroyRenderTargetView",
    "DestroyRasterizerState",
    "DestroyDepthStencilState",
    "DestroyBlendState",
    "DestroyElementLayout",
    "DestroyShader",
    "DestroyShader",
    "DestroyResource",
    "DestroyResource",
    "DestroyResource",
};

#define EXPECTED_COUNT ((unsigned int)(sizeof(EXPECTED) / sizeof(EXPECTED[0])))

static void check(int condition, const char *what)
{
    if (condition)
    {
        printf("[ ok ] %s\n", what);
    }
    else
    {
        printf("[fail] %s\n", what);
        ++failures;
    }
}

static const void *handle_at(const char *slot, unsigned int which)
{
    unsigned int index;
    unsigned int seen = 0;

    for (index = 0; index < event_count && index < MAX_EVENTS; ++index)
    {
        if (strcmp(events[index].slot, slot) != 0)
            continue;
        if (seen == which)
            return events[index].handle;
        ++seen;
    }
    return NULL;
}

static void check_sequence(void)
{
    unsigned int index;

    if (event_count != EXPECTED_COUNT)
    {
        printf("[fail] the frame issued %u calls, expected %u\n", event_count,
                EXPECTED_COUNT);
        ++failures;
        return;
    }
    for (index = 0; index < EXPECTED_COUNT; ++index)
    {
        if (strcmp(events[index].slot, EXPECTED[index]) == 0)
            continue;
        printf("[fail] call %u was %s, expected %s\n", index,
                events[index].slot, EXPECTED[index]);
        ++failures;
        return;
    }
    printf("[ ok ] the frame issued %u calls in the expected order\n",
            event_count);
}

int main(void)
{
    D3DWDDM2_6DDI_DEVICEFUNCS funcs;
    D3D10DDI_HDEVICE device;
    D3D11DDIARG_CREATERESOURCE create_resource;
    D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW create_rtv;
    D3D10DDI_HRESOURCE vertex_buffer;
    D3D10DDI_HRESOURCE render_target;
    D3D10DDI_HRESOURCE staging_resource;
    D3D10DDI_HRESOURCE bound_buffers[1];
    D3D10DDI_HRENDERTARGETVIEW rtv;
    D3D10DDI_HRENDERTARGETVIEW bound_rtvs[1];
    D3D10DDI_HSHADER vertex_shader;
    D3D10DDI_HSHADER pixel_shader;
    D3D10DDI_HELEMENTLAYOUT element_layout;
    D3D10DDI_HBLENDSTATE blend_state;
    D3D10DDI_HDEPTHSTENCILSTATE depth_stencil_state;
    D3D10DDI_HRASTERIZERSTATE rasterizer_state;
    UINT strides[1];
    UINT offsets[1];
    const FLOAT clear_colour[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const FLOAT blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    FLOAT clear_argument[4];
    D3D10DDI_MAPPED_SUBRESOURCE mapped;
    const unsigned char *pixels;
    const unsigned char *centre;
    const unsigned char *corner;

    install(&funcs);

    memset(&device, 0, sizeof(device));
    memset(&create_resource, 0, sizeof(create_resource));
    memset(&create_rtv, 0, sizeof(create_rtv));
    memset(&vertex_buffer, 0, sizeof(vertex_buffer));
    memset(&render_target, 0, sizeof(render_target));
    memset(&staging_resource, 0, sizeof(staging_resource));
    memset(&rtv, 0, sizeof(rtv));
    memset(&vertex_shader, 0, sizeof(vertex_shader));
    memset(&pixel_shader, 0, sizeof(pixel_shader));
    memset(&element_layout, 0, sizeof(element_layout));
    memset(&blend_state, 0, sizeof(blend_state));
    memset(&depth_stencil_state, 0, sizeof(depth_stencil_state));
    memset(&rasterizer_state, 0, sizeof(rasterizer_state));
    memset(&mapped, 0, sizeof(mapped));

    /* Creation. Each object's private block is sized by the driver and
     * allocated by us, exactly as the runtime does it. */
    vertex_buffer.pDrvPrivate = allocate_private(
            funcs.pfnCalcPrivateResourceSize(device, &create_resource));
    funcs.pfnCreateResource(device, &create_resource, vertex_buffer,
            (D3D10DDI_HRTRESOURCE){0});

    render_target.pDrvPrivate = allocate_private(
            funcs.pfnCalcPrivateResourceSize(device, &create_resource));
    funcs.pfnCreateResource(device, &create_resource, render_target,
            (D3D10DDI_HRTRESOURCE){0});

    staging_resource.pDrvPrivate = allocate_private(
            funcs.pfnCalcPrivateResourceSize(device, &create_resource));
    funcs.pfnCreateResource(device, &create_resource, staging_resource,
            (D3D10DDI_HRTRESOURCE){0});

    vertex_shader.pDrvPrivate = allocate_private(
            funcs.pfnCalcPrivateShaderSize(device, NULL, NULL));
    funcs.pfnCreateVertexShader(device, NULL, vertex_shader,
            (D3D10DDI_HRTSHADER){0}, NULL);

    pixel_shader.pDrvPrivate = allocate_private(
            funcs.pfnCalcPrivateShaderSize(device, NULL, NULL));
    funcs.pfnCreatePixelShader(device, NULL, pixel_shader,
            (D3D10DDI_HRTSHADER){0}, NULL);

    element_layout.pDrvPrivate = allocate_private(
            funcs.pfnCalcPrivateElementLayoutSize(device, NULL));
    funcs.pfnCreateElementLayout(device, NULL, element_layout,
            (D3D10DDI_HRTELEMENTLAYOUT){0});

    blend_state.pDrvPrivate = allocate_private(
            funcs.pfnCalcPrivateBlendStateSize(device, NULL));
    funcs.pfnCreateBlendState(device, NULL, blend_state,
            (D3D10DDI_HRTBLENDSTATE){0});

    depth_stencil_state.pDrvPrivate = allocate_private(
            funcs.pfnCalcPrivateDepthStencilStateSize(device, NULL));
    funcs.pfnCreateDepthStencilState(device, NULL, depth_stencil_state,
            (D3D10DDI_HRTDEPTHSTENCILSTATE){0});

    rasterizer_state.pDrvPrivate = allocate_private(
            funcs.pfnCalcPrivateRasterizerStateSize(device, NULL));
    funcs.pfnCreateRasterizerState(device, NULL, rasterizer_state,
            (D3D10DDI_HRTRASTERIZERSTATE){0});

    /* The view is created over the render target, and that link is the one
     * thing about this call the frame depends on. */
    create_rtv.hDrvResource = render_target;
    rtv.pDrvPrivate = allocate_private(
            funcs.pfnCalcPrivateRenderTargetViewSize(device, &create_rtv));
    funcs.pfnCreateRenderTargetView(device, &create_rtv, rtv,
            (D3D10DDI_HRTRENDERTARGETVIEW){0});

    /* Binding. */
    bound_rtvs[0] = rtv;
    funcs.pfnSetRenderTargets(device, bound_rtvs, 1, 0,
            (D3D10DDI_HDEPTHSTENCILVIEW){0}, NULL, NULL, 1, 0, 1, 0);
    funcs.pfnSetViewports(device, 1, 0, NULL);
    funcs.pfnIaSetInputLayout(device, element_layout);
    bound_buffers[0] = vertex_buffer;
    strides[0] = 2 * (UINT)sizeof(FLOAT);
    offsets[0] = 0;
    funcs.pfnIaSetVertexBuffers(device, 0, 1, bound_buffers, strides, offsets);
    /* The topology constant is not named by this header -- see the
     * binding-types group -- so the frame passes the transport value and
     * asserts nothing about it. */
    funcs.pfnIaSetTopology(device, 0);
    funcs.pfnVsSetShader(device, vertex_shader);
    funcs.pfnPsSetShader(device, pixel_shader);
    funcs.pfnSetBlendState(device, blend_state, blend_factor, 0xffffffffu);
    funcs.pfnSetDepthStencilState(device, depth_stencil_state, 0);
    funcs.pfnSetRasterizerState(device, rasterizer_state);

    /* Clear and draw. The clear argument is non-const in the published
     * signature, so it is copied into a mutable array rather than cast. */
    memcpy(clear_argument, clear_colour, sizeof(clear_argument));
    funcs.pfnClearRenderTargetView(device, rtv, clear_argument);
    funcs.pfnDraw(device, 3, 0);

    /* Readback follows the application-level test's exact contract: copy the
     * completed render target, map the staging resource, inspect the centre
     * and corner texels, then unmap before teardown.  The DDI map page does
     * not publish numeric enum values, so zero is only a transport sentinel
     * for this recording stub and is not asserted as a named map mode. */
    funcs.pfnResourceCopy(device, staging_resource, render_target);
    funcs.pfnResourceMap(device, staging_resource, 0, 0, 0, &mapped);
    pixels = (const unsigned char *)mapped.pData;
    centre = pixels + (TARGET_HEIGHT / 2) * mapped.RowPitch
            + (TARGET_WIDTH / 2) * BYTES_PER_TEXEL;
    corner = pixels;
    check(centre[0] == 0xff && centre[1] == 0x00
            && centre[2] == 0x00 && centre[3] == 0xff,
            "the centre texel carries the drawn colour");
    check(corner[0] == 0x00 && corner[1] == 0x00
            && corner[2] == 0xff && corner[3] == 0xff,
            "the corner texel carries the clear colour");
    check(mapped.RowPitch == TARGET_ROW_PITCH
            && mapped.DepthPitch == TARGET_BYTES,
            "the mapped pitches describe the staging buffer");
    funcs.pfnResourceUnmap(device, staging_resource, 0);

    /* Teardown, in reverse. */
    funcs.pfnDestroyRenderTargetView(device, rtv);
    funcs.pfnDestroyRasterizerState(device, rasterizer_state);
    funcs.pfnDestroyDepthStencilState(device, depth_stencil_state);
    funcs.pfnDestroyBlendState(device, blend_state);
    funcs.pfnDestroyElementLayout(device, element_layout);
    funcs.pfnDestroyShader(device, pixel_shader);
    funcs.pfnDestroyShader(device, vertex_shader);
    funcs.pfnDestroyResource(device, staging_resource);
    funcs.pfnDestroyResource(device, render_target);
    funcs.pfnDestroyResource(device, vertex_buffer);

    check_sequence();

    /* Handle plumbing: each binding must have received the object the
     * matching creation produced, and not one of its neighbours. */
    check(rtv_source_resource == render_target.pDrvPrivate,
            "the render target view was created over the render target");
    check(handle_at("SetRenderTargets", 0) == rtv.pDrvPrivate,
            "the view bound as render target is the one created");
    check(handle_at("IaSetVertexBuffers", 0) == vertex_buffer.pDrvPrivate,
            "the buffer bound to the input assembler is the vertex buffer");
    check(handle_at("IaSetInputLayout", 0) == element_layout.pDrvPrivate,
            "the input layout bound is the one created");
    check(handle_at("VsSetShader", 0) == vertex_shader.pDrvPrivate,
            "the vertex stage received the vertex shader");
    check(handle_at("PsSetShader", 0) == pixel_shader.pDrvPrivate,
            "the pixel stage received the pixel shader");
    check(handle_at("ClearRenderTargetView", 0) == rtv.pDrvPrivate,
            "the clear targeted the bound view");
    check(handle_at("ResourceCopy", 0) == staging_resource.pDrvPrivate
            && copy_source_resource == render_target.pDrvPrivate
            && handle_at("ResourceMap", 0) == staging_resource.pDrvPrivate
            && handle_at("ResourceUnmap", 0) == staging_resource.pDrvPrivate,
            "copy, map, and unmap used the staging resource");
    check(handle_at("SetBlendState", 0) == blend_state.pDrvPrivate
            && handle_at("SetDepthStencilState", 0)
                    == depth_stencil_state.pDrvPrivate
            && handle_at("SetRasterizerState", 0)
                    == rasterizer_state.pDrvPrivate,
            "each state object bound is the one created");

    /* Every object got a distinct private block, which is what makes the
     * checks above discriminating rather than vacuous. */
    check(vertex_buffer.pDrvPrivate != render_target.pDrvPrivate
            && vertex_shader.pDrvPrivate != pixel_shader.pDrvPrivate
            && rtv.pDrvPrivate != render_target.pDrvPrivate
            && staging_resource.pDrvPrivate != render_target.pDrvPrivate,
            "the driver private blocks are distinct");

    /* Scalar arguments that carry the frame's meaning. */
    check(bound_rtv_count == 1, "exactly one render target was bound");
    check(drawn_vertex_count == 3 && drawn_start_vertex == 0,
            "the draw asked for three vertices from the start of the buffer");
    check(recorded_stride == 2 * (UINT)sizeof(FLOAT),
            "the vertex stride survived the call");
    check(recorded_clear[2] == 1.0f && recorded_clear[3] == 1.0f
            && recorded_clear[0] == 0.0f && recorded_clear[1] == 0.0f,
            "the clear colour survived the call in RGBA order");

    if (failures)
    {
        printf("[fail] %d check(s) failed\n", failures);
        return 1;
    }
    printf("[ ok ] the triangle frame is expressible against the promoted "
            "device function table\n");
    return 0;
}
