/* SPDX-License-Identifier: GPL-3.0-only
 *
 * Hardware acceptance for the Tier 1 surface: constant buffers, dynamic
 * WRITE_DISCARD maps, UpdateSubresource, samplers, blend state and scissor.
 *
 * tests/e2e_d3d11_triangle.cpp proves a frame reaches the render target with
 * shaders that read nothing.  This frame needs every Tier 1 operation, and
 * each sampled texel is owned by exactly one of them, so a wrong texel names
 * the operation that failed:
 *
 *   64x64 target, cleared blue.
 *   Pass 1  top half       constant-buffer colour, green.
 *   Pass 2  bottom half    same buffer re-mapped WRITE_DISCARD, yellow.  Pass
 *                          1 must keep green: discard renames the buffer.
 *   Pass 3  right half     a 2x2 texture filled by UpdateSubresource, point
 *                          sampled, alpha blended, scissored to rows 0-47.
 *
 *   texel (8, 8)   green    constant buffer read by pass 1
 *   texel (8, 56)  yellow   WRITE_DISCARD left pass 1's draw intact
 *   texel (40, 8)  red      UpdateSubresource and the sampler (opaque texel)
 *   texel (56, 8)  green    blending: a zero-alpha texel keeps the destination
 *   texel (40, 40) black    sampling inside the scissor
 *   texel (56, 40) magenta  sampling inside the scissor
 *   texel (40, 56) yellow   the scissor rejected pass 3 below row 48
 *
 * Every expected value is an exact byte: the blended texels have alpha 0 or
 * 1, so no case depends on how the hardware rounds a partial blend.  The
 * vertex buffer is created as zeros and filled by UpdateSubresource, so if
 * the buffer update fails nothing is drawn.
 *
 * Offscreen like the triangle test, and run by the same launcher:
 * scripts/run-d3dmetal-frame.py --test tier1.
 */

#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>

namespace {

const UINT TARGET_WIDTH = 64;
const UINT TARGET_HEIGHT = 64;
const FLOAT CLEAR_COLOUR[4] = {0.0f, 0.0f, 1.0f, 1.0f};
const FLOAT GREEN[4] = {0.0f, 1.0f, 0.0f, 1.0f};
const FLOAT YELLOW[4] = {1.0f, 1.0f, 0.0f, 1.0f};

const char COLOUR_VS[] =
    "float4 main(float2 position : POSITION) : SV_POSITION\n"
    "{\n"
    "    return float4(position, 0.0f, 1.0f);\n"
    "}\n";

const char COLOUR_PS[] =
    "cbuffer colour : register(b0) { float4 colour; };\n"
    "float4 main() : SV_TARGET\n"
    "{\n"
    "    return colour;\n"
    "}\n";

const char TEXTURE_VS[] =
    "struct output { float4 position : SV_POSITION; float2 uv : TEXCOORD; };\n"
    "output main(float2 position : POSITION, float2 uv : TEXCOORD)\n"
    "{\n"
    "    output o;\n"
    "    o.position = float4(position, 0.0f, 1.0f);\n"
    "    o.uv = uv;\n"
    "    return o;\n"
    "}\n";

const char TEXTURE_PS[] =
    "Texture2D image : register(t0);\n"
    "SamplerState point_sampler : register(s0);\n"
    "float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET\n"
    "{\n"
    "    return image.Sample(point_sampler, uv);\n"
    "}\n";

struct Vertex { FLOAT x, y, u, v; };

/* Three quads as triangle lists, six vertices each.  UVs matter only for the
 * right-half quad; v runs downwards from the top edge. */
const Vertex QUADS[18] = {
    /* Top half. */
    {-1, 1, 0, 0}, {1, 1, 0, 0}, {-1, 0, 0, 0},
    {-1, 0, 0, 0}, {1, 1, 0, 0}, {1, 0, 0, 0},
    /* Bottom half. */
    {-1, 0, 0, 0}, {1, 0, 0, 0}, {-1, -1, 0, 0},
    {-1, -1, 0, 0}, {1, 0, 0, 0}, {1, -1, 0, 0},
    /* Right half, textured. */
    {0, 1, 0, 0}, {1, 1, 1, 0}, {0, -1, 0, 1},
    {0, -1, 0, 1}, {1, 1, 1, 0}, {1, -1, 1, 1},
};

/* RGBA8, rows top to bottom: red, white with alpha 0; black, magenta. */
const BYTE TEXELS[16] = {
    0xff, 0x00, 0x00, 0xff,   0xff, 0xff, 0xff, 0x00,
    0x00, 0x00, 0x00, 0xff,   0xff, 0x00, 0xff, 0xff,
};

template<typename T> void release(T *&object)
{
    if (object) object->Release();
    object = NULL;
}

struct FrameResources
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D12Device *device12 = NULL;
    ID3D12CommandQueue *queue = NULL;
    ID3D12Fence *fence = NULL;
    ID3DBlob *colour_vs_code = NULL, *colour_ps_code = NULL;
    ID3DBlob *texture_vs_code = NULL, *texture_ps_code = NULL;
    ID3D11VertexShader *colour_vs = NULL, *texture_vs = NULL;
    ID3D11PixelShader *colour_ps = NULL, *texture_ps = NULL;
    ID3D11InputLayout *colour_layout = NULL, *texture_layout = NULL;
    ID3D11Buffer *vertices = NULL, *constants = NULL;
    ID3D11Texture2D *image = NULL, *target = NULL, *staging = NULL;
    ID3D11ShaderResourceView *image_view = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11SamplerState *sampler = NULL;
    ID3D11BlendState *blend = NULL;
    ID3D11RasterizerState *plain = NULL, *scissored = NULL;
    HANDLE event = NULL;
    HMODULE router = NULL;
    bool mapped = false;

