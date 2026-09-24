/* SPDX-License-Identifier: GPL-3.0-only
 * D3D11On12 core ABI and input validation.
 */
#define WIN32_LEAN_AND_MEAN
#include <cstdio>
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

#include "d3d11on12core.h"
#include "wine_d3d11on12_shader.h"
#include "wine_d3d11_diag.h"
#include "../compat/relay_d3d12_struct_return.hpp"

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

/* The maximum number of elements in an input-layout declaration.
 *
 * The Windows SDK's d3d11.h defines this; mingw-w64's does not. Its d3d11.idl
 * publishes D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT (the input slot bound,
 * 32) but not the element-count bound, which is separately specified and also
 * 32. Guarded so that a toolchain which does supply it wins, and so that this
 * disappears on its own once mingw-w64 catches up.
 *
 * Specification: https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d11-graphics-reference-d3d11-constants */
#ifndef D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT
#define D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT 32u
#endif

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

/* Opt-in stage markers also identify a driver call that never returns. */
void traceCreation(const char *stage, HRESULT hr = S_OK) noexcept
{
    wchar_t enabled[2] = {};
    if (GetEnvironmentVariableW(L"RELAY12_TRACE_CREATION", enabled, 2) != 1
            || enabled[0] != L'1')
        return;
    char message[256];
    std::snprintf(message, sizeof(message), "d3d11on12core: %s (hr=0x%08lx)\n",
            stage, static_cast<unsigned long>(hr));
    wineD3D11DiagReport(message);
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

using HostCreateFn = HRESULT (WINAPI *)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *,
        UINT, IUnknown *const *, UINT, UINT, ID3D11Device **,
        ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);
INIT_ONCE hostInitOnce = INIT_ONCE_STATIC_INIT;
/* shared-state: published once through hostInitOnce */
HMODULE hostModule;
/* shared-state: published once through hostInitOnce */
HostCreateFn hostCreate;
BOOL CALLBACK initializeHost(PINIT_ONCE, PVOID, PVOID *) noexcept
{
    hostModule = LoadLibraryW(L"d3d11on12host.dll");
    if (hostModule)
    {
        FARPROC address = GetProcAddress(hostModule, "D3D11On12CreateDevice");
        memcpy(&hostCreate, &address, sizeof(hostCreate));
    }
    return TRUE;
}

struct AdapterState;
struct InputLayoutState;
struct ShaderState;
struct RenderTargetState;
struct ResourceState
{
    ResourceState *registryNext;
    ResourceState *ownerNext;
    AdapterState *owner;
    void *publicHandle;
    UINT kind;
    D3D11_TEXTURE2D_DESC description;
    BYTE *mapped;
    UINT viewCount;
    D3D11_RESOURCE_FLAGS flags;
    D3D10DDI_HRESOURCE driverHandle;
    D3D10DDI_HRTRESOURCE runtimeHandle;
    bool created;
    ID3D12Resource *wrappedResource;
    D3D12_RESOURCE_STATES inputState, outputState;
    bool acquired;
    alignas(8) unsigned char privateResource[1];
};

#define RESOURCE_KIND_BUFFER 1u
#define RESOURCE_KIND_TEXTURE2D 2u

INIT_ONCE resourceRegistryOnce = INIT_ONCE_STATIC_INIT;
/* shared-state: published once through resourceRegistryOnce */
SRWLOCK resourceRegistryLock;
/* shared-state: published once through resourceRegistryOnce */
ResourceState *resourceRegistry;
/* shared-state: published once through resourceRegistryOnce */
InputLayoutState *inputLayoutRegistry;
/* shared-state: published once through resourceRegistryOnce */
ShaderState *shaderRegistry;
/* shared-state: published once through resourceRegistryOnce */
RenderTargetState *renderTargetRegistry;

BOOL CALLBACK initializeResourceRegistry(PINIT_ONCE, PVOID, PVOID *) noexcept
{
    InitializeSRWLock(&resourceRegistryLock);
    resourceRegistry = nullptr;
    inputLayoutRegistry = nullptr;
    shaderRegistry = nullptr;
    return TRUE;
}

struct FrameLock
{
    FrameLock() noexcept
    {
        InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry, nullptr, nullptr);
        AcquireSRWLockExclusive(&resourceRegistryLock);
    }
    ~FrameLock() noexcept { ReleaseSRWLockExclusive(&resourceRegistryLock); }
};

/* The two callbacks the driver copies out of SOpenAdapterArgs and calls back
 * into. They are not reached during adapter or device creation, but the
 * driver keeps them for the lifetime of the adapter, so a null here would be
 * a crash later rather than an error now.
 *
 * Both answer conservatively and say so once. Resource sharing and the flags
 * a wrapped resource was created with are runtime state this milestone does
 * not track; reporting no flags and refusing the share is the answer that
 * makes a caller fail rather than proceed on an invented one. */
D3D11_RESOURCE_FLAGS CALLBACK hostGetResourceFlags(D3D10DDI_HRESOURCE resource,
        bool *acquireableOnWrite) noexcept
{
    traceCreation("enter resource flags callback");
    if (acquireableOnWrite)
        *acquireableOnWrite = false;
    D3D11_RESOURCE_FLAGS flags = {};

    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockShared(&resourceRegistryLock);
    for (ResourceState *entry = resourceRegistry; entry;
            entry = entry->registryNext)
    {
        if (entry->driverHandle.pDrvPrivate == resource.pDrvPrivate)
        {
            traceCreation("found resource flags");
            flags = entry->flags;
            ReleaseSRWLockShared(&resourceRegistryLock);
            traceCreation("leave resource flags callback");
            return flags;
        }
    }
    ReleaseSRWLockShared(&resourceRegistryLock);

    wineD3D11DiagReportOnce(&reportedResourceFlagsQuery,
            "d3d11on12core: the driver queried flags for an unknown resource; "
            "none are reported.\n");
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
    volatile LONG lastDdiError;
    ResourceState *resources;
    ResourceState *boundRenderTarget;
    HRESULT wrappedOwnershipError; // guarded by resourceRegistryLock
    InputLayoutState *inputLayouts;
    ShaderState *shaders;
    RenderTargetState *renderTargets;
    alignas(8) unsigned char privateDevice[1];
};

struct ShaderState
{
    ShaderState *registryNext;
    ShaderState *ownerNext;
    AdapterState *owner;
    WineD3D11On12Shader *publicHandle;
    UINT stage;
    D3D10DDI_HSHADER driverHandle;
    D3D10DDI_HRTSHADER runtimeHandle;
    bool created;
    alignas(8) unsigned char privateShader[1];
};

void unlinkShader(ShaderState *shader) noexcept
{
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    ShaderState **link = &shaderRegistry;
    while (*link && *link != shader)
        link = &(*link)->registryNext;
    if (*link)
        *link = shader->registryNext;
    ReleaseSRWLockExclusive(&resourceRegistryLock);
}

void unlinkOwnerShader(ShaderState *shader) noexcept
{
    ShaderState **link = &shader->owner->shaders;
    while (*link && *link != shader)
        link = &(*link)->ownerNext;
    if (*link)
        *link = shader->ownerNext;
}

void destroyShaderState(ShaderState *shader) noexcept
{
    if (!shader)
        return;
    AdapterState *owner = shader->owner;
    if (shader->created && owner && owner->deviceCreated
            && owner->deviceFuncs.pfnDestroyShader)
        owner->deviceFuncs.pfnDestroyShader(owner->hDevice,
                shader->driverHandle);
    unlinkShader(shader);
    if (shader->publicHandle)
    {
        shader->publicHandle->hDrvShader = nullptr;
        shader->publicHandle->runtimeState = nullptr;
    }
    HeapFree(GetProcessHeap(), 0, shader);
}

struct InputLayoutState
{
    InputLayoutState *registryNext;
    InputLayoutState *ownerNext;
    AdapterState *owner;
    WineD3D11On12InputLayout *publicHandle;
    D3D10DDI_HELEMENTLAYOUT driverHandle;
    D3D10DDI_HRTELEMENTLAYOUT runtimeHandle;
    bool created;
    alignas(8) unsigned char privateLayout[1];
};

void unlinkInputLayout(InputLayoutState *layout) noexcept
{
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    InputLayoutState **link = &inputLayoutRegistry;
    while (*link && *link != layout)
        link = &(*link)->registryNext;
    if (*link)
        *link = layout->registryNext;
    ReleaseSRWLockExclusive(&resourceRegistryLock);
}

void unlinkOwnerInputLayout(InputLayoutState *layout) noexcept
{
    InputLayoutState **link = &layout->owner->inputLayouts;
    while (*link && *link != layout)
        link = &(*link)->ownerNext;
    if (*link)
        *link = layout->ownerNext;
}

void destroyInputLayoutState(InputLayoutState *layout) noexcept
{
    if (!layout)
        return;
    AdapterState *owner = layout->owner;
    if (layout->created && owner && owner->deviceCreated
            && owner->deviceFuncs.pfnDestroyElementLayout)
        owner->deviceFuncs.pfnDestroyElementLayout(owner->hDevice,
                layout->driverHandle);
    unlinkInputLayout(layout);
    if (layout->publicHandle)
    {
        layout->publicHandle->hDrvElementLayout = nullptr;
        layout->publicHandle->runtimeState = nullptr;
    }
    HeapFree(GetProcessHeap(), 0, layout);
}

void unlinkResource(ResourceState *resource) noexcept
{
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    ResourceState **link = &resourceRegistry;
    while (*link && *link != resource)
        link = &(*link)->registryNext;
    if (*link)
        *link = resource->registryNext;
    ReleaseSRWLockExclusive(&resourceRegistryLock);
}

void unlinkOwnerResource(ResourceState *resource) noexcept
{
    AdapterState *owner = resource->owner;
    if (!owner)
        return;
    ResourceState **link = &owner->resources;
    while (*link && *link != resource)
        link = &(*link)->ownerNext;
    if (*link)
        *link = resource->ownerNext;
}

