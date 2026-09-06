/* SPDX-License-Identifier: GPL-3.0-only
 * D3D11On12 core ABI and input validation.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

#include "d3d11on12core.h"

namespace
{
template<typename Interface>
class ComRef
{
public:
    ComRef() noexcept = default;
    ~ComRef() noexcept
    {
        if (pointer_)
            pointer_->Release();
    }

    ComRef(const ComRef &) = delete;
    ComRef &operator=(const ComRef &) = delete;

    Interface *get() const noexcept { return pointer_; }
    Interface **put() noexcept { return &pointer_; }

private:
    Interface *pointer_ = nullptr;
};

void clearOutputs(ID3D11Device **device, ID3D11DeviceContext **context,
        D3D_FEATURE_LEVEL *featureLevel) noexcept
{
    if (device)
        *device = nullptr;
    if (context)
        *context = nullptr;
    if (featureLevel)
        *featureLevel = static_cast<D3D_FEATURE_LEVEL>(0);
}

bool sameComObject(IUnknown *left, IUnknown *right) noexcept
{
    ComRef<IUnknown> leftIdentity;
    ComRef<IUnknown> rightIdentity;

    if (FAILED(left->QueryInterface(IID_IUnknown,
            reinterpret_cast<void **>(leftIdentity.put()))))
        return false;
    if (FAILED(right->QueryInterface(IID_IUnknown,
            reinterpret_cast<void **>(rightIdentity.put()))))
        return false;
    return leftIdentity.get() == rightIdentity.get();
}
}

extern "C" UINT WINAPI WineD3D11On12GetABIVersion() noexcept
{
    return WINE_D3D11ON12_ABI_VERSION;
}

extern "C" HRESULT WINAPI WineD3D11On12GetInterface(UINT requestedVersion,
        UINT interfaceSize, WineD3D11On12Interface *interfaceOut) noexcept
{
    if (!interfaceOut || interfaceSize != sizeof(*interfaceOut))
        return E_INVALIDARG;

    ZeroMemory(interfaceOut, sizeof(*interfaceOut));
    interfaceOut->size = sizeof(*interfaceOut);
    interfaceOut->version = WINE_D3D11ON12_ABI_VERSION;

    if (requestedVersion != WINE_D3D11ON12_ABI_VERSION)
        return E_NOINTERFACE;

    interfaceOut->capabilities = WINE_D3D11ON12_CAP_VALIDATION;
    interfaceOut->createDevice = WineD3D11On12CreateDeviceV1;
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12CreateDeviceV1(IUnknown *deviceObject,
        UINT, const D3D_FEATURE_LEVEL *featureLevels, UINT featureLevelCount,
        IUnknown *const *queueObjects, UINT queueCount, UINT nodeMask,
        ID3D11Device **device11, ID3D11DeviceContext **context11,
        D3D_FEATURE_LEVEL *chosenFeatureLevel) noexcept
{
    clearOutputs(device11, context11, chosenFeatureLevel);

    if (!deviceObject || !queueObjects || queueCount != 1 || !queueObjects[0])
        return E_INVALIDARG;
    if ((featureLevels == nullptr) != (featureLevelCount == 0))
        return E_INVALIDARG;
    if (nodeMask && (nodeMask & (nodeMask - 1)))
        return E_INVALIDARG;

    ComRef<ID3D12Device> device12;
    HRESULT hr = deviceObject->QueryInterface(IID_ID3D12Device,
            reinterpret_cast<void **>(device12.put()));
    if (FAILED(hr))
        return E_INVALIDARG;

    ComRef<ID3D12CommandQueue> queue;
    hr = queueObjects[0]->QueryInterface(IID_ID3D12CommandQueue,
            reinterpret_cast<void **>(queue.put()));
    if (FAILED(hr))
        return E_INVALIDARG;
    if (queue.get()->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return E_INVALIDARG;

    ComRef<ID3D12Device> queueDevice;
    hr = queue.get()->GetDevice(IID_ID3D12Device,
            reinterpret_cast<void **>(queueDevice.put()));
    if (FAILED(hr) || !sameComObject(device12.get(), queueDevice.get()))
        return E_INVALIDARG;

    if (featureLevelCount)
    {
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
        levels.NumFeatureLevels = featureLevelCount;
        levels.pFeatureLevelsRequested = featureLevels;
        hr = device12.get()->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,
                &levels, sizeof(levels));
        if (FAILED(hr) || !levels.MaxSupportedFeatureLevel)
            return FAILED(hr) ? hr : E_INVALIDARG;
    }

    /* The next milestone constructs the Wine D3D11 runtime/DDI host here.
     * Never return success until genuine ID3D11Device and context objects are
     * backed by the supplied device and queue. */
    return DXGI_ERROR_UNSUPPORTED;
}
