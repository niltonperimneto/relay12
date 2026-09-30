// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d12.h>
#include <dxgi.h>

namespace relay12 {
inline bool UmaInitialTextureEligible(const D3D12_RESOURCE_DESC& desc) noexcept
{
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width && desc.Height &&
        desc.MipLevels == 1 && desc.DepthOrArraySize == 1 &&
        desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
        desc.Flags == D3D12_RESOURCE_FLAG_NONE && desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN &&
        (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
}
inline bool UnsupportedUmaOperation(HRESULT hr) noexcept
{
    return hr == E_INVALIDARG || hr == E_NOTIMPL || hr == E_NOINTERFACE || hr == DXGI_ERROR_UNSUPPORTED;
}
// S_FALSE means unsupported: no object or partial initialization escapes.
// Allocation failure and device loss remain errors; the caller must propagate.
inline HRESULT CreateUmaInitialTexture(ID3D12Device* device, UINT nodeMask,
    const D3D12_RESOURCE_DESC& desc, const void* data, UINT rowPitch, UINT depthPitch,
    ID3D12Resource** output) noexcept
{
    *output = nullptr;
    if (!UmaInitialTextureEligible(desc) || !data || desc.Width > rowPitch / 4u) return S_FALSE;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    heap.CreationNodeMask = heap.VisibleNodeMask = nodeMask;
    ID3D12Resource* candidate = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource, reinterpret_cast<void**>(&candidate));
    if (SUCCEEDED(hr) && !candidate) hr = E_FAIL;
    if (SUCCEEDED(hr))
    {
        D3D12_RANGE noRead = {0, 0};
        hr = candidate->Map(0, &noRead, nullptr);
        if (SUCCEEDED(hr))
        {
            hr = candidate->WriteToSubresource(0, nullptr, data, rowPitch, depthPitch);
            candidate->Unmap(0, nullptr);
        }
    }
    if (FAILED(hr))
    {
        if (candidate) candidate->Release();
        return UnsupportedUmaOperation(hr) ? S_FALSE : hr;
    }
    *output = candidate;
    return S_OK;
}
}
