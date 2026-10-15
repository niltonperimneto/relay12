// SPDX-License-Identifier: GPL-3.0-only
// Real D3D11On12 initial-upload, sampling and byte-exact readback test.
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "uma_gpu_probe.hpp"

template<class T> struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
    void** out() { return reinterpret_cast<void**>(&p); }
};
#define HR(call) do { const HRESULT hr_ = (call); if (FAILED(hr_)) { \
    std::printf("[fail] %s hr=%08lx\n", #call, static_cast<unsigned long>(hr_)); return false; } } while (0)

static bool run(bool bench, bool transferOnly)
{
    auto module = LoadLibraryW(L"d3d11.dll");
    using CreateFn = HRESULT (WINAPI*)(IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT,
        IUnknown* const*, UINT, UINT, ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);
    auto create11 = module ? reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(module, "D3D11On12CreateDevice"))) : nullptr;
    if (!create11) return false;
    std::puts("[info] initializing DXGI adapter");
    Ref<IDXGIFactory1> factory;
    HR(CreateDXGIFactory1(IID_IDXGIFactory1, factory.out()));
    Ref<IDXGIAdapter1> adapter;
    HR(factory->EnumAdapters1(0, &adapter.p));
    std::puts("[info] DXGI adapter ready");
    Ref<ID3D12Device> device12;
    HR(D3D12CreateDevice(adapter.p, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, device12.out()));
    Ref<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qdesc = {}; qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(device12->CreateCommandQueue(&qdesc, IID_ID3D12CommandQueue, queue.out()));
    D3D12_FEATURE_DATA_ARCHITECTURE1 arch = {};
    const HRESULT archHr = device12->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch));
    std::printf("[info] architecture_hr=%08lx uma=%d coherent=%d\n", static_cast<unsigned long>(archHr), arch.UMA, arch.CacheCoherentUMA);
    for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM})
    {
        const HRESULT probe = uma_probe::check(device12.p, queue.p, format);
        std::printf("[info] direct_gpu_probe format=%u hr=%08lx\n", unsigned(format), static_cast<unsigned long>(probe));
        if (FAILED(probe)) return false;
    }
    Ref<ID3D11Device> device;
    Ref<ID3D11DeviceContext> context;
    IUnknown* queues[] = {queue.p};
    HR(create11(device12.p, 0, nullptr, 0, queues, 1, 0, &device.p, &context.p, nullptr));
    constexpr char shader[] =
        "Texture2D<float4> source:register(t0);"
        "float4 vs(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}"
        "float4 ps(float4 p:SV_Position):SV_Target{return source.Load(int3(int2(p.xy),0));}";
    Ref<ID3DBlob> vsCode, psCode;
    HR(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsCode.p, nullptr));
    HR(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psCode.p, nullptr));
    Ref<ID3D11VertexShader> vs; Ref<ID3D11PixelShader> ps;
    HR(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs.p));
    HR(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps.p));
    context->VSSetShader(vs.p, nullptr, 0); context->PSSetShader(ps.p, nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT width = bench ? 1023 : 257, height = bench ? 513 : 9;
    const UINT pitch = width * 4 + 12;
    std::vector<unsigned char> source(pitch * height, 0xa5);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x)
    {
        auto* pixel = &source[y * pitch + x * 4];
        pixel[0] = static_cast<unsigned char>(x); pixel[1] = static_cast<unsigned char>(y);
        pixel[2] = static_cast<unsigned char>(x + y); pixel[3] = 255;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    Ref<ID3D11Texture2D> target, staging, stagingBgra;
    HR(device->CreateTexture2D(&desc, nullptr, &target.p));
    Ref<ID3D11RenderTargetView> rtv;
    HR(device->CreateRenderTargetView(target.p, nullptr, &rtv.p));
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HR(device->CreateTexture2D(&desc, nullptr, &staging.p));
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    HR(device->CreateTexture2D(&desc, nullptr, &stagingBgra.p));
    context->OMSetRenderTargets(1, &rtv.p, nullptr);
    D3D11_VIEWPORT viewport = {0, 0, float(width), float(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    for (unsigned cycle = 0; cycle < (bench ? 5u : 2u); ++cycle)
    {
        LARGE_INTEGER start, end; QueryPerformanceCounter(&start);
        // Burst retains textures before sampling, exposing staging cache peaks.
        const unsigned count = bench ? 48 : 2;
        std::vector<ID3D11Texture2D*> textures(count, nullptr);
        desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (unsigned i = 0; i < count; ++i)
        {
            desc.Format = i % 2 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
            D3D11_SUBRESOURCE_DATA data = {source.data(), pitch, pitch * height};
            HR(device->CreateTexture2D(&desc, &data, &textures[i]));
        }
        LARGE_INTEGER uploaded; QueryPerformanceCounter(&uploaded);
        PROCESS_MEMORY_COUNTERS peak = {}; peak.cb = sizeof(peak);
        const BOOL havePeak = GetProcessMemoryInfo(GetCurrentProcess(), &peak, sizeof(peak));
        std::printf("[upload] cycle=%u wall_ms=%.3f working_set_bytes=%llu memory_available=%d\n",
            cycle, double(uploaded.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart,
            static_cast<unsigned long long>(peak.WorkingSetSize), havePeak);
        for (unsigned i = 0; i < count; ++i)
        {
            LARGE_INTEGER frameStart, frameEnd; QueryPerformanceCounter(&frameStart);
            Ref<ID3D11ShaderResourceView> srv;
            ID3D11Texture2D* readback = transferOnly && i % 2 ? stagingBgra.p : staging.p;
            if (!transferOnly)
            {
                HR(device->CreateShaderResourceView(textures[i], nullptr, &srv.p));
                context->PSSetShaderResources(0, 1, &srv.p);
                context->Draw(3, 0);
            }
            context->CopyResource(readback, transferOnly ? textures[i] : target.p);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            HR(context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped));
            bool correct = true;
            for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x)
            {
                const auto* actual = static_cast<unsigned char*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                const auto* expected = &source[y * pitch + x * 4];
                for (unsigned c = 0; c < 4; ++c)
                    correct = correct && actual[c] == expected[!transferOnly && i % 2 && c != 1 && c != 3 ? 2 - c : c];
            }
            context->Unmap(readback, 0);
            if (!correct) { std::printf("[fail] sampled pixels cycle=%u texture=%u\n", cycle, i); return false; }
            if (!transferOnly)
            {
                ID3D11ShaderResourceView* nullView = nullptr;
                context->PSSetShaderResources(0, 1, &nullView);
            }
            textures[i]->Release(); textures[i] = nullptr;
            QueryPerformanceCounter(&frameEnd);
            std::printf("[frame] cycle=%u texture=%u wall_ms=%.3f\n", cycle, i,
                double(frameEnd.QuadPart - frameStart.QuadPart) * 1000 / frequency.QuadPart);
        }
        context->Flush();
        QueryPerformanceCounter(&end);
        PROCESS_MEMORY_COUNTERS memory = {}; memory.cb = sizeof(memory);
        const BOOL haveMemory = GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
        std::printf("[sample] cycle=%u textures=%u wall_ms=%.3f working_set_bytes=%llu memory_available=%d\n",
            cycle, count, double(end.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart,
            static_cast<unsigned long long>(memory.WorkingSetSize), haveMemory);
    }
    context->ClearState(); context->Flush();
    return true;
}
int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("[info] starting UMA memory test");
    bool bench = false, transferOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--bench")) bench = true;
        else if (!std::strcmp(argv[i], "--transfer-only")) transferOnly = true;
        else return 2;
    }
    const bool ok = run(bench, transferOnly);
    std::printf("[%s] UMA initialization, %s and readback\n", ok ? " ok " : "fail",
        transferOnly ? "GPU copy" : "shader sampling");
    return ok ? 0 : 1;
}