    ~FrameResources()
    {
        if (mapped) context->Unmap(staging, 0);
        if (context) context->ClearState();
        release(scissored); release(plain); release(blend); release(sampler);
        release(rtv); release(image_view); release(staging); release(target); release(image);
        release(constants); release(vertices); release(texture_layout); release(colour_layout);
        release(texture_ps); release(colour_ps); release(texture_vs); release(colour_vs);
        release(texture_ps_code); release(texture_vs_code);
        release(colour_ps_code); release(colour_vs_code);
        release(context); release(device); release(fence); release(queue); release(device12);
        if (event) CloseHandle(event);
        if (router) FreeLibrary(router);
    }
};

void log_modules()
{
    for (const wchar_t *name : {L"d3d11.dll", L"d3d11on12core.dll", L"d3d11on12host.dll",
            L"d3d11on12.dll", L"dxilconv.dll", L"d3d12.dll", L"dxgi.dll"})
    {
        wchar_t path[MAX_PATH] = {};
        HMODULE module = GetModuleHandleW(name);
        if (module) GetModuleFileNameW(module, path, MAX_PATH);
        printf("[module] %ls: %ls\n", name, module ? path : L"not loaded");
    }
}

int failures;

bool failed(const char *what, HRESULT hr)
{
    if (SUCCEEDED(hr))
        return false;
    printf("[fail] %s: hr=0x%08lx\n", what, (unsigned long)hr);
    ++failures;
    return true;
}

ID3DBlob *compile(const char *source, size_t length, const char *target)
{
    ID3DBlob *code = NULL;
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(source, length, NULL, NULL, NULL, "main", target,
            0, 0, &code, &errors);
    if (FAILED(hr))
    {
        printf("[fail] compiling %s: %s\n", target,
                errors ? (const char *)errors->GetBufferPointer() : "no log");
        ++failures;
    }
    if (errors)
        errors->Release();
    return SUCCEEDED(hr) ? code : NULL;
}

/* A dynamic constant buffer is only ever written through WRITE_DISCARD. */
bool write_colour(ID3D11DeviceContext *context, ID3D11Buffer *buffer, const FLOAT colour[4])
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (failed("Map constant buffer (WRITE_DISCARD)", hr)) return false;
    if (!mapped.pData)
    {
        printf("[fail] constant buffer map returned no pointer\n");
        ++failures;
        context->Unmap(buffer, 0);
        return false;
    }
    memcpy(mapped.pData, colour, 4 * sizeof(FLOAT));
    context->Unmap(buffer, 0);
    return true;
}

struct Expected
{
    UINT x, y;
    BYTE rgba[4];
    const char *meaning;
};

