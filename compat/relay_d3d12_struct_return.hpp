#pragma once

// MinGW follows the Win64 ABI for COM methods returning structures by value,
// but exposes the hidden return slot explicitly in its generated headers.
// Keep call sites source-compatible with MSVC without changing the ABI.
#ifdef __MINGW32__
#define RELAY_D3D12_STRUCT_RETURN(type, call) \
    ([&]() { type relayResult{}; call; return relayResult; }())
#define RelayD3D12ResourceDesc(object) \
    RELAY_D3D12_STRUCT_RETURN(D3D12_RESOURCE_DESC, (object)->GetDesc(&relayResult))
#define RelayD3D12HeapDesc(object) \
    RELAY_D3D12_STRUCT_RETURN(D3D12_HEAP_DESC, (object)->GetDesc(&relayResult))
#define RelayD3D12VideoDecoderDesc(object) \
    RELAY_D3D12_STRUCT_RETURN(D3D12_VIDEO_DECODER_DESC, (object)->GetDesc(&relayResult))
#define RelayD3D12VideoDecoderHeapDesc(object) \
    RELAY_D3D12_STRUCT_RETURN(D3D12_VIDEO_DECODER_HEAP_DESC, (object)->GetDesc(&relayResult))
#define RelayD3D12CPUDescriptorHeapStart(object) \
    RELAY_D3D12_STRUCT_RETURN(D3D12_CPU_DESCRIPTOR_HANDLE, \
        (object)->GetCPUDescriptorHandleForHeapStart(&relayResult))
#define RelayD3D12GPUDescriptorHeapStart(object) \
    RELAY_D3D12_STRUCT_RETURN(D3D12_GPU_DESCRIPTOR_HANDLE, \
        (object)->GetGPUDescriptorHandleForHeapStart(&relayResult))
#define RelayD3D12CustomHeapProperties(object, nodeMask, heapType) \
    RELAY_D3D12_STRUCT_RETURN(D3D12_HEAP_PROPERTIES, \
        (object)->GetCustomHeapProperties(&relayResult, nodeMask, heapType))
#define RelayD3D12AdapterLuid(object) \
    RELAY_D3D12_STRUCT_RETURN(LUID, (object)->GetAdapterLuid(&relayResult))
#define RelayD3D12ResourceAllocationInfo(object, mask, count, descriptions) \
    RELAY_D3D12_STRUCT_RETURN(D3D12_RESOURCE_ALLOCATION_INFO, \
        (object)->GetResourceAllocationInfo(&relayResult, mask, count, descriptions))
#else
#define RelayD3D12ResourceDesc(object) ((object)->GetDesc())
#define RelayD3D12HeapDesc(object) ((object)->GetDesc())
#define RelayD3D12VideoDecoderDesc(object) ((object)->GetDesc())
#define RelayD3D12VideoDecoderHeapDesc(object) ((object)->GetDesc())
#define RelayD3D12CPUDescriptorHeapStart(object) \
    ((object)->GetCPUDescriptorHandleForHeapStart())
#define RelayD3D12GPUDescriptorHeapStart(object) \
    ((object)->GetGPUDescriptorHandleForHeapStart())
#define RelayD3D12CustomHeapProperties(object, nodeMask, heapType) \
    ((object)->GetCustomHeapProperties(nodeMask, heapType))
#define RelayD3D12AdapterLuid(object) ((object)->GetAdapterLuid())
#define RelayD3D12ResourceAllocationInfo(object, mask, count, descriptions) \
    ((object)->GetResourceAllocationInfo(mask, count, descriptions))
#endif