void destroyResourceState(ResourceState *resource) noexcept
{
    if (!resource)
        return;
    AdapterState *owner = resource->owner;
    if (resource->mapped && owner && owner->deviceCreated)
        for (UINT i = 0; i < resource->description.MipLevels * resource->description.ArraySize; ++i)
            if (resource->mapped[i] && owner->deviceFuncs.pfnStagingResourceUnmap)
                owner->deviceFuncs.pfnStagingResourceUnmap(owner->hDevice, resource->driverHandle, i);
    if (resource->created && owner && owner->deviceCreated
            && owner->deviceFuncs.pfnDestroyResource)
        owner->deviceFuncs.pfnDestroyResource(owner->hDevice,
                resource->driverHandle);
    {
        FrameLock lock;
        if (owner && owner->boundRenderTarget == resource) owner->boundRenderTarget = nullptr;
    }
    unlinkResource(resource);
    if (resource->publicHandle)
    {
        WineD3D11On12Buffer *handle = static_cast<WineD3D11On12Buffer *>(
                resource->publicHandle);
        handle->hDrvResource = nullptr;
        handle->runtimeState = nullptr;
    }
    if (resource->wrappedResource) resource->wrappedResource->Release();
    if (resource->mapped) HeapFree(GetProcessHeap(), 0, resource->mapped);
    HeapFree(GetProcessHeap(), 0, resource);
}

void CALLBACK hostSetError(D3D10DDI_HRTCORELAYER runtimeDevice,
        HRESULT result) noexcept
{
    AdapterState *state = static_cast<AdapterState *>(runtimeDevice.handle);

    if (state)
        InterlockedExchange(&state->lastDdiError, result);
}

void CALLBACK hostPerformAmortizedProcessing(D3D10DDI_HRTCORELAYER runtimeDevice) noexcept
{
    (void)runtimeDevice;
}

void destroyRenderTargetState(RenderTargetState *view) noexcept;
void destroyAllRenderTargets(AdapterState *owner) noexcept;

void destroyAdapterState(AdapterState *state) noexcept
{
    if (!state)
        return;

    destroyAllRenderTargets(state);
    while (state->inputLayouts)
    {
        InputLayoutState *layout = state->inputLayouts;
        state->inputLayouts = layout->ownerNext;
        destroyInputLayoutState(layout);
    }
    while (state->shaders)
    {
        ShaderState *shader = state->shaders;
        state->shaders = shader->ownerNext;
        destroyShaderState(shader);
    }
    while (state->resources)
    {
        ResourceState *resource = state->resources;
        state->resources = resource->ownerNext;
        destroyResourceState(resource);
    }
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
    createDevice.hRTCoreLayer.handle = state;
    state->coreCallbacks.pfnSetErrorCb = hostSetError;
    state->coreCallbacks.pfnPerformAmortizedProcessingCb = hostPerformAmortizedProcessing;

    state->hDevice.pDrvPrivate = state->privateDevice;
    createDevice.hDrvDevice = state->hDevice;

    traceCreation("enter driver CreateDevice");
    hr = state->adapterFuncs.pfnCreateDevice(state->hAdapter, &createDevice);
    traceCreation("leave driver CreateDevice", hr);
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
            | WINE_D3D11ON12_CAP_IMMEDIATE_CONTEXT_FLUSH
            | WINE_D3D11ON12_CAP_IMMEDIATE_CONTEXT_DRAW
            | WINE_D3D11ON12_CAP_WRAPPED_RESOURCE_VALIDATION
            | WINE_D3D11ON12_CAP_INDEXED_INSTANCED_DRAW
            | WINE_D3D11ON12_CAP_INPUT_ASSEMBLER_TOPOLOGY;
    interfaceOut->createDevice = WineD3D11On12CreateDeviceV1;
    interfaceOut->createDirectDevice = WineD3D11CreateDeviceV2;
    interfaceOut->createDirectDeviceAndSwapChain =
            WineD3D11CreateDeviceAndSwapChainV2;
    interfaceOut->closeAdapterDevice = WineD3D11On12CloseAdapterDeviceV1;
    interfaceOut->flushAdapterDevice = WineD3D11On12FlushAdapterDeviceV1;
    interfaceOut->drawAdapterDevice = WineD3D11On12DrawAdapterDeviceV1;
    interfaceOut->validateWrappedResource =
            WineD3D11On12ValidateWrappedResourceV1;
    interfaceOut->dispatchDraw = WineD3D11On12DispatchDrawV1;
    interfaceOut->setPrimitiveTopology =
            WineD3D11On12SetPrimitiveTopologyV1;
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
    traceCreation("validate caller device and queue", hr);
    if (FAILED(hr))
        return hr;

    if (featureLevelCount)
    {
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
        levels.NumFeatureLevels = featureLevelCount;
        levels.pFeatureLevelsRequested = featureLevels;
        hr = device12.get()->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,
                &levels, sizeof(levels));
        traceCreation("check requested feature levels", hr);
        if (FAILED(hr) || !levels.MaxSupportedFeatureLevel)
            return FAILED(hr) ? hr : E_INVALIDARG;
    }

    /* Publication is experimental until the hardware acceptance run is green.
     * The host opens the adapter exactly once and owns its lifetime. Never
     * call the public d3d11.dll export here: that would recurse into us. */
    wchar_t enabled[2] = {};
    if ((flags & ~D3D11_CREATE_DEVICE_BGRA_SUPPORT) || GetEnvironmentVariableW(L"RELAY12_EXPERIMENTAL_FRAME", enabled, 2) != 1
            || enabled[0] != L'1')
    {
        wineD3D11DiagReportOnce(&reportedNoHost,
                "d3d11on12core: experimental frame publication is disabled.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }
    traceCreation("load runtime host");
    InitOnceExecuteOnce(&hostInitOnce, initializeHost, nullptr, nullptr);
    if (!hostCreate)
    {
        wineD3D11DiagReportOnce(&reportedNoHost,
                "d3d11on12core: no compatible D3D11 runtime host is installed.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }
    ComRef<ID3D11Device> createdDevice;
    ComRef<ID3D11DeviceContext> createdContext;
    D3D_FEATURE_LEVEL selected = static_cast<D3D_FEATURE_LEVEL>(0);
    traceCreation("enter runtime host creation");
    hr = hostCreate(deviceObject, flags, featureLevels, featureLevelCount,
            queueObjects, queueCount, nodeMask, createdDevice.put(), createdContext.put(), &selected);
    traceCreation("leave runtime host creation", hr);
    if (FAILED(hr)) return hr;
    if (!createdDevice.get() || !createdContext.get() || !selected)
        return DXGI_ERROR_UNSUPPORTED;
    if (device11) *device11 = createdDevice.detach();
    if (context11) *context11 = createdContext.detach();
    if (chosenFeatureLevel) *chosenFeatureLevel = selected;
    return S_OK;
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

    traceCreation("enter driver OpenAdapter");
    hr = driverOpenAdapter(&openAdapter, &driverArgs);
    traceCreation("leave driver OpenAdapter", hr);
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

extern "C" HRESULT WINAPI WineD3D11On12DrawAdapterDeviceV1(
        WineD3D11On12AdapterDevice *adapterDevice, UINT vertexCount,
        UINT startVertexLocation) noexcept
{
    FrameLock lock;
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice))
        return E_INVALIDARG;

    AdapterState *state = static_cast<AdapterState *>(
            adapterDevice->runtimeState);
    if (!state || !state->deviceCreated || !state->deviceFuncs.pfnDraw)
        return DXGI_ERROR_UNSUPPORTED;

    if (state->boundRenderTarget && state->boundRenderTarget->wrappedResource
            && (FAILED(state->wrappedOwnershipError) || !state->boundRenderTarget->acquired))
        return FAILED(state->wrappedOwnershipError) ? state->wrappedOwnershipError : E_INVALIDARG;
    state->deviceFuncs.pfnDraw(state->hDevice, vertexCount,
            startVertexLocation);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12ValidateWrappedResourceV1(
        WineD3D11On12AdapterDevice *adapterDevice,
        IUnknown *resourceObject) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice)
            || !resourceObject)
        return E_INVALIDARG;

    AdapterState *state = static_cast<AdapterState *>(
            adapterDevice->runtimeState);
    if (!state || !state->deviceCreated || !state->device12)
        return DXGI_ERROR_UNSUPPORTED;

    ComRef<ID3D12Resource> resource;
    HRESULT hr = strictResult(resourceObject->QueryInterface(IID_ID3D12Resource,
            reinterpret_cast<void **>(resource.put())), resource);
    if (FAILED(hr))
        return hr;

    ComRef<ID3D12Device> resourceDevice;
    hr = strictResult(resource.get()->GetDevice(IID_ID3D12Device,
            reinterpret_cast<void **>(resourceDevice.put())), resourceDevice);
    if (FAILED(hr))
        return hr;

    bool identical = false;
    hr = comObjectsIdentical(state->device12, resourceDevice.get(),
            &identical);
    if (FAILED(hr))
        return hr;
    return identical ? S_OK : E_INVALIDARG;
}

extern "C" HRESULT WINAPI WineD3D11On12DispatchDrawV1(
        WineD3D11On12AdapterDevice *adapterDevice, UINT kind, UINT count0,
        UINT count1, UINT start0, INT baseVertex, UINT startInstance) noexcept
{
    FrameLock lock;
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice))
        return E_INVALIDARG;

    AdapterState *state = static_cast<AdapterState *>(
            adapterDevice->runtimeState);
    if (!state || !state->deviceCreated)
        return DXGI_ERROR_UNSUPPORTED;

    if (state->boundRenderTarget && state->boundRenderTarget->wrappedResource
            && (FAILED(state->wrappedOwnershipError) || !state->boundRenderTarget->acquired))
        return FAILED(state->wrappedOwnershipError) ? state->wrappedOwnershipError : E_INVALIDARG;
    switch (kind)
    {
        case WINE_D3D11ON12_DRAW_INDEXED:
            if (!state->deviceFuncs.pfnDrawIndexed)
                return DXGI_ERROR_UNSUPPORTED;
            state->deviceFuncs.pfnDrawIndexed(state->hDevice, count0, start0,
                    baseVertex);
            return S_OK;

        case WINE_D3D11ON12_DRAW_INSTANCED:
            if (!state->deviceFuncs.pfnDrawInstanced)
                return DXGI_ERROR_UNSUPPORTED;
            state->deviceFuncs.pfnDrawInstanced(state->hDevice, count0,
                    count1, start0, startInstance);
            return S_OK;

        case WINE_D3D11ON12_DRAW_INDEXED_INSTANCED:
            if (!state->deviceFuncs.pfnDrawIndexedInstanced)
                return DXGI_ERROR_UNSUPPORTED;
            state->deviceFuncs.pfnDrawIndexedInstanced(state->hDevice,
                    count0, count1, start0, baseVertex, startInstance);
            return S_OK;

        default:
            return E_INVALIDARG;
    }
}

