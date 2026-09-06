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
#include <cstdio>

#include "d3d11on12core.h"

namespace
{
/* The router is an ordinary PE module rather than a Wine builtin, so Wine's
 * ERR and TRACE macros are unavailable here.  OutputDebugStringA reaches the
 * same debug output and imports only from kernel32. */
void reportDiagnostic(const char *message) noexcept
{
    OutputDebugStringA(message);
}

void reportLoadFailure(const char *module, DWORD error) noexcept
{
    char message[192];

    std::snprintf(message, sizeof(message),
            "d3d11shim: LoadLibraryW(%s) failed with error %lu; the D3D11 "
            "entry points it provides will report ERROR_PROC_NOT_FOUND.\n",
            module, static_cast<unsigned long>(error));
    OutputDebugStringA(message);
}

using CreateDeviceFn = decltype(&D3D11CreateDevice);
using CreateDeviceAndSwapChainFn = decltype(&D3D11CreateDeviceAndSwapChain);
struct Backend
{
    HMODULE apple;
    HMODULE on12;
    CreateDeviceFn createDevice;
    CreateDeviceAndSwapChainFn createDeviceAndSwapChain;
    WineD3D11On12Interface on12Interface;
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
        if (!backend.createDevice || !backend.createDeviceAndSwapChain)
            reportDiagnostic("d3d11shim: d3d11mt.dll is missing an expected "
                    "D3D11 creation export; the deployment is not Apple's "
                    "renamed forwarder.\n");
    }
    else
    {
        /* A missing renamed forwarder means deployment went wrong.  Failing
         * silently here would look like an application bug. */
        reportLoadFailure("d3d11mt.dll", GetLastError());
    }

    /* This module is deliberately optional.  The router is safe to deploy
     * before the mapping layer, and reports unsupported instead of returning
     * an object with false D3D11On12 semantics. */
    backend.on12 = LoadLibraryW(L"d3d11on12core.dll");
    if (backend.on12)
    {
        const auto getInterface = resolve<WineD3D11On12GetInterfaceFn>(
                backend.on12, "WineD3D11On12GetInterface");
        if (!getInterface || FAILED(getInterface(WINE_D3D11ON12_ABI_VERSION,
                sizeof(backend.on12Interface), &backend.on12Interface))
                || backend.on12Interface.size != sizeof(backend.on12Interface)
                || backend.on12Interface.version != WINE_D3D11ON12_ABI_VERSION
                || !backend.on12Interface.createDevice)
        {
            /* An incompatible core is worse than an absent one: it is a
             * mismatched deployment, and it must not be called. */
            reportDiagnostic("d3d11shim: d3d11on12core.dll does not publish a "
                    "compatible WineD3D11On12Interface; D3D11On12CreateDevice "
                    "will report DXGI_ERROR_UNSUPPORTED.\n");
            ZeroMemory(&backend.on12Interface,
                    sizeof(backend.on12Interface));
        }
    }
    else
    {
        reportDiagnostic("d3d11shim: no d3d11on12core.dll in this prefix; "
                "D3D11On12CreateDevice will report DXGI_ERROR_UNSUPPORTED.\n");
    }

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
    if (!backend.on12Interface.createDevice)
        return DXGI_ERROR_UNSUPPORTED;

    return backend.on12Interface.createDevice(device12, flags, featureLevels,
            featureLevelCount, commandQueues, queueCount, nodeMask, device11,
            immediateContext, chosenFeatureLevel);
}
