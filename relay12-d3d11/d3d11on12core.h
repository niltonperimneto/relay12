/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>

/* The router and the core are C++, but the validation tests are C so they can
 * build mock COM objects from the widl C vtable declarations.  Both languages
 * must see one set of declarations, or the tests would validate a copy of the
 * ABI instead of the ABI itself. */
#ifdef __cplusplus
# include <cstddef>
# include <type_traits>
# define WINE_D3D11ON12_LINKAGE extern "C"
# define WINE_D3D11ON12_NOEXCEPT noexcept
# define WINE_D3D11ON12_ASSERT(condition) static_assert(condition, #condition)
#else
# include <stddef.h>
# define WINE_D3D11ON12_LINKAGE extern
# define WINE_D3D11ON12_NOEXCEPT
# define WINE_D3D11ON12_ASSERT(condition) _Static_assert(condition, #condition)
#endif

#define WINE_D3D11ON12_ABI_VERSION 3u
#define WINE_D3D11ON12_CAP_VALIDATION 0x0000000000000001ull
#define WINE_D3D11ON12_CAP_D3DMETAL_BOOTSTRAP 0x0000000000000002ull
#define WINE_D3D11ON12_CAP_DEVICE_LIFECYCLE 0x0000000000000004ull
#define WINE_D3D11ON12_CAP_IMMEDIATE_CONTEXT_FLUSH 0x0000000000000008ull
#define WINE_D3D11ON12_CAP_IMMEDIATE_CONTEXT_DRAW 0x0000000000000010ull
#define WINE_D3D11ON12_CAP_WRAPPED_RESOURCE_VALIDATION 0x0000000000000020ull
#define WINE_D3D11ON12_CAP_INDEXED_INSTANCED_DRAW 0x0000000000000040ull
#define WINE_D3D11ON12_CAP_INPUT_ASSEMBLER_TOPOLOGY 0x0000000000000080ull

#define WINE_D3D11ON12_DRAW_INDEXED 1u
#define WINE_D3D11ON12_DRAW_INSTANCED 2u
#define WINE_D3D11ON12_DRAW_INDEXED_INSTANCED 3u

typedef UINT (WINAPI *WineD3D11On12GetABIVersionFn)(void);
typedef HRESULT (WINAPI *WineD3D11On12CreateDeviceFn)(IUnknown *, UINT,
        const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *, UINT, UINT,
        ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);
typedef HRESULT (WINAPI *WineD3D11CreateDeviceFn)(IDXGIAdapter *,
        D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
        ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *WineD3D11CreateDeviceAndSwapChainFn)(IDXGIAdapter *,
        D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
        const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

struct WineD3D11On12AdapterDevice;
struct WineD3D11On12Buffer;
typedef HRESULT (WINAPI *WineD3D11On12CloseAdapterDeviceFn)(
        struct WineD3D11On12AdapterDevice *);
typedef HRESULT (WINAPI *WineD3D11On12FlushAdapterDeviceFn)(
        struct WineD3D11On12AdapterDevice *, UINT, UINT, BOOL *);
typedef HRESULT (WINAPI *WineD3D11On12DrawAdapterDeviceFn)(
        struct WineD3D11On12AdapterDevice *, UINT, UINT);
typedef HRESULT (WINAPI *WineD3D11On12ValidateWrappedResourceFn)(
        struct WineD3D11On12AdapterDevice *, IUnknown *);
typedef HRESULT (WINAPI *WineD3D11On12DispatchDrawFn)(
        struct WineD3D11On12AdapterDevice *, UINT, UINT, UINT, UINT, INT,
        UINT);
typedef HRESULT (WINAPI *WineD3D11On12SetPrimitiveTopologyFn)(
        struct WineD3D11On12AdapterDevice *, INT);

struct WineD3D11On12Interface
{
    UINT size;
    UINT version;
    UINT64 capabilities;
    WineD3D11On12CreateDeviceFn createDevice;
    WineD3D11CreateDeviceFn createDirectDevice;
    WineD3D11CreateDeviceAndSwapChainFn createDirectDeviceAndSwapChain;
    WineD3D11On12CloseAdapterDeviceFn closeAdapterDevice;
    WineD3D11On12FlushAdapterDeviceFn flushAdapterDevice;
    WineD3D11On12DrawAdapterDeviceFn drawAdapterDevice;
    WineD3D11On12ValidateWrappedResourceFn validateWrappedResource;
    WineD3D11On12DispatchDrawFn dispatchDraw;
    WineD3D11On12SetPrimitiveTopologyFn setPrimitiveTopology;
};

#ifndef __cplusplus
typedef struct WineD3D11On12Interface WineD3D11On12Interface;
#endif

#ifdef __cplusplus
WINE_D3D11ON12_ASSERT(std::is_standard_layout_v<WineD3D11On12Interface>);
#endif
WINE_D3D11ON12_ASSERT(sizeof(void *) != 8 || sizeof(WineD3D11On12Interface) == 88);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Interface, capabilities) == 8);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Interface, createDevice) == 16);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Interface, createDirectDevice) == 24);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Interface,
        createDirectDeviceAndSwapChain) == 32);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Interface,
        closeAdapterDevice) == 40);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Interface,
        flushAdapterDevice) == 48);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Interface,
        drawAdapterDevice) == 56);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Interface,
        validateWrappedResource) == 64);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Interface, dispatchDraw) == 72);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Interface,
        setPrimitiveTopology) == 80);