extern "C" HRESULT WINAPI WineD3D11On12SetPrimitiveTopologyV1(
        WineD3D11On12AdapterDevice *adapterDevice, INT topology) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice))
        return E_INVALIDARG;

    AdapterState *state = static_cast<AdapterState *>(
            adapterDevice->runtimeState);
    if (!state || !state->deviceCreated
            || !state->deviceFuncs.pfnIaSetTopology)
        return DXGI_ERROR_UNSUPPORTED;

    state->deviceFuncs.pfnIaSetTopology(state->hDevice, topology);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12CreateBufferV1(
        WineD3D11On12AdapterDevice *adapterDevice,
        const D3D11_BUFFER_DESC *description,
        const D3D11_SUBRESOURCE_DATA *initialData,
        WineD3D11On12Buffer *buffer) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice)
            || !description || !description->ByteWidth || !buffer
            || buffer->size != sizeof(*buffer))
        return E_INVALIDARG;
    buffer->reserved = 0;
    buffer->hDrvResource = nullptr;
    buffer->runtimeState = nullptr;

    AdapterState *owner = static_cast<AdapterState *>(
            adapterDevice->runtimeState);
    if (!owner || !owner->deviceCreated
            || !owner->deviceFuncs.pfnCalcPrivateResourceSize
            || !owner->deviceFuncs.pfnCreateResource
            || !owner->deviceFuncs.pfnDestroyResource)
        return DXGI_ERROR_UNSUPPORTED;

    D3D10DDI_MIPINFO mip = {description->ByteWidth, 1, 1,
            description->ByteWidth, 1, 1};
    D3D10_DDIARG_SUBRESOURCE_UP upload = {};
    if (initialData)
    {
        if (!initialData->pSysMem)
            return E_INVALIDARG;
        upload.pSysMem = initialData->pSysMem;
        upload.SysMemPitch = initialData->SysMemPitch;
        upload.SysMemSlicePitch = initialData->SysMemSlicePitch;
    }

    D3D11DDIARG_CREATERESOURCE args = {};
    args.pMipInfoList = &mip;
    args.pInitialDataUP = initialData ? &upload : nullptr;
    args.ResourceDimension = D3D10DDIRESOURCE_BUFFER;
    args.Usage = description->Usage;
    args.BindFlags = description->BindFlags;
    args.MapFlags = description->CPUAccessFlags;
    args.MiscFlags = description->MiscFlags;
    args.Format = DXGI_FORMAT_UNKNOWN;
    args.SampleDesc.Count = 1;
    args.MipLevels = 1;
    args.ArraySize = 1;
    args.ByteStride = description->StructureByteStride;

    traceCreation("calculate buffer storage");
    SIZE_T privateSize = owner->deviceFuncs.pfnCalcPrivateResourceSize(
            owner->hDevice, &args);
    if (!privateSize || privateSize > ~static_cast<SIZE_T>(0)
            - offsetof(ResourceState, privateResource))
        return E_FAIL;
    ResourceState *resource = static_cast<ResourceState *>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            offsetof(ResourceState, privateResource) + privateSize));
    if (!resource)
        return E_OUTOFMEMORY;

    resource->owner = owner;
    resource->publicHandle = buffer;
    resource->kind = RESOURCE_KIND_BUFFER;
    resource->flags.BindFlags = description->BindFlags;
    resource->flags.MiscFlags = description->MiscFlags;
    resource->flags.CPUAccessFlags = description->CPUAccessFlags;
    resource->flags.StructureByteStride = description->StructureByteStride;
    resource->driverHandle.pDrvPrivate = resource->privateResource;
    resource->runtimeHandle.handle = resource;

    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    resource->registryNext = resourceRegistry;
    resourceRegistry = resource;
    resource->ownerNext = owner->resources;
    owner->resources = resource;
    ReleaseSRWLockExclusive(&resourceRegistryLock);

    InterlockedExchange(&owner->lastDdiError, S_OK);
    traceCreation("enter driver buffer creation");
    owner->deviceFuncs.pfnCreateResource(owner->hDevice, &args,
            resource->driverHandle, resource->runtimeHandle);
    traceCreation("leave driver buffer creation");
    HRESULT hr = InterlockedCompareExchange(&owner->lastDdiError, S_OK, S_OK);
    if (FAILED(hr))
    {
        AcquireSRWLockExclusive(&resourceRegistryLock);
        unlinkOwnerResource(resource);
        ReleaseSRWLockExclusive(&resourceRegistryLock);
        unlinkResource(resource);
        HeapFree(GetProcessHeap(), 0, resource);
        return hr;
    }

    resource->created = true;
    buffer->hDrvResource = resource->driverHandle.pDrvPrivate;
    buffer->runtimeState = resource;
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12DestroyBufferV1(
        WineD3D11On12Buffer *buffer) noexcept
{
    if (!buffer || buffer->size != sizeof(*buffer))
        return E_INVALIDARG;
    ResourceState *resource = static_cast<ResourceState *>(
            buffer->runtimeState);
    if (!resource)
    {
        buffer->hDrvResource = nullptr;
        return S_OK;
    }

    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    unlinkOwnerResource(resource);
    ReleaseSRWLockExclusive(&resourceRegistryLock);
    destroyResourceState(resource);
    return S_OK;
}

/* Resolve a public buffer without trusting its runtimeState pointer.  Public
 * handles cross the frontend/core DLL boundary and may be stale or belong to
 * another device, so membership and ownership are established while the
 * registry lock prevents concurrent destruction. */
ResourceState *findBufferLocked(AdapterState *owner,
        WineD3D11On12Buffer *buffer, UINT requiredBindFlag) noexcept
{
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    if (!buffer || buffer->size != sizeof(*buffer) || !buffer->runtimeState
            || !buffer->hDrvResource)
        return nullptr;

    for (ResourceState *resource = resourceRegistry; resource;
            resource = resource->registryNext)
    {
        if (resource == buffer->runtimeState && resource->owner == owner
                && resource->publicHandle == buffer && resource->created
                && resource->kind == RESOURCE_KIND_BUFFER
                && resource->driverHandle.pDrvPrivate == buffer->hDrvResource
                && (resource->flags.BindFlags & requiredBindFlag))
            return resource;
    }
    return nullptr;
}

