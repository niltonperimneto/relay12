// SPDX-License-Identifier: GPL-3.0-only
//
// Reproduce the D3D11On12 device creation Unity's D3D12 renderer performs,
// then report every interface the resulting objects do and do not answer.
//
// PEAK (Unity 6000.3.15f1) reports "d3d12: failed to create D3D11On12 device
// (0x80004002)" although D3D11On12CreateDevice succeeds through Relay12 and no
// QueryInterface reaches the traced Wine d3d11 objects.  An E_NOINTERFACE with
// no traced query points at an object D3DMetal owns, or at an interface asked
// of the D3D12 side; this probe asks each candidate once and prints the result
// so the failing query is named by evidence rather than by guess.
//
// Every query is independent: a failure is reported and the probe continues.
//
// It then checks, on the real D3DMetal stack, what patch 0025 promises and the
// mock-driver frontend test cannot show: IDXGIDevice's adapter is the one the
// D3D12 device runs on, both through GetAdapter and GetParent, and private
// data set through ID3D11Device reads back through IDXGIDevice.  Those checks
// print [ok]/[fail] and a final RESULT line, and set the exit status.
//
// Before any of that it reports, as "perf:" lines, the D3DMetal capabilities
// that decide two items of docs/PERFORMANCE-RESEARCH-ROADMAP.md.  They are
// facts to record, not checks, and never change the verdict:
//
//   * persistent PSO cache -- D3D12_FEATURE_SHADER_CACHE, and whether
//     ID3D12Device1::CreatePipelineLibrary gives a library that serializes;
//   * UMA staging -- D3D12_FEATURE_ARCHITECTURE1, and whether a CUSTOM heap
//     with WRITE_BACK pages in L0 can hold a buffer the CPU can map, and a
//     texture.
//
// Run it through Whisky, never the standalone D3DMetal rig.

#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include "uma_gpu_probe.hpp"
#include "../compat/relay_d3d12_struct_return.hpp"

typedef HRESULT(WINAPI *CreateDevice12Fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT(WINAPI *On12CreateDeviceFn)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT,
        IUnknown *const *, UINT, UINT, ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);

// From DirectX-Headers include/directx/d3d12compatibility.h; the mingw-w64
// headers do not declare the compatibility interfaces.
DEFINE_GUID(IID_ID3D12CompatibilityDevice, 0x8f1c0e3c, 0xfae3, 0x4a82,
        0xb0, 0x98, 0xbf, 0xe1, 0x70, 0x82, 0x07, 0xff);

struct Candidate
{
    const char *name;
    const GUID *iid;
};

static void probe(const char *owner, IUnknown *object, const Candidate *candidates, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        IUnknown *out = nullptr;
        HRESULT hr = object->QueryInterface(*candidates[i].iid, reinterpret_cast<void **>(&out));
        std::printf("%-8s %-28s hr=0x%08lx%s\n", owner, candidates[i].name,
                static_cast<unsigned long>(hr),
                SUCCEEDED(hr) && !out ? " (null pointer)" : "");
        if (out)
            out->Release();
    }
}

#define C(i) { #i, &IID_##i }

static int failures;

#define CHECK(cond, name) do { \
        if (cond) std::printf("[ok] %s\n", name); \
        else { std::printf("[fail] %s (line %d)\n", name, __LINE__); ++failures; } \
    } while (0)

static unsigned long hresult(HRESULT hr)
{
    return static_cast<unsigned long>(hr);
}

static void reportPsoCache(ID3D12Device *device)
{
    D3D12_FEATURE_DATA_SHADER_CACHE cache = {};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &cache, sizeof(cache));
    std::printf("perf: SHADER_CACHE hr=0x%08lx SupportFlags=0x%x\n", hresult(hr),
            static_cast<unsigned>(cache.SupportFlags));

    ID3D12Device1 *device1 = nullptr;
    hr = device->QueryInterface(IID_ID3D12Device1, reinterpret_cast<void **>(&device1));
    if (FAILED(hr) || !device1)
    {
        std::printf("perf: CreatePipelineLibrary skipped, no ID3D12Device1 (hr=0x%08lx)\n", hresult(hr));
        return;
    }
    ID3D12PipelineLibrary *library = nullptr;
    hr = device1->CreatePipelineLibrary(nullptr, 0, IID_ID3D12PipelineLibrary,
            reinterpret_cast<void **>(&library));
    std::printf("perf: CreatePipelineLibrary hr=0x%08lx library=%p\n", hresult(hr), (void *)library);
    if (library)
    {
        const SIZE_T size = library->GetSerializedSize();
        void *blob = size ? HeapAlloc(GetProcessHeap(), 0, size) : nullptr;
        hr = blob ? library->Serialize(blob, size) : E_OUTOFMEMORY;
        std::printf("perf: PipelineLibrary serialized size=%llu hr=0x%08lx\n",
                static_cast<unsigned long long>(size), hresult(hr));
        if (blob)
            HeapFree(GetProcessHeap(), 0, blob);
        library->Release();
    }
    device1->Release();
}