const Expected EXPECTED[] = {
    { 8,  8, {0x00, 0xff, 0x00, 0xff}, "constant buffer read by pass 1"},
    { 8, 56, {0xff, 0xff, 0x00, 0xff}, "WRITE_DISCARD left pass 1's draw intact"},
    {40,  8, {0xff, 0x00, 0x00, 0xff}, "UpdateSubresource texel through the sampler"},
    {56,  8, {0x00, 0xff, 0x00, 0xff}, "blending kept the destination under alpha 0"},
    {40, 40, {0x00, 0x00, 0x00, 0xff}, "sampling inside the scissor"},
    {56, 40, {0xff, 0x00, 0xff, 0xff}, "sampling inside the scissor"},
    {40, 56, {0xff, 0xff, 0x00, 0xff}, "the scissor rejected pass 3 below row 48"},
};

void check_pixels(const BYTE *pixels, UINT row_pitch)
{
    for (const Expected &e : EXPECTED)
    {
        const BYTE *texel = pixels + e.y * row_pitch + e.x * 4;
        if (!memcmp(texel, e.rgba, 4))
        {
            printf("[ ok ] texel (%u, %u): %s\n", e.x, e.y, e.meaning);
            continue;
        }
        printf("[fail] texel (%u, %u) is %02x%02x%02x%02x, expected %02x%02x%02x%02x: %s\n",
                e.x, e.y, texel[0], texel[1], texel[2], texel[3],
                e.rgba[0], e.rgba[1], e.rgba[2], e.rgba[3], e.meaning);
        ++failures;
    }
}

}

