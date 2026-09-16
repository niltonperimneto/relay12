/* SPDX-License-Identifier: GPL-3.0-only
 * D3D11On12 core ABI and input validation.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>

/* For UINT_MAX and SIZE_MAX, which bound the two conversions the shader path
 * performs on a caller-supplied length.  Macro-only headers, so they do not
 * pull in the C++ runtime this module must not link. */
#include <climits>
#include <cstdint>

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

/* The shader-creation argument structure, and the two types its neighbours in
 * the interface name.  SHADER_DESC is a container pointer, a byte count and a
 * class-linkage pointer; the driver reads all three. */
struct SHADER_DESC
{
    const BYTE *pFunction;
    UINT SizeInBytes;
    ID3D11ClassLinkage *pLinkage;
};

/* Transcribed rather than approximated, because it is passed by value in a
 * vtable slot this host must lay out exactly even though it never calls it. */
enum class WrapReason
{
    CreateWrappedResource = 1
};

/* Pointer-only, so incomplete: no slot this host calls dereferences one. */
struct ResourceInfo;

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

/* The device sub-object interface, as an explicit vtable.
 *
 * ID3D11On12DDIDevice is how the pinned driver exposes the work its DDI
 * function table does not cover, and CastFrom in its own header says how it
 * is reached: the interface pointer *is* D3D10DDI_HDEVICE.pDrvPrivate, the
 * private device block this host allocated.  DeviceBase derives from it
 * first and from nothing else, so the vtable pointer sits at offset zero.
 *
 * Written as a structure of function pointers rather than as a pure-virtual
 * class on purpose.  The interface has no IUnknown base and no virtual
 * destructor, so its slot numbering is exactly its declaration order -- and
 * saying so in a table makes that order reviewable and lets the C tests build
 * a mock, instead of resting on two compilers emitting the same vtable for a
 * class neither of them can see the definition of.
 *
 * Every method up to CreatePixelShader is declared, in order, because a slot
 * this host never calls still determines the index of the two it does.  The
 * arguments of those unused slots are transcribed to the right size and no
 * further: D3DKMT_PRESENT, ResourceInfo, ID3D11On12DDIResource and
 * ID3D11On12DDIFence are pointer-only and stay incomplete, which is the same
 * treatment the clean-room header gives D3D11_1DDIARG_STAGE_IO_SIGNATURES.
 *
 * Methods after CreatePixelShader are deliberately absent.  Nothing indexes
 * past it, and transcribing slots to no purpose would be more surface for
 * scripts/check_adapter_args.py to police than the host needs.
 *
 * Provenance: third_party/D3D11On12/interface/D3D11On12DDI.h, MIT, pinned.
 * That gate compares this declaration to it on every run -- an upstream
 * insertion anywhere above CreateVertexShader silently renumbers the two
 * slots this host calls, and nothing in the build would otherwise notice. */
typedef UINT32 D3DKMT_HANDLE;
struct D3DKMT_PRESENT;
struct ID3D11On12DDIResource;
struct ID3D11On12DDIFence;
struct ID3D11On12DDIDevice;

