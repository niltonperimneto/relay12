/* SPDX-License-Identifier: GPL-3.0-only
 * D3DMetal d3d11.dll router.
 *
 * Normal D3D11 creation remains owned by Apple's forwarder, renamed to
 * d3d11mt.dll at deployment time.  D3D11On12CreateDevice is routed to an
 * independently versioned implementation so an incomplete mapping layer can
 * never impersonate a working ID3D11On12Device.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>

namespace
{
using CreateDeviceFn = decltype(&D3D11CreateDevice);
using CreateDeviceAndSwapChainFn = decltype(&D3D11CreateDeviceAndSwapChain);
using On12CreateDeviceFn = HRESULT (WINAPI *)(IUnknown *, UINT,
        const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *, UINT, UINT,
        ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);

struct Backend
{
    HMODULE apple;
    HMODULE on12;
    CreateDeviceFn createDevice;
    CreateDeviceAndSwapChainFn createDeviceAndSwapChain;
    On12CreateDeviceFn createOn12Device;
};

INIT_ONCE initOnce = INIT_ONCE_STATIC_INIT;
Backend backend = {};

template<typename Function>
Function resolve(HMODULE module, const char *name) noexcept
{
    const FARPROC address = GetProcAddress(module, name);
    Function function = nullptr;

    static_assert(sizeof(function) == sizeof(address),
            "Win32 function and FARPROC pointers must have equal size");
    __builtin_memcpy(&function, &address, sizeof(function));
    return function;
}

BOOL CALLBACK initializeBackend(PINIT_ONCE, PVOID, PVOID *) noexcept
{
    /* These are Wine builtin modules installed in the prefix's system32.
     * Plain LoadLibrary is required so Wine's builtin-module lookup participates
     * in resolution, matching the existing D3D12 and DXGI interposers. */
    backend.apple = LoadLibraryW(L"d3d11mt.dll");
    if (backend.apple)
    {
        backend.createDevice = resolve<CreateDeviceFn>(backend.apple,
                "D3D11CreateDevice");
        backend.createDeviceAndSwapChain = resolve<CreateDeviceAndSwapChainFn>(
                backend.apple, "D3D11CreateDeviceAndSwapChain");
    }

    /* This module is deliberately optional.  The router is safe to deploy
     * before the mapping layer, and reports unsupported instead of returning
     * an object with false D3D11On12 semantics. */
    backend.on12 = LoadLibraryW(L"d3d11on12core.dll");
    if (backend.on12)
        backend.createOn12Device = resolve<On12CreateDeviceFn>(backend.on12,
                "WineD3D11On12CreateDeviceV1");

    return TRUE;
}

void initialize() noexcept
{
    InitOnceExecuteOnce(&initOnce, initializeBackend, nullptr, nullptr);
}
}

extern "C" HRESULT WINAPI shimD3D11CreateDevice(IDXGIAdapter *adapter,
        D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
        const D3D_FEATURE_LEVEL *featureLevels, UINT featureLevelCount,
        UINT sdkVersion, ID3D11Device **device, D3D_FEATURE_LEVEL *featureLevel,
        ID3D11DeviceContext **immediateContext) noexcept
{
    initialize();
    if (!backend.createDevice)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    return backend.createDevice(adapter, driverType, software, flags,
            featureLevels, featureLevelCount, sdkVersion, device, featureLevel,
            immediateContext);
}

extern "C" HRESULT WINAPI shimD3D11CreateDeviceAndSwapChain(
        IDXGIAdapter *adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
        UINT flags, const D3D_FEATURE_LEVEL *featureLevels,
        UINT featureLevelCount, UINT sdkVersion,
        const DXGI_SWAP_CHAIN_DESC *swapChainDesc, IDXGISwapChain **swapChain,
        ID3D11Device **device, D3D_FEATURE_LEVEL *featureLevel,
        ID3D11DeviceContext **immediateContext) noexcept
{
    initialize();
    if (!backend.createDeviceAndSwapChain)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    return backend.createDeviceAndSwapChain(adapter, driverType, software,
            flags, featureLevels, featureLevelCount, sdkVersion, swapChainDesc,
            swapChain, device, featureLevel, immediateContext);
}

extern "C" HRESULT WINAPI shimD3D11On12CreateDevice(IUnknown *device12,
        UINT flags, const D3D_FEATURE_LEVEL *featureLevels,
        UINT featureLevelCount, IUnknown *const *commandQueues, UINT queueCount,
        UINT nodeMask, ID3D11Device **device11,
        ID3D11DeviceContext **immediateContext,
        D3D_FEATURE_LEVEL *chosenFeatureLevel) noexcept
{
    if (device11)
        *device11 = nullptr;
    if (immediateContext)
        *immediateContext = nullptr;
    if (chosenFeatureLevel)
        *chosenFeatureLevel = static_cast<D3D_FEATURE_LEVEL>(0);

    if (!device12)
        return E_INVALIDARG;

    initialize();
    if (!backend.createOn12Device)
        return DXGI_ERROR_UNSUPPORTED;

    return backend.createOn12Device(device12, flags, featureLevels,
            featureLevelCount, commandQueues, queueCount, nodeMask, device11,
            immediateContext, chosenFeatureLevel);
}
