// SPDX-License-Identifier: GPL-3.0-only
#include <initguid.h>
#include "relay_uma_upload.hpp"
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
static void check(bool value) { if (!value) std::abort(); }
struct Resource {
    HRESULT map = S_OK, write = S_OK;
    unsigned releases = 0, unmaps = 0;
    HRESULT Map(UINT, const D3D12_RANGE* range, void** ptr) {
        check(!ptr && range->Begin == 0 && range->End == 0); return map;
    }
    HRESULT WriteToSubresource(UINT, const D3D12_BOX* box, const void* data, UINT pitch, UINT) {
        check(!box && data && pitch == 1040); return write;
    }
    void Unmap(UINT, const D3D12_RANGE*) { ++unmaps; }
    ULONG Release() { return ++releases; }
};
struct Device {
    Resource resource;
    HRESULT create = S_OK;
    bool nullOutput = false;
    unsigned creates = 0;
    HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE*, REFIID, void** output) {
        check(heap->Type == D3D12_HEAP_TYPE_CUSTOM && heap->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK);
        check(heap->MemoryPoolPreference == D3D12_MEMORY_POOL_L0 && heap->CreationNodeMask == 1 && heap->VisibleNodeMask == 1);
        check(flags == D3D12_HEAP_FLAG_NONE && state == D3D12_RESOURCE_STATE_COMMON);
        ++creates;
        *output = nullOutput ? nullptr : &resource;
        return create;
    }
};
int main()
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = 257; desc.Height = 9;
    desc.MipLevels = desc.DepthOrArraySize = 1; desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    char data[1040 * 9] = {};
    for (HRESULT error : {E_INVALIDARG, E_NOTIMPL, E_NOINTERFACE, DXGI_ERROR_UNSUPPORTED, E_OUTOFMEMORY, DXGI_ERROR_DEVICE_REMOVED})
    {
        for (unsigned stage = 0; stage < 3; ++stage)
        {
            Device device;
            if (stage == 0) device.create = error;
            if (stage == 1) device.resource.map = error;
            if (stage == 2) device.resource.write = error;
            Resource* output = &device.resource;
            const auto hr = relay12::CreateUmaInitialTexture(&device, 1, desc, data, 1040, sizeof(data), &output);
            check(hr == (relay12::UnsupportedUmaOperation(error) ? S_FALSE : error));
            check(!output && device.resource.releases == 1);
            check(device.resource.unmaps == (stage == 2 ? 1u : 0u));
        }
    }
    Device device; Resource* output = nullptr;
    check(relay12::CreateUmaInitialTexture(&device, 1, desc, data, 1040, sizeof(data), &output) == S_OK);
    check(output && device.resource.unmaps == 1 && device.resource.releases == 0);
    device.nullOutput = true;
    check(relay12::CreateUmaInitialTexture(&device, 1, desc, data, 1040, sizeof(data), &output) == E_FAIL && !output);
    const auto creates = device.creates;
    desc.SampleDesc.Count = 4;
    check(relay12::CreateUmaInitialTexture(&device, 1, desc, data, 1040, sizeof(data), &output) == S_FALSE);
    check(device.creates == creates);
    desc.SampleDesc.Count = 1; desc.MipLevels = 2;
    check(!relay12::UmaInitialTextureEligible(desc));
    desc.MipLevels = 1; desc.Format = DXGI_FORMAT_BC1_UNORM;
    check(!relay12::UmaInitialTextureEligible(desc));
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    check(relay12::UmaInitialTextureEligible(desc));
    std::puts("[ ok ] UMA direct-upload eligibility, initialization, fallback and failure propagation");
}
