/* SPDX-License-Identifier: GPL-3.0-only
 * D3D11On12 core ABI and input validation.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

#include "d3d11on12core.h"
#include "wine_d3d11_diag.h"

/* The clean-room DDI declarations, then the driver's own boundary header.
 *
 * Order matters and is not incidental. D3D11On12DDI.h has no includes of its
 * own -- it names D3D10DDIARG_OPENADAPTER, D3D10DDI_HRESOURCE, ID3D12Device1
 * and D3D11_RESOURCE_FLAGS and expects its includer to have supplied them.
 * That is what keeps it from reaching the licensed SDK overlay from here: the
 * DDI types it uses come from the clean-room header above it, and the D3D11
 * and D3D12 ones from the public headers this file already includes.
 *
 * docs/CLEANROOM-DDI.md records interface/D3D11On12DDI.h as MIT and usable
 * directly; it is the host-driver boundary, not a WDK header.
 *
 * The negotiation helpers arrive with the declarations they operate on --
 * wine_d3d11ddi_negotiate.h includes wine_d3d11ddi.h itself -- so including
 * it supplies both, and wineD3D11DdiSelectVersion is reused rather than this
 * file reimplementing the supported-version arithmetic. */
#include "wine_d3d11ddi_negotiate.h"

/* The adapter-arguments subset of interface/D3D11On12DDI.h, transcribed under
 * its MIT licence rather than included.
 *
 * Including the header outright does not work from here. Its
 * ID3D11On12DDIDevice declarations name D3DKMT_PRESENT, D3DKMT_HANDLE and
 * D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT -- WDK and DDI types the
 * clean-room header has not authored, and which this host never uses. The
 * driver compiles it with the licensed overlay on its include path; the core
 * must not have that path, so it takes the four declarations it actually
 * needs.
 *
 * scripts/check_adapter_args.py compares these to the pinned header on every
 * run. The structure is passed by address to separately compiled code, so a
 * field added upstream would not fail to compile here -- the driver would
 * read past the end of a structure this side believes it filled, and nothing
 * else in the build would notice. */
namespace D3D11On12
{
struct PrivateCallbacks
{
    D3D11_RESOURCE_FLAGS (CALLBACK *GetResourceFlags)(D3D10DDI_HRESOURCE,
            bool *pbAcquireableOnWrite);
    bool (CALLBACK *NotifySharedResourceCreation)(HANDLE, IUnknown *);
};

struct Present11On12CBArgs;

struct PrivateCallbacks2
{
    HRESULT (CALLBACK *Present11On12CB)(HANDLE, Present11On12CBArgs *);
};

constexpr UINT c_CurrentD3D11On12InterfaceVersion = 7;

struct SOpenAdapterArgs
{
    ID3D12Device1 *pDevice;
    ID3D12CommandQueue *p3DCommandQueue;
    IUnknown *pAdapter;
    UINT NodeIndex;
    PrivateCallbacks Callbacks;
    bool bDisableGPUTimeout;

    bool bSupportDisplayableTextures;
    bool bSupportDeferredContexts;

    UINT D3D11On12InterfaceVersion = c_CurrentD3D11On12InterfaceVersion;

    PrivateCallbacks2 *Callbacks2;
    bool bSupportPrepatchedShaders;
};
}

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

    Interface *detach() noexcept
    {
        Interface *result = pointer_;
        pointer_ = nullptr;
        return result;
    }

    /* Releasing first is what keeps a second acquisition through the same
     * holder from dropping the first reference on the floor.  No call site
     * reuses a holder today, and this is here so that the holder is not the
     * reason one cannot. */
    Interface **put() noexcept
    {
        if (pointer_)
        {
            pointer_->Release();
            pointer_ = nullptr;
        }
        return &pointer_;
    }

private:
    Interface *pointer_ = nullptr;
};

/* Every interface this boundary acquires goes through here.
 *
 * A success code returned with no interface breaks the COM contract, and the
 * D3DMetal payload does it, so the pointer has to be checked at every
 * acquisition and not just at most of them.  Making that one funnel rather
 * than a rule each new call site has to remember is the point: CI rejects a
 * QueryInterface or GetDevice call that does not pass its holder through
 * this.
 *
 * The holder is taken by reference, not as a pointer value.  Argument
 * evaluation order is unspecified, so passing acquired.get() alongside the
 * call that fills it could read the pointer before the call writes it and
 * reject every success.  Binding a reference reads nothing; the read happens
 * in the body, once both arguments are evaluated. */
template<typename Interface>
HRESULT strictResult(HRESULT hr, const ComRef<Interface> &acquired) noexcept
{
    if (SUCCEEDED(hr) && !acquired.get())
        return E_NOINTERFACE;
    return hr;
}

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

