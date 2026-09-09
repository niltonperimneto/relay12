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
 * It is cross-compiled by .github/workflows/integration-test.yml and not run.
 * D3D11CreateDevice cannot currently reach the relay, so a run would fail for
 * a reason unrelated to whatever change is under test.
 *
 * Deliberately offscreen: no swap chain and no window.  Wine's DXGI emulates
 * presentation above the DDI (docs/D3D11ON12-SKIPPABLE-ELEMENTS.md section 1),
 * so involving it would test Wine's compositor rather than the relay.
 */

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>

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

int main(void)
{
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
            NULL, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (failed("D3D11CreateDevice", hr))
    {
        printf("[note] no D3D11 device: the relay or D3DMetal is not loaded\n");
        return 1;
    }
    printf("[ ok ] device created at feature level 0x%x\n", (unsigned)level);

    ID3DBlob *vs_code = compile(VERTEX_SHADER, sizeof(VERTEX_SHADER) - 1,
            "vs_5_0");
    ID3DBlob *ps_code = compile(PIXEL_SHADER, sizeof(PIXEL_SHADER) - 1,
            "ps_5_0");
    if (!vs_code || !ps_code)
        return 1;

    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    hr = device->CreateVertexShader(vs_code->GetBufferPointer(),
            vs_code->GetBufferSize(), NULL, &vs);
    if (failed("CreateVertexShader", hr))
        return 1;
    hr = device->CreatePixelShader(ps_code->GetBufferPointer(),
            ps_code->GetBufferSize(), NULL, &ps);
    if (failed("CreatePixelShader", hr))
        return 1;

    D3D11_INPUT_ELEMENT_DESC element = {};
    element.SemanticName = "POSITION";
    element.Format = DXGI_FORMAT_R32G32_FLOAT;
    element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;

    ID3D11InputLayout *layout = NULL;
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

    ID3D11Buffer *vertices = NULL;
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

    ID3D11Texture2D *target = NULL;
    hr = device->CreateTexture2D(&target_desc, NULL, &target);
    if (failed("CreateTexture2D", hr))
        return 1;

    ID3D11RenderTargetView *rtv = NULL;
    hr = device->CreateRenderTargetView(target, NULL, &rtv);
    if (failed("CreateRenderTargetView", hr))
        return 1;

    D3D11_VIEWPORT viewport = {};
    viewport.Width = (FLOAT)TARGET_WIDTH;
    viewport.Height = (FLOAT)TARGET_HEIGHT;
    viewport.MaxDepth = 1.0f;

    const UINT stride = 2 * sizeof(FLOAT);
    const UINT offset = 0;

    context->OMSetRenderTargets(1, &rtv, NULL);
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(layout);
    context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs, NULL, 0);
    context->PSSetShader(ps, NULL, 0);
    context->ClearRenderTargetView(rtv, CLEAR_COLOUR);
    context->Draw(3, 0);
    context->Flush();

    D3D11_TEXTURE2D_DESC staging_desc = target_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D *staging = NULL;
    hr = device->CreateTexture2D(&staging_desc, NULL, &staging);
    if (failed("CreateTexture2D (staging)", hr))
        return 1;

    context->CopyResource(staging, target);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (failed("Map", hr))
        return 1;
    check_pixels((const BYTE *)mapped.pData, mapped.RowPitch);
    context->Unmap(staging, 0);

    staging->Release();
    rtv->Release();
    target->Release();
    vertices->Release();
    layout->Release();
    ps->Release();
    vs->Release();
    ps_code->Release();
    vs_code->Release();
    context->Release();
    device->Release();

    if (failures)
    {
        printf("[fail] %d check(s) failed\n", failures);
        return 1;
    }
    printf("[ ok ] the triangle reached the render target\n");
    return 0;
}