struct ID3D11On12DDIDeviceVtbl
{
    HRESULT (STDMETHODCALLTYPE *GetD3D12Device)(ID3D11On12DDIDevice *This,
            REFIID riid, void **ppv);
    HRESULT (STDMETHODCALLTYPE *GetGraphicsQueue)(ID3D11On12DDIDevice *This,
            REFIID riid, void **ppv);
    HRESULT (STDMETHODCALLTYPE *EnqueueSetEvent)(ID3D11On12DDIDevice *This,
            HANDLE hEvent);
    UINT (STDMETHODCALLTYPE *GetNodeMask)(ID3D11On12DDIDevice *This);
    HRESULT (STDMETHODCALLTYPE *Present)(ID3D11On12DDIDevice *This,
            D3DKMT_PRESENT *pArgs);
    UINT (STDMETHODCALLTYPE *GetResourcePrivateDataSize)(
            ID3D11On12DDIDevice *This);
    HRESULT (STDMETHODCALLTYPE *OpenSharedHandle)(ID3D11On12DDIDevice *This,
            HANDLE hSharedHandle, void *pPrivateDriverData,
            UINT PrivateDriverDataSize, D3DKMT_HANDLE *hKMTHandle);
    HRESULT (STDMETHODCALLTYPE *CreateWrappingHandle)(
            ID3D11On12DDIDevice *This, IUnknown *pResource,
            D3D11On12::WrapReason reason, void *pPrivateDriverData,
            UINT PrivateDriverDataSize, D3DKMT_HANDLE *hKMTHandle);
    HRESULT (STDMETHODCALLTYPE *FillResourceInfo)(ID3D11On12DDIDevice *This,
            D3DKMT_HANDLE hKMTHandle, D3D11_RESOURCE_FLAGS const *pFlagOverrides,
            D3D11On12::ResourceInfo *pResourceInfo);
    void (STDMETHODCALLTYPE *DestroyKMTHandle)(ID3D11On12DDIDevice *This,
            D3DKMT_HANDLE);
    void (STDMETHODCALLTYPE *TransitionResourceForRelease)(
            ID3D11On12DDIDevice *This, ID3D11On12DDIResource *pResource,
            D3D12_RESOURCE_STATES State);
    void (STDMETHODCALLTYPE *ApplyAllResourceTransitions)(
            ID3D11On12DDIDevice *This);
    HRESULT (STDMETHODCALLTYPE *CreateFence)(ID3D11On12DDIDevice *This,
            UINT64 InitialValue, UINT DXGIInternalFenceFlags,
            ID3D11On12DDIFence **ppFence);
    HRESULT (STDMETHODCALLTYPE *OpenFence)(ID3D11On12DDIDevice *This,
            HANDLE hSharedFence, bool *pbMonitored,
            ID3D11On12DDIFence **ppFence);
    HRESULT (STDMETHODCALLTYPE *Wait)(ID3D11On12DDIDevice *This,
            ID3D11On12DDIFence *pFence, UINT64 Value);
    HRESULT (STDMETHODCALLTYPE *Signal)(ID3D11On12DDIDevice *This,
            ID3D11On12DDIFence *pFence, UINT64 Value);
    HRESULT (STDMETHODCALLTYPE *CreateVertexShader)(ID3D11On12DDIDevice *This,
            D3D10DDI_HSHADER hShader, D3D11On12::SHADER_DESC const *pDesc);
    HRESULT (STDMETHODCALLTYPE *CreatePixelShader)(ID3D11On12DDIDevice *This,
            D3D10DDI_HSHADER hShader, D3D11On12::SHADER_DESC const *pDesc);
};

struct ID3D11On12DDIDevice
{
    const ID3D11On12DDIDeviceVtbl *lpVtbl;
};

/* The slot numbers d3d11on12core.h publishes, checked against the
 * transcription above rather than trusted.  This is the half of the loop the
 * drift gate cannot see: the gate compares the transcription to the driver,
 * and these compare the published numbers to the transcription, so the C mock
 * driver's stubs land where the host will look for them. */
WINE_D3D11ON12_ASSERT(offsetof(ID3D11On12DDIDeviceVtbl, CreateVertexShader)
        == WINE_D3D11ON12_DDIDEVICE_SLOT_CREATEVERTEXSHADER * sizeof(void *));
WINE_D3D11ON12_ASSERT(offsetof(ID3D11On12DDIDeviceVtbl, CreatePixelShader)
        == WINE_D3D11ON12_DDIDEVICE_SLOT_CREATEPIXELSHADER * sizeof(void *));
/* Every slot is one pointer wide, which is what makes an index an offset at
 * all.  A method returning a class by value could break that on some ABIs. */
WINE_D3D11ON12_ASSERT(sizeof(ID3D11On12DDIDeviceVtbl)
        == (WINE_D3D11ON12_DDIDEVICE_SLOT_CREATEPIXELSHADER + 1)
                * sizeof(void *));
WINE_D3D11ON12_ASSERT(sizeof(D3D11On12::SHADER_DESC)
        == WINE_D3D11ON12_SHADER_DESC_SIZE);
WINE_D3D11ON12_ASSERT(offsetof(D3D11On12::SHADER_DESC, SizeInBytes) == 8);
WINE_D3D11ON12_ASSERT(offsetof(D3D11On12::SHADER_DESC, pLinkage) == 16);

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
struct ShaderState;

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
    /* Every shader created on this device and not yet destroyed.
     *
     * Not guarded by a lock, and deliberately: a D3D11 immediate context is
     * not free-threaded, so shader creation and destruction on one device are
     * already serialised by the caller.  The list is only ever touched from
     * the shader entry points and from close, all of which take this device.
     */
    ShaderState *shaders;
    bool adapterOpened;
    bool deviceCreated;
    unsigned char privateDevice[1];
};