extern "C" HRESULT WINAPI WineD3D11On12CreateTexture2DV1(
        WineD3D11On12AdapterDevice *adapterDevice,
        const D3D11_TEXTURE2D_DESC *description,
        const D3D11_SUBRESOURCE_DATA *initialData,
        WineD3D11On12Texture2D *texture) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice)
            || !description || !description->Width || !description->Height
            || !description->ArraySize || !description->SampleDesc.Count
            || description->Format == DXGI_FORMAT_UNKNOWN || !texture
            || texture->size != sizeof(*texture))
        return E_INVALIDARG;
    texture->reserved = 0;
    texture->hDrvResource = nullptr;
    texture->runtimeState = nullptr;
    AdapterState *owner = static_cast<AdapterState *>(adapterDevice->runtimeState);
    if (!owner || !owner->deviceCreated
            || !owner->deviceFuncs.pfnCalcPrivateResourceSize
            || !owner->deviceFuncs.pfnCreateResource
            || !owner->deviceFuncs.pfnDestroyResource)
        return DXGI_ERROR_UNSUPPORTED;

    UINT fullMipLevels = 1;
    for (UINT extent = description->Width > description->Height
            ? description->Width : description->Height; extent > 1; extent >>= 1)
        ++fullMipLevels;
    const UINT mipLevels = description->MipLevels
            ? description->MipLevels : fullMipLevels;
    if (mipLevels > fullMipLevels || description->ArraySize > ~0u / mipLevels)
        return E_INVALIDARG;
    const UINT subresourceCount = mipLevels * description->ArraySize;

    /* Asserted rather than tested. subresourceCount is a UINT, so the byte
     * counts below are at most UINT_MAX times a small element, which cannot
     * overflow a 64-bit SIZE_T -- Clang rightly rejects the runtime form as
     * a comparison that is always false. The bound is still checked, just at
     * compile time, so a 32-bit target would fail to build here instead of
     * silently losing the guard. The UINT overflow that *can* happen is
     * mipLevels * ArraySize, and that is tested above. */
    static_assert((~static_cast<SIZE_T>(0)) / sizeof(D3D10DDI_MIPINFO)
            >= 0xffffffffu, "a UINT subresource count must not overflow the "
            "mip-info byte count");
    static_assert((~static_cast<SIZE_T>(0))
            / sizeof(D3D10_DDIARG_SUBRESOURCE_UP) >= 0xffffffffu,
            "a UINT subresource count must not overflow the upload byte "
            "count");
    D3D10DDI_MIPINFO *mips = static_cast<D3D10DDI_MIPINFO *>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            subresourceCount * sizeof(*mips)));
    D3D10_DDIARG_SUBRESOURCE_UP *uploads = initialData
            ? static_cast<D3D10_DDIARG_SUBRESOURCE_UP *>(HeapAlloc(
                    GetProcessHeap(), HEAP_ZERO_MEMORY,
                    subresourceCount * sizeof(*uploads))) : nullptr;
    if (!mips || (initialData && !uploads))
    {
        if (uploads) HeapFree(GetProcessHeap(), 0, uploads);
        if (mips) HeapFree(GetProcessHeap(), 0, mips);
        return E_OUTOFMEMORY;
    }
    for (UINT array = 0; array < description->ArraySize; ++array)
        for (UINT mip = 0; mip < mipLevels; ++mip)
        {
            const UINT index = array * mipLevels + mip;
            const UINT width = description->Width >> mip
                    ? description->Width >> mip : 1;
            const UINT height = description->Height >> mip
                    ? description->Height >> mip : 1;
            mips[index] = {width, height, 1, width, height, 1};
            if (initialData)
            {
                if (!initialData[index].pSysMem)
                {
                    HeapFree(GetProcessHeap(), 0, uploads);
                    HeapFree(GetProcessHeap(), 0, mips);
                    return E_INVALIDARG;
                }
                uploads[index].pSysMem = initialData[index].pSysMem;
                uploads[index].SysMemPitch = initialData[index].SysMemPitch;
                uploads[index].SysMemSlicePitch = initialData[index].SysMemSlicePitch;
            }
        }
    D3D11DDIARG_CREATERESOURCE args = {};
    args.pMipInfoList = mips;
    args.pInitialDataUP = uploads;
    args.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
    args.Usage = description->Usage;
    args.BindFlags = description->BindFlags;
    args.MapFlags = description->CPUAccessFlags;
    args.MiscFlags = description->MiscFlags;
    args.Format = description->Format;
    args.SampleDesc = description->SampleDesc;
    args.MipLevels = mipLevels;
    args.ArraySize = description->ArraySize;
    const SIZE_T privateSize = owner->deviceFuncs.pfnCalcPrivateResourceSize(
            owner->hDevice, &args);
    if (!privateSize || privateSize > ~static_cast<SIZE_T>(0)
            - offsetof(ResourceState, privateResource))
    {
        if (uploads) HeapFree(GetProcessHeap(), 0, uploads);
        HeapFree(GetProcessHeap(), 0, mips);
        return E_FAIL;
    }
    ResourceState *resource = static_cast<ResourceState *>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            offsetof(ResourceState, privateResource) + privateSize));
    if (!resource)
    {
        if (uploads) HeapFree(GetProcessHeap(), 0, uploads);
        HeapFree(GetProcessHeap(), 0, mips);
        return E_OUTOFMEMORY;
    }
    resource->owner = owner;
    resource->publicHandle = texture;
    resource->kind = RESOURCE_KIND_TEXTURE2D;
    resource->description = *description;
    resource->description.MipLevels = mipLevels;
    resource->mapped = static_cast<BYTE *>(HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, subresourceCount));
    if (!resource->mapped)
    {
        if (uploads) HeapFree(GetProcessHeap(), 0, uploads);
        HeapFree(GetProcessHeap(), 0, mips);
        HeapFree(GetProcessHeap(), 0, resource);
        return E_OUTOFMEMORY;
    }
    resource->flags.BindFlags = description->BindFlags;
    resource->flags.MiscFlags = description->MiscFlags;
    resource->flags.CPUAccessFlags = description->CPUAccessFlags;
    resource->driverHandle.pDrvPrivate = resource->privateResource;
    resource->runtimeHandle.handle = resource;
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    resource->registryNext = resourceRegistry;
    resourceRegistry = resource;
    resource->ownerNext = owner->resources;
    owner->resources = resource;
    ReleaseSRWLockExclusive(&resourceRegistryLock);
    InterlockedExchange(&owner->lastDdiError, S_OK);
    owner->deviceFuncs.pfnCreateResource(owner->hDevice, &args,
            resource->driverHandle, resource->runtimeHandle);
    if (uploads) HeapFree(GetProcessHeap(), 0, uploads);
    HeapFree(GetProcessHeap(), 0, mips);
    const HRESULT hr = InterlockedCompareExchange(&owner->lastDdiError,
            S_OK, S_OK);
    if (FAILED(hr))
    {
        AcquireSRWLockExclusive(&resourceRegistryLock);
        unlinkOwnerResource(resource);
        ReleaseSRWLockExclusive(&resourceRegistryLock);
        unlinkResource(resource);
        HeapFree(GetProcessHeap(), 0, resource->mapped);
        HeapFree(GetProcessHeap(), 0, resource);
        return hr;
    }
    resource->created = true;
    texture->hDrvResource = resource->driverHandle.pDrvPrivate;
    texture->runtimeState = resource;
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12DestroyTexture2DV1(
        WineD3D11On12Texture2D *texture) noexcept
{
    if (!texture || texture->size != sizeof(*texture))
        return E_INVALIDARG;
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry, nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    ResourceState *resource = nullptr;
    for (ResourceState *entry = resourceRegistry; entry; entry = entry->registryNext)
        if (entry == texture->runtimeState && entry->publicHandle == texture
                && entry->kind == RESOURCE_KIND_TEXTURE2D
                && entry->driverHandle.pDrvPrivate == texture->hDrvResource)
        {
            for (UINT i = 0; i < entry->description.MipLevels * entry->description.ArraySize; ++i)
                if (entry->mapped[i])
                {
                    ReleaseSRWLockExclusive(&resourceRegistryLock);
                    return DXGI_ERROR_INVALID_CALL;
                }
            entry->publicHandle = nullptr;
            if (!entry->viewCount)
            {
                resource = entry;
                unlinkOwnerResource(entry);
            }
            break;
        }
    texture->hDrvResource = nullptr;
    texture->runtimeState = nullptr;
    ReleaseSRWLockExclusive(&resourceRegistryLock);
    if (resource) destroyResourceState(resource);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12SetVertexBuffersV1(
        WineD3D11On12AdapterDevice *adapterDevice, UINT startSlot,
        UINT bufferCount, WineD3D11On12Buffer *const *buffers,
        const UINT *strides, const UINT *offsets) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice)
            || startSlot > D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
            || bufferCount > D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
                    - startSlot
            || (bufferCount && (!buffers || !strides || !offsets)))
        return E_INVALIDARG;

    AdapterState *owner = static_cast<AdapterState *>(
            adapterDevice->runtimeState);
    if (!owner || !owner->deviceCreated
            || !owner->deviceFuncs.pfnIaSetVertexBuffers)
        return DXGI_ERROR_UNSUPPORTED;

    D3D10DDI_HRESOURCE handles[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockShared(&resourceRegistryLock);
    for (UINT i = 0; i < bufferCount; ++i)
    {
        if (!buffers[i])
            continue;
        ResourceState *resource = findBufferLocked(owner, buffers[i],
                D3D11_BIND_VERTEX_BUFFER);
        if (!resource)
        {
            ReleaseSRWLockShared(&resourceRegistryLock);
            return E_INVALIDARG;
        }
        handles[i] = resource->driverHandle;
    }
    owner->deviceFuncs.pfnIaSetVertexBuffers(owner->hDevice, startSlot,
            bufferCount, handles, strides, offsets);
    ReleaseSRWLockShared(&resourceRegistryLock);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12SetIndexBufferV1(
        WineD3D11On12AdapterDevice *adapterDevice,
        WineD3D11On12Buffer *buffer, DXGI_FORMAT format, UINT offset) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice)
            || (buffer && format != DXGI_FORMAT_R16_UINT
                    && format != DXGI_FORMAT_R32_UINT)
            || (!buffer && format != DXGI_FORMAT_UNKNOWN))
        return E_INVALIDARG;

    AdapterState *owner = static_cast<AdapterState *>(
            adapterDevice->runtimeState);
    if (!owner || !owner->deviceCreated
            || !owner->deviceFuncs.pfnIaSetIndexBuffer)
        return DXGI_ERROR_UNSUPPORTED;

    D3D10DDI_HRESOURCE handle = {};
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockShared(&resourceRegistryLock);
    if (buffer)
    {
        ResourceState *resource = findBufferLocked(owner, buffer,
                D3D11_BIND_INDEX_BUFFER);
        if (!resource)
        {
            ReleaseSRWLockShared(&resourceRegistryLock);
            return E_INVALIDARG;
        }
        handle = resource->driverHandle;
    }
    owner->deviceFuncs.pfnIaSetIndexBuffer(owner->hDevice, handle, format,
            offset);
    ReleaseSRWLockShared(&resourceRegistryLock);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12CreateInputLayoutV1(
        WineD3D11On12AdapterDevice *adapterDevice,
        const D3D11_INPUT_ELEMENT_DESC *elements, const UINT *registers,
        UINT elementCount, WineD3D11On12InputLayout *layout) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice)
            || !elements || !registers || !elementCount
            || elementCount > D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT
            || !layout || layout->size != sizeof(*layout))
        return E_INVALIDARG;
    layout->reserved = 0;
    layout->hDrvElementLayout = nullptr;
    layout->runtimeState = nullptr;

    AdapterState *owner = static_cast<AdapterState *>(adapterDevice->runtimeState);
    if (!owner || !owner->deviceCreated
            || !owner->deviceFuncs.pfnCalcPrivateElementLayoutSize
            || !owner->deviceFuncs.pfnCreateElementLayout
            || !owner->deviceFuncs.pfnDestroyElementLayout)
        return DXGI_ERROR_UNSUPPORTED;

    D3D10DDIARG_INPUT_ELEMENT_DESC ddiElements[
            D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT] = {};
    for (UINT i = 0; i < elementCount; ++i)
    {
        if (elements[i].InputSlot >= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
                || elements[i].InputSlotClass > D3D11_INPUT_PER_INSTANCE_DATA)
            return E_INVALIDARG;
        ddiElements[i].InputSlot = elements[i].InputSlot;
        ddiElements[i].AlignedByteOffset = elements[i].AlignedByteOffset;
        ddiElements[i].Format = elements[i].Format;
        ddiElements[i].InputSlotClass = elements[i].InputSlotClass;
        ddiElements[i].InstanceDataStepRate = elements[i].InstanceDataStepRate;
        ddiElements[i].InputRegister = registers[i];
    }
    D3D10DDIARG_CREATEELEMENTLAYOUT args = {};
    args.pVertexElements = ddiElements;
    args.NumElements = elementCount;
    const SIZE_T privateSize = owner->deviceFuncs.pfnCalcPrivateElementLayoutSize(
            owner->hDevice, &args);
    if (!privateSize || privateSize > ~static_cast<SIZE_T>(0)
            - offsetof(InputLayoutState, privateLayout))
        return E_FAIL;
    InputLayoutState *state = static_cast<InputLayoutState *>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            offsetof(InputLayoutState, privateLayout) + privateSize));
    if (!state)
        return E_OUTOFMEMORY;
    state->owner = owner;
    state->publicHandle = layout;
    state->driverHandle.pDrvPrivate = state->privateLayout;
    state->runtimeHandle.handle = state;

    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    state->registryNext = inputLayoutRegistry;
    inputLayoutRegistry = state;
    state->ownerNext = owner->inputLayouts;
    owner->inputLayouts = state;
    ReleaseSRWLockExclusive(&resourceRegistryLock);

    InterlockedExchange(&owner->lastDdiError, S_OK);
    owner->deviceFuncs.pfnCreateElementLayout(owner->hDevice, &args,
            state->driverHandle, state->runtimeHandle);
    HRESULT hr = InterlockedCompareExchange(&owner->lastDdiError, S_OK, S_OK);
    if (FAILED(hr))
    {
        AcquireSRWLockExclusive(&resourceRegistryLock);
        unlinkOwnerInputLayout(state);
        ReleaseSRWLockExclusive(&resourceRegistryLock);
        unlinkInputLayout(state);
        HeapFree(GetProcessHeap(), 0, state);
        return hr;
    }
    state->created = true;
    layout->hDrvElementLayout = state->driverHandle.pDrvPrivate;
    layout->runtimeState = state;
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12DestroyInputLayoutV1(
        WineD3D11On12InputLayout *layout) noexcept
{
    if (!layout || layout->size != sizeof(*layout))
        return E_INVALIDARG;
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    InputLayoutState *state = nullptr;
    InputLayoutState **registryLink = &inputLayoutRegistry;
    for (InputLayoutState *entry = inputLayoutRegistry; entry;
            registryLink = &entry->registryNext, entry = entry->registryNext)
        if (entry == layout->runtimeState && entry->publicHandle == layout
                && entry->driverHandle.pDrvPrivate == layout->hDrvElementLayout)
        {
            state = entry;
            unlinkOwnerInputLayout(entry);
            *registryLink = entry->registryNext;
            layout->hDrvElementLayout = nullptr;
            layout->runtimeState = nullptr;
            break;
        }
    ReleaseSRWLockExclusive(&resourceRegistryLock);
    if (!state)
    {
        layout->hDrvElementLayout = nullptr;
        layout->runtimeState = nullptr;
        return S_OK;
    }
    /* Removing the object under the exclusive registry lock waits for every
     * in-flight binding (which holds the shared lock) and prevents any new
     * one from finding it before the driver sees DestroyElementLayout. */
    if (state->created && state->owner && state->owner->deviceCreated
            && state->owner->deviceFuncs.pfnDestroyElementLayout)
        state->owner->deviceFuncs.pfnDestroyElementLayout(
                state->owner->hDevice, state->driverHandle);
    HeapFree(GetProcessHeap(), 0, state);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12SetInputLayoutV1(
        WineD3D11On12AdapterDevice *adapterDevice,
        WineD3D11On12InputLayout *layout) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice))
        return E_INVALIDARG;
    AdapterState *owner = static_cast<AdapterState *>(adapterDevice->runtimeState);
    if (!owner || !owner->deviceCreated || !owner->deviceFuncs.pfnIaSetInputLayout)
        return DXGI_ERROR_UNSUPPORTED;
    D3D10DDI_HELEMENTLAYOUT handle = {};
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockShared(&resourceRegistryLock);
    if (layout)
    {
        InputLayoutState *found = nullptr;
        for (InputLayoutState *entry = inputLayoutRegistry; entry;
                entry = entry->registryNext)
            if (entry == layout->runtimeState && entry->owner == owner
                    && entry->publicHandle == layout && entry->created
                    && entry->driverHandle.pDrvPrivate == layout->hDrvElementLayout)
            {
                found = entry;
                break;
            }
        if (!found)
        {
            ReleaseSRWLockShared(&resourceRegistryLock);
            return E_INVALIDARG;
        }
        handle = found->driverHandle;
    }
    owner->deviceFuncs.pfnIaSetInputLayout(owner->hDevice, handle);
    ReleaseSRWLockShared(&resourceRegistryLock);
    return S_OK;
}