/* Decide whether two objects are the same COM object.
 *
 * IUnknown is the canonical test: it is the only interface COM requires to
 * return one stable pointer per object.  But a payload that answers an
 * IUnknown query with S_OK and no pointer cannot be identified that way at
 * all, and comparing two such answers would make every object identical to
 * every other -- which is how a queue belonging to a different device used to
 * pass this check.
 *
 * So identity is never inferred from a failure.  An indeterminate answer is
 * propagated to the caller, which fails closed.  The typed-pointer fast path
 * at the call site keeps that from rejecting a payload whose only defect is
 * the IUnknown query. */
HRESULT comObjectsIdentical(IUnknown *left, IUnknown *right,
        bool *identical) noexcept
{
    ComRef<IUnknown> leftIdentity;
    ComRef<IUnknown> rightIdentity;
    HRESULT hr;

    *identical = false;

    hr = strictResult(left->QueryInterface(IID_IUnknown,
            reinterpret_cast<void **>(leftIdentity.put())), leftIdentity);
    if (FAILED(hr))
        return hr;

    hr = strictResult(right->QueryInterface(IID_IUnknown,
            reinterpret_cast<void **>(rightIdentity.put())), rightIdentity);
    if (FAILED(hr))
        return hr;

    *identical = leftIdentity.get() == rightIdentity.get();
    return S_OK;
}

/* The public creation flags this boundary recognizes.
 *
 * Composed from the named enumerators rather than from literals: the numbers
 * are the SDK's to define, and a hand-copied bitmask here would be one more
 * place for them to be wrong.  An application passing a bit outside this set
 * is passing something no published flag names, and accepting it silently
 * would mean promising to honour it.
 *
 * Honouring the ones inside the set is a separate matter, and not this
 * milestone's: which public flag sets which member of the DDI's Flags word is
 * a runtime implementation detail that is not publicly specified, so this
 * validates and records rather than translating.  See docs/CLEANROOM-DDI.md
 * for the open question. */
constexpr UINT knownCreateDeviceFlags = D3D11_CREATE_DEVICE_SINGLETHREADED
        | D3D11_CREATE_DEVICE_DEBUG
        | D3D11_CREATE_DEVICE_SWITCH_TO_REF
        | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS
        | D3D11_CREATE_DEVICE_BGRA_SUPPORT
        | D3D11_CREATE_DEVICE_DEBUGGABLE
        | D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY
        | D3D11_CREATE_DEVICE_DISABLE_GPU_TIMEOUT
        | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

/* One guard per repeatable condition.  An application that probes
 * D3D11On12CreateDevice in its initialization loop must not be able to flood
 * the log with these. */
volatile LONG reportedDeviceNotD3D12;
volatile LONG reportedQueueNotD3D12;
volatile LONG reportedQueueDevice;
volatile LONG reportedIdentity;
volatile LONG reportedNodeCount;
volatile LONG reportedUntranslatedFlags;
volatile LONG reportedNoHost;

using D3D12CreateDeviceFn = HRESULT (WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL,
        REFIID, void **);

INIT_ONCE d3d12InitOnce = INIT_ONCE_STATIC_INIT;
/* shared-state: published once through d3d12InitOnce */
HMODULE d3d12Module;
D3D12CreateDeviceFn d3d12CreateDevice;

BOOL CALLBACK initializeD3D12(PINIT_ONCE, PVOID, PVOID *) noexcept
{
    d3d12Module = LoadLibraryW(L"d3d12.dll");
    if (d3d12Module)
    {
        const FARPROC address = GetProcAddress(d3d12Module,
                "D3D12CreateDevice");
        static_assert(sizeof(d3d12CreateDevice) == sizeof(address),
                "Win32 function pointers must have equal size");
        __builtin_memcpy(&d3d12CreateDevice, &address,
                sizeof(d3d12CreateDevice));
    }
    return TRUE;
}

void clearDirectOutputs(ID3D11Device **device,
        D3D_FEATURE_LEVEL *featureLevel,
        ID3D11DeviceContext **context) noexcept
{
    clearOutputs(device, context, featureLevel);
}

/* How many supported-version words this host is willing to read.
 *
 * pfnGetSupportedVersions writes its own count whenever the buffer pointer is
 * non-null, so the count it is given is not a capacity and the buffer must be
 * large enough for whatever the driver reports. wineD3D11DdiSelectVersion
 * refuses to show the driver a buffer smaller than its reported count rather
 * than overflowing, so this bound turns a driver advertising an implausible
 * number of versions into a clean refusal. */
#define WINE_D3D11ON12_MAX_SUPPORTED_VERSIONS 64u

/* One guard per repeatable condition on the adapter path, for the same
 * reason as the set above. */