/* One allocation per shader, on the same terms as AdapterState.
 *
 * privateShader is the block CalcPrivateShaderSize asked for, and the driver
 * placement-constructs its shader object into it and keeps its address as
 * D3D10DDI_HSHADER.pDrvPrivate until DestroyShader.  So it is a flexible tail
 * rather than a second allocation: it must not move.
 *
 * Aligned rather than left wherever the preceding members end.  The driver
 * constructs a C++ object in there whose alignment requirement this host
 * cannot see, and HeapAlloc's own 16-byte guarantee only covers the start of
 * the block, not an interior member. */
struct ShaderState
{
    /* Linked into the owning AdapterState so that closing a lifecycle whose
     * shaders the caller never destroyed releases them rather than leaking.
     * Doubly linked because an application with thousands of shaders would
     * otherwise pay a walk per destruction. */
    ShaderState *next;
    ShaderState *previous;
    UINT stage;
    D3D10DDI_HSHADER hShader;
    alignas(16) unsigned char privateShader[1];
};

/* One guard per repeatable condition on the shader paths. */
volatile LONG reportedShaderSlotsMissing;
volatile LONG reportedShaderInterfaceMissing;
volatile LONG reportedShaderCreateFailed;
volatile LONG reportedSetShaderMissing;

/* The private device block reinterpreted as the driver's sub-object
 * interface.
 *
 * Not a QueryInterface, so it does not go through strictResult: there is
 * nothing to query.  ID3D11On12DDIDevice::CastFrom in the pinned header is
 * this same reinterpret_cast, and the interface has no IUnknown to ask.
 *
 * What can still be checked is checked.  A device the driver never created
 * has no vtable pointer to read, and the host reaches this only with an
 * hDevice a successful CreateDevice wrote, so a null block is a caller
 * error rather than a driver one. */
ID3D11On12DDIDevice *ddiDeviceFromHandle(D3D10DDI_HDEVICE hDevice) noexcept
{
    if (!hDevice.pDrvPrivate)
        return nullptr;
    return static_cast<ID3D11On12DDIDevice *>(hDevice.pDrvPrivate);
}

/* The opened lifecycle every shader entry point starts from.  Null means the
 * caller passed something other than a structure a successful
 * WineD3D11On12OpenAdapterV1 filled. */
AdapterState *openedAdapterState(
        WineD3D11On12AdapterDevice *adapterDevice) noexcept
{
    if (!adapterDevice || adapterDevice->size != sizeof(*adapterDevice))
        return nullptr;
    return static_cast<AdapterState *>(adapterDevice->runtimeState);
}

bool isKnownShaderStage(UINT stage) noexcept
{
    return stage == WINE_D3D11ON12_SHADER_VERTEX
            || stage == WINE_D3D11ON12_SHADER_PIXEL;
}

void linkShader(AdapterState *state, ShaderState *shader) noexcept
{
    shader->previous = nullptr;
    shader->next = state->shaders;
    if (state->shaders)
        state->shaders->previous = shader;
    state->shaders = shader;
}

void unlinkShader(AdapterState *state, ShaderState *shader) noexcept
{
    if (shader->previous)
        shader->previous->next = shader->next;
    else
        state->shaders = shader->next;
    if (shader->next)
        shader->next->previous = shader->previous;
    shader->next = nullptr;
    shader->previous = nullptr;
}

/* Tell the driver a shader is gone, then release the block it lived in.
 *
 * The order is the only one that works: pDrvPrivate is the driver's object,
 * so the block cannot be freed until DestroyShader has run its destructor. */
void destroyShaderState(AdapterState *state, ShaderState *shader) noexcept
{
    if (state->deviceFuncs.pfnDestroyShader)
        state->deviceFuncs.pfnDestroyShader(state->hDevice, shader->hShader);
    HeapFree(GetProcessHeap(), 0, shader);
}