HRESULT createBasicShader(WineD3D11On12AdapterDevice *adapterDevice,
        const void *byteCode, SIZE_T byteCodeLength,
        WineD3D11On12Shader *shader, UINT stage) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice)
            || !byteCode || byteCodeLength < 32 || byteCodeLength % sizeof(UINT)
            || !shader
            || shader->size != sizeof(*shader))
        return E_INVALIDARG;
    shader->stage = 0;
    shader->hDrvShader = nullptr;
    shader->runtimeState = nullptr;
    AdapterState *owner = static_cast<AdapterState *>(adapterDevice->runtimeState);
    if (!owner || !owner->deviceCreated || byteCodeLength > UINT_MAX
            || !owner->deviceFuncs.pfnCalcPrivateShaderSize
            || !owner->deviceFuncs.pfnDestroyShader)
        return DXGI_ERROR_UNSUPPORTED;
    /* The MIT interface's CastFrom uses this exact address. It is not COM:
     * there is no QueryInterface, AddRef or Release on the DDI sub-object. */
    auto *ddiDevice = static_cast<WineD3D11On12DDIDevice *>(owner->hDevice.pDrvPrivate);
    if (!ddiDevice || !ddiDevice->lpVtbl)
        return DXGI_ERROR_UNSUPPORTED;
    auto createShader = stage == WINE_D3D11ON12_SHADER_VERTEX
            ? ddiDevice->lpVtbl->CreateVertexShader
            : ddiDevice->lpVtbl->CreatePixelShader;
    if (!createShader)
        return DXGI_ERROR_UNSUPPORTED;

    const unsigned char *bytes = static_cast<const unsigned char *>(byteCode);
    UINT magic, containerSize, chunkCount;
    memcpy(&magic, bytes, sizeof(magic));
    memcpy(&containerSize, bytes + 24, sizeof(containerSize));
    memcpy(&chunkCount, bytes + 28, sizeof(chunkCount));
    if (magic != 0x43425844 || containerSize != byteCodeLength
            || chunkCount > (byteCodeLength - 32) / sizeof(UINT))
        return E_INVALIDARG;
    /* Sizing takes driver tokens, creation takes a DXBC container. The pinned
     * driver's sizing callback ignores both arguments and returns sizeof;
     * never pass container bytes as a token stream. */
    const SIZE_T privateSize = owner->deviceFuncs.pfnCalcPrivateShaderSize(
            owner->hDevice, nullptr, nullptr);
    if (!privateSize || privateSize > ~static_cast<SIZE_T>(0)
            - offsetof(ShaderState, privateShader))
        return E_FAIL;
    ShaderState *state = static_cast<ShaderState *>(HeapAlloc(GetProcessHeap(),
            HEAP_ZERO_MEMORY, offsetof(ShaderState, privateShader) + privateSize));
    if (!state)
        return E_OUTOFMEMORY;
    state->owner = owner;
    state->publicHandle = shader;
    state->stage = stage;
    state->driverHandle.pDrvPrivate = state->privateShader;
    state->runtimeHandle.handle = state;

    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    state->registryNext = shaderRegistry;
    shaderRegistry = state;
    state->ownerNext = owner->shaders;
    owner->shaders = state;
    ReleaseSRWLockExclusive(&resourceRegistryLock);

    WineD3D11On12ShaderDesc desc = {};
    desc.pFunction = bytes;
    desc.SizeInBytes = static_cast<UINT>(byteCodeLength);
    InterlockedExchange(&owner->lastDdiError, S_OK);
    HRESULT hr = createShader(ddiDevice, state->driverHandle, &desc);
    if (SUCCEEDED(hr))
        hr = InterlockedCompareExchange(&owner->lastDdiError, S_OK, S_OK);
    if (FAILED(hr))
    {
        AcquireSRWLockExclusive(&resourceRegistryLock);
        unlinkOwnerShader(state);
        ReleaseSRWLockExclusive(&resourceRegistryLock);
        unlinkShader(state);
        HeapFree(GetProcessHeap(), 0, state);
        return hr;
    }
    state->created = true;
    shader->stage = stage;
    shader->hDrvShader = state->driverHandle.pDrvPrivate;
    shader->runtimeState = state;
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12CreateVertexShaderV1(
        WineD3D11On12AdapterDevice *adapterDevice, const void *byteCode,
        SIZE_T byteCodeLength, WineD3D11On12Shader *shader) noexcept
{
    return createBasicShader(adapterDevice, byteCode, byteCodeLength, shader,
            WINE_D3D11ON12_SHADER_VERTEX);
}

extern "C" HRESULT WINAPI WineD3D11On12CreatePixelShaderV1(
        WineD3D11On12AdapterDevice *adapterDevice, const void *byteCode,
        SIZE_T byteCodeLength, WineD3D11On12Shader *shader) noexcept
{
    return createBasicShader(adapterDevice, byteCode, byteCodeLength, shader,
            WINE_D3D11ON12_SHADER_PIXEL);
}

