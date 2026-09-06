/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <cstddef>
#include <type_traits>

#define WINE_D3D11ON12_ABI_VERSION 1u
#define WINE_D3D11ON12_CAP_VALIDATION 0x0000000000000001ull

using WineD3D11On12GetABIVersionFn = UINT (WINAPI *)();
using WineD3D11On12CreateDeviceFn = HRESULT (WINAPI *)(IUnknown *, UINT,
        const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *, UINT, UINT,
        ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);

struct WineD3D11On12Interface
{
    UINT size;
    UINT version;
    UINT64 capabilities;
    WineD3D11On12CreateDeviceFn createDevice;
    void *reserved[8];
};

static_assert(std::is_standard_layout_v<WineD3D11On12Interface>);
static_assert(sizeof(void *) != 8 || sizeof(WineD3D11On12Interface) == 88);
static_assert(offsetof(WineD3D11On12Interface, capabilities) == 8);
static_assert(offsetof(WineD3D11On12Interface, createDevice) == 16);
static_assert(offsetof(WineD3D11On12Interface, reserved) == 24);

using WineD3D11On12GetInterfaceFn = HRESULT (WINAPI *)(UINT, UINT,
        WineD3D11On12Interface *);

extern "C" UINT WINAPI WineD3D11On12GetABIVersion() noexcept;
extern "C" HRESULT WINAPI WineD3D11On12GetInterface(UINT, UINT,
        WineD3D11On12Interface *) noexcept;
extern "C" HRESULT WINAPI WineD3D11On12CreateDeviceV1(IUnknown *, UINT,
        const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *, UINT, UINT,
        ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *) noexcept;