void destroyAdapterState(AdapterState *state) noexcept
{
    if (!state)
        return;

    /* Shaders first, and before the device, because DestroyShader is a call
     * into a device that is about to stop existing.  A caller that released
     * its shaders already leaves an empty list here; one that did not would
     * otherwise leak a block per shader, and the stale handles it still holds
     * are unreachable afterwards because every shader entry point requires
     * the adapter/device pair this function clears. */
    while (state->shaders)
    {
        ShaderState *shader = state->shaders;

        unlinkShader(state, shader);
        destroyShaderState(state, shader);
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
            | WINE_D3D11ON12_CAP_SHADER_LIFECYCLE;
    interfaceOut->createDevice = WineD3D11On12CreateDeviceV1;
    interfaceOut->createDirectDevice = WineD3D11CreateDeviceV2;
    interfaceOut->createDirectDeviceAndSwapChain =
            WineD3D11CreateDeviceAndSwapChainV2;
    interfaceOut->closeAdapterDevice = WineD3D11On12CloseAdapterDeviceV1;
    interfaceOut->createShader = WineD3D11On12CreateShaderV1;
    interfaceOut->destroyShader = WineD3D11On12DestroyShaderV1;
    interfaceOut->setShader = WineD3D11On12SetShaderV1;
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

    /* Still no ID3D11Device, and deliberately so.  The driver exports one
     * symbol and it yields a DDI device function table, not a runtime object;
     * WineD3D11On12OpenAdapterV1 below is how that table is reached.  Building
     * ID3D11Device over it is the D3D11 runtime's work, planned in
     * docs/D3D11ON12.md as a Wine d3d11 frontend refactor.  Never return
     * success until genuine ID3D11Device and context objects are backed by the
     * supplied device and queue. */
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

extern "C" HRESULT WINAPI WineD3D11On12CreateShaderV1(
        WineD3D11On12AdapterDevice *adapterDevice, UINT stage,
        const void *bytecode, SIZE_T bytecodeSize,
        WineD3D11On12Shader *out) noexcept
{
    if (!out || out->size != sizeof(*out))
        return E_INVALIDARG;

    out->stage = 0;
    out->hDrvShader = nullptr;
    out->runtimeState = nullptr;
    ZeroMemory(out->reserved, sizeof(out->reserved));

    AdapterState *state = openedAdapterState(adapterDevice);
    if (!state || !isKnownShaderStage(stage) || !bytecode || !bytecodeSize)
        return E_INVALIDARG;
    /* SHADER_DESC.SizeInBytes is a UINT.  Truncating a larger container would
     * hand the driver a prefix of a shader and call it a shader. */
    if (bytecodeSize > UINT_MAX)
        return E_INVALIDARG;

    if (!state->deviceFuncs.pfnCalcPrivateShaderSize
            || !state->deviceFuncs.pfnDestroyShader)
    {
        wineD3D11DiagReportOnce(&reportedShaderSlotsMissing,
                "d3d11on12core: the driver filled no shader sizing or "
                "destruction slot, so no shader can be owned for its "
                "lifetime.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }

    ID3D11On12DDIDevice *ddiDevice = ddiDeviceFromHandle(state->hDevice);
    if (!ddiDevice || !ddiDevice->lpVtbl)
    {
        wineD3D11DiagReportOnce(&reportedShaderInterfaceMissing,
                "d3d11on12core: the driver's private device block exposes no "
                "ID3D11On12DDIDevice vtable, which is the only path on which "
                "the pinned driver creates shaders.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }

    HRESULT (STDMETHODCALLTYPE *createShader)(ID3D11On12DDIDevice *,
            D3D10DDI_HSHADER, D3D11On12::SHADER_DESC const *) =
            stage == WINE_D3D11ON12_SHADER_VERTEX
            ? ddiDevice->lpVtbl->CreateVertexShader
            : ddiDevice->lpVtbl->CreatePixelShader;
    if (!createShader)
    {
        wineD3D11DiagReportOnce(&reportedShaderInterfaceMissing,
                "d3d11on12core: the driver's ID3D11On12DDIDevice leaves the "
                "requested stage's shader-creation slot empty.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }

    /* Both arguments null, and not because they are unknown.
     *
     * The sizing slot takes the DDI's driver bytecode -- a token stream whose
     * length is its own second word -- while the creation path above takes a
     * DXBC container.  The two cannot be the same pointer, so offering the
     * container here would be offering a length that is really part of a
     * hash.  The pinned driver reads neither argument: its CalcPrivateSize
     * ignores both and returns a fixed sizeof.  A driver that did read them
     * could not be served by this pairing at all, which is the driver's own
     * inconsistency and is recorded in docs/CLEANROOM-DDI.md. */
    const SIZE_T privateSize = state->deviceFuncs.pfnCalcPrivateShaderSize(
            state->hDevice, nullptr, nullptr);
    if (!privateSize)
        return E_FAIL;
    /* The size came from the driver, so the addition below is arithmetic on a
     * value this module did not choose. */
    if (privateSize > SIZE_MAX - sizeof(ShaderState))
        return E_OUTOFMEMORY;

    ShaderState *shader = static_cast<ShaderState *>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(ShaderState) + privateSize));
    if (!shader)
        return E_OUTOFMEMORY;

    shader->stage = stage;
    shader->hShader.pDrvPrivate = shader->privateShader;

    /* pLinkage stays null.  It is what tells the driver to compile without
     * class-instance interfaces; the pfn*SetShaderWithIfaces family that
     * would need them is unpromoted, so a non-null linkage here would promise
     * a binding path that does not exist. */
    D3D11On12::SHADER_DESC desc = {};
    desc.pFunction = static_cast<const BYTE *>(bytecode);
    desc.SizeInBytes = static_cast<UINT>(bytecodeSize);
    desc.pLinkage = nullptr;

    const HRESULT hr = createShader(ddiDevice, shader->hShader, &desc);
    if (FAILED(hr))
    {
        wineD3D11DiagReportOnce(&reportedShaderCreateFailed,
                "d3d11on12core: the driver rejected shader creation.\n");
        HeapFree(GetProcessHeap(), 0, shader);
        return hr;
    }

    linkShader(state, shader);

    out->stage = stage;
    out->hDrvShader = shader->hShader.pDrvPrivate;
    out->runtimeState = shader;
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12DestroyShaderV1(
        WineD3D11On12AdapterDevice *adapterDevice,
        WineD3D11On12Shader *shaderOut) noexcept
{
    if (!shaderOut || shaderOut->size != sizeof(*shaderOut))
        return E_INVALIDARG;

    AdapterState *state = openedAdapterState(adapterDevice);
    if (!state)
        return E_INVALIDARG;

    ShaderState *shader = static_cast<ShaderState *>(shaderOut->runtimeState);

    shaderOut->stage = 0;
    shaderOut->hDrvShader = nullptr;
    shaderOut->runtimeState = nullptr;
    ZeroMemory(shaderOut->reserved, sizeof(shaderOut->reserved));

    /* Idempotent, like closing an empty adapter lifecycle: a shader that was
     * never created, or one already destroyed, is not an error to destroy. */
    if (!shader)
        return S_OK;

    unlinkShader(state, shader);
    destroyShaderState(state, shader);
    return S_OK;
}

extern "C" HRESULT WINAPI WineD3D11On12SetShaderV1(
        WineD3D11On12AdapterDevice *adapterDevice, UINT stage,
        const WineD3D11On12Shader *shaderIn) noexcept
{
    AdapterState *state = openedAdapterState(adapterDevice);
    if (!state || !isKnownShaderStage(stage))
        return E_INVALIDARG;

    /* Zeroed, which is how the DDI spells "no shader on this stage".  A bind
     * of a null shader is an unbind, not an error, because that is what
     * VSSetShader(nullptr) means one layer up. */
    D3D10DDI_HSHADER hShader = {};

    if (shaderIn)
    {
        if (shaderIn->size != sizeof(*shaderIn) || !shaderIn->runtimeState)
            return E_INVALIDARG;

        const ShaderState *shader = static_cast<const ShaderState *>(
                shaderIn->runtimeState);
        /* Recorded at creation, compared here.  Binding a pixel shader to the
         * vertex stage would otherwise reach the driver as a well-formed call
         * and come back as a wrong frame. */
        if (shader->stage != stage)
            return E_INVALIDARG;
        hShader = shader->hShader;
    }

    PFND3D10DDI_SETSHADER setShader =
            stage == WINE_D3D11ON12_SHADER_VERTEX
            ? state->deviceFuncs.pfnVsSetShader
            : state->deviceFuncs.pfnPsSetShader;
    if (!setShader)
    {
        wineD3D11DiagReportOnce(&reportedSetShaderMissing,
                "d3d11on12core: the driver filled no shader-binding slot for "
                "the requested stage.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }

    setShader(state->hDevice, hShader);
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