extern "C" HRESULT WINAPI WineD3D11On12DestroyShaderV1(
        WineD3D11On12Shader *shader) noexcept
{
    if (!shader || shader->size != sizeof(*shader))
        return E_INVALIDARG;
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockExclusive(&resourceRegistryLock);
    ShaderState *state = nullptr;
    ShaderState **link = &shaderRegistry;
    for (ShaderState *entry = shaderRegistry; entry;
            link = &entry->registryNext, entry = entry->registryNext)
        if (entry == shader->runtimeState && entry->publicHandle == shader
                && entry->driverHandle.pDrvPrivate == shader->hDrvShader
                && entry->stage == shader->stage)
        {
            state = entry;
            unlinkOwnerShader(entry);
            *link = entry->registryNext;
            shader->stage = 0;
            shader->hDrvShader = nullptr;
            shader->runtimeState = nullptr;
            break;
        }
    ReleaseSRWLockExclusive(&resourceRegistryLock);
    if (!state)
    {
        shader->stage = 0;
        shader->hDrvShader = nullptr;
        shader->runtimeState = nullptr;
        return S_OK;
    }
    if (state->created && state->owner && state->owner->deviceCreated
            && state->owner->deviceFuncs.pfnDestroyShader)
        state->owner->deviceFuncs.pfnDestroyShader(state->owner->hDevice,
                state->driverHandle);
    HeapFree(GetProcessHeap(), 0, state);
    return S_OK;
}

HRESULT setBasicShader(WineD3D11On12AdapterDevice *adapterDevice,
        WineD3D11On12Shader *shader, UINT stage) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice))
        return E_INVALIDARG;
    AdapterState *owner = static_cast<AdapterState *>(adapterDevice->runtimeState);
    PFND3D10DDI_SETSHADER setShader = nullptr;
    if (owner)
        setShader = stage == WINE_D3D11ON12_SHADER_VERTEX
                ? owner->deviceFuncs.pfnVsSetShader
                : owner->deviceFuncs.pfnPsSetShader;
    if (!owner || !owner->deviceCreated || !setShader)
        return DXGI_ERROR_UNSUPPORTED;
    D3D10DDI_HSHADER handle = {};
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry,
            nullptr, nullptr);
    AcquireSRWLockShared(&resourceRegistryLock);
    if (shader)
    {
        ShaderState *found = nullptr;
        for (ShaderState *entry = shaderRegistry; entry;
                entry = entry->registryNext)
            if (entry == shader->runtimeState && entry->owner == owner
                    && entry->publicHandle == shader && entry->created
                    && entry->stage == stage && shader->stage == stage
                    && entry->driverHandle.pDrvPrivate == shader->hDrvShader)
            {
                found = entry;
                break;
            }
        if (!found)
        {
            ReleaseSRWLockShared(&resourceRegistryLock);
            return E_INVALIDARG;
        }
        handle = found->driverHandle;
    }
    setShader(owner->hDevice, handle);
    ReleaseSRWLockShared(&resourceRegistryLock);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12SetVertexShaderV1(
        WineD3D11On12AdapterDevice *adapterDevice,
        WineD3D11On12Shader *shader) noexcept
{
    return setBasicShader(adapterDevice, shader,
            WINE_D3D11ON12_SHADER_VERTEX);
}

extern "C" HRESULT WINAPI WineD3D11On12SetPixelShaderV1(
        WineD3D11On12AdapterDevice *adapterDevice,
        WineD3D11On12Shader *shader) noexcept
{
    return setBasicShader(adapterDevice, shader,
            WINE_D3D11ON12_SHADER_PIXEL);
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

namespace
{
/* All frame handles are resolved while the registry lock pins their storage.
 * The immediate context serializes calls; the lock additionally drains a
 * racing binder before destruction reaches the driver. */
struct RenderTargetState
{
    RenderTargetState *registryNext;
    RenderTargetState *ownerNext;
    AdapterState *owner;
    ResourceState *resource;
    WineD3D11On12RenderTargetView *publicHandle;
    D3D10DDI_HRENDERTARGETVIEW driverHandle;
    bool created;
    alignas(8) unsigned char privateView[1];
};

AdapterState *frameOwner(WineD3D11On12AdapterDevice *device) noexcept
{
    if (!device || device->size != sizeof(*device)) return nullptr;
    auto *owner = static_cast<AdapterState *>(device->runtimeState);
    return owner && owner->deviceCreated ? owner : nullptr;
}

ResourceState *findTextureLocked(AdapterState *owner, WineD3D11On12Texture2D *texture) noexcept
{
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry, nullptr, nullptr);
    if (!texture || texture->size != sizeof(*texture)) return nullptr;
    for (auto *r = resourceRegistry; r; r = r->registryNext)
        if (r == texture->runtimeState && r->publicHandle == texture && r->owner == owner
                && r->kind == RESOURCE_KIND_TEXTURE2D && r->created
                && r->driverHandle.pDrvPrivate == texture->hDrvResource)
            return r;
    return nullptr;
}

RenderTargetState *findViewLocked(AdapterState *owner, WineD3D11On12RenderTargetView *view) noexcept
{
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry, nullptr, nullptr);
    if (!view || view->size != sizeof(*view)) return nullptr;
    for (auto *v = renderTargetRegistry; v; v = v->registryNext)
        if (v == view->runtimeState && v->publicHandle == view && v->owner == owner
                && v->created && v->driverHandle.pDrvPrivate == view->hDrvView)
            return v;
    return nullptr;
}

HRESULT frameError(AdapterState *owner) noexcept
{
    return InterlockedCompareExchange(&owner->lastDdiError, S_OK, S_OK);
}

void destroyRenderTargetState(RenderTargetState *view) noexcept
{
    /* Caller holds the registry lock and has removed the view from both lists. */
    if (view->created)
        view->owner->deviceFuncs.pfnDestroyRenderTargetView(view->owner->hDevice, view->driverHandle);
    if (view->publicHandle)
    {
        view->publicHandle->runtimeState = nullptr;
        view->publicHandle->hDrvView = nullptr;
    }
    --view->resource->viewCount;
    HeapFree(GetProcessHeap(), 0, view);
}

void destroyAllRenderTargets(AdapterState *owner) noexcept
{
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry, nullptr, nullptr);
    FrameLock lock;
    while (owner->renderTargets)
    {
        auto *v = owner->renderTargets;
        owner->renderTargets = v->ownerNext;
        auto **link = &renderTargetRegistry;
        while (*link && *link != v) link = &(*link)->registryNext;
        if (*link) *link = v->registryNext;
        destroyRenderTargetState(v);
    }
}
}

extern "C" HRESULT WINAPI WineD3D11On12CreateRenderTargetViewV1(
        WineD3D11On12AdapterDevice *device, WineD3D11On12Texture2D *texture,
        const D3D11_RENDER_TARGET_VIEW_DESC *desc, WineD3D11On12RenderTargetView *out) noexcept
{
    if (!out || out->size != sizeof(*out)) return E_INVALIDARG;
    out->hDrvView = nullptr;
    out->runtimeState = nullptr;
    FrameLock lock;
    auto *owner = frameOwner(device);
    auto *resource = findTextureLocked(owner, texture);
    if (!owner || !resource) return E_INVALIDARG;
    const auto &d = resource->description;
    if (!(d.BindFlags & D3D11_BIND_RENDER_TARGET)) return E_INVALIDARG;
    if ((d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) || d.ArraySize != 1 || d.SampleDesc.Count != 1)
        return DXGI_ERROR_UNSUPPORTED;
    if (desc && (desc->Format != d.Format || desc->ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D
            || desc->Texture2D.MipSlice >= d.MipLevels)) return E_INVALIDARG;
    auto &f = owner->deviceFuncs;
    if (!f.pfnCalcPrivateRenderTargetViewSize || !f.pfnCreateRenderTargetView || !f.pfnDestroyRenderTargetView)
        return DXGI_ERROR_UNSUPPORTED;
    D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW args = {};
    args.hDrvResource = resource->driverHandle;
    args.Format = d.Format;
    args.ResourceDimension = D3D10DDIRESOURCE_TEXTURE2D;
    args.Tex2D.MipSlice = desc ? desc->Texture2D.MipSlice : 0;
    args.Tex2D.ArraySize = 1;
    traceCreation("calculate view storage");
    const SIZE_T size = f.pfnCalcPrivateRenderTargetViewSize(owner->hDevice, &args);
    if (!size || size > SIZE_MAX - offsetof(RenderTargetState, privateView)) return E_OUTOFMEMORY;
    auto *v = static_cast<RenderTargetState *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            offsetof(RenderTargetState, privateView) + size));
    if (!v) return E_OUTOFMEMORY;
    v->owner = owner;
    v->resource = resource;
    v->publicHandle = out;
    v->driverHandle.pDrvPrivate = v->privateView;
    D3D10DDI_HRTRENDERTARGETVIEW runtime = {v};
    InterlockedExchange(&owner->lastDdiError, S_OK);
    traceCreation("enter driver view creation");
    f.pfnCreateRenderTargetView(owner->hDevice, &args, v->driverHandle, runtime);
    traceCreation("leave driver view creation");
    HRESULT hr = frameError(owner);
    if (FAILED(hr)) { HeapFree(GetProcessHeap(), 0, v); return hr; }
    v->created = true;
    ++resource->viewCount;
    v->ownerNext = owner->renderTargets;
    owner->renderTargets = v;
    v->registryNext = renderTargetRegistry;
    renderTargetRegistry = v;
    out->hDrvView = v->driverHandle.pDrvPrivate;
    out->runtimeState = v;
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12DestroyRenderTargetViewV1(WineD3D11On12RenderTargetView *view) noexcept
{
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry, nullptr, nullptr);
    if (!view || view->size != sizeof(*view)) return E_INVALIDARG;
    ResourceState *orphan = nullptr;
    {
        FrameLock lock;
        auto **link = &renderTargetRegistry;
        while (*link && !((*link)->publicHandle == view && *link == view->runtimeState
                && (*link)->driverHandle.pDrvPrivate == view->hDrvView)) link = &(*link)->registryNext;
        if (*link)
        {
            auto *v = *link;
            *link = v->registryNext;
            auto **ownerLink = &v->owner->renderTargets;
            while (*ownerLink && *ownerLink != v) ownerLink = &(*ownerLink)->ownerNext;
            if (*ownerLink) *ownerLink = v->ownerNext;
            auto *r = v->resource;
            destroyRenderTargetState(v);
            if (!r->viewCount && !r->publicHandle)
            {
                orphan = r;
                unlinkOwnerResource(r);
            }
        }
        view->hDrvView = nullptr;
        view->runtimeState = nullptr;
    }
    if (orphan) destroyResourceState(orphan);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12SetRenderTargetV1(
        WineD3D11On12AdapterDevice *device, WineD3D11On12RenderTargetView *view) noexcept
{
    FrameLock lock;
    auto *owner = frameOwner(device);
    if (!owner || !owner->deviceFuncs.pfnSetRenderTargets) return DXGI_ERROR_UNSUPPORTED;
    auto *v = findViewLocked(owner, view);
    if (view && !v) return E_INVALIDARG;
    D3D10DDI_HRENDERTARGETVIEW handle = v ? v->driverHandle : D3D10DDI_HRENDERTARGETVIEW{};
    InterlockedExchange(&owner->lastDdiError, S_OK);
    owner->deviceFuncs.pfnSetRenderTargets(owner->hDevice, &handle, v ? 1 : 0,
            v ? 7 : 8, {}, nullptr, nullptr, 0, 0, 0, 0);
    const HRESULT hr = frameError(owner);
    if (SUCCEEDED(hr)) owner->boundRenderTarget = v ? v->resource : nullptr;
    return hr;
}