static void reportUma(ID3D12Device *device)
{
    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    const BOOL hasMemory = GlobalMemoryStatusEx(&memory);
    const auto uploadProperties = RelayD3D12CustomHeapProperties(device, 1, D3D12_HEAP_TYPE_UPLOAD);
    const auto readbackProperties = RelayD3D12CustomHeapProperties(device, 1, D3D12_HEAP_TYPE_READBACK);
    std::printf("perf: physical_bytes=%llu memory_query=%d upload_page=%u readback_page=%u\n",
        static_cast<unsigned long long>(memory.ullTotalPhys), hasMemory,
        unsigned(uploadProperties.CPUPageProperty), unsigned(readbackProperties.CPUPageProperty));
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture = {};
    HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture,
            sizeof(architecture));
    std::printf("perf: ARCHITECTURE1 hr=0x%08lx UMA=%d CacheCoherentUMA=%d TileBasedRenderer=%d\n",
            hresult(hr), architecture.UMA, architecture.CacheCoherentUMA,
            architecture.TileBasedRenderer);

    // What DTL would ask for a CPU-read/write staging resource on a
    // cache-coherent UMA device, in place of a READBACK heap and a copy.
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;

    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 65536;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *resource = nullptr;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource,
            reinterpret_cast<void **>(&resource));
    std::printf("perf: CUSTOM WRITE_BACK/L0 buffer hr=0x%08lx\n", hresult(hr));
    if (resource)
    {
        // A CPU round trip only: it shows the mapping is usable, not that the
        // GPU sees the bytes, which needs a copy the probe does not record.
        void *raw = nullptr;
        hr = resource->Map(0, nullptr, &raw);
        volatile unsigned char *mapped = static_cast<volatile unsigned char *>(raw);
        bool cpuRoundTrip = false;
        if (SUCCEEDED(hr) && mapped)
        {
            mapped[4095] = 0x5a;
            cpuRoundTrip = mapped[4095] == 0x5a;
            resource->Unmap(0, nullptr);
        }
        std::printf("perf: CUSTOM WRITE_BACK/L0 buffer Map hr=0x%08lx cpu_round_trip=%s\n", hresult(hr),
                cpuRoundTrip ? "yes" : "no");
        resource->Release();
    }

    D3D12_RESOURCE_DESC texture = {};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = 256;
    texture.Height = 256;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    resource = nullptr;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource,
            reinterpret_cast<void **>(&resource));
    std::printf("perf: CUSTOM WRITE_BACK/L0 texture hr=0x%08lx\n", hresult(hr));
    if (resource)
        resource->Release();
}

static bool adapterHasLuid(IDXGIAdapter *adapter, const LUID &luid)
{
    DXGI_ADAPTER_DESC desc = {};

    return adapter && SUCCEEDED(adapter->GetDesc(&desc))
            && desc.AdapterLuid.LowPart == luid.LowPart
            && desc.AdapterLuid.HighPart == luid.HighPart;
}