volatile LONG reportedDeviceNotD3D12_1;
volatile LONG reportedDriverMissing;
volatile LONG reportedDriverEntryMissing;
volatile LONG reportedOpenAdapterFailed;
volatile LONG reportedNoSupportedVersion;
volatile LONG reportedCreateDeviceFailed;
volatile LONG reportedResourceFlagsQuery;
volatile LONG reportedSharedResourceNotify;

using OpenAdapterFn = HRESULT (WINAPI *)(D3D10DDIARG_OPENADAPTER *,
        D3D11On12::SOpenAdapterArgs *);

INIT_ONCE driverInitOnce = INIT_ONCE_STATIC_INIT;
/* shared-state: published once through driverInitOnce */
HMODULE driverModule;
/* shared-state: published once through driverInitOnce */
OpenAdapterFn driverOpenAdapter;

BOOL CALLBACK initializeDriver(PINIT_ONCE, PVOID, PVOID *) noexcept
{
    driverModule = LoadLibraryW(L"d3d11on12.dll");
    if (driverModule)
    {
        const FARPROC address = GetProcAddress(driverModule,
                "OpenAdapter_D3D11On12");
        static_assert(sizeof(driverOpenAdapter) == sizeof(address),
                "Win32 function pointers must have equal size");
        __builtin_memcpy(&driverOpenAdapter, &address,
                sizeof(driverOpenAdapter));
    }
    return TRUE;
}

/* The two callbacks the driver copies out of SOpenAdapterArgs and calls back
 * into. They are not reached during adapter or device creation, but the
 * driver keeps them for the lifetime of the adapter, so a null here would be
 * a crash later rather than an error now.
 *
 * Both answer conservatively and say so once. Resource sharing and the flags
 * a wrapped resource was created with are runtime state this milestone does
 * not track; reporting no flags and refusing the share is the answer that
 * makes a caller fail rather than proceed on an invented one. */
D3D11_RESOURCE_FLAGS CALLBACK hostGetResourceFlags(D3D10DDI_HRESOURCE,
        bool *acquireableOnWrite) noexcept
{
    wineD3D11DiagReportOnce(&reportedResourceFlagsQuery,
            "d3d11on12core: the driver asked for a wrapped resource's D3D11 "
            "creation flags; no runtime tracks them in this milestone, so "
            "none are reported.\n");
    if (acquireableOnWrite)
        *acquireableOnWrite = false;
    D3D11_RESOURCE_FLAGS flags = {};
    return flags;
}

bool CALLBACK hostNotifySharedResourceCreation(HANDLE, IUnknown *) noexcept
{
    wineD3D11DiagReportOnce(&reportedSharedResourceNotify,
            "d3d11on12core: the driver announced a shared resource; no "
            "runtime tracks shared resources in this milestone, so the "
            "notification is refused rather than silently accepted.\n");
    return false;
}

/* SOpenAdapterArgs takes a node index where the public API takes a mask.
 * The mask is already known to have exactly one bit set; this is which. */
UINT nodeMaskToIndex(UINT nodeMask) noexcept
{
    UINT index = 0;

    while (nodeMask > 1u)
    {
        nodeMask >>= 1;
        ++index;
    }
    return index;
}

/* One allocation holds everything the driver keeps a pointer to.  The adapter
 * function table, the two callback tables and the driver's private device
 * block all have to outlive the call that creates them, and tying their
 * lifetime to one block is what will let CloseAdapter release them together.
 *
 * privateDevice is a flexible tail rather than a separate allocation: the
 * driver is handed its address as D3D10DDI_HDEVICE.pDrvPrivate and keeps it
 * for the device's lifetime, so it must not move or be freed separately. */
struct AdapterState
{
    D3D10_2DDI_ADAPTERFUNCS adapterFuncs;
    D3DWDDM2_6DDI_DEVICEFUNCS deviceFuncs;
    D3DDDI_DEVICECALLBACKS kernelCallbacks;
    D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS coreCallbacks;
    PFND3D10DDI_RETRIEVESUBOBJECT retrieveSubObject;
    D3D10DDI_HADAPTER hAdapter;
    D3D10DDI_HDEVICE hDevice;
    ID3D12Device1 *device12;
    ID3D12CommandQueue *queue;
    bool adapterOpened;
    bool deviceCreated;
    unsigned char privateDevice[1];
};

void destroyAdapterState(AdapterState *state) noexcept
{
    if (!state)
        return;

    if (state->deviceCreated && state->deviceFuncs.pfnDestroyDevice)
        state->deviceFuncs.pfnDestroyDevice(state->hDevice);
    if (state->adapterOpened && state->adapterFuncs.pfnCloseAdapter)
        state->adapterFuncs.pfnCloseAdapter(state->hAdapter);
    if (state->queue)
        state->queue->Release();
    if (state->device12)
        state->device12->Release();
    HeapFree(GetProcessHeap(), 0, state);
}