extern "C" HRESULT WINAPI WineD3D11On12SetViewportV1(
        WineD3D11On12AdapterDevice *device, const D3D11_VIEWPORT *viewport) noexcept
{
    FrameLock lock;
    auto *owner = frameOwner(device);
    if (!owner || !owner->deviceFuncs.pfnSetViewports) return DXGI_ERROR_UNSUPPORTED;
    if (viewport && (!(viewport->Width >= 0 && viewport->Width <= 32767)
            || !(viewport->Height >= 0 && viewport->Height <= 32767)
            || !(viewport->TopLeftX >= -32768 && viewport->TopLeftX <= 32767)
            || !(viewport->TopLeftY >= -32768 && viewport->TopLeftY <= 32767)
            || !(viewport->MinDepth >= 0 && viewport->MaxDepth <= 1
                && viewport->MinDepth <= viewport->MaxDepth))) return E_INVALIDARG;
    D3D10_DDI_VIEWPORT ddi = {};
    static_assert(sizeof(ddi) == sizeof(*viewport), "viewport transport");
    if (viewport) memcpy(&ddi, viewport, sizeof(ddi));
    InterlockedExchange(&owner->lastDdiError, S_OK);
    owner->deviceFuncs.pfnSetViewports(owner->hDevice, viewport ? 1 : 0,
            viewport ? 15 : 16, viewport ? &ddi : nullptr);
    return frameError(owner);
}

extern "C" HRESULT WINAPI WineD3D11On12ClearRenderTargetV1(
        WineD3D11On12AdapterDevice *device, WineD3D11On12RenderTargetView *view, const FLOAT *color) noexcept
{
    FrameLock lock;
    auto *owner = frameOwner(device);
    auto *v = findViewLocked(owner, view);
    if (!owner || !v || !color) return E_INVALIDARG;
    if (v->resource->wrappedResource && (FAILED(owner->wrappedOwnershipError) || !v->resource->acquired))
        return FAILED(owner->wrappedOwnershipError) ? owner->wrappedOwnershipError : E_INVALIDARG;
    if (!owner->deviceFuncs.pfnClearRenderTargetView) return DXGI_ERROR_UNSUPPORTED;
    FLOAT rgba[4];
    memcpy(rgba, color, sizeof(rgba));
    InterlockedExchange(&owner->lastDdiError, S_OK);
    owner->deviceFuncs.pfnClearRenderTargetView(owner->hDevice, v->driverHandle, rgba);
    return frameError(owner);
}

extern "C" HRESULT WINAPI WineD3D11On12CopyTexture2DV1(WineD3D11On12AdapterDevice *device,
        WineD3D11On12Texture2D *destination, WineD3D11On12Texture2D *source) noexcept
{
    FrameLock lock;
    auto *owner = frameOwner(device);
    auto *dst = findTextureLocked(owner, destination);
    auto *src = findTextureLocked(owner, source);
    if (!owner || !dst || !src || dst == src) return E_INVALIDARG;
    const auto &d = dst->description;
    const auto &s = src->description;
    if ((src->wrappedResource || dst->wrappedResource) && FAILED(owner->wrappedOwnershipError))
        return owner->wrappedOwnershipError;
    if ((src->wrappedResource && !src->acquired) || (dst->wrappedResource && !dst->acquired))
        return E_INVALIDARG;
    if (d.Width != s.Width || d.Height != s.Height || d.Format != s.Format
            || d.MipLevels != s.MipLevels || d.ArraySize != s.ArraySize
            || d.SampleDesc.Count != s.SampleDesc.Count || d.SampleDesc.Quality != s.SampleDesc.Quality
            || d.Usage == D3D11_USAGE_IMMUTABLE) return E_INVALIDARG;
    for (UINT i = 0; i < d.MipLevels * d.ArraySize; ++i)
        if (dst->mapped[i] || src->mapped[i]) return DXGI_ERROR_INVALID_CALL;
    if (!owner->deviceFuncs.pfnResourceCopy) return DXGI_ERROR_UNSUPPORTED;
    InterlockedExchange(&owner->lastDdiError, S_OK);
    owner->deviceFuncs.pfnResourceCopy(owner->hDevice, dst->driverHandle, src->driverHandle);
    return frameError(owner);
}

extern "C" HRESULT WINAPI WineD3D11On12MapTexture2DV1(WineD3D11On12AdapterDevice *device,
        WineD3D11On12Texture2D *texture, UINT subresource, D3D11_MAP mode, UINT flags,
        D3D11_MAPPED_SUBRESOURCE *out) noexcept
{
    if (!out) return E_INVALIDARG;
    ZeroMemory(out, sizeof(*out));
    FrameLock lock;
    auto *owner = frameOwner(device);
    auto *r = findTextureLocked(owner, texture);
    if (!owner || !r || subresource >= r->description.MipLevels * r->description.ArraySize)
        return E_INVALIDARG;
    const auto &d = r->description;
    if (d.Usage != D3D11_USAGE_STAGING || mode != D3D11_MAP_READ
            || !(d.CPUAccessFlags & D3D11_CPU_ACCESS_READ) || flags) return DXGI_ERROR_UNSUPPORTED;
    if (r->mapped[subresource]) return DXGI_ERROR_INVALID_CALL;
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM || d.SampleDesc.Count != 1) return DXGI_ERROR_UNSUPPORTED;
    auto &f = owner->deviceFuncs;
    if (!f.pfnStagingResourceMap || !f.pfnStagingResourceUnmap) return DXGI_ERROR_UNSUPPORTED;
    D3D10DDI_MAPPED_SUBRESOURCE mapped = {};
    InterlockedExchange(&owner->lastDdiError, S_OK);
    /* The pinned driver's staging Map synchronizes the copy with the GPU. */
    f.pfnStagingResourceMap(owner->hDevice, r->driverHandle, subresource,
            static_cast<D3D10_DDI_MAP>(mode), flags, &mapped);
    HRESULT hr = frameError(owner);
    if (FAILED(hr)) return hr;
    UINT width = d.Width >> (subresource % d.MipLevels);
    UINT height = d.Height >> (subresource % d.MipLevels);
    if (!width) width = 1;
    if (!height) height = 1;
    if (!mapped.pData || static_cast<UINT64>(width) * 4 > mapped.RowPitch
            || static_cast<UINT64>(mapped.RowPitch) * height > SIZE_MAX
            || (mapped.DepthPitch && static_cast<UINT64>(mapped.RowPitch) * height > mapped.DepthPitch))
    {
        f.pfnStagingResourceUnmap(owner->hDevice, r->driverHandle, subresource);
        return E_FAIL;
    }
    r->mapped[subresource] = 1;
    out->pData = mapped.pData;
    out->RowPitch = mapped.RowPitch;
    out->DepthPitch = mapped.DepthPitch;
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12UnmapTexture2DV1(WineD3D11On12AdapterDevice *device,
        WineD3D11On12Texture2D *texture, UINT subresource) noexcept
{
    FrameLock lock;
    auto *owner = frameOwner(device);
    auto *r = findTextureLocked(owner, texture);
    if (!owner || !r || subresource >= r->description.MipLevels * r->description.ArraySize
            || !r->mapped[subresource]) return E_INVALIDARG;
    InterlockedExchange(&owner->lastDdiError, S_OK);
    owner->deviceFuncs.pfnStagingResourceUnmap(owner->hDevice, r->driverHandle, subresource);
    r->mapped[subresource] = 0;
    return frameError(owner);
}

extern "C" HRESULT WINAPI WineD3D11On12CheckFrameSupportV1(WineD3D11On12AdapterDevice *device) noexcept
{
    auto *owner = frameOwner(device);
    if (!owner) return E_INVALIDARG;
    const auto &f = owner->deviceFuncs;
    auto *ddi = static_cast<WineD3D11On12DDIDevice *>(owner->hDevice.pDrvPrivate);
    if (!ddi || !ddi->lpVtbl || !ddi->lpVtbl->CreateVertexShader || !ddi->lpVtbl->CreatePixelShader
            || !f.pfnCalcPrivateShaderSize || !f.pfnDestroyShader || !f.pfnVsSetShader || !f.pfnPsSetShader
            || !f.pfnCalcPrivateResourceSize || !f.pfnCreateResource || !f.pfnDestroyResource
            || !f.pfnCalcPrivateElementLayoutSize || !f.pfnCreateElementLayout || !f.pfnDestroyElementLayout
            || !f.pfnIaSetInputLayout || !f.pfnIaSetVertexBuffers || !f.pfnIaSetTopology
            || !f.pfnCalcPrivateRenderTargetViewSize || !f.pfnCreateRenderTargetView || !f.pfnDestroyRenderTargetView
            || !f.pfnSetRenderTargets || !f.pfnSetViewports || !f.pfnClearRenderTargetView
            || !f.pfnResourceCopy || !f.pfnStagingResourceMap || !f.pfnStagingResourceUnmap
            || !f.pfnDraw || !f.pfnFlush) return DXGI_ERROR_UNSUPPORTED;
    return S_OK;
}

