/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>

#define WINE_D3D11ON12_ABI_VERSION 1u

using WineD3D11On12GetABIVersionFn = UINT (WINAPI *)();
using WineD3D11On12CreateDeviceFn = HRESULT (WINAPI *)(IUnknown *, UINT,
        const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *, UINT, UINT,
        ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);

extern "C" UINT WINAPI WineD3D11On12GetABIVersion() noexcept;
extern "C" HRESULT WINAPI WineD3D11On12CreateDeviceV1(IUnknown *, UINT,
        const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *, UINT, UINT,
        ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *) noexcept;