/* Negotiate the device interface version, then create the DDI device.
 *
 * The version is data, not a compile-time literal: the driver advertises its
 * supported words through pfnGetSupportedVersions and the highest is selected
 * and handed back as D3D10DDIARG_CREATEDEVICE.Interface.  That is why this
 * host needs no unreleased WDK version constants, and why the negotiated
 * value is reported to the caller rather than assumed.
 *
 * The private device block is the runtime's to allocate and the driver's to
 * use: CalcPrivateDeviceSize says how large, and its address becomes
 * hDrvDevice.pDrvPrivate. */
HRESULT createDriverDevice(AdapterState **statePtr,
        WineD3D11On12AdapterDevice *out) noexcept
{
    AdapterState *state = *statePtr;
    UINT64 words[WINE_D3D11ON12_MAX_SUPPORTED_VERSIONS] = {};
    UINT64 selectedWord = 0;
    UINT selectedInterface = 0;

    HRESULT hr = wineD3D11DdiSelectVersion(
            state->adapterFuncs.pfnGetSupportedVersions, state->hAdapter,
            words, WINE_D3D11ON12_MAX_SUPPORTED_VERSIONS, &selectedWord,
            &selectedInterface);
    if (FAILED(hr))
    {
        wineD3D11DiagReportOnce(&reportedNoSupportedVersion,
                "d3d11on12core: the driver advertised no usable DDI "
                "interface version.\n");
        return hr;
    }

    if (!state->adapterFuncs.pfnCalcPrivateDeviceSize
            || !state->adapterFuncs.pfnCreateDevice)
        return DXGI_ERROR_UNSUPPORTED;

    D3D10DDIARG_CREATEDEVICE createDevice = {};
    createDevice.Interface = selectedInterface;
    createDevice.Version = WINE_D3D11_DDI_BUILD_FROM_SUPPORTED(selectedWord);
    createDevice.pKTCallbacks = &state->kernelCallbacks;
    createDevice.pWDDM2_6DeviceFuncs = &state->deviceFuncs;
    createDevice.pWDDM2_6UMCallbacks = &state->coreCallbacks;
    createDevice.ppfnRetrieveSubObject = &state->retrieveSubObject;

    /* Its own argument structure, not the create-device one: the sizing call
     * is told which interface and version the device will be created with,
     * and nothing else. */
    D3D10DDIARG_CALCPRIVATEDEVICESIZE sizeArgs = {};
    sizeArgs.Interface = createDevice.Interface;
    sizeArgs.Version = createDevice.Version;
    sizeArgs.Flags = createDevice.Flags;

    const SIZE_T privateSize = state->adapterFuncs.pfnCalcPrivateDeviceSize(
            state->hAdapter, &sizeArgs);
    if (!privateSize)
        return E_FAIL;

    /* Grown in place, because the driver has not been shown the block yet.
     * Reallocating after CreateDevice would move memory the driver holds. */
    const SIZE_T total = sizeof(AdapterState) + privateSize;
    AdapterState *grown = static_cast<AdapterState *>(HeapReAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, state, total));
    if (!grown)
        return E_OUTOFMEMORY;
    state = grown;
    *statePtr = grown;

    /* Re-point every interior pointer: HeapReAlloc may have moved the block,
     * and createDevice still holds the old addresses. */
    createDevice.pKTCallbacks = &state->kernelCallbacks;
    createDevice.pWDDM2_6DeviceFuncs = &state->deviceFuncs;
    createDevice.pWDDM2_6UMCallbacks = &state->coreCallbacks;
    createDevice.ppfnRetrieveSubObject = &state->retrieveSubObject;

    state->hDevice.pDrvPrivate = state->privateDevice;
    createDevice.hDrvDevice = state->hDevice;

    hr = state->adapterFuncs.pfnCreateDevice(state->hAdapter, &createDevice);
    if (FAILED(hr))
    {
        wineD3D11DiagReportOnce(&reportedCreateDeviceFailed,
                "d3d11on12core: the driver rejected CreateDevice.\n");
        return hr;
    }
    if (!state->deviceFuncs.pfnDestroyDevice)
        return DXGI_ERROR_UNSUPPORTED;
    state->deviceCreated = true;

    out->negotiatedInterfaceVersion = selectedInterface;
    out->deviceFuncs = &state->deviceFuncs;
    out->hDrvAdapter = state->hAdapter.pDrvPrivate;
    out->hDrvDevice = state->hDevice.pDrvPrivate;
    out->runtimeState = state;
    return S_OK;
}

