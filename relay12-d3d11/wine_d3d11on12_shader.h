/* SPDX-License-Identifier: GPL-3.0-only
 * MIT interface transcription from the pinned
 * third_party/D3D11On12/interface/D3D11On12DDI.h.
 * CastFrom identifies hDevice.pDrvPrivate as this interface. It has neither
 * IUnknown nor a virtual destructor. Unused prefix methods reserve slots only
 * and must never be called. check_adapter_args.py pins their order upstream.
 */
#pragma once
#include "d3d11on12core.h"
#include "ddi/wine_d3d11ddi.h"

typedef struct WineD3D11On12ShaderDesc
{
    const BYTE *pFunction;
    UINT SizeInBytes;
    ID3D11ClassLinkage *pLinkage;
} WineD3D11On12ShaderDesc;

/* ID3D11On12DDIResource is also a non-COM MIT driver interface. */
typedef struct WineD3D11On12DDIResource WineD3D11On12DDIResource;
typedef struct WineD3D11On12DDIResourceVtbl
{
    ID3D12Resource *(STDMETHODCALLTYPE *GetUnderlyingResource)(WineD3D11On12DDIResource *);
    void (STDMETHODCALLTYPE *SetGraphicsCurrentState)(WineD3D11On12DDIResource *, D3D12_RESOURCE_STATES, UINT);
    void (STDMETHODCALLTYPE *CreateSharedHandle)(void);
} WineD3D11On12DDIResourceVtbl;
struct WineD3D11On12DDIResource
{
    const WineD3D11On12DDIResourceVtbl *lpVtbl;
};
typedef struct WineD3D11On12DDIDevice WineD3D11On12DDIDevice;
typedef struct WineD3D11On12DDIDeviceVtbl
{
    void (STDMETHODCALLTYPE *GetD3D12Device)(void);
    void (STDMETHODCALLTYPE *GetGraphicsQueue)(void);
    void (STDMETHODCALLTYPE *EnqueueSetEvent)(void);
    void (STDMETHODCALLTYPE *GetNodeMask)(void);
    void (STDMETHODCALLTYPE *Present)(void);
    UINT (STDMETHODCALLTYPE *GetResourcePrivateDataSize)(WineD3D11On12DDIDevice *);
    void (STDMETHODCALLTYPE *OpenSharedHandle)(void);
    HRESULT (STDMETHODCALLTYPE *CreateWrappingHandle)(WineD3D11On12DDIDevice *, IUnknown *, UINT, void *, UINT, UINT *);
    void (STDMETHODCALLTYPE *FillResourceInfo)(void);
    void (STDMETHODCALLTYPE *DestroyKMTHandle)(WineD3D11On12DDIDevice *, UINT);
    void (STDMETHODCALLTYPE *TransitionResourceForRelease)(WineD3D11On12DDIDevice *, WineD3D11On12DDIResource *, D3D12_RESOURCE_STATES);
    void (STDMETHODCALLTYPE *ApplyAllResourceTransitions)(WineD3D11On12DDIDevice *);
    void (STDMETHODCALLTYPE *CreateFence)(void);
    void (STDMETHODCALLTYPE *OpenFence)(void);
    void (STDMETHODCALLTYPE *Wait)(void);
    void (STDMETHODCALLTYPE *Signal)(void);
    HRESULT (STDMETHODCALLTYPE *CreateVertexShader)(WineD3D11On12DDIDevice *,
            D3D10DDI_HSHADER, const WineD3D11On12ShaderDesc *);
    HRESULT (STDMETHODCALLTYPE *CreatePixelShader)(WineD3D11On12DDIDevice *,
            D3D10DDI_HSHADER, const WineD3D11On12ShaderDesc *);
} WineD3D11On12DDIDeviceVtbl;
struct WineD3D11On12DDIDevice
{
    const WineD3D11On12DDIDeviceVtbl *lpVtbl;
};
WINE_D3D11ON12_ASSERT(sizeof(WineD3D11On12ShaderDesc) == 24);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12ShaderDesc, SizeInBytes) == 8);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12ShaderDesc, pLinkage) == 16);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12DDIDeviceVtbl, CreateVertexShader) == 128);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12DDIDeviceVtbl, CreatePixelShader) == 136);

WINE_D3D11ON12_ASSERT(sizeof(WineD3D11On12DDIResourceVtbl) == 24);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12DDIResourceVtbl, SetGraphicsCurrentState) == 8);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12DDIDeviceVtbl, CreateWrappingHandle) == 56);
WINE_D3D11ON12_ASSERT(offsetof(WineD3D11On12DDIDeviceVtbl, TransitionResourceForRelease) == 80);