int main()
{
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d12 || !d3d11)
    {
        std::printf("load failed: d3d12=%p d3d11=%p\n", (void *)d3d12, (void *)d3d11);
        return 2;
    }
    auto createDevice12 = reinterpret_cast<CreateDevice12Fn>(
            reinterpret_cast<void *>(GetProcAddress(d3d12, "D3D12CreateDevice")));
    auto on12CreateDevice = reinterpret_cast<On12CreateDeviceFn>(
            reinterpret_cast<void *>(GetProcAddress(d3d11, "D3D11On12CreateDevice")));
    if (!createDevice12 || !on12CreateDevice)
    {
        std::printf("missing export\n");
        return 2;
    }

    // Match normal application startup: D3DMetal's Wine bridge requires DXGI
    // initialization before a direct D3D12CreateDevice call on this runtime.
    using InitFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE initDxgi = LoadLibraryW(L"dxgi.dll");
    auto initFactory = initDxgi ? reinterpret_cast<InitFactoryFn>(
        reinterpret_cast<void*>(GetProcAddress(initDxgi, "CreateDXGIFactory1"))) : nullptr;
    IDXGIFactory1* initialFactory = nullptr;
    IDXGIAdapter1* initialAdapter = nullptr;
    if (!initFactory || FAILED(initFactory(IID_IDXGIFactory1, reinterpret_cast<void**>(&initialFactory))) || !initialFactory)
        return 1;
    const HRESULT adapterHr = initialFactory->EnumAdapters1(0, &initialAdapter);
    initialFactory->Release();
    if (FAILED(adapterHr) || !initialAdapter) return 1;

    ID3D12Device *device12 = nullptr;
    HRESULT hr = createDevice12(initialAdapter, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
            reinterpret_cast<void **>(&device12));
    initialAdapter->Release();
    std::printf("D3D12CreateDevice hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr) || !device12)
        return 1;

    reportPsoCache(device12);
    reportUma(device12);

    // The adapter an IDXGIDevice on this device must report is the one whose
    // LUID the D3D12 device carries.
    typedef HRESULT(WINAPI *CreateFactory1Fn)(REFIID, void **);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto createFactory1 = dxgi ? reinterpret_cast<CreateFactory1Fn>(
            reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
    IDXGIFactory4 *factory = nullptr;
    if (createFactory1 && SUCCEEDED(createFactory1(IID_IDXGIFactory4,
            reinterpret_cast<void **>(&factory))) && factory)
    {
        LUID luid = device12->GetAdapterLuid();
        IDXGIAdapter *adapter = nullptr;
        hr = factory->EnumAdapterByLuid(luid, IID_IDXGIAdapter, reinterpret_cast<void **>(&adapter));
        DXGI_ADAPTER_DESC desc = {};
        if (adapter)
            adapter->GetDesc(&desc);
        std::printf("EnumAdapterByLuid(%08lx:%08lx) hr=0x%08lx adapter=%ls\n",
                static_cast<unsigned long>(luid.HighPart), luid.LowPart,
                static_cast<unsigned long>(hr), adapter ? desc.Description : L"(none)");
        if (adapter)
            adapter->Release();
        factory->Release();
    }
    else
        std::printf("IDXGIFactory4 unavailable\n");

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    hr = device12->CreateCommandQueue(&queueDesc, IID_ID3D12CommandQueue,
            reinterpret_cast<void **>(&queue));
    std::printf("CreateCommandQueue hr=0x%08lx\n", static_cast<unsigned long>(hr));
    if (FAILED(hr) || !queue)
        return 1;

    for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM})
    {
        const HRESULT gpuHr = uma_probe::check(device12, queue, format);
        std::printf("perf: UMA GPU visibility format=%u hr=0x%08lx result=%s\n", unsigned(format),
            static_cast<unsigned long>(gpuHr), gpuHr == S_OK ? "pass" : gpuHr == S_FALSE ? "unsupported" : "fail");
        if (FAILED(gpuHr)) return 1;
    }

    ID3D11Device *device11 = nullptr;
    ID3D11DeviceContext *context11 = nullptr;
    D3D_FEATURE_LEVEL level = static_cast<D3D_FEATURE_LEVEL>(0);
    IUnknown *queues[] = { queue };
    hr = on12CreateDevice(device12, 0, nullptr, 0, queues, 1, 0, &device11, &context11, &level);
    std::printf("D3D11On12CreateDevice hr=0x%08lx level=0x%x device=%p context=%p\n",
            static_cast<unsigned long>(hr), level, (void *)device11, (void *)context11);
    if (FAILED(hr) || !device11)
        return 1;

    static const Candidate deviceCandidates[] = {
        C(ID3D11Device), C(ID3D11Device1), C(ID3D11Device2), C(ID3D11Device3),
        C(ID3D11Device4), C(ID3D11Device5), C(ID3D11On12Device), C(ID3D11On12Device1),
        C(ID3D11On12Device2), C(IDXGIDevice), C(IDXGIDevice1), C(IDXGIDevice2),
        C(IDXGIDevice3), C(IDXGIDevice4), C(ID3D10Multithread), C(ID3D11Multithread),
        C(ID3D11VideoDevice),
    };
    static const Candidate contextCandidates[] = {
        C(ID3D11DeviceContext1), C(ID3D11DeviceContext2), C(ID3D11DeviceContext3),
        C(ID3D11DeviceContext4), C(ID3D11Multithread), C(ID3DUserDefinedAnnotation),
        C(ID3D11VideoContext),
    };
    static const Candidate device12Candidates[] = {
        C(ID3D12Device1), C(ID3D12Device2), C(ID3D12Device3), C(ID3D12Device4),
        C(ID3D12Device5), C(ID3D12Device6), C(ID3D12Device7), C(ID3D12Device8),
        C(ID3D12Device9), C(ID3D12CompatibilityDevice), C(ID3D12InfoQueue),
    };
    static const Candidate queueCandidates[] = {
        C(ID3D12CommandQueue), C(ID3D12DeviceChild), C(ID3D12Pageable),
    };
    probe("device11", device11, deviceCandidates, ARRAYSIZE(deviceCandidates));
    if (context11)
        probe("context", context11, contextCandidates, ARRAYSIZE(contextCandidates));
    probe("device12", device12, device12Candidates, ARRAYSIZE(device12Candidates));
    probe("queue", queue, queueCandidates, ARRAYSIZE(queueCandidates));

    ID3D11On12Device1 *on12 = nullptr;
    if (SUCCEEDED(device11->QueryInterface(IID_ID3D11On12Device1, reinterpret_cast<void **>(&on12))) && on12)
    {
        ID3D12Device *back = nullptr;
        hr = on12->GetD3D12Device(IID_ID3D12Device, &back);
        CHECK(SUCCEEDED(hr) && back == device12, "ID3D11On12Device1::GetD3D12Device returns the caller's device");
        if (back)
            back->Release();
        on12->Release();
    }
    else
        CHECK(false, "ID3D11On12Device1 is answered");

    {
        static const GUID key = {0x5b1e6c3a, 0x2f47, 0x4c55, {0x9d, 0x1e, 0x6a, 0x77, 0x10, 0x42, 0x3c, 0x81}};
        const LUID luid = device12->GetAdapterLuid();
        IDXGIDevice *dxgiDevice = nullptr;
        IDXGIAdapter *adapter = nullptr;
        IDXGIAdapter *parent = nullptr;
        ID3D11Device *back11 = nullptr;
        DWORD value = 0x0025c0de, read = 0;
        UINT size = sizeof(read);

        hr = device11->QueryInterface(IID_IDXGIDevice, reinterpret_cast<void **>(&dxgiDevice));
        CHECK(SUCCEEDED(hr) && dxgiDevice, "IDXGIDevice is answered");
        if (dxgiDevice)
        {
            hr = dxgiDevice->GetAdapter(&adapter);
            CHECK(SUCCEEDED(hr) && adapterHasLuid(adapter, luid),
                    "IDXGIDevice::GetAdapter is the D3D12 device's adapter");
            hr = dxgiDevice->GetParent(IID_IDXGIAdapter, reinterpret_cast<void **>(&parent));
            CHECK(SUCCEEDED(hr) && adapterHasLuid(parent, luid),
                    "IDXGIDevice::GetParent is the D3D12 device's adapter");
            hr = dxgiDevice->QueryInterface(IID_ID3D11Device, reinterpret_cast<void **>(&back11));
            CHECK(SUCCEEDED(hr) && back11 == device11, "IDXGIDevice and ID3D11Device are one object");
            CHECK(SUCCEEDED(device11->SetPrivateData(key, sizeof(value), &value))
                    && SUCCEEDED(dxgiDevice->GetPrivateData(key, &size, &read))
                    && size == sizeof(value) && read == value,
                    "ID3D11Device private data reads back through IDXGIDevice");
            if (back11)
                back11->Release();
            if (parent)
                parent->Release();
            if (adapter)
                adapter->Release();
            dxgiDevice->Release();
        }
    }

    if (context11)
        context11->Release();
    device11->Release();
    queue->Release();
    device12->Release();
    if (failures)
        std::printf("RESULT: FAIL, %d On12 device checks failed\n", failures);
    else
        std::printf("RESULT: On12 device checks ok\n");
    return failures ? 1 : 0;
}