/* The device and queue checks both creation entry points perform.
 *
 * Factored rather than copied because they are the project's first
 * non-negotiable invariant -- work must not be submitted to a queue owned by
 * another device -- and two copies would be two places for that to drift.
 * Every acquisition still goes through strictResult, which CI enforces. */
HRESULT acquireDeviceAndQueue(IUnknown *deviceObject,
        IUnknown *const *queueObjects, UINT queueCount, UINT nodeMask,
        ComRef<ID3D12Device> &device12,
        ComRef<ID3D12CommandQueue> &queue) noexcept
{
    if (!deviceObject || !queueObjects || queueCount != 1 || !queueObjects[0])
        return E_INVALIDARG;
    if (nodeMask && (nodeMask & (nodeMask - 1)))
        return E_INVALIDARG;

    HRESULT hr = strictResult(deviceObject->QueryInterface(IID_ID3D12Device,
            reinterpret_cast<void **>(device12.put())), device12);
    if (FAILED(hr))
    {
        wineD3D11DiagReportOnce(&reportedDeviceNotD3D12,
                "d3d11on12core: the supplied device object does not implement "
                "ID3D12Device.\n");
        return hr;
    }

    if (nodeMask)
    {
        const UINT nodeCount = device12.get()->GetNodeCount();

        if (!nodeCount)
        {
            wineD3D11DiagReportOnce(&reportedNodeCount,
                    "d3d11on12core: the supplied device reports zero nodes, so "
                    "no node mask can name a node it has.\n");
            return E_INVALIDARG;
        }
        if (nodeCount < 32 && nodeMask >= (1u << nodeCount))
            return E_INVALIDARG;
    }

    hr = strictResult(queueObjects[0]->QueryInterface(IID_ID3D12CommandQueue,
            reinterpret_cast<void **>(queue.put())), queue);
    if (FAILED(hr))
    {
        wineD3D11DiagReportOnce(&reportedQueueNotD3D12,
                "d3d11on12core: the supplied queue object does not implement "
                "ID3D12CommandQueue.\n");
        return hr;
    }
    if (queue.get()->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return E_INVALIDARG;

    ComRef<ID3D12Device> queueDevice;
    hr = strictResult(queue.get()->GetDevice(IID_ID3D12Device,
            reinterpret_cast<void **>(queueDevice.put())), queueDevice);
    if (FAILED(hr))
    {
        wineD3D11DiagReportOnce(&reportedQueueDevice,
                "d3d11on12core: the supplied queue would not report its "
                "owning ID3D12Device.\n");
        return hr;
    }

    bool identical = device12.get() == queueDevice.get();
    if (!identical)
    {
        hr = comObjectsIdentical(device12.get(), queueDevice.get(),
                &identical);
        if (FAILED(hr))
        {
            wineD3D11DiagReportOnce(&reportedIdentity,
                    "d3d11on12core: the COM identity of the supplied device "
                    "and the queue's owning device could not be established; "
                    "refusing to assume they match.\n");
            return hr;
        }
    }
    if (!identical)
        return E_INVALIDARG;

    return S_OK;
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

    interfaceOut->capabilities = WINE_D3D11ON12_CAP_VALIDATION
            | WINE_D3D11ON12_CAP_D3DMETAL_BOOTSTRAP
            | WINE_D3D11ON12_CAP_DEVICE_LIFECYCLE
            | WINE_D3D11ON12_CAP_IMMEDIATE_CONTEXT_FLUSH;
    interfaceOut->createDevice = WineD3D11On12CreateDeviceV1;
    interfaceOut->createDirectDevice = WineD3D11CreateDeviceV2;
    interfaceOut->createDirectDeviceAndSwapChain =
            WineD3D11CreateDeviceAndSwapChainV2;
    interfaceOut->closeAdapterDevice = WineD3D11On12CloseAdapterDeviceV1;
    interfaceOut->flushAdapterDevice = WineD3D11On12FlushAdapterDeviceV1;
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12CreateDeviceV1(IUnknown *deviceObject,
        UINT flags, const D3D_FEATURE_LEVEL *featureLevels,
        UINT featureLevelCount, IUnknown *const *queueObjects, UINT queueCount,
        UINT nodeMask, ID3D11Device **device11,
        ID3D11DeviceContext **context11,
        D3D_FEATURE_LEVEL *chosenFeatureLevel) noexcept
{
    clearOutputs(device11, context11, chosenFeatureLevel);

    if ((featureLevels == nullptr) != (featureLevelCount == 0))
        return E_INVALIDARG;
    if (flags & ~knownCreateDeviceFlags)
        return E_INVALIDARG;

    /* Accepted, but carried no further than this function.  Saying so once is
     * the difference between a known gap and a parameter that looks honoured
     * because nothing complained. */
    if (flags)
        wineD3D11DiagReportOnce(&reportedUntranslatedFlags,
                "d3d11on12core: the requested D3D11 creation flags are "
                "validated but not yet translated to the DDI's create-device "
                "flags; no flag is being honoured in this milestone.\n");

    ComRef<ID3D12Device> device12;
    ComRef<ID3D12CommandQueue> queue;
    HRESULT hr = acquireDeviceAndQueue(deviceObject, queueObjects, queueCount,
            nodeMask, device12, queue);
    if (FAILED(hr))
        return hr;

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

    /* Exercise the real adapter/device construction path here rather than
     * leaving the public entry point disconnected from it.  The lifetime is
     * closed again before returning because the Wine frontend cannot own the
     * token yet.  Once that frontend supplies a controlling ID3D11Device, the
     * same token moves into its backend_private member instead.
     *
     * Failure remains DXGI_ERROR_UNSUPPORTED at this public boundary.  The
     * detailed adapter diagnostic has already been emitted, and exposing its
     * private HRESULT here would make availability depend on deployment
     * details instead of the documented D3D11 fallback result. */
    WineD3D11On12AdapterDevice adapterDevice = {};
    adapterDevice.size = sizeof(adapterDevice);
    hr = WineD3D11On12OpenAdapterV1(deviceObject, queueObjects, queueCount,
            nodeMask, &adapterDevice);
    if (SUCCEEDED(hr))
        WineD3D11On12CloseAdapterDeviceV1(&adapterDevice);

    /* Still no ID3D11Device, and deliberately so.  Never return success until
     * genuine device and context objects own the DDI lifetime and all methods
     * they expose have truthful backing behavior. */
    wineD3D11DiagReportOnce(&reportedNoHost,
            "d3d11on12core: device and queue accepted, but no D3D11 "
            "runtime/DDI host is implemented in this milestone; returning "
            "DXGI_ERROR_UNSUPPORTED instead of a fabricated device.\n");
    return DXGI_ERROR_UNSUPPORTED;
}

extern "C" HRESULT WINAPI WineD3D11On12OpenAdapterV1(IUnknown *deviceObject,
        IUnknown *const *queueObjects, UINT queueCount, UINT nodeMask,
        WineD3D11On12AdapterDevice *out) noexcept
{
    if (!out || out->size != sizeof(*out))
        return E_INVALIDARG;

    out->negotiatedInterfaceVersion = 0;
    out->deviceFuncs = nullptr;
    out->hDrvAdapter = nullptr;
    out->hDrvDevice = nullptr;
    out->runtimeState = nullptr;

    ComRef<ID3D12Device> device12;
    ComRef<ID3D12CommandQueue> queue;
    HRESULT hr = acquireDeviceAndQueue(deviceObject, queueObjects, queueCount,
            nodeMask, device12, queue);
    if (FAILED(hr))
        return hr;

    /* SOpenAdapterArgs takes ID3D12Device1, not ID3D12Device.  A payload that
     * implements the base interface and not this one cannot drive the driver,
     * and saying which interface is missing is more use than a generic
     * failure. */
    ComRef<ID3D12Device1> device12_1;
    hr = strictResult(device12.get()->QueryInterface(IID_ID3D12Device1,
            reinterpret_cast<void **>(device12_1.put())), device12_1);
    if (FAILED(hr))
    {
        wineD3D11DiagReportOnce(&reportedDeviceNotD3D12_1,
                "d3d11on12core: the supplied device implements ID3D12Device "
                "but not ID3D12Device1, which the driver's adapter arguments "
                "require.\n");
        return hr;
    }

    if (!InitOnceExecuteOnce(&driverInitOnce, initializeDriver, nullptr,
            nullptr))
        return E_FAIL;
    if (!driverModule)
    {
        wineD3D11DiagReportOnce(&reportedDriverMissing,
                "d3d11on12core: d3d11on12.dll could not be loaded; the "
                "translation driver is not deployed beside this module.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }
    if (!driverOpenAdapter)
    {
        wineD3D11DiagReportOnce(&reportedDriverEntryMissing,
                "d3d11on12core: d3d11on12.dll is present but exports no "
                "OpenAdapter_D3D11On12; it is not the pinned driver.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }

    AdapterState *state = static_cast<AdapterState *>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(AdapterState)));
    if (!state)
        return E_OUTOFMEMORY;

    D3D11On12::PrivateCallbacks privateCallbacks = {};
    privateCallbacks.GetResourceFlags = hostGetResourceFlags;
    privateCallbacks.NotifySharedResourceCreation =
            hostNotifySharedResourceCreation;

    /* Version 7 is the pinned driver's current interface, and at 7 it
     * dereferences Callbacks2 unconditionally.  A null pointer here would be
     * a crash inside the driver's constructor rather than a rejected
     * argument, so the table is always supplied; a null Present11On12CB
     * inside it is how the driver is told the new present path is absent. */
    D3D11On12::PrivateCallbacks2 privateCallbacks2 = {};
    privateCallbacks2.Present11On12CB = nullptr;

    D3D11On12::SOpenAdapterArgs driverArgs = {};
    driverArgs.pDevice = device12_1.get();
    driverArgs.p3DCommandQueue = queue.get();
    driverArgs.pAdapter = nullptr;
    driverArgs.NodeIndex = nodeMask ? nodeMaskToIndex(nodeMask) : 0u;
    driverArgs.Callbacks = privateCallbacks;
    driverArgs.bDisableGPUTimeout = false;
    driverArgs.bSupportDisplayableTextures = false;
    driverArgs.bSupportDeferredContexts = false;
    driverArgs.D3D11On12InterfaceVersion =
            D3D11On12::c_CurrentD3D11On12InterfaceVersion;
    driverArgs.Callbacks2 = &privateCallbacks2;
    driverArgs.bSupportPrepatchedShaders = false;

    /* pAdapterCallbacks stays null.  The driver's Adapter constructor reads
     * only pAdapterFuncs_2 and hAdapter from this structure, so the kernel
     * adapter callbacks it never consults are not authored, and passing a
     * fabricated table would be worse than passing none. */
    D3D10DDIARG_OPENADAPTER openAdapter = {};
    /* Zero, and documented as such.  docs/CLEANROOM-DDI.md records that
     * OpenAdapter_D3D11On12 never inspects Interface or Version -- its whole
     * body constructs an Adapter from pArgs2 -- and the device interface is
     * negotiated below from the words the driver itself advertises.  Passing
     * an invented literal here would be pretending to a version nobody
     * checks. */
    openAdapter.Interface = 0;
    openAdapter.Version = 0;
    openAdapter.pAdapterCallbacks = nullptr;
    openAdapter.pAdapterFuncs_2 = &state->adapterFuncs;

    hr = driverOpenAdapter(&openAdapter, &driverArgs);
    if (FAILED(hr))
    {
        wineD3D11DiagReportOnce(&reportedOpenAdapterFailed,
                "d3d11on12core: the driver rejected OpenAdapter_D3D11On12.\n");
        HeapFree(GetProcessHeap(), 0, state);
        return hr;
    }
    state->hAdapter = openAdapter.hAdapter;
    state->adapterOpened = true;

    /* An adapter without its required destruction callback cannot
     * participate in the owned lifecycle promised by ABI v3. */
    if (!state->adapterFuncs.pfnCloseAdapter)
    {
        destroyAdapterState(state);
        return DXGI_ERROR_UNSUPPORTED;
    }

    /* The driver's adapter also retains these today, but the host's lifetime
     * must not depend on that implementation detail. */
    device12_1.get()->AddRef();
    state->device12 = device12_1.get();
    queue.get()->AddRef();
    state->queue = queue.get();

    hr = createDriverDevice(&state, out);
    if (FAILED(hr))
    {
        destroyAdapterState(state);
        return hr;
    }

    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12CloseAdapterDeviceV1(
        WineD3D11On12AdapterDevice *adapterDevice) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice))
        return E_INVALIDARG;

    AdapterState *state = static_cast<AdapterState *>(
            adapterDevice->runtimeState);
    adapterDevice->negotiatedInterfaceVersion = 0;
    adapterDevice->deviceFuncs = nullptr;
    adapterDevice->hDrvAdapter = nullptr;
    adapterDevice->hDrvDevice = nullptr;
    adapterDevice->runtimeState = nullptr;
    ZeroMemory(adapterDevice->reserved, sizeof(adapterDevice->reserved));

    destroyAdapterState(state);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12FlushAdapterDeviceV1(
        WineD3D11On12AdapterDevice *adapterDevice, UINT contextType,
        UINT flushFlags, BOOL *submitted) noexcept
{
    if (submitted)
        *submitted = FALSE;
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice)
            || !submitted)
        return E_INVALIDARG;

    AdapterState *state = static_cast<AdapterState *>(
            adapterDevice->runtimeState);
    if (!state || !state->deviceCreated || !state->deviceFuncs.pfnFlush)
        return DXGI_ERROR_UNSUPPORTED;

    *submitted = state->deviceFuncs.pfnFlush(state->hDevice, contextType,
            flushFlags);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11CreateDeviceV2(IDXGIAdapter *adapter,
        D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
        const D3D_FEATURE_LEVEL *featureLevels, UINT featureLevelCount,
        UINT sdkVersion, ID3D11Device **device11,
        D3D_FEATURE_LEVEL *chosenFeatureLevel,
        ID3D11DeviceContext **context11) noexcept
{
    clearDirectOutputs(device11, chosenFeatureLevel, context11);

    if (sdkVersion != D3D11_SDK_VERSION)
        return E_INVALIDARG;
    if ((featureLevels == nullptr) != (featureLevelCount == 0))
        return E_INVALIDARG;
    if (adapter)
    {
        if (driverType != D3D_DRIVER_TYPE_UNKNOWN || software)
            return E_INVALIDARG;
    }
    else if (driverType != D3D_DRIVER_TYPE_HARDWARE || software)
    {
        return DXGI_ERROR_UNSUPPORTED;
    }

    InitOnceExecuteOnce(&d3d12InitOnce, initializeD3D12, nullptr, nullptr);
    if (!d3d12CreateDevice)
        return DXGI_ERROR_UNSUPPORTED;

    static const D3D_FEATURE_LEVEL defaults[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    const D3D_FEATURE_LEVEL *levels = featureLevels;
    UINT levelCount = featureLevelCount;
    if (!levels)
    {
        levels = defaults;
        levelCount = ARRAYSIZE(defaults);
    }

    ComRef<ID3D12Device> device12;
    HRESULT hr = DXGI_ERROR_UNSUPPORTED;
    for (UINT index = 0; index < levelCount; ++index)
    {
        hr = strictResult(d3d12CreateDevice(adapter, levels[index],
                IID_ID3D12Device,
                reinterpret_cast<void **>(device12.put())), device12);
        if (SUCCEEDED(hr))
            break;
    }
    if (FAILED(hr))
        return hr;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    ComRef<ID3D12CommandQueue> queue;
    hr = strictResult(device12.get()->CreateCommandQueue(&queueDesc,
            IID_ID3D12CommandQueue,
            reinterpret_cast<void **>(queue.put())), queue);
    if (FAILED(hr))
        return hr;

    IUnknown *queues[] = { queue.get() };
    return WineD3D11On12CreateDeviceV1(device12.get(), flags, levels,
            levelCount, queues, ARRAYSIZE(queues), 0, device11, context11,
            chosenFeatureLevel);
}

extern "C" HRESULT WINAPI WineD3D11CreateDeviceAndSwapChainV2(
        IDXGIAdapter *adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
        UINT flags, const D3D_FEATURE_LEVEL *featureLevels,
        UINT featureLevelCount, UINT sdkVersion,
        const DXGI_SWAP_CHAIN_DESC *swapChainDesc, IDXGISwapChain **swapChain,
        ID3D11Device **device11, D3D_FEATURE_LEVEL *chosenFeatureLevel,
        ID3D11DeviceContext **context11) noexcept
{
    if (swapChain)
        *swapChain = nullptr;
    clearDirectOutputs(device11, chosenFeatureLevel, context11);
    if (!swapChainDesc || !swapChain)
        return E_INVALIDARG;

    ComRef<ID3D11Device> device;
    ComRef<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selected = static_cast<D3D_FEATURE_LEVEL>(0);
    HRESULT hr = WineD3D11CreateDeviceV2(adapter, driverType, software, flags,
            featureLevels, featureLevelCount, sdkVersion, device.put(),
            &selected, context.put());
    if (FAILED(hr))
        return hr;

    ComRef<IDXGIDevice> dxgiDevice;
    hr = strictResult(device.get()->QueryInterface(IID_IDXGIDevice,
            reinterpret_cast<void **>(dxgiDevice.put())), dxgiDevice);
    if (FAILED(hr))
        return hr;

    ComRef<IDXGIAdapter> owningAdapter;
    hr = strictResult(dxgiDevice.get()->GetAdapter(owningAdapter.put()),
            owningAdapter);
    if (FAILED(hr))
        return hr;

    ComRef<IDXGIFactory> factory;
    hr = strictResult(owningAdapter.get()->GetParent(IID_IDXGIFactory,
            reinterpret_cast<void **>(factory.put())), factory);
    if (FAILED(hr))
        return hr;

    DXGI_SWAP_CHAIN_DESC mutableSwapChainDesc = *swapChainDesc;
    hr = factory.get()->CreateSwapChain(device.get(), &mutableSwapChainDesc,
            swapChain);
    if (FAILED(hr) || !*swapChain)
    {
        if (SUCCEEDED(hr))
            hr = E_NOINTERFACE;
        if (*swapChain)
        {
            (*swapChain)->Release();
            *swapChain = nullptr;
        }
        return hr;
    }

    if (chosenFeatureLevel)
        *chosenFeatureLevel = selected;
    if (device11)
        *device11 = device.detach();
    if (context11)
        *context11 = context.detach();
    return S_OK;
}
