/* SPDX-License-Identifier: GPL-3.0-only
 * Caller-owned D3D12 texture -> On12 clear -> caller-owned D3D12 copy/readback.
 * No D3D11 staging texture can conceal an accidentally independent resource.
 */
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <cstdio>
#include <cstring>
#include "../compat/relay_d3d12_struct_return.hpp"

template<class T> struct Ref
{
    T *p = nullptr;
    ~Ref() { if (p) p->Release(); }
    T *operator->() const { return p; }
    void **out() { return reinterpret_cast<void **>(&p); }
};
#define HR(call) do { HRESULT h = (call); if (FAILED(h)) { \
    printf("[fail] %s: %08lx\n", #call, (unsigned long)h); return false; } } while (0)
#define CHECK(test) do { if (!(test)) { printf("[fail] %s\n", #test); return false; } } while (0)

struct Event
{
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() { if (handle) CloseHandle(handle); }
};

bool wait_queue(ID3D12CommandQueue *queue, ID3D12Fence *fence, HANDLE event, UINT64 value)
{
    HR(queue->Signal(fence, value));
    HR(fence->SetEventOnCompletion(value, event));
    CHECK(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
    return true;
}

void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

bool run(DXGI_FORMAT format)
{
    Ref<ID3D12Device> device;
    Ref<ID3D12CommandQueue> queue;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    Ref<ID3D12Resource> target, readback;
    Ref<ID3D12DescriptorHeap> heap;
    Ref<ID3D11Device> device11;
    Ref<ID3D11DeviceContext> context;
    Ref<ID3D11On12Device1> on12;
    Ref<ID3D11Resource> wrapped;
    Ref<ID3D11RenderTargetView> view;
    Event event;
    CHECK(event.handle);
    HR(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, device.out()));
    D3D12_COMMAND_QUEUE_DESC q = {};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(device->CreateCommandQueue(&q, IID_ID3D12CommandQueue, queue.out()));
    HR(device->CreateCommandAllocator(q.Type, IID_ID3D12CommandAllocator, allocator.out()));
    HR(device->CreateCommandList(0, q.Type, allocator.p, nullptr, IID_ID3D12GraphicsCommandList, list.out()));
    HR(list->Close());
    HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, fence.out()));
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    props.CreationNodeMask = props.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 64;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    HR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_ID3D12Resource, target.out()));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 size = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
    CHECK(size && footprint.Footprint.RowPitch >= 256);
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = size;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    props.Type = D3D12_HEAP_TYPE_READBACK;
    HR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_ID3D12Resource, readback.out()));
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    HR(device->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, heap.out()));
    auto rtv = RelayD3D12CPUDescriptorHeapStart(heap.p);
    device->CreateRenderTargetView(target.p, nullptr, rtv);

    HMODULE router = LoadLibraryW(L"d3d11.dll");
    CHECK(router && GetProcAddress(router, "WineD3D11ShimGetStatus"));
    using Create = decltype(&D3D11On12CreateDevice);
    FARPROC address = GetProcAddress(router, "D3D11On12CreateDevice");
    Create create = nullptr;
    memcpy(&create, &address, sizeof(create));
    CHECK(create);
    IUnknown *queues[] = {queue.p};
    HRESULT created = create(device.p, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            queues, 1, 0, &device11.p, &context.p, nullptr);
    FreeLibrary(router);
    HR(created);
    HR(device11->QueryInterface(IID_ID3D11On12Device1, on12.out()));
    Ref<ID3D12Device> original;
    HR(on12->GetD3D12Device(IID_ID3D12Device, &original.p));
    CHECK(original.p == device.p);
    D3D11_RESOURCE_FLAGS flags = {};
    flags.BindFlags = D3D11_BIND_RENDER_TARGET;
    printf("[stage] wrapping original D3D12 texture, format %u\n", (unsigned)format);
    HR(on12->CreateWrappedResource(target.p, &flags, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE, IID_ID3D11Resource, wrapped.out()));
    HR(device11->CreateRenderTargetView(wrapped.p, nullptr, &view.p));
    Ref<ID3D11Texture2D> texture11;
    HR(wrapped->QueryInterface(IID_ID3D11Texture2D, texture11.out()));
    D3D11_TEXTURE2D_DESC desc11 = {};
    texture11->GetDesc(&desc11);
    CHECK(desc11.Width == 64 && desc11.Height == 64 && desc11.Format == format);
    UINT64 value = 0;
    for (unsigned cycle = 0; cycle < 3; ++cycle)
    {
        HR(allocator->Reset()); HR(list->Reset(allocator.p, nullptr));
        if (cycle) barrier(list.p, target.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float blue[] = {0, 0, 1, 1};
        list->ClearRenderTargetView(rtv, blue, 0, nullptr);
        HR(list->Close());
        ID3D12CommandList *commands[] = {list.p};
        queue->ExecuteCommandLists(1, commands);
        CHECK(wait_queue(queue.p, fence.p, event.handle, ++value));
        on12->AcquireWrappedResources(&wrapped.p, 1);
        const float red[] = {1, 0, 0, 1};
        context->ClearRenderTargetView(view.p, red);
        if (cycle == 1) on12->ReleaseWrappedResources(nullptr, 0);
        else on12->ReleaseWrappedResources(&wrapped.p, 1);
        context->Flush();
        CHECK(wait_queue(queue.p, fence.p, event.handle, ++value));
        HR(allocator->Reset()); HR(list->Reset(allocator.p, nullptr));
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = target.p;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.p;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        // ReleaseWrappedResources supplied COPY_SOURCE; the caller adds no barrier here.
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        HR(list->Close()); queue->ExecuteCommandLists(1, commands);
        CHECK(wait_queue(queue.p, fence.p, event.handle, ++value));
        BYTE *pixels = nullptr;
        D3D12_RANGE range = {0, static_cast<SIZE_T>(size)};
        HR(readback->Map(0, &range, reinterpret_cast<void **>(&pixels)));
        bool correct = pixels != nullptr;
        for (UINT y = 0; correct && y < 64; ++y)
            for (UINT x = 0; x < 64; ++x)
            {
                const BYTE *p = pixels + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4;
                const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM;
                if (p[0] != (bgra ? 0 : 255) || p[1] || p[2] != (bgra ? 255 : 0) || p[3] != 255)
                { printf("[fail] pixel %u,%u = %02x%02x%02x%02x\n", x, y, p[0], p[1], p[2], p[3]); correct = false; break; }
            }
        D3D12_RANGE written = {0, 0}; readback->Unmap(0, &written);
        CHECK(correct);
        printf("[ ok ] format %u cycle %u: original D3D12 texture contains all 4096 red pixels; fence %llu completed\n",
                (unsigned)format, cycle + 1, (unsigned long long)value);
    }
    HR(device->GetDeviceRemovedReason());
    context->ClearState(); context->Flush();
    CHECK(wait_queue(queue.p, fence.p, event.handle, ++value));
    return true;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!run(DXGI_FORMAT_R8G8B8A8_UNORM) || !run(DXGI_FORMAT_B8G8R8A8_UNORM)) return 1;
    puts("[ ok ] RGBA and BGRA wrapped-resource ownership, caller readback and teardown completed");
    return 0;
}
