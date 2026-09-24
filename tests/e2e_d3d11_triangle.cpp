/* SPDX-License-Identifier: GPL-3.0-only
 *
 * The application-level counterpart to tests/d3d11ddi_triangle.c.
 *
 * The DDI harness proves the frame is expressible against the promoted
 * device function table: that the slots a triangle needs are declared, accept
 * the handles the frame produces, and can be called in order.  It cannot
 * prove anything renders, because nothing stands behind the table yet.
 *
 * This is the same frame written against the public D3D11 API -- VS and PS,
 * one render target view, one draw, and a staging readback -- so that when a
 * host does stand behind the table, the two tests assert the same thing at
 * their two levels and disagreeing is a finding.
 *
 * The experimental host is enabled explicitly by the test launcher. The
 * caller creates and retains the D3D12 device and direct queue; readback and
 * fence completion must succeed before a frame is accepted.
 *
 * Deliberately offscreen: no swap chain and no window.  Wine's DXGI emulates
 * presentation above the DDI (docs/D3D11ON12-SKIPPABLE-ELEMENTS.md section 1),
 * so involving it would test Wine's compositor rather than the relay.
 */

#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <initializer_list>

namespace {

/* 64x64 is large enough that the centre texel is unambiguously interior and
 * the corner unambiguously exterior, at any sane rasteriser fill rule. */
const UINT TARGET_WIDTH = 64;
const UINT TARGET_HEIGHT = 64;

/* Distinguishable in a single byte each, so a channel-order mistake in the
 * translation layer reads as a wrong colour rather than a near-miss. */
const FLOAT CLEAR_COLOUR[4] = {0.0f, 0.0f, 1.0f, 1.0f};

const char VERTEX_SHADER[] =
    "float4 main(float2 position : POSITION) : SV_POSITION\n"
    "{\n"
    "    return float4(position, 0.0f, 1.0f);\n"
    "}\n";

const char PIXEL_SHADER[] =
    "float4 main() : SV_TARGET\n"
    "{\n"
    "    return float4(1.0f, 0.0f, 0.0f, 1.0f);\n"
    "}\n";

/* Covers the centre and none of the corners. */
const FLOAT TRIANGLE[6] = {
    -0.8f, -0.8f,
     0.0f,  0.8f,
     0.8f, -0.8f,
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
    ID3D11On12Device1 *on12 = NULL;
    ID3D12Device *original = NULL;
    ID3DBlob *vs_code = NULL, *ps_code = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11InputLayout *layout = NULL;
    ID3D11Buffer *vertices = NULL;
    ID3D11Texture2D *target = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    HANDLE event = NULL;
    HMODULE router = NULL;
    bool mapped = false;

    ~FrameResources()
    {
        if (mapped) context->Unmap(staging, 0);
        if (context) context->ClearState();
        release(staging); release(rtv); release(target);
        release(vertices); release(layout); release(ps); release(vs);
        release(ps_code); release(vs_code); release(original); release(on12);
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

/* The readback is the point of the test: the centre must be the colour the
 * pixel shader wrote and the corner the colour the clear wrote.  Checking only
 * the centre would pass against a target the clear had filled red. */
void check_pixels(const BYTE *pixels, UINT row_pitch)
{
    const BYTE *centre = pixels + (TARGET_HEIGHT / 2) * row_pitch
            + (TARGET_WIDTH / 2) * 4;
    const BYTE *corner = pixels;

    if (centre[0] == 0xff && centre[1] == 0x00 && centre[2] == 0x00
            && centre[3] == 0xff)
    {
        printf("[ ok ] the centre texel carries the drawn colour\n");
    }
    else
    {
        printf("[fail] centre texel is %02x%02x%02x%02x, expected ff0000ff\n",
                centre[0], centre[1], centre[2], centre[3]);
        ++failures;
    }

    if (corner[0] == 0x00 && corner[1] == 0x00 && corner[2] == 0xff
            && corner[3] == 0xff)
    {
        printf("[ ok ] the corner texel carries the clear colour\n");
    }
    else
    {
        printf("[fail] corner texel is %02x%02x%02x%02x, expected 0000ffff\n",
                corner[0], corner[1], corner[2], corner[3]);
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

    auto &device12 = owned.device12;
    auto &queue = owned.queue;
    printf("[stage] creating the caller's D3D12 device\n");
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
            IID_ID3D12Device, reinterpret_cast<void **>(&device12));
    if (failed("D3DMetal D3D12CreateDevice", hr) || !device12) return 1;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device12->CreateCommandQueue(&queue_desc, IID_ID3D12CommandQueue,
            reinterpret_cast<void **>(&queue));
    if (failed("CreateCommandQueue", hr) || !queue) return 1;
    printf("[stage] loading the Relay router\n");
    auto &router = owned.router;
    router = LoadLibraryW(L"d3d11.dll");
    if (!router || !GetProcAddress(router, "WineD3D11ShimGetStatus"))
    {
        printf("[fail] the Relay router is not loaded\n");
        return 1;
    }
    using CreateOn12 = decltype(&D3D11On12CreateDevice);
    FARPROC address = GetProcAddress(router, "D3D11On12CreateDevice");
    CreateOn12 create_on12 = NULL;
    memcpy(&create_on12, &address, sizeof(create_on12));
    if (!create_on12) return 1;
    IUnknown *queues[] = {queue};
    printf("[stage] creating the D3D11On12 device\n");
    hr = create_on12(device12, 0, &requested_level, 1, queues, 1, 0,
            &device, &context, &level);
    if (failed("D3D11On12CreateDevice", hr) || !device || !context)
    {
        log_modules();
        return 1;
    }
    auto &on12 = owned.on12;
    hr = device->QueryInterface(IID_ID3D11On12Device1, reinterpret_cast<void **>(&on12));
    if (failed("Query On12 device", hr) || !on12) return 1;
    auto &original = owned.original;
    hr = on12->GetD3D12Device(IID_ID3D12Device, &original);
    if (failed("GetD3D12Device", hr) || original != device12)
    {
        printf("[fail] the caller's D3D12 device was replaced\n");
        return 1;
    }
    release(original);
    release(on12);
    log_modules();
    printf("[ ok ] device created at feature level 0x%x\n", (unsigned)level);

    printf("[stage] compiling DXBC shaders\n");
    auto &vs_code = owned.vs_code;
    vs_code = compile(VERTEX_SHADER, sizeof(VERTEX_SHADER) - 1,
            "vs_5_0");
    auto &ps_code = owned.ps_code;
    ps_code = compile(PIXEL_SHADER, sizeof(PIXEL_SHADER) - 1,
            "ps_5_0");
    if (!vs_code || !ps_code)
        return 1;

    printf("[stage] creating vertex shader\n");
    auto &vs = owned.vs;
    auto &ps = owned.ps;
    hr = device->CreateVertexShader(vs_code->GetBufferPointer(),
            vs_code->GetBufferSize(), NULL, &vs);
    if (failed("CreateVertexShader", hr))
    {
        log_modules();
        return 1;
    }
    printf("[stage] creating pixel shader\n");
    hr = device->CreatePixelShader(ps_code->GetBufferPointer(),
            ps_code->GetBufferSize(), NULL, &ps);
    if (failed("CreatePixelShader", hr))
        return 1;

    printf("[stage] creating frame resources\n");
    D3D11_INPUT_ELEMENT_DESC element = {};
    element.SemanticName = "POSITION";
    element.Format = DXGI_FORMAT_R32G32_FLOAT;
    element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

    auto &layout = owned.layout;
    printf("[stage] creating input layout\n");
    hr = device->CreateInputLayout(&element, 1, vs_code->GetBufferPointer(),
            vs_code->GetBufferSize(), &layout);
    if (failed("CreateInputLayout", hr))
        return 1;

    D3D11_BUFFER_DESC vertex_desc = {};
    vertex_desc.ByteWidth = sizeof(TRIANGLE);
    vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertex_data = {};
    vertex_data.pSysMem = TRIANGLE;

    auto &vertices = owned.vertices;
    printf("[stage] creating vertex buffer\n");
    hr = device->CreateBuffer(&vertex_desc, &vertex_data, &vertices);
    if (failed("CreateBuffer", hr))
        return 1;

    D3D11_TEXTURE2D_DESC target_desc = {};
    target_desc.Width = TARGET_WIDTH;
    target_desc.Height = TARGET_HEIGHT;
    target_desc.MipLevels = 1;
    target_desc.ArraySize = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Usage = D3D11_USAGE_DEFAULT;
    target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    auto &target = owned.target;
    printf("[stage] creating render target\n");
    hr = device->CreateTexture2D(&target_desc, NULL, &target);
    if (failed("CreateTexture2D", hr))
        return 1;

    auto &rtv = owned.rtv;
    printf("[stage] creating target view\n");
    hr = device->CreateRenderTargetView(target, NULL, &rtv);
    if (failed("CreateRenderTargetView", hr))
        return 1;

    D3D11_VIEWPORT viewport = {};
    viewport.Width = (FLOAT)TARGET_WIDTH;
    viewport.Height = (FLOAT)TARGET_HEIGHT;
    viewport.MaxDepth = 1.0f;

    const UINT stride = 2 * sizeof(FLOAT);
    const UINT offset = 0;

    printf("[stage] binding frame resources\n");
    context->OMSetRenderTargets(1, &rtv, NULL);
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(layout);
    context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs, NULL, 0);
    context->PSSetShader(ps, NULL, 0);
    context->ClearRenderTargetView(rtv, CLEAR_COLOUR);
    /* Bindings must retain objects after their final public Release. */
    release(rtv); release(vertices); release(layout); release(vs); release(ps);
    printf("[stage] drawing\n");
    context->Draw(3, 0);
    printf("[stage] flushing draw\n");
    context->Flush();
    printf("[stage] creating staging texture\n");

    D3D11_TEXTURE2D_DESC staging_desc = target_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    auto &staging = owned.staging;
    hr = device->CreateTexture2D(&staging_desc, NULL, &staging);
    if (failed("CreateTexture2D (staging)", hr))
        return 1;

    printf("[stage] copying readback\n");
    context->CopyResource(staging, target);
    context->Flush();
    printf("[stage] waiting for caller queue\n");
    hr = device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
            reinterpret_cast<void **>(&owned.fence));
    if (failed("CreateFence", hr)) return 1;
    owned.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!owned.event) return 1;
    if (failed("Signal caller queue", queue->Signal(owned.fence, 1))) return 1;
    if (failed("SetEventOnCompletion", owned.fence->SetEventOnCompletion(1, owned.event))) return 1;
    if (WaitForSingleObject(owned.event, 10000) != WAIT_OBJECT_0)
    {
        printf("[fail] caller queue did not complete within 10 seconds\n");
        return 1;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    printf("[stage] mapping readback\n");
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (failed("Map", hr))
        return 1;
    owned.mapped = true;
    if (!mapped.pData || mapped.RowPitch < TARGET_WIDTH * 4)
    {
        printf("[fail] invalid readback pointer or row pitch\n");
        return 1;
    }
    check_pixels((const BYTE *)mapped.pData, mapped.RowPitch);
    context->Unmap(staging, 0);

    owned.mapped = false;
    log_modules();

    if (failures)
    {
        printf("[fail] %d check(s) failed\n", failures);
        return 1;
    }
    printf("[ ok ] the triangle reached the render target\n");
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
    printf("[ ok ] three frames and device teardowns completed\n");
    return 0;
}