/* In-process wrapping uses the driver's local handle map, not a Windows
 * shared handle. The driver consumes one resource reference on a successful
 * OpenResource; DestroyKMTHandle owns that reference on failure. */
extern "C" HRESULT WINAPI WineD3D11On12CreateWrappedTexture2DV1(
        WineD3D11On12AdapterDevice *device, IUnknown *object,
        const D3D11_RESOURCE_FLAGS *flags, D3D12_RESOURCE_STATES inputState,
        D3D12_RESOURCE_STATES outputState, D3D11_TEXTURE2D_DESC *description,
        WineD3D11On12Texture2D *texture) noexcept
{
    InitOnceExecuteOnce(&resourceRegistryOnce, initializeResourceRegistry, nullptr, nullptr);
    if (!texture || texture->size != sizeof(*texture)) return E_INVALIDARG;
    texture->hDrvResource = nullptr;
    texture->runtimeState = nullptr;
    texture->reserved = 0;
    if (description) *description = {};
    auto *owner = frameOwner(device);
    if (!owner || !object || !flags || !description) return E_INVALIDARG;
    /* The first wrapped slice supports a single ordinary render target.
     * Shared/keyed-mutex heaps, arrays, multisampling and other views remain
     * explicit unsupported cases rather than partially described resources. */
    if (flags->BindFlags != D3D11_BIND_RENDER_TARGET || flags->MiscFlags
            || flags->CPUAccessFlags || flags->StructureByteStride)
        return DXGI_ERROR_UNSUPPORTED;
    const UINT allowedStates = D3D12_RESOURCE_STATE_RENDER_TARGET
            | D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST
            | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto validState = [allowedStates](UINT s) noexcept {
        const UINT writes = s & (D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_COPY_DEST);
        return !(s & ~allowedStates) && (!writes || s == D3D12_RESOURCE_STATE_RENDER_TARGET
                || s == D3D12_RESOURCE_STATE_COPY_DEST);
    };
    if (!validState(inputState) || !validState(outputState)) return E_INVALIDARG;
    ComRef<ID3D12Resource> original;
    HRESULT hr = strictResult(object->QueryInterface(IID_ID3D12Resource,
            reinterpret_cast<void **>(original.put())), original);
    if (FAILED(hr)) return hr;
    ComRef<ID3D12Device> originalDevice;
    hr = strictResult(original.get()->GetDevice(IID_ID3D12Device,
            reinterpret_cast<void **>(originalDevice.put())), originalDevice);
    if (FAILED(hr)) return hr;
    if (originalDevice.get() != owner->device12)
    {
        bool same = false;
        hr = comObjectsIdentical(originalDevice.get(), owner->device12, &same);
        if (FAILED(hr)) return hr;
        if (!same) return E_INVALIDARG;
    }
    const auto d = RelayD3D12ResourceDesc(original.get());
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !d.Width || !d.Height
            || d.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || d.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || d.DepthOrArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1
            || d.SampleDesc.Quality || !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
            || (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM))
        return DXGI_ERROR_UNSUPPORTED;
    D3D12_HEAP_PROPERTIES heap = {};
    D3D12_HEAP_FLAGS heapFlags = {};
    hr = original.get()->GetHeapProperties(&heap, &heapFlags);
    if (FAILED(hr)) return hr;
    if (heap.Type != D3D12_HEAP_TYPE_DEFAULT
            || (heapFlags & (D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)))
        return DXGI_ERROR_UNSUPPORTED;
    auto *ddi = static_cast<WineD3D11On12DDIDevice *>(owner->hDevice.pDrvPrivate);
    auto &f = owner->deviceFuncs;
    if (!ddi || !ddi->lpVtbl || !ddi->lpVtbl->GetResourcePrivateDataSize
            || !ddi->lpVtbl->CreateWrappingHandle || !ddi->lpVtbl->DestroyKMTHandle
            || !ddi->lpVtbl->TransitionResourceForRelease || !ddi->lpVtbl->ApplyAllResourceTransitions
            || !f.pfnCalcPrivateOpenedResourceSize || !f.pfnOpenResource || !f.pfnDestroyResource)
        return DXGI_ERROR_UNSUPPORTED;
    const UINT dataSize = ddi->lpVtbl->GetResourcePrivateDataSize(ddi);
    if (!dataSize) return E_FAIL;
    void *data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dataSize);
    if (!data) return E_OUTOFMEMORY;
    UINT handle = 0;
    original.get()->AddRef(); // transferred only if CreateWrappingHandle succeeds
    hr = ddi->lpVtbl->CreateWrappingHandle(ddi, original.get(), 1, data, dataSize, &handle);
    if (FAILED(hr) || !handle)
    {
        original.get()->Release();
        HeapFree(GetProcessHeap(), 0, data);
        return FAILED(hr) ? hr : E_FAIL;
    }
    D3D10DDIARG_OPENRESOURCE args = {};
    args.hKMResource.handle = handle;
    args.pPrivateDriverData = data;
    args.PrivateDriverDataSize = dataSize;
    const SIZE_T size = f.pfnCalcPrivateOpenedResourceSize(owner->hDevice, &args);
    ResourceState *r = nullptr;
    if (size && size <= SIZE_MAX - offsetof(ResourceState, privateResource))
        r = static_cast<ResourceState *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                offsetof(ResourceState, privateResource) + size));
    if (!r)
    {
        ddi->lpVtbl->DestroyKMTHandle(ddi, handle);
        HeapFree(GetProcessHeap(), 0, data);
        return E_OUTOFMEMORY;
    }
    r->mapped = static_cast<BYTE *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1));
    if (!r->mapped)
    {
        ddi->lpVtbl->DestroyKMTHandle(ddi, handle);
        HeapFree(GetProcessHeap(), 0, data);
        HeapFree(GetProcessHeap(), 0, r);
        return E_OUTOFMEMORY;
    }
    r->owner = owner;
    r->publicHandle = texture;
    r->kind = RESOURCE_KIND_TEXTURE2D;
    r->flags = *flags;
    r->description = {static_cast<UINT>(d.Width), d.Height, 1, 1, d.Format,
            d.SampleDesc, D3D11_USAGE_DEFAULT, flags->BindFlags, 0, 0};
    r->inputState = inputState;
    r->outputState = outputState;
    r->driverHandle.pDrvPrivate = r->privateResource;
    r->runtimeHandle.handle = r;
    {
        FrameLock lock;
        r->registryNext = resourceRegistry;
        resourceRegistry = r;
        r->ownerNext = owner->resources;
        owner->resources = r;
    }
    InterlockedExchange(&owner->lastDdiError, S_OK);
    f.pfnOpenResource(owner->hDevice, &args, r->driverHandle, r->runtimeHandle);
    hr = frameError(owner);
    HeapFree(GetProcessHeap(), 0, data);
    if (FAILED(hr))
    {
        ddi->lpVtbl->DestroyKMTHandle(ddi, handle);
        { FrameLock lock; unlinkOwnerResource(r); }
        destroyResourceState(r);
        return hr;
    }
    r->created = true;
    auto *resourceDDI = reinterpret_cast<WineD3D11On12DDIResource *>(r->privateResource);
    if (!resourceDDI->lpVtbl || !resourceDDI->lpVtbl->SetGraphicsCurrentState)
    {
        { FrameLock lock; unlinkOwnerResource(r); }
        destroyResourceState(r);
        return DXGI_ERROR_UNSUPPORTED;
    }
    FrameLock publicationLock;
    r->wrappedResource = original.detach();
    resourceDDI->lpVtbl->SetGraphicsCurrentState(resourceDDI, inputState, 0 /* Create */);
    texture->hDrvResource = r->driverHandle.pDrvPrivate;
    texture->runtimeState = r;
    *description = r->description;
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12SetWrappedOwnershipV1(
        WineD3D11On12AdapterDevice *device, WineD3D11On12Texture2D *const *textures,
        UINT count, BOOL acquire) noexcept
{
    FrameLock lock;
    auto *owner = frameOwner(device);
    if (!owner || (acquire != TRUE && acquire != FALSE)) return E_INVALIDARG;
    if (FAILED(owner->wrappedOwnershipError)) return owner->wrappedOwnershipError;
    if (acquire && count && !textures) return E_INVALIDARG;
    // Validate the entire list before changing any state; duplicate entries
    // otherwise turn a batch into a double acquisition or unmatched release.
    if (textures)
        for (UINT i = 0; i < count; ++i)
        {
            auto *r = findTextureLocked(owner, textures[i]);
            if (!r || !r->wrappedResource || r->acquired == !!acquire) return E_INVALIDARG;
            for (UINT j = 0; j < i; ++j)
                if (textures[i] == textures[j]) return E_INVALIDARG;
        }
    auto *ddi = static_cast<WineD3D11On12DDIDevice *>(owner->hDevice.pDrvPrivate);
    InterlockedExchange(&owner->lastDdiError, S_OK);
    for (auto *r = owner->resources; r; r = r->ownerNext)
    {
        if (!r->wrappedResource) continue;
        bool selected = !textures && !acquire && r->acquired;
        for (UINT i = 0; textures && i < count; ++i)
            selected |= r->publicHandle == textures[i];
        if (!selected) continue;
        auto *resourceDDI = reinterpret_cast<WineD3D11On12DDIResource *>(r->privateResource);
        if (acquire)
            resourceDDI->lpVtbl->SetGraphicsCurrentState(resourceDDI, r->inputState, 1 /* Acquire */);
        else
            ddi->lpVtbl->TransitionResourceForRelease(ddi, resourceDDI, r->outputState);
        if (FAILED(frameError(owner)))
        {
            // GPU transitions cannot be rolled back safely. Refuse further
            // wrapped use on this device until it is destroyed and recreated.
            owner->wrappedOwnershipError = frameError(owner);
            return owner->wrappedOwnershipError;
        }
        r->acquired = !!acquire;
    }
    if (!acquire) ddi->lpVtbl->ApplyAllResourceTransitions(ddi);
    const HRESULT hr = frameError(owner);
    if (FAILED(hr)) owner->wrappedOwnershipError = hr;
    return hr;
}
