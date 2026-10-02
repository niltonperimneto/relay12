// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "../compat/relay_uma_upload.hpp"
#include <cstdio>
#include <vector>

namespace uma_probe {
template<class T> struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    void** out() { return reinterpret_cast<void**>(&p); }
};
// Executes a GPU texture-to-buffer copy and validates every active byte. A
// successful CPU Map alone cannot prove the custom heap is GPU-visible.
inline HRESULT check(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format)
{
    constexpr UINT width = 257, height = 9, pitch = width * 4 + 12;
    std::vector<unsigned char> source(pitch * height, 0xa5);
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width * 4; ++x) source[y * pitch + x] = static_cast<unsigned char>(x * 17 + y);
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Format = format;
    Ref<ID3D12Resource> texture, readback;
    HRESULT hr = relay12::CreateUmaInitialTexture(device, 1, desc, source.data(), pitch, pitch * height, &texture.p);
    std::printf("perf: UMA custom initialization format=%u width=%u height=%u source_pitch=%u hr=0x%08lx\n",
        unsigned(format), width, height, pitch, static_cast<unsigned long>(hr));
    if (hr != S_OK) return hr;
    D3D12_HEAP_PROPERTIES actualHeap = {};
    D3D12_HEAP_FLAGS actualFlags = D3D12_HEAP_FLAG_NONE;
    hr = texture.p->GetHeapProperties(&actualHeap, &actualFlags);
    std::printf("perf: UMA custom texture heap hr=0x%08lx type=%u page=%u pool=%u\n",
        static_cast<unsigned long>(hr), unsigned(actualHeap.Type), unsigned(actualHeap.CPUPageProperty),
        unsigned(actualHeap.MemoryPoolPreference));
    if (FAILED(hr)) return hr;
    if (actualHeap.Type != D3D12_HEAP_TYPE_CUSTOM || actualHeap.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_WRITE_BACK ||
        actualHeap.MemoryPoolPreference != D3D12_MEMORY_POOL_L0) return E_FAIL;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_ID3D12Resource, readback.out());
    if (FAILED(hr)) return hr;
    Ref<ID3D12CommandAllocator> allocator;
    Ref<ID3D12GraphicsCommandList> list;
    Ref<ID3D12Fence> fence;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, allocator.out());
    if (FAILED(hr)) return hr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.p, nullptr, IID_ID3D12GraphicsCommandList, list.out());
    if (FAILED(hr)) return hr;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, fence.out());
    if (FAILED(hr)) return hr;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.p;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list.p->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = readback.p; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
    src.pResource = texture.p; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list.p->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    hr = list.p->Close(); if (FAILED(hr)) return hr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return E_OUTOFMEMORY;
    ID3D12CommandList* lists[] = {list.p};
    queue->ExecuteCommandLists(1, lists);
    hr = queue->Signal(fence.p, 1);
    if (SUCCEEDED(hr)) hr = fence.p->SetEventOnCompletion(1, event);
    if (SUCCEEDED(hr) && WaitForSingleObject(event, 30000) != WAIT_OBJECT_0)
    {
        // Exit the probe on timeout rather than release GPU-referenced objects.
        std::fputs("[fail] UMA GPU copy timed out\n", stderr);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    CloseHandle(event);
    if (FAILED(hr)) return hr;
    void* mapped = nullptr;
    D3D12_RANGE range = {0, static_cast<SIZE_T>(bytes)};
    hr = readback.p->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped) return FAILED(hr) ? hr : E_FAIL;
    UINT mismatches = 0;
    const auto* result = static_cast<const unsigned char*>(mapped) + footprint.Offset;
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width * 4; ++x)
            if (result[y * footprint.Footprint.RowPitch + x] != source[y * pitch + x]) ++mismatches;
    std::printf("perf: UMA GPU bytes format=%u readback_pitch=%u active_bytes=%u mismatches=%u\n",
        unsigned(format), footprint.Footprint.RowPitch, width * height * 4, mismatches);
    D3D12_RANGE noWrite = {0, 0}; readback.p->Unmap(0, &noWrite);
    return mismatches ? E_FAIL : S_OK;
}
}