typedef HRESULT (WINAPI *WineD3D11On12GetInterfaceFn)(UINT, UINT,
        WineD3D11On12Interface *);

/* What a successful OpenAdapter leaves behind.
 *
 * The members are pointers to incomplete types on purpose. A caller that only
 * forwards the device function table needs no DDI declarations to do so, and
 * this header stays compilable as C for the validation tests -- the same
 * reason the clean-room header keeps pointer-only types incomplete.
 *
 * hDrvAdapter and hDrvDevice are the pDrvPrivate words of D3D10DDI_HADAPTER
 * and D3D10DDI_HDEVICE. runtimeState is the core's ownership token. None may
 * be freed by the caller; pass the complete structure to closeAdapterDevice.
 *
 * negotiatedInterfaceVersion is what pfnGetSupportedVersions settled on, not
 * what was requested: recording it is what lets a caller tell "the driver
 * accepted our highest word" from "the driver chose a lower one". */
struct WineD3D11On12AdapterDevice
{
    UINT size;
    UINT negotiatedInterfaceVersion;
    struct D3DWDDM2_6DDI_DEVICEFUNCS *deviceFuncs;
    void *hDrvAdapter;
    void *hDrvDevice;
    void *runtimeState;
    void *reserved[2];
};

struct WineD3D11On12Buffer
{
    UINT size;
    UINT reserved;
    void *hDrvResource;
    void *runtimeState;
};

#ifndef __cplusplus
typedef struct WineD3D11On12AdapterDevice WineD3D11On12AdapterDevice;
#endif

#ifdef __cplusplus
WINE_D3D11ON12_ASSERT(std::is_standard_layout_v<WineD3D11On12AdapterDevice>);
#endif
WINE_D3D11ON12_ASSERT(sizeof(void *) != 8
        || sizeof(WineD3D11On12AdapterDevice) == 56);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12AdapterDevice, deviceFuncs) == 8);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12AdapterDevice, hDrvAdapter) == 16);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12AdapterDevice, hDrvDevice) == 24);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12AdapterDevice, runtimeState) == 32);
WINE_D3D11ON12_ASSERT(sizeof(void *) != 8
        || sizeof(WineD3D11On12Buffer) == 24);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Buffer, hDrvResource) == 8);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12Buffer, runtimeState) == 16);

typedef HRESULT (WINAPI *WineD3D11On12OpenAdapterFn)(IUnknown *,
        IUnknown *const *, UINT, UINT, WineD3D11On12AdapterDevice *);

WINE_D3D11ON12_LINKAGE UINT WINAPI WineD3D11On12GetABIVersion(void)
        WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12GetInterface(UINT, UINT,
        WineD3D11On12Interface *) WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12CreateDeviceV1(IUnknown *,
        UINT, const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *, UINT, UINT,
        ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *)
        WINE_D3D11ON12_NOEXCEPT;
/* Opens the pinned D3D11On12 driver against a caller-supplied D3D12 device
 * and queue, negotiates the DDI interface version, and creates a DDI device.
 *
 * This is deliberately not WineD3D11On12CreateDeviceV1. d3d11on12.dll exports
 * one symbol, OpenAdapter_D3D11On12, and what it yields is a device function
 * table -- there is no ID3D11Device inside the driver to return. Building one
 * over this table is the D3D11 runtime's job, which docs/D3D11ON12.md plans
 * as a Wine d3d11 frontend refactor. Until that exists, CreateDeviceV1 keeps
 * failing closed rather than fabricating a device, and this entry point is
 * how the table is reached and proven. */
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12OpenAdapterV1(IUnknown *,
        IUnknown *const *, UINT, UINT, WineD3D11On12AdapterDevice *)
        WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12CloseAdapterDeviceV1(
        WineD3D11On12AdapterDevice *) WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12FlushAdapterDeviceV1(
        WineD3D11On12AdapterDevice *, UINT, UINT, BOOL *)
        WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12DrawAdapterDeviceV1(
        WineD3D11On12AdapterDevice *, UINT, UINT)
        WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12ValidateWrappedResourceV1(
        WineD3D11On12AdapterDevice *, IUnknown *)
        WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12DispatchDrawV1(
        WineD3D11On12AdapterDevice *, UINT, UINT, UINT, UINT, INT, UINT)
        WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12SetPrimitiveTopologyV1(
        WineD3D11On12AdapterDevice *, INT) WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12CreateBufferV1(
        WineD3D11On12AdapterDevice *, const D3D11_BUFFER_DESC *,
        const D3D11_SUBRESOURCE_DATA *, WineD3D11On12Buffer *)
        WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11On12DestroyBufferV1(
        WineD3D11On12Buffer *) WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11CreateDeviceV2(IDXGIAdapter *,
        D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
        ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **)
        WINE_D3D11ON12_NOEXCEPT;
WINE_D3D11ON12_LINKAGE HRESULT WINAPI WineD3D11CreateDeviceAndSwapChainV2(
        IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *,
        IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
        ID3D11DeviceContext **) WINE_D3D11ON12_NOEXCEPT;