int run_frame()
{
    FrameResources owned;
    auto &device = owned.device;
    auto &context = owned.context;
    const D3D_FEATURE_LEVEL requested_level = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL level = {};

    printf("[stage] creating the caller's D3D12 device\n");
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
            IID_ID3D12Device, reinterpret_cast<void **>(&owned.device12));
    if (failed("D3DMetal D3D12CreateDevice", hr) || !owned.device12) return 1;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = owned.device12->CreateCommandQueue(&queue_desc, IID_ID3D12CommandQueue,
            reinterpret_cast<void **>(&owned.queue));
    if (failed("CreateCommandQueue", hr) || !owned.queue) return 1;

    printf("[stage] loading the Relay router\n");
    owned.router = LoadLibraryW(L"d3d11.dll");
    if (!owned.router || !GetProcAddress(owned.router, "WineD3D11ShimGetStatus"))
    {
        printf("[fail] the Relay router is not loaded\n");
        return 1;
    }
    using CreateOn12 = decltype(&D3D11On12CreateDevice);
    FARPROC address = GetProcAddress(owned.router, "D3D11On12CreateDevice");
    CreateOn12 create_on12 = NULL;
    memcpy(&create_on12, &address, sizeof(create_on12));
    if (!create_on12) return 1;
    IUnknown *queues[] = {owned.queue};
    printf("[stage] creating the D3D11On12 device\n");
    hr = create_on12(owned.device12, 0, &requested_level, 1, queues, 1, 0,
            &device, &context, &level);
    if (failed("D3D11On12CreateDevice", hr) || !device || !context)
    {
        log_modules();
        return 1;
    }
    log_modules();

    printf("[stage] compiling DXBC shaders\n");
    owned.colour_vs_code = compile(COLOUR_VS, sizeof(COLOUR_VS) - 1, "vs_5_0");
    owned.colour_ps_code = compile(COLOUR_PS, sizeof(COLOUR_PS) - 1, "ps_5_0");
    owned.texture_vs_code = compile(TEXTURE_VS, sizeof(TEXTURE_VS) - 1, "vs_5_0");
    owned.texture_ps_code = compile(TEXTURE_PS, sizeof(TEXTURE_PS) - 1, "ps_5_0");
    if (!owned.colour_vs_code || !owned.colour_ps_code
            || !owned.texture_vs_code || !owned.texture_ps_code)
        return 1;

    printf("[stage] creating shaders and input layouts\n");
    if (failed("CreateVertexShader (colour)", device->CreateVertexShader(
            owned.colour_vs_code->GetBufferPointer(), owned.colour_vs_code->GetBufferSize(),
            NULL, &owned.colour_vs))) return 1;
    if (failed("CreatePixelShader (colour)", device->CreatePixelShader(
            owned.colour_ps_code->GetBufferPointer(), owned.colour_ps_code->GetBufferSize(),
            NULL, &owned.colour_ps))) return 1;
    if (failed("CreateVertexShader (texture)", device->CreateVertexShader(
            owned.texture_vs_code->GetBufferPointer(), owned.texture_vs_code->GetBufferSize(),
            NULL, &owned.texture_vs))) return 1;
    if (failed("CreatePixelShader (texture)", device->CreatePixelShader(
            owned.texture_ps_code->GetBufferPointer(), owned.texture_ps_code->GetBufferSize(),
            NULL, &owned.texture_ps))) return 1;

    D3D11_INPUT_ELEMENT_DESC elements[2] = {};
    elements[0].SemanticName = "POSITION";
    elements[0].Format = DXGI_FORMAT_R32G32_FLOAT;
    elements[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    elements[1].SemanticName = "TEXCOORD";
    elements[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    elements[1].AlignedByteOffset = 2 * sizeof(FLOAT);
    elements[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
    if (failed("CreateInputLayout (colour)", device->CreateInputLayout(elements, 1,
            owned.colour_vs_code->GetBufferPointer(), owned.colour_vs_code->GetBufferSize(),
            &owned.colour_layout))) return 1;
    if (failed("CreateInputLayout (texture)", device->CreateInputLayout(elements, 2,
            owned.texture_vs_code->GetBufferPointer(), owned.texture_vs_code->GetBufferSize(),
            &owned.texture_layout))) return 1;

    printf("[stage] creating buffers\n");
    /* Created as zeros: every quad is degenerate until UpdateSubresource. */
    static const Vertex ZEROS[18] = {};
    D3D11_BUFFER_DESC vertex_desc = {};
    vertex_desc.ByteWidth = sizeof(QUADS);
    vertex_desc.Usage = D3D11_USAGE_DEFAULT;
    vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA zero_data = {};
    zero_data.pSysMem = ZEROS;
    if (failed("CreateBuffer (vertices)", device->CreateBuffer(&vertex_desc, &zero_data,
            &owned.vertices))) return 1;

    D3D11_BUFFER_DESC constant_desc = {};
    constant_desc.ByteWidth = 16;
    constant_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (failed("CreateBuffer (constants)", device->CreateBuffer(&constant_desc, NULL,
            &owned.constants))) return 1;

    printf("[stage] creating textures and views\n");
    D3D11_TEXTURE2D_DESC image_desc = {};
    image_desc.Width = 2;
    image_desc.Height = 2;
    image_desc.MipLevels = 1;
    image_desc.ArraySize = 1;
    image_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    image_desc.SampleDesc.Count = 1;
    image_desc.Usage = D3D11_USAGE_DEFAULT;
    image_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (failed("CreateTexture2D (image)", device->CreateTexture2D(&image_desc, NULL,
            &owned.image))) return 1;
    if (failed("CreateShaderResourceView", device->CreateShaderResourceView(owned.image, NULL,
            &owned.image_view))) return 1;

    D3D11_TEXTURE2D_DESC target_desc = image_desc;
    target_desc.Width = TARGET_WIDTH;
    target_desc.Height = TARGET_HEIGHT;
    target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (failed("CreateTexture2D (target)", device->CreateTexture2D(&target_desc, NULL,
            &owned.target))) return 1;
    if (failed("CreateRenderTargetView", device->CreateRenderTargetView(owned.target, NULL,
            &owned.rtv))) return 1;

    printf("[stage] creating state objects\n");
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (failed("CreateSamplerState", device->CreateSamplerState(&sampler_desc,
            &owned.sampler))) return 1;

    /* Colour follows source alpha; destination alpha is kept, so the target
     * stays opaque and every expected byte is exact. */
    D3D11_BLEND_DESC blend_desc = {};
    auto &rt = blend_desc.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
    rt.DestBlendAlpha = D3D11_BLEND_ONE;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (failed("CreateBlendState", device->CreateBlendState(&blend_desc, &owned.blend)))
        return 1;

    D3D11_RASTERIZER_DESC raster_desc = {};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    if (failed("CreateRasterizerState (plain)", device->CreateRasterizerState(&raster_desc,
            &owned.plain))) return 1;
    raster_desc.ScissorEnable = TRUE;
    if (failed("CreateRasterizerState (scissored)", device->CreateRasterizerState(&raster_desc,
            &owned.scissored))) return 1;

    printf("[stage] updating resources\n");
    context->UpdateSubresource(owned.vertices, 0, NULL, QUADS, 0, 0);
    context->UpdateSubresource(owned.image, 0, NULL, TEXELS, 2 * 4, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = (FLOAT)TARGET_WIDTH;
    viewport.Height = (FLOAT)TARGET_HEIGHT;
    viewport.MaxDepth = 1.0f;
    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;

    printf("[stage] binding shared state\n");
    context->OMSetRenderTargets(1, &owned.rtv, NULL);
    context->RSSetViewports(1, &viewport);
    context->IASetVertexBuffers(0, 1, &owned.vertices, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->ClearRenderTargetView(owned.rtv, CLEAR_COLOUR);

    printf("[stage] passes 1 and 2: constant-buffer colours\n");
    context->RSSetState(owned.plain);
    context->IASetInputLayout(owned.colour_layout);
    context->VSSetShader(owned.colour_vs, NULL, 0);
    context->PSSetShader(owned.colour_ps, NULL, 0);
    context->PSSetConstantBuffers(0, 1, &owned.constants);
    if (!write_colour(context, owned.constants, GREEN)) return 1;
    context->Draw(6, 0);
    if (!write_colour(context, owned.constants, YELLOW)) return 1;
    context->Draw(6, 6);

    printf("[stage] pass 3: sampled, blended and scissored\n");
    const D3D11_RECT scissor = {(LONG)TARGET_WIDTH / 2, 0, (LONG)TARGET_WIDTH, 48};
    context->RSSetState(owned.scissored);
    context->RSSetScissorRects(1, &scissor);
    context->OMSetBlendState(owned.blend, NULL, 0xffffffff);
    context->IASetInputLayout(owned.texture_layout);
    context->VSSetShader(owned.texture_vs, NULL, 0);
    context->PSSetShader(owned.texture_ps, NULL, 0);
    context->PSSetShaderResources(0, 1, &owned.image_view);
    context->PSSetSamplers(0, 1, &owned.sampler);
    /* Bindings must retain state objects after their final public Release. */
    release(owned.sampler); release(owned.blend); release(owned.scissored);
    context->Draw(6, 12);
    context->Flush();

    printf("[stage] reading back\n");
    D3D11_TEXTURE2D_DESC staging_desc = target_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (failed("CreateTexture2D (staging)", device->CreateTexture2D(&staging_desc, NULL,
            &owned.staging))) return 1;
    context->CopyResource(owned.staging, owned.target);
    context->Flush();

    printf("[stage] waiting for caller queue\n");
    hr = owned.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
            reinterpret_cast<void **>(&owned.fence));
    if (failed("CreateFence", hr)) return 1;
    owned.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!owned.event) return 1;
    if (failed("Signal caller queue", owned.queue->Signal(owned.fence, 1))) return 1;
    if (failed("SetEventOnCompletion", owned.fence->SetEventOnCompletion(1, owned.event))) return 1;
    if (WaitForSingleObject(owned.event, 10000) != WAIT_OBJECT_0)
    {
        printf("[fail] caller queue did not complete within 10 seconds\n");
        return 1;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(owned.staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (failed("Map (staging)", hr)) return 1;
    owned.mapped = true;
    if (!mapped.pData || mapped.RowPitch < TARGET_WIDTH * 4)
    {
        printf("[fail] invalid readback pointer or row pitch\n");
        return 1;
    }
    check_pixels((const BYTE *)mapped.pData, mapped.RowPitch);
    context->Unmap(owned.staging, 0);
    owned.mapped = false;

    if (failures)
    {
        printf("[fail] %d check(s) failed\n", failures);
        return 1;
    }
    printf("[ ok ] every Tier 1 operation reached the render target\n");
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned int iteration = 0; iteration < 3; ++iteration)
    {
        printf("[stage] frame iteration %u\n", iteration + 1);
        if (run_frame()) return 1;
    }
    printf("[ ok ] three Tier 1 frames and device teardowns completed\n");
    return 0;
}
