/* SPDX-License-Identifier: GPL-3.0-only
 * Public COM lifetime regression against the compiled Wine host and mock DDI.
 * Releasing objects while bound must preserve them until ClearState or device
 * teardown, without creating a device/context/child reference cycle.
 */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11_4.h>
#include <stdio.h>
#include "d3d11on12core.h"
#include "d3d11on12mocks.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("[fail] line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
#define REQUIRE(x) do { CHECK(x); if (failures) return 1; } while (0)

typedef HRESULT (WINAPI *create_fn)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT,
        IUnknown *const *, UINT, UINT, ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);

int main(void)
{
    static const char vs_source[] = "float4 main(float2 p:POSITION):SV_POSITION {return float4(p,0,1);}";
    static const char ps_source[] = "float4 main():SV_TARGET {return float4(1,0,0,1);}";
    const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
    struct mock_device underlying;
    struct mock_queue queue;
    IUnknown *queues[1];
    HMODULE core, driver;
    create_fn create;
    FARPROC address;
    ID3DBlob *vs_code = NULL, *ps_code = NULL;
    void (WINAPI *resources)(LONG *, LONG *, int *);
    void (WINAPI *shaders)(LONG *, LONG *, LONG *, LONG *, LONG *, void **, void **, int *);
    LONG created, destroyed, vs_created, ps_created, shaders_destroyed;
    unsigned int iteration;
    setvbuf(stdout, NULL, _IONBF, 0);
    mock_device_init(&underlying);
    underlying.support_device1 = 1;
    underlying.expected_levels = &requested;
    underlying.expected_level_count = 1;
    mock_queue_init(&queue, &underlying, D3D12_COMMAND_LIST_TYPE_DIRECT);
    queues[0] = (IUnknown *)&queue.ID3D12CommandQueue_iface;
    core = LoadLibraryW(L"d3d11on12core.dll");
    REQUIRE(core != NULL);
    address = GetProcAddress(core, "WineD3D11On12CreateDeviceV1");
    memcpy(&create, &address, sizeof(create));
    REQUIRE(create != NULL);
    REQUIRE(SUCCEEDED(D3DCompile(vs_source, sizeof(vs_source), NULL, NULL, NULL,
            "main", "vs_5_0", 0, 0, &vs_code, NULL)));
    REQUIRE(SUCCEEDED(D3DCompile(ps_source, sizeof(ps_source), NULL, NULL, NULL,
            "main", "ps_5_0", 0, 0, &ps_code, NULL)));
    SetEnvironmentVariableW(L"RELAY12_EXPERIMENTAL_FRAME", L"1");
    for (iteration = 0; iteration < 3; ++iteration)
    {
        ID3D11Device *device = NULL;
        ID3D11DeviceContext *context = NULL;
        ID3D11Buffer *buffer = NULL;
        ID3D11Texture2D *texture = NULL;
        ID3D11RenderTargetView *view = NULL;
        ID3D11Resource *revived = NULL;
        ID3D11VertexShader *vs = NULL;
        ID3D11PixelShader *ps = NULL;
        ID3D11InputLayout *layout = NULL;
        D3D_FEATURE_LEVEL selected = 0;
        D3D11_BUFFER_DESC bd = {0};
        D3D11_TEXTURE2D_DESC td = {0};
        D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,
                0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        UINT stride = 8, offset = 0;
        REQUIRE(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
                queues, 1, 0, &device, &context, &selected) == S_OK);
        REQUIRE(device && context && selected == requested);
        if (!iteration)
            CHECK(!GetModuleHandleW(L"d3d11on12.dll"));
        {
            ID3D11ComputeShader *unsupported = (void *)(UINT_PTR)1;
            ID3D11ShaderResourceView *views[2] = {(void *)(UINT_PTR)1, (void *)(UINT_PTR)1};
            ID3D11Device5 *extended = (void *)(UINT_PTR)1;
            CHECK(ID3D11Device_CreateComputeShader(device, NULL, 0, NULL,
                    &unsupported) == DXGI_ERROR_UNSUPPORTED);
            CHECK(!unsupported);
            CHECK(ID3D11Device_QueryInterface(device, &IID_ID3D11Device5,
                    (void **)&extended) == E_NOINTERFACE);
            CHECK(!extended);
            ID3D11DeviceContext_PSSetShaderResources(context, 0, 0, NULL);
            ID3D11DeviceContext_PSGetShaderResources(context, 0, 2, views);
            CHECK(!views[0] && !views[1]);
        }
        {
            /* A standalone On12 device answers IDXGIDevice itself: callers
             * find the adapter through it, and ID3D11Device private data is
             * kept behind it.  Nothing beyond the base interface is offered. */
            static const GUID key = {0x5b1e6c3a, 0x2f47, 0x4c55,
                    {0x9d, 0x1e, 0x6a, 0x77, 0x10, 0x42, 0x3c, 0x81}};
            IDXGIDevice *dxgi_device = NULL;
            IDXGIDevice1 *dxgi_device1 = (void *)(UINT_PTR)1;
            IDXGIAdapter *adapter = (void *)(UINT_PTR)1;
            IDXGISurface *surface = (void *)(UINT_PTR)1;
            ID3D11Device *round_trip = NULL;
            void *parent = (void *)(UINT_PTR)1;
            DWORD value = 0xc0ffee, read = 0;
            UINT size = sizeof(read);
            INT priority = 1;
            unsigned int luid_calls = underlying.adapter_luid_calls;
            HRESULT hr;

            REQUIRE(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice,
                    (void **)&dxgi_device) == S_OK && dxgi_device);
            CHECK(IDXGIDevice_QueryInterface(dxgi_device, &IID_ID3D11Device,
                    (void **)&round_trip) == S_OK);
            CHECK(round_trip == device);
            if (round_trip)
                ID3D11Device_Release(round_trip);
            CHECK(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice1,
                    (void **)&dxgi_device1) == E_NOINTERFACE);
            CHECK(!dxgi_device1);

            CHECK(ID3D11Device_SetPrivateData(device, &key, sizeof(value), &value) == S_OK);
            CHECK(IDXGIDevice_GetPrivateData(dxgi_device, &key, &size, &read) == S_OK);
            CHECK(size == sizeof(value) && read == value);

            /* The mock's LUID names no DXGI adapter: the lookup must be made
             * and must fail without leaving a pointer behind. */
            CHECK(IDXGIDevice_GetAdapter(dxgi_device, NULL) == E_INVALIDARG);
            hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
            CHECK(underlying.adapter_luid_calls > luid_calls);
            CHECK(FAILED(hr) && !adapter);
            if (SUCCEEDED(hr) && adapter)
                IDXGIAdapter_Release(adapter);
            hr = IDXGIDevice_GetParent(dxgi_device, &IID_IDXGIAdapter, &parent);
            CHECK(FAILED(hr) && !parent);

            CHECK(IDXGIDevice_CreateSurface(dxgi_device, NULL, 1, 0, NULL,
                    &surface) == DXGI_ERROR_UNSUPPORTED);
            CHECK(!surface);
            CHECK(IDXGIDevice_QueryResourceResidency(dxgi_device, NULL, NULL, 0)
                    == DXGI_ERROR_UNSUPPORTED);
            CHECK(IDXGIDevice_SetGPUThreadPriority(dxgi_device, 0) == DXGI_ERROR_UNSUPPORTED);
            CHECK(IDXGIDevice_GetGPUThreadPriority(dxgi_device, &priority)
                    == DXGI_ERROR_UNSUPPORTED);
            CHECK(!priority);
            IDXGIDevice_Release(dxgi_device);
        }
        ID3D11DeviceContext_Flush(context);
        driver = GetModuleHandleW(L"d3d11on12.dll");
        address = GetProcAddress(driver, "WineD3D11On12MockDriverGetResourceCounts");
        memcpy(&resources, &address, sizeof(resources));
        address = GetProcAddress(driver, "WineD3D11On12MockDriverGetShaderCounts");
        memcpy(&shaders, &address, sizeof(shaders));
        REQUIRE(resources && shaders);
        {
            ID3D11Texture2D *sampled = NULL;
            ID3D11ShaderResourceView *srv = NULL, *retained = NULL;
            ID3D11RenderTargetView *output = NULL;
            ID3D11Resource *resource = NULL;
            D3D11_TEXTURE2D_DESC desc = {0};
            desc.Width = 8; desc.Height = 4;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            REQUIRE(ID3D11Device_CreateTexture2D(device, &desc, NULL, &sampled) == S_OK);
            REQUIRE(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)sampled, NULL, &srv) == S_OK);
            REQUIRE(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)sampled, NULL, &output) == S_OK);
            ID3D11DeviceContext_PSSetShaderResources(context, 5, 1, &srv);
            CHECK(ID3D11Texture2D_Release(sampled) == 0);
            CHECK(ID3D11ShaderResourceView_Release(srv) == 0);
            ID3D11DeviceContext_PSGetShaderResources(context, 5, 1, &retained);
            REQUIRE(retained == srv);
            ID3D11ShaderResourceView_GetResource(retained, &resource);
            CHECK(resource == (ID3D11Resource *)sampled);
            ID3D11Resource_Release(resource);
            ID3D11DeviceContext_OMSetRenderTargets(context, 1, &output, NULL);
            ID3D11DeviceContext_PSGetShaderResources(context, 5, 1, &srv);
            CHECK(!srv); /* Output binding clears the conflicting input. */
            ID3D11DeviceContext_PSSetShaderResources(context, 5, 1, &retained);
            ID3D11DeviceContext_PSGetShaderResources(context, 5, 1, &srv);
            CHECK(!srv); /* An input alias of the bound output becomes NULL. */
            ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
            ID3D11DeviceContext_PSSetShaderResources(context, 5, 1, &retained);
            CHECK(ID3D11ShaderResourceView_Release(retained) == 0);
            CHECK(ID3D11RenderTargetView_Release(output) == 0);
            ID3D11DeviceContext_ClearState(context);
            resources(&created, &destroyed, NULL);
            CHECK(created == destroyed);
        }
        bd.ByteWidth = 32; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        REQUIRE(ID3D11Device_CreateBuffer(device, &bd, NULL, &buffer) == S_OK);
        {
            IUnknown *dxgi = (void *)(UINT_PTR)1;
            CHECK(ID3D11Buffer_QueryInterface(buffer, &IID_IDXGIResource,
                    (void **)&dxgi) == E_NOINTERFACE);
            CHECK(!dxgi);
        }
        td.Width = 8; td.Height = 4; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        REQUIRE(ID3D11Device_CreateTexture2D(device, &td, NULL, &texture) == S_OK);
        REQUIRE(ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)texture, NULL, &view) == S_OK);
        REQUIRE(ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vs_code),
                ID3D10Blob_GetBufferSize(vs_code), NULL, &vs) == S_OK);
        REQUIRE(ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(ps_code),
                ID3D10Blob_GetBufferSize(ps_code), NULL, &ps) == S_OK);
        REQUIRE(ID3D11Device_CreateInputLayout(device, &element, 1, ID3D10Blob_GetBufferPointer(vs_code),
                ID3D10Blob_GetBufferSize(vs_code), &layout) == S_OK);
        ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &buffer, &stride, &offset);
        ID3D11DeviceContext_IASetInputLayout(context, layout);
        ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);
        ID3D11DeviceContext_OMSetRenderTargets(context, 1, &view, NULL);
        /* The view keeps the texture alive after its last public Release. */
        CHECK(ID3D11Texture2D_Release(texture) == 0);
        ID3D11RenderTargetView_GetResource(view, &revived);
        CHECK(revived == (ID3D11Resource *)texture);
        ID3D11Resource_Release(revived);
        CHECK(ID3D11Buffer_Release(buffer) == 0);
        CHECK(ID3D11InputLayout_Release(layout) == 0);
        CHECK(ID3D11VertexShader_Release(vs) == 0);
        CHECK(ID3D11PixelShader_Release(ps) == 0);
        CHECK(ID3D11RenderTargetView_Release(view) == 0);
        resources(&created, &destroyed, NULL);
        shaders(&vs_created, &ps_created, &shaders_destroyed, NULL, NULL, NULL, NULL, NULL);
        CHECK(created - destroyed == 2);
        CHECK(vs_created + ps_created - shaders_destroyed == 2);
        if (iteration != 1)
        {
            ID3D11DeviceContext_ClearState(context);
            ID3D11DeviceContext_ClearState(context);
            resources(&created, &destroyed, NULL);
            shaders(&vs_created, &ps_created, &shaders_destroyed, NULL, NULL, NULL, NULL, NULL);
            CHECK(created == destroyed);
            CHECK(vs_created + ps_created == shaders_destroyed);
        }
        CHECK(ID3D11Device_Release(device) == 1);
        CHECK(ID3D11DeviceContext_Release(context) == 0);
        resources(&created, &destroyed, NULL);
        shaders(&vs_created, &ps_created, &shaders_destroyed, NULL, NULL, NULL, NULL, NULL);
        CHECK(created == destroyed);
        CHECK(vs_created + ps_created == shaders_destroyed);
        CHECK(underlying.refcount == 1 && queue.refcount == 1);
    }
    {
        /* Patch 0029: constant buffers bind, are retained while bound, read
         * back through the getters, and map and update through the core. */
        ID3D11Device *device = NULL;
        ID3D11DeviceContext *context = NULL;
        ID3D11Buffer *dynamic = NULL, *fixed = NULL, *got[2] = {NULL, NULL};
        D3D11_BUFFER_DESC bd = {0};
        D3D11_MAPPED_SUBRESOURCE mapped = {0};
        D3D_FEATURE_LEVEL selected = 0;
        UINT first[2] = {7, 7}, count[2] = {7, 7};
        const BYTE constants[64] = {1, 2, 3, 4};
        ID3D11DeviceContext1 *context1 = NULL;

        REQUIRE(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
                queues, 1, 0, &device, &context, &selected) == S_OK);
        bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        REQUIRE(ID3D11Device_CreateBuffer(device, &bd, NULL, &dynamic) == S_OK);
        bd.Usage = D3D11_USAGE_DEFAULT; bd.CPUAccessFlags = 0;
        REQUIRE(ID3D11Device_CreateBuffer(device, &bd, NULL, &fixed) == S_OK);
        CHECK(ID3D11DeviceContext_Map(context, (ID3D11Resource *)dynamic, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped) == S_OK);
        CHECK(mapped.pData != NULL);
        if (mapped.pData) memcpy(mapped.pData, constants, sizeof(constants));
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)dynamic, 0);
        CHECK(ID3D11DeviceContext_Map(context, (ID3D11Resource *)fixed, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped) == DXGI_ERROR_INVALID_CALL);
        ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)fixed, 0, NULL,
                constants, 0, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(context, 0, 1, &dynamic);
        ID3D11DeviceContext_PSSetConstantBuffers(context, 13, 1, &fixed);
        /* Bound buffers outlive their last public reference. */
        CHECK(ID3D11Buffer_Release(dynamic) == 0);
        CHECK(ID3D11Buffer_Release(fixed) == 0);
        ID3D11DeviceContext_VSGetConstantBuffers(context, 0, 2, got);
        CHECK(got[0] == dynamic && !got[1]);
        if (got[0]) ID3D11Buffer_Release(got[0]);
        if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(context, &IID_ID3D11DeviceContext1,
                (void **)&context1)))
        {
            got[0] = NULL;
            ID3D11DeviceContext1_PSGetConstantBuffers1(context1, 13, 1, got, first, count);
            CHECK(got[0] == fixed && first[0] == 0 && count[0] == 4);
            if (got[0]) ID3D11Buffer_Release(got[0]);
            ID3D11DeviceContext1_Release(context1);
        }
        ID3D11DeviceContext_ClearState(context);
        ID3D11DeviceContext_VSGetConstantBuffers(context, 0, 1, got);
        CHECK(!got[0]);
        resources(&created, &destroyed, NULL);
        CHECK(created == destroyed);
        CHECK(ID3D11Device_Release(device) == 1);
        CHECK(ID3D11DeviceContext_Release(context) == 0);
        CHECK(underlying.refcount == 1 && queue.refcount == 1);
    }
    {
        /* Patch 0030: state objects reach the driver once per unique
         * description, are retained while bound, read back through the
         * getters, and are destroyed when the last binding lets go. */
        ID3D11Device *device = NULL;
        ID3D11Device1 *device1 = NULL;
        ID3D11DeviceContext *context = NULL;
        ID3D11BlendState *blend = NULL, *same = NULL, *got_blend = NULL;
        ID3D11BlendState1 *logic = (void *)(UINT_PTR)1;
        ID3D11DepthStencilState *depth = NULL, *got_depth = NULL;
        ID3D11RasterizerState *raster = NULL, *got_raster = NULL;
        ID3D11SamplerState *sampler = NULL, *got_samplers[2] = {NULL, NULL};
        D3D11_BLEND_DESC bd = {0};
        D3D11_BLEND_DESC1 bd1 = {0};
        D3D11_DEPTH_STENCIL_DESC dd = {0};
        D3D11_RASTERIZER_DESC rd = {0};
        D3D11_SAMPLER_DESC sd = {0};
        D3D11_RECT rects[2] = {{0, 0, 4, 4}, {2, 2, 8, 8}}, got_rects[2];
        D3D_FEATURE_LEVEL selected = 0;
        const float factor[4] = {0.5f, 0.25f, 0.125f, 1.0f};
        float got_factor[4];
        UINT mask = 0, ref = 0, rect_count = 0;
        LONG creates[3], destroys[3], binds[3], before[3], samplers_created, samplers_destroyed;
        void (WINAPI *state_counts)(LONG *, LONG *, LONG *);
        void (WINAPI *sampler_record)(LONG *, LONG *, LONG *, void *, int *);

        SetEnvironmentVariableW(L"RELAY12_MOCK_STATE_SLOTS", L"1");
        REQUIRE(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
                queues, 1, 0, &device, &context, &selected) == S_OK);
        address = GetProcAddress(driver, "WineD3D11On12MockDriverGetStateCounts");
        memcpy(&state_counts, &address, sizeof(state_counts));
        address = GetProcAddress(driver, "WineD3D11On12MockDriverGetSamplerRecord");
        memcpy(&sampler_record, &address, sizeof(sampler_record));
        REQUIRE(state_counts && sampler_record);
        ID3D11DeviceContext_Flush(context); /* opens the adapter with the slots */
        state_counts(before, NULL, NULL);
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        REQUIRE(ID3D11Device_CreateBlendState(device, &bd, &blend) == S_OK);
        CHECK(ID3D11Device_CreateBlendState(device, &bd, &same) == S_OK && same == blend);
        if (same) ID3D11BlendState_Release(same);
        if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_ID3D11Device1, (void **)&device1)))
        {
            bd1.RenderTarget[0].LogicOpEnable = TRUE;
            bd1.RenderTarget[0].LogicOp = D3D11_LOGIC_OP_XOR;
            bd1.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            CHECK(ID3D11Device1_CreateBlendState1(device1, &bd1, &logic) == DXGI_ERROR_UNSUPPORTED);
            CHECK(!logic);
            ID3D11Device1_Release(device1);
        }
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        REQUIRE(ID3D11Device_CreateDepthStencilState(device, &dd, &depth) == S_OK);
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE; rd.ScissorEnable = TRUE;
        REQUIRE(ID3D11Device_CreateRasterizerState(device, &rd, &raster) == S_OK);
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        REQUIRE(ID3D11Device_CreateSamplerState(device, &sd, &sampler) == S_OK);
        state_counts(creates, NULL, NULL);
        CHECK(creates[0] == before[0] + 1 && creates[1] == before[1] + 1 && creates[2] == before[2] + 1);

        ID3D11DeviceContext_OMSetBlendState(context, blend, factor, 0x0f);
        ID3D11DeviceContext_OMSetDepthStencilState(context, depth, 7);
        ID3D11DeviceContext_RSSetState(context, raster);
        ID3D11DeviceContext_VSSetSamplers(context, 0, 1, &sampler);
        ID3D11DeviceContext_PSSetSamplers(context, 3, 1, &sampler);
        ID3D11DeviceContext_RSSetScissorRects(context, 2, rects);
        state_counts(NULL, NULL, binds);
        CHECK(binds[0] && binds[1] && binds[2]);
        /* Bound state outlives its last public reference. */
        CHECK(ID3D11BlendState_Release(blend) == 0);
        CHECK(ID3D11DepthStencilState_Release(depth) == 0);
        CHECK(ID3D11RasterizerState_Release(raster) == 0);
        CHECK(ID3D11SamplerState_Release(sampler) == 0);
        state_counts(NULL, destroys, NULL);
        sampler_record(&samplers_created, &samplers_destroyed, NULL, NULL, NULL);
        CHECK(destroys[0] == before[0] && samplers_created == samplers_destroyed + 1);
        ID3D11DeviceContext_OMGetBlendState(context, &got_blend, got_factor, &mask);
        CHECK(got_blend == blend && !memcmp(got_factor, factor, sizeof(factor)) && mask == 0x0f);
        if (got_blend) ID3D11BlendState_Release(got_blend);
        ID3D11DeviceContext_OMGetDepthStencilState(context, &got_depth, &ref);
        CHECK(got_depth == depth && ref == 7);
        if (got_depth) ID3D11DepthStencilState_Release(got_depth);
        ID3D11DeviceContext_RSGetState(context, &got_raster);
        CHECK(got_raster == raster);
        if (got_raster) ID3D11RasterizerState_Release(got_raster);
        ID3D11DeviceContext_PSGetSamplers(context, 2, 2, got_samplers);
        CHECK(!got_samplers[0] && got_samplers[1] == sampler);
        if (got_samplers[1]) ID3D11SamplerState_Release(got_samplers[1]);
        ID3D11DeviceContext_RSGetScissorRects(context, &rect_count, NULL);
        CHECK(rect_count == 2);
        ID3D11DeviceContext_RSGetScissorRects(context, &rect_count, got_rects);
        CHECK(rect_count == 2 && !memcmp(got_rects, rects, sizeof(rects)));

        /* ClearState lets go of the last bindings: each reaches the driver's
         * destroy exactly once, sampler included. */
        ID3D11DeviceContext_ClearState(context);
        state_counts(NULL, destroys, NULL);
        sampler_record(&samplers_created, &samplers_destroyed, NULL, NULL, NULL);
        CHECK(destroys[0] == before[0] + 1 && destroys[1] == before[1] + 1 && destroys[2] == before[2] + 1);
        CHECK(samplers_created == samplers_destroyed);
        ID3D11DeviceContext_RSGetScissorRects(context, &rect_count, NULL);
        CHECK(rect_count == 0);
        /* The description tree no longer holds the destroyed state. */
        REQUIRE(ID3D11Device_CreateBlendState(device, &bd, &blend) == S_OK);
        state_counts(creates, NULL, NULL);
        CHECK(creates[0] == before[0] + 2);
        CHECK(ID3D11BlendState_Release(blend) == 0);
        state_counts(NULL, destroys, NULL);
        CHECK(destroys[0] == before[0] + 2);
        CHECK(ID3D11Device_Release(device) == 1);
        CHECK(ID3D11DeviceContext_Release(context) == 0);
        CHECK(underlying.refcount == 1 && queue.refcount == 1);
    }
    {
        /* Patch 0028: removal reaches GetDeviceRemovedReason from all three
         * sources -- D3D12 before the adapter opens, then a worker-thread
         * error the core latched, which outlives D3D12 recovering. */
        ID3D11Device *device = NULL;
        ID3D11DeviceContext *context = NULL;
        ID3D11Buffer *buffer = (void *)(UINT_PTR)1;
        D3D11_BUFFER_DESC bd = {0};
        D3D_FEATURE_LEVEL selected = 0;
        HRESULT (WINAPI *report_worker)(HRESULT);
        LONG opened_before = 0, opened_after = 0;
        void (WINAPI *counts)(LONG *, LONG *, LONG *, LONG *);

        REQUIRE(create((IUnknown *)&underlying.ID3D12Device_iface, 0, &requested, 1,
                queues, 1, 0, &device, &context, &selected) == S_OK);
        address = GetProcAddress(driver, "WineD3D11On12MockDriverReportWorkerError");
        memcpy(&report_worker, &address, sizeof(report_worker));
        address = GetProcAddress(driver, "WineD3D11On12MockDriverGetCounts");
        memcpy(&counts, &address, sizeof(counts));
        REQUIRE(report_worker && counts);
        counts(&opened_before, NULL, NULL, NULL);
        CHECK(ID3D11Device_GetDeviceRemovedReason(device) == S_OK);
        InterlockedExchange(&underlying.removed_reason, DXGI_ERROR_DEVICE_HUNG);
        CHECK(ID3D11Device_GetDeviceRemovedReason(device) == DXGI_ERROR_DEVICE_HUNG);
        InterlockedExchange(&underlying.removed_reason, S_OK);
        counts(&opened_after, NULL, NULL, NULL);
        CHECK(opened_after == opened_before); /* Asking opened no adapter. */
        ID3D11DeviceContext_Flush(context);
        CHECK(ID3D11Device_GetDeviceRemovedReason(device) == S_OK);
        CHECK(report_worker(E_OUTOFMEMORY) == S_OK);
        CHECK(ID3D11Device_GetDeviceRemovedReason(device) == DXGI_ERROR_DRIVER_INTERNAL_ERROR);
        bd.ByteWidth = 32; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        CHECK(ID3D11Device_CreateBuffer(device, &bd, NULL, &buffer) == DXGI_ERROR_DEVICE_REMOVED);
        CHECK(!buffer);
        ID3D11DeviceContext_Draw(context, 3, 0);
        CHECK(ID3D11Device_Release(device) == 1);
        CHECK(ID3D11DeviceContext_Release(context) == 0);
        CHECK(underlying.refcount == 1 && queue.refcount == 1);
    }
    ID3D10Blob_Release(vs_code);
    ID3D10Blob_Release(ps_code);
    FreeLibrary(core);
    printf("[%s] public frontend lifetime: %d failures\n", failures ? "fail" : " ok ", failures);
    return !!failures;
}
