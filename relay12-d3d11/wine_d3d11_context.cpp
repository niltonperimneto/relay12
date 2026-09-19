/* SPDX-License-Identifier: GPL-3.0-only
 *
 * ID3D11DeviceContext dispatched onto D3DWDDM2_6DDI_DEVICEFUNCS.
 *
 * Three rules govern every method below, and they are what make this file
 * reviewable rather than a wall of forwarding:
 *
 *   1. A method whose DDI slot is promoted converts its arguments and calls
 *      the slot.  Nothing else.
 *   2. A method whose slot is still a non-callable placeholder does not call
 *      anything.  It reports once, by slot family, and returns -- an honest
 *      failure where it returns an HRESULT, and nothing at all where the
 *      public method returns void.  Silently doing nothing is what would make
 *      a half-rendered frame look like a driver bug.
 *   3. A method whose slot is promoted but whose argument structure the
 *      clean-room header deliberately leaves incomplete is treated as case 2,
 *      and says so specifically.  RSSetViewports is the whole of this case:
 *      the slot is callable, but D3D10_DDI_VIEWPORT is an opaque type here
 *      (wine_d3d11ddi.h:1966), so the host cannot build the argument.  That
 *      is a declaration gap, not a translation one, and naming it separately
 *      is what keeps it from being mistaken for an unpromoted slot.
 *
 * State tracking is absent, on purpose and visibly.  Every Get* method
 * reports nothing bound.  The DDI has no getters -- tracking bound state is
 * the runtime's job and this milestone does not do it -- so an application
 * that sets state and then reads it back gets null rather than what it set.
 * That is wrong for such an application and right for this milestone: the
 * alternative is a shadow copy of the entire pipeline state, which is real
 * work with its own correctness burden, and inventing partial answers would
 * hide which of the two we have.  Each Get* family latches once.
 *
 * Class instances are refused rather than dropped.  Every SetShader slot here
 * takes a shader handle and nothing else; the interface-carrying variants are
 * pfnXsSetShaderWithIfaces, which are unpromoted.  A shader set with class
 * instances is therefore not set at all, because binding it without them
 * would run the wrong code.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

/* For the placement form of operator new only.  It is declared inline and
 * links nothing, so including this does not introduce the C++ runtime
 * dependency scripts/check_pe_audit.py rejects -- the object still comes from
 * HeapAlloc, and placement construction is only how its vtable gets written. */
#include <new>

#include "wine_d3d11_context.h"
#include "wine_d3d11_com.h"
#include "wine_d3d11_diag.h"
#include "wine_d3d11_host.h"

namespace
{

/* One latch per slot family, not per method.
 *
 * Per-method would be more precise and much noisier: an application binding
 * constant buffers for six stages in its draw loop would report six times to
 * say one thing.  The families are the DDI's own groupings, so a latch names
 * the promotion that is missing rather than the call that noticed.
 *
 * shared-state: each is a volatile LONG moved only through
 * wineD3D11DiagReportOnce, which uses InterlockedCompareExchange. */
volatile LONG reportedConstantBuffers;
volatile LONG reportedShaderResources;
volatile LONG reportedSamplers;
volatile LONG reportedIndexedDraw;
volatile LONG reportedInstancedDraw;
volatile LONG reportedIndirectDraw;
volatile LONG reportedDrawAuto;
volatile LONG reportedIndexBuffer;
volatile LONG reportedQuery;
volatile LONG reportedPredication;
volatile LONG reportedStreamOutput;
volatile LONG reportedDispatch;
volatile LONG reportedScissorRects;
volatile LONG reportedViewportsOpaque;
volatile LONG reportedCopyRegion;
volatile LONG reportedUpdateSubresource;
volatile LONG reportedStructureCount;
volatile LONG reportedUnorderedAccess;
volatile LONG reportedDepthStencilView;
volatile LONG reportedGenMips;
volatile LONG reportedResourceMinLOD;
volatile LONG reportedResolve;
volatile LONG reportedMap;
volatile LONG reportedClearState;
volatile LONG reportedFlush;
volatile LONG reportedDeferred;
volatile LONG reportedStateReadback;
volatile LONG reportedClassInstances;
volatile LONG reportedSlotMissing;
volatile LONG reportedBindingRejected;
volatile LONG reportedPrivateData;

/* The bounds the public API itself defines.  A call past them is invalid in
 * D3D11 and would overrun the fixed arrays below, so it is rejected before
 * any handle is resolved.
 *
 * Typed UINT rather than an enumeration: every count they are compared
 * against is a UINT, and an int-typed enumerator would make each comparison a
 * signed/unsigned one, which this build turns into an error. */
constexpr UINT MAX_VERTEX_BUFFERS = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
constexpr UINT MAX_RENDER_TARGETS = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
constexpr UINT MAX_UNORDERED_ACCESS_VIEWS = 64;

/* The DDI's primitive topology is numerically the public enumeration.
 *
 * This is the one enumeration the host converts whose values the DDI's own
 * reference page does not publish, so the conversion needs a source, and the
 * source is the pinned driver: third_party/D3D11On12/src/pipelinestate.cpp
 * casts the value it is given straight to the public type --
 *
 *     GetBatchedContext().IaSetTopology(
 *             static_cast<D3D_PRIMITIVE_TOPOLOGY>(topology));
 *
 * -- under a static_assert that the DDI's first patch-list enumerator is 33,
 * which is also the public D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST.
 * The identity therefore holds across the adjacency and patch-list ranges,
 * which are exactly where sequential numbering would have diverged and which
 * wine_d3d11ddi.h's binding-types group records as the reason it declines to
 * name the constants.
 *
 * That file is MIT and docs/CLEANROOM-DDI.md records it as usable directly,
 * so this is a cited fact rather than a pass-through assumption.  It is
 * written as a function with one caller so that a future driver pin which
 * breaks the identity has one place to be fixed. */
D3D10_DDI_PRIMITIVE_TOPOLOGY ddiTopologyFromPublic(
        D3D11_PRIMITIVE_TOPOLOGY topology) noexcept
{
    return static_cast<D3D10_DDI_PRIMITIVE_TOPOLOGY>(topology);
}

class HostContext final : public ID3D11DeviceContext,
        public IWineD3D11HostObject
{
public:
    void initialize(const WineD3D11ContextBinding &binding) noexcept
    {
        references_ = 1;
        funcs_ = binding.deviceFuncs;
        hDevice_.pDrvPrivate = binding.hDrvDevice;
        hostDevice_ = binding.hostDevice;
        parentDevice_ = binding.parentDevice;

        /* The context holds a reference on its device, which is what D3D11
         * does: GetDevice hands out a counted reference, and the pair is a
         * cycle the application breaks by releasing both.  Mirroring that is
         * less surprising than inventing a weak reference the public
         * contract does not describe. */
        if (parentDevice_)
            parentDevice_->AddRef();
    }

    /* IUnknown */

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
            void **out) noexcept override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;

        if (IsEqualGUID(riid, IID_IUnknown)
                || IsEqualGUID(riid, IID_ID3D11DeviceChild)
                || IsEqualGUID(riid, IID_ID3D11DeviceContext))
        {
            *out = static_cast<ID3D11DeviceContext *>(this);
        }
        else if (IsEqualGUID(riid, IID_IWineD3D11HostObject))
        {
            *out = static_cast<IWineD3D11HostObject *>(this);
        }
        else
        {
            /* ID3D11DeviceContext1 and the later revisions are deliberately
             * not claimed.  Answering for an interface whose extra methods
             * are absent would be the mock success the invariants forbid. */
            return E_NOINTERFACE;
        }

        /* AddRef on this, not through *out.  The object inherits two
         * interfaces that each carry an IUnknown base, so casting the void*
         * back to IUnknown* would pick whichever subobject happened to be
         * stored -- correct today, and only because both vtables begin with
         * the same three slots.  There is one final overrider; calling it
         * directly does not depend on that coincidence. */
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override
    {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override
    {
        const LONG remaining = InterlockedDecrement(&references_);

        if (!remaining)
        {
            ID3D11Device *device = parentDevice_;

            /* Freed before the device is released: the device may be holding
             * the last reference to the module, so touching this object
             * afterwards would be a use-after-free of our own code. */
            HeapFree(GetProcessHeap(), 0, this);
            if (device)
                device->Release();
        }
        return static_cast<ULONG>(remaining);
    }

    /* IWineD3D11HostObject */

    HRESULT STDMETHODCALLTYPE GetHostHandle(
            WineD3D11HostHandle *handle) noexcept override
    {
        if (!handle)
            return E_POINTER;

        handle->size = sizeof(*handle);
        handle->kind = WINE_D3D11_HOST_KIND_CONTEXT;
        /* The immediate context's handle *is* the device handle; see the
         * header.  Reporting it is what lets the device recognise its own
         * context without a second convention. */
        handle->pDrvPrivate = hDevice_.pDrvPrivate;
        handle->device = hostDevice_;
        return S_OK;
    }

    /* ID3D11DeviceChild */

    void STDMETHODCALLTYPE GetDevice(ID3D11Device **device) noexcept override
    {
        if (!device)
            return;
        *device = parentDevice_;
        if (parentDevice_)
            parentDevice_->AddRef();
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *dataSize,
            void *data) noexcept override
    {
        /* No private-data store, and saying so beats reporting an empty one:
         * DXGI_ERROR_NOT_FOUND is what a real object returns for a GUID that
         * was never set, and it is true of every GUID here. */
        (void)data;
        if (dataSize)
            *dataSize = 0;
        return DXGI_ERROR_NOT_FOUND;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT,
            const void *) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedPrivateData,
                "d3d11context: private data is not stored by this milestone; "
                "debug object names and similar annotations are refused "
                "rather than silently dropped.\n");
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID,
            const IUnknown *) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedPrivateData,
                "d3d11context: private data is not stored by this milestone; "
                "debug object names and similar annotations are refused "
                "rather than silently dropped.\n");
        return E_NOTIMPL;
    }

    /* ID3D11DeviceContext -- constant buffers, resources, samplers.
     *
     * pfnXsSetConstantBuffers, pfnXsSetShaderResources and pfnXsSetSamplers
     * are all still PFNWINE_D3D11DDI_UNDECLARED_CB, so none of the eighteen
     * methods below can call anything. */

    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT, UINT,
            ID3D11Buffer *const *) noexcept override
    { reportConstantBuffers(); }

    void STDMETHODCALLTYPE PSSetShaderResources(UINT, UINT,
            ID3D11ShaderResourceView *const *) noexcept override
    { reportShaderResources(); }

    void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader *shader,
            ID3D11ClassInstance *const *instances,
            UINT instanceCount) noexcept override
    { setShader(funcs_->pfnPsSetShader, shader, instances, instanceCount); }

    void STDMETHODCALLTYPE PSSetSamplers(UINT, UINT,
            ID3D11SamplerState *const *) noexcept override
    { reportSamplers(); }

    void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader *shader,
            ID3D11ClassInstance *const *instances,
            UINT instanceCount) noexcept override
    { setShader(funcs_->pfnVsSetShader, shader, instances, instanceCount); }

    void STDMETHODCALLTYPE DrawIndexed(UINT, UINT, INT) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedIndexedDraw,
                "d3d11context: pfnDrawIndexed is not a promoted slot, so "
                "indexed draws are dropped rather than issued.\n");
    }

    void STDMETHODCALLTYPE Draw(UINT vertexCount,
            UINT startVertexLocation) noexcept override
    {
        if (!funcs_->pfnDraw)
        {
            reportMissingSlot();
            return;
        }
        funcs_->pfnDraw(hDevice_, vertexCount, startVertexLocation);
    }

    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource *, UINT, D3D11_MAP, UINT,
            D3D11_MAPPED_SUBRESOURCE *mapped) noexcept override
    {
        /* The seven Map slots are promoted, and the host still cannot call
         * one: their D3D10_DDI_MAP argument is a transport typedef to INT
         * whose enumerator values the specification does not publish
         * (wine_d3d11ddi.h:2023), and no MIT source in the tree pins them the
         * way the driver pins the topology values.  Passing the public
         * D3D11_MAP value would be inventing the mapping.
         *
         * There is a route that needs no such constant: the driver's
         * ID3D11On12DDIDevice sub-object carries WriteToSubresource and
         * ReadFromSubresource, which move data by subresource index and box.
         * That is where uploads and readback belong until the enumeration is
         * sourced. */
        if (mapped)
            ZeroMemory(mapped, sizeof(*mapped));
        wineD3D11DiagReportOnce(&reportedMap,
                "d3d11context: Map is refused because the DDI's map-type "
                "enumeration has no published values; use the sub-object's "
                "WriteToSubresource/ReadFromSubresource path instead.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }

    void STDMETHODCALLTYPE Unmap(ID3D11Resource *resource,
            UINT subresource) noexcept override
    {
        /* Wired even though Map is not.  The slot's arguments are a handle
         * and an index, so nothing about it is unpublished, and an
         * application that obtained a mapping by some other route must be
         * able to end it. */
        void *handle;

        if (!funcs_->pfnResourceUnmap)
        {
            reportMissingSlot();
            return;
        }
        if (FAILED(resolve(resource, WINE_D3D11_HOST_KIND_RESOURCE, &handle)))
            return;

        D3D10DDI_HRESOURCE hResource;
        hResource.pDrvPrivate = handle;
        funcs_->pfnResourceUnmap(hDevice_, hResource, subresource);
    }

    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT, UINT,
            ID3D11Buffer *const *) noexcept override
    { reportConstantBuffers(); }

    void STDMETHODCALLTYPE IASetInputLayout(
            ID3D11InputLayout *layout) noexcept override
    {
        void *handle;

        if (!funcs_->pfnIaSetInputLayout)
        {
            reportMissingSlot();
            return;
        }
        if (FAILED(resolveOptional(layout,
                WINE_D3D11_HOST_KIND_ELEMENTLAYOUT, &handle)))
            return;

        D3D10DDI_HELEMENTLAYOUT hLayout;
        hLayout.pDrvPrivate = handle;
        funcs_->pfnIaSetInputLayout(hDevice_, hLayout);
    }

    void STDMETHODCALLTYPE IASetVertexBuffers(UINT startSlot, UINT bufferCount,
            ID3D11Buffer *const *buffers, const UINT *strides,
            const UINT *offsets) noexcept override
    {
        D3D10DDI_HRESOURCE handles[MAX_VERTEX_BUFFERS];
        UINT zeroes[MAX_VERTEX_BUFFERS];

        if (!funcs_->pfnIaSetVertexBuffers)
        {
            reportMissingSlot();
            return;
        }
        if (!bufferCount)
            return;
        if (bufferCount > MAX_VERTEX_BUFFERS
                || startSlot > MAX_VERTEX_BUFFERS - bufferCount)
        {
            reportRejectedBinding();
            return;
        }

        /* Resolved into the local array first, and the slot called only if
         * every handle resolved.  A partial bind would leave the input
         * assembler describing a frame the application did not ask for. */
        for (UINT index = 0; index < bufferCount; ++index)
        {
            void *handle;

            if (FAILED(resolveOptional(buffers ? buffers[index] : nullptr,
                    WINE_D3D11_HOST_KIND_RESOURCE, &handle)))
                return;
            handles[index].pDrvPrivate = handle;
            zeroes[index] = 0;
        }

        /* Null strides or offsets mean zero for every slot in the public API;
         * the DDI takes arrays either way. */
        funcs_->pfnIaSetVertexBuffers(hDevice_, startSlot, bufferCount,
                handles, strides ? strides : zeroes,
                offsets ? offsets : zeroes);
    }

    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer *, DXGI_FORMAT,
            UINT) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedIndexBuffer,
                "d3d11context: pfnIaSetIndexBuffer is not a promoted slot, so "
                "the index buffer is not bound.\n");
    }

    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT, UINT, UINT, INT,
            UINT) noexcept override
    { reportInstancedDraw(); }

    void STDMETHODCALLTYPE DrawInstanced(UINT, UINT, UINT,
            UINT) noexcept override
    { reportInstancedDraw(); }

    void STDMETHODCALLTYPE GSSetConstantBuffers(UINT, UINT,
            ID3D11Buffer *const *) noexcept override
    { reportConstantBuffers(); }

    void STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader *shader,
            ID3D11ClassInstance *const *instances,
            UINT instanceCount) noexcept override
    { setShader(funcs_->pfnGsSetShader, shader, instances, instanceCount); }

    void STDMETHODCALLTYPE IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY topology) noexcept override
    {
        if (!funcs_->pfnIaSetTopology)
        {
            reportMissingSlot();
            return;
        }
        funcs_->pfnIaSetTopology(hDevice_, ddiTopologyFromPublic(topology));
    }

    void STDMETHODCALLTYPE VSSetShaderResources(UINT, UINT,
            ID3D11ShaderResourceView *const *) noexcept override
    { reportShaderResources(); }

    void STDMETHODCALLTYPE VSSetSamplers(UINT, UINT,
            ID3D11SamplerState *const *) noexcept override
    { reportSamplers(); }

    void STDMETHODCALLTYPE Begin(ID3D11Asynchronous *) noexcept override
    { reportQuery(); }

    void STDMETHODCALLTYPE End(ID3D11Asynchronous *) noexcept override
    { reportQuery(); }

    HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous *, void *data,
            UINT dataSize, UINT) noexcept override
    {
        if (data && dataSize)
            ZeroMemory(data, dataSize);
        reportQuery();
        return DXGI_ERROR_UNSUPPORTED;
    }

    void STDMETHODCALLTYPE SetPredication(ID3D11Predicate *,
            BOOL) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedPredication,
                "d3d11context: pfnSetPredication is not a promoted slot, so "
                "predication is not applied.\n");
    }

    void STDMETHODCALLTYPE GSSetShaderResources(UINT, UINT,
            ID3D11ShaderResourceView *const *) noexcept override
    { reportShaderResources(); }

    void STDMETHODCALLTYPE GSSetSamplers(UINT, UINT,
            ID3D11SamplerState *const *) noexcept override
    { reportSamplers(); }

    void STDMETHODCALLTYPE OMSetRenderTargets(UINT viewCount,
            ID3D11RenderTargetView *const *views,
            ID3D11DepthStencilView *depthStencil) noexcept override
    {
        setRenderTargets(viewCount, views, depthStencil, 0, 0, nullptr,
                nullptr);
    }

    void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(
            UINT viewCount, ID3D11RenderTargetView *const *views,
            ID3D11DepthStencilView *depthStencil, UINT uavStartSlot,
            UINT uavCount, ID3D11UnorderedAccessView *const *uavs,
            const UINT *uavInitialCounts) noexcept override
    {
        setRenderTargets(viewCount, views, depthStencil, uavStartSlot,
                uavCount, uavs, uavInitialCounts);
    }

    void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState *state,
            const FLOAT blendFactor[4], UINT sampleMask) noexcept override
    {
        static const FLOAT defaultFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        void *handle;

        if (!funcs_->pfnSetBlendState)
        {
            reportMissingSlot();
            return;
        }
        if (FAILED(resolveOptional(state, WINE_D3D11_HOST_KIND_BLENDSTATE,
                &handle)))
            return;

        D3D10DDI_HBLENDSTATE hState;
        hState.pDrvPrivate = handle;
        /* A null blend factor means white in the public API. */
        funcs_->pfnSetBlendState(hDevice_, hState,
                blendFactor ? blendFactor : defaultFactor, sampleMask);
    }

    void STDMETHODCALLTYPE OMSetDepthStencilState(
            ID3D11DepthStencilState *state, UINT stencilRef) noexcept override
    {
        void *handle;

        if (!funcs_->pfnSetDepthStencilState)
        {
            reportMissingSlot();
            return;
        }
        if (FAILED(resolveOptional(state,
                WINE_D3D11_HOST_KIND_DEPTHSTENCILSTATE, &handle)))
            return;

        D3D10DDI_HDEPTHSTENCILSTATE hState;
        hState.pDrvPrivate = handle;
        funcs_->pfnSetDepthStencilState(hDevice_, hState, stencilRef);
    }

    void STDMETHODCALLTYPE SOSetTargets(UINT, ID3D11Buffer *const *,
            const UINT *) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedStreamOutput,
                "d3d11context: pfnSoSetTargets is not a promoted slot, so "
                "stream-output targets are not bound.\n");
    }

    void STDMETHODCALLTYPE DrawAuto() noexcept override
    {
        wineD3D11DiagReportOnce(&reportedDrawAuto,
                "d3d11context: pfnDrawAuto is not a promoted slot, so the "
                "draw is dropped.\n");
    }

    void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(ID3D11Buffer *,
            UINT) noexcept override
    { reportIndirectDraw(); }

    void STDMETHODCALLTYPE DrawInstancedIndirect(ID3D11Buffer *,
            UINT) noexcept override
    { reportIndirectDraw(); }

    void STDMETHODCALLTYPE Dispatch(UINT, UINT, UINT) noexcept override
    { reportDispatch(); }

    void STDMETHODCALLTYPE DispatchIndirect(ID3D11Buffer *,
            UINT) noexcept override
    { reportDispatch(); }

    void STDMETHODCALLTYPE RSSetState(
            ID3D11RasterizerState *state) noexcept override
    {
        void *handle;

        if (!funcs_->pfnSetRasterizerState)
        {
            reportMissingSlot();
            return;
        }
        if (FAILED(resolveOptional(state, WINE_D3D11_HOST_KIND_RASTERIZERSTATE,
                &handle)))
            return;

        D3D10DDI_HRASTERIZERSTATE hState;
        hState.pDrvPrivate = handle;
        funcs_->pfnSetRasterizerState(hDevice_, hState);
    }

    void STDMETHODCALLTYPE RSSetViewports(UINT,
            const D3D11_VIEWPORT *) noexcept override
    {
        /* Case 3 from the file header, and the only instance of it.
         * pfnSetViewports is promoted and callable -- tests/d3d11ddi_triangle.c
         * calls it with a null array -- but its D3D10_DDI_VIEWPORT argument is
         * an opaque type in the clean-room header, so this host cannot build
         * one.  Authoring that structure unblocks this method and nothing
         * else. */
        wineD3D11DiagReportOnce(&reportedViewportsOpaque,
                "d3d11context: viewports are not set because "
                "D3D10_DDI_VIEWPORT is not yet an authored structure; the "
                "slot is promoted but its argument cannot be constructed.\n");
    }

    void STDMETHODCALLTYPE RSSetScissorRects(UINT,
            const D3D11_RECT *) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedScissorRects,
                "d3d11context: pfnSetScissorRects is not a promoted slot, so "
                "scissor rectangles are not set.\n");
    }

    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource *, UINT, UINT,
            UINT, UINT, ID3D11Resource *, UINT,
            const D3D11_BOX *) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedCopyRegion,
                "d3d11context: pfnResourceCopyRegion is not a promoted slot, "
                "so the region copy is dropped.\n");
    }

    void STDMETHODCALLTYPE CopyResource(ID3D11Resource *destination,
            ID3D11Resource *source) noexcept override
    {
        void *destinationHandle;
        void *sourceHandle;

        if (!funcs_->pfnResourceCopy)
        {
            reportMissingSlot();
            return;
        }
        if (FAILED(resolve(destination, WINE_D3D11_HOST_KIND_RESOURCE,
                &destinationHandle)))
            return;
        if (FAILED(resolve(source, WINE_D3D11_HOST_KIND_RESOURCE,
                &sourceHandle)))
            return;

        D3D10DDI_HRESOURCE hDestination;
        D3D10DDI_HRESOURCE hSource;
        hDestination.pDrvPrivate = destinationHandle;
        hSource.pDrvPrivate = sourceHandle;
        funcs_->pfnResourceCopy(hDevice_, hDestination, hSource);
    }

    void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource *, UINT,
            const D3D11_BOX *, const void *, UINT, UINT) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedUpdateSubresource,
                "d3d11context: pfnResourceUpdateSubresourceUP is not a "
                "promoted slot, so the subresource update is dropped; the "
                "sub-object's WriteToSubresource is the available route.\n");
    }

    void STDMETHODCALLTYPE CopyStructureCount(ID3D11Buffer *, UINT,
            ID3D11UnorderedAccessView *) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedStructureCount,
                "d3d11context: pfnCopyStructureCount is not a promoted slot, "
                "so the structure count is not copied.\n");
    }

    void STDMETHODCALLTYPE ClearRenderTargetView(
            ID3D11RenderTargetView *view,
            const FLOAT colorRGBA[4]) noexcept override
    {
        void *handle;
        FLOAT color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        if (!funcs_->pfnClearRenderTargetView)
        {
            reportMissingSlot();
            return;
        }
        if (FAILED(resolve(view, WINE_D3D11_HOST_KIND_RENDERTARGETVIEW,
                &handle)))
            return;

        /* Copied rather than cast: the slot's declaration takes a non-const
         * FLOAT[4], and casting away the public method's const would be
         * undefined if the driver wrote through it. */
        if (colorRGBA)
        {
            for (int component = 0; component < 4; ++component)
                color[component] = colorRGBA[component];
        }

        D3D10DDI_HRENDERTARGETVIEW hView;
        hView.pDrvPrivate = handle;
        funcs_->pfnClearRenderTargetView(hDevice_, hView, color);
    }

    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(
            ID3D11UnorderedAccessView *, const UINT[4]) noexcept override
    { reportUnorderedAccess(); }

    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(
            ID3D11UnorderedAccessView *, const FLOAT[4]) noexcept override
    { reportUnorderedAccess(); }

    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView *,
            UINT, FLOAT, UINT8) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedDepthStencilView,
                "d3d11context: pfnClearDepthStencilView is not a promoted "
                "slot, so the depth-stencil clear is dropped.\n");
    }

    void STDMETHODCALLTYPE GenerateMips(
            ID3D11ShaderResourceView *) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedGenMips,
                "d3d11context: pfnGenMips is not a promoted slot, so mip "
                "levels are not generated.\n");
    }

    void STDMETHODCALLTYPE SetResourceMinLOD(ID3D11Resource *,
            FLOAT) noexcept override
    { reportResourceMinLOD(); }

    FLOAT STDMETHODCALLTYPE GetResourceMinLOD(
            ID3D11Resource *) noexcept override
    {
        reportResourceMinLOD();
        return 0.0f;
    }

    void STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource *, UINT,
            ID3D11Resource *, UINT, DXGI_FORMAT) noexcept override
    {
        wineD3D11DiagReportOnce(&reportedResolve,
                "d3d11context: pfnResourceResolveSubresource is not a "
                "promoted slot, so the resolve is dropped.\n");
    }

    void STDMETHODCALLTYPE ExecuteCommandList(ID3D11CommandList *commandList,
            BOOL restoreContextState) noexcept override
    {
        void *handle;

        if (!funcs_->pfnCommandListExecute)
        {
            reportMissingSlot();
            return;
        }
        if (FAILED(resolve(commandList, WINE_D3D11_HOST_KIND_COMMANDLIST,
                &handle)))
            return;

        D3D11DDI_HCOMMANDLIST hCommandList;
        hCommandList.pDrvPrivate = handle;
        funcs_->pfnCommandListExecute(hDevice_, hCommandList);

        /* RestoreContextState is the runtime's promise to put back the state
         * that was bound before the list ran, and restoring state requires
         * having tracked it.  This milestone tracks none, so the request is
         * reported rather than quietly ignored. */
        if (restoreContextState)
            wineD3D11DiagReportOnce(&reportedStateReadback,
                    "d3d11context: ExecuteCommandList was asked to restore "
                    "the previous context state, which requires state "
                    "tracking this milestone does not do; the state after "
                    "the call is whatever the command list left.\n");
    }

    void STDMETHODCALLTYPE HSSetShaderResources(UINT, UINT,
            ID3D11ShaderResourceView *const *) noexcept override
    { reportShaderResources(); }

    void STDMETHODCALLTYPE HSSetShader(ID3D11HullShader *shader,
            ID3D11ClassInstance *const *instances,
            UINT instanceCount) noexcept override
    { setShader(funcs_->pfnHsSetShader, shader, instances, instanceCount); }

    void STDMETHODCALLTYPE HSSetSamplers(UINT, UINT,
            ID3D11SamplerState *const *) noexcept override
    { reportSamplers(); }

    void STDMETHODCALLTYPE HSSetConstantBuffers(UINT, UINT,
            ID3D11Buffer *const *) noexcept override
    { reportConstantBuffers(); }

    void STDMETHODCALLTYPE DSSetShaderResources(UINT, UINT,
            ID3D11ShaderResourceView *const *) noexcept override
    { reportShaderResources(); }

    void STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader *shader,
            ID3D11ClassInstance *const *instances,
            UINT instanceCount) noexcept override
    { setShader(funcs_->pfnDsSetShader, shader, instances, instanceCount); }

    void STDMETHODCALLTYPE DSSetSamplers(UINT, UINT,
            ID3D11SamplerState *const *) noexcept override
    { reportSamplers(); }

    void STDMETHODCALLTYPE DSSetConstantBuffers(UINT, UINT,
            ID3D11Buffer *const *) noexcept override
    { reportConstantBuffers(); }

    void STDMETHODCALLTYPE CSSetShaderResources(UINT, UINT,
            ID3D11ShaderResourceView *const *) noexcept override
    { reportShaderResources(); }

    void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT, UINT,
            ID3D11UnorderedAccessView *const *,
            const UINT *) noexcept override
    { reportUnorderedAccess(); }

    void STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader *shader,
            ID3D11ClassInstance *const *instances,
            UINT instanceCount) noexcept override
    { setShader(funcs_->pfnCsSetShader, shader, instances, instanceCount); }

    void STDMETHODCALLTYPE CSSetSamplers(UINT, UINT,
            ID3D11SamplerState *const *) noexcept override
    { reportSamplers(); }

    void STDMETHODCALLTYPE CSSetConstantBuffers(UINT, UINT,
            ID3D11Buffer *const *) noexcept override
    { reportConstantBuffers(); }

    /* The Get* families.
     *
     * Each zeroes what it was given and reports once.  The public contract is
     * that these hand out counted references, so writing null is the only
     * answer that does not leave a caller releasing a pointer nobody
     * addrefed. */

    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT, UINT count,
            ID3D11Buffer **buffers) noexcept override
    { clearOut(buffers, count); }

    void STDMETHODCALLTYPE PSGetShaderResources(UINT, UINT count,
            ID3D11ShaderResourceView **views) noexcept override
    { clearOut(views, count); }

    void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader **shader,
            ID3D11ClassInstance **instances,
            UINT *instanceCount) noexcept override
    { getShader(shader, instances, instanceCount); }

    void STDMETHODCALLTYPE PSGetSamplers(UINT, UINT count,
            ID3D11SamplerState **samplers) noexcept override
    { clearOut(samplers, count); }

    void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader **shader,
            ID3D11ClassInstance **instances,
            UINT *instanceCount) noexcept override
    { getShader(shader, instances, instanceCount); }

    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT, UINT count,
            ID3D11Buffer **buffers) noexcept override
    { clearOut(buffers, count); }

    void STDMETHODCALLTYPE IAGetInputLayout(
            ID3D11InputLayout **layout) noexcept override
    { clearOut(layout, 1); }

    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT, UINT count,
            ID3D11Buffer **buffers, UINT *strides,
            UINT *offsets) noexcept override
    {
        clearOut(buffers, count);
        for (UINT index = 0; index < count; ++index)
        {
            if (strides)
                strides[index] = 0;
            if (offsets)
                offsets[index] = 0;
        }
    }

    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer **buffer,
            DXGI_FORMAT *format, UINT *offset) noexcept override
    {
        clearOut(buffer, 1);
        if (format)
            *format = DXGI_FORMAT_UNKNOWN;
        if (offset)
            *offset = 0;
    }

    void STDMETHODCALLTYPE GSGetConstantBuffers(UINT, UINT count,
            ID3D11Buffer **buffers) noexcept override
    { clearOut(buffers, count); }

    void STDMETHODCALLTYPE GSGetShader(ID3D11GeometryShader **shader,
            ID3D11ClassInstance **instances,
            UINT *instanceCount) noexcept override
    { getShader(shader, instances, instanceCount); }

    void STDMETHODCALLTYPE IAGetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY *topology) noexcept override
    {
        reportStateReadback();
        if (topology)
            *topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }

    void STDMETHODCALLTYPE VSGetShaderResources(UINT, UINT count,
            ID3D11ShaderResourceView **views) noexcept override
    { clearOut(views, count); }

    void STDMETHODCALLTYPE VSGetSamplers(UINT, UINT count,
            ID3D11SamplerState **samplers) noexcept override
    { clearOut(samplers, count); }

    void STDMETHODCALLTYPE GetPredication(ID3D11Predicate **predicate,
            BOOL *value) noexcept override
    {
        clearOut(predicate, 1);
        if (value)
            *value = FALSE;
    }

    void STDMETHODCALLTYPE GSGetShaderResources(UINT, UINT count,
            ID3D11ShaderResourceView **views) noexcept override
    { clearOut(views, count); }

    void STDMETHODCALLTYPE GSGetSamplers(UINT, UINT count,
            ID3D11SamplerState **samplers) noexcept override
    { clearOut(samplers, count); }

    void STDMETHODCALLTYPE OMGetRenderTargets(UINT count,
            ID3D11RenderTargetView **views,
            ID3D11DepthStencilView **depthStencil) noexcept override
    {
        clearOut(views, count);
        clearOut(depthStencil, 1);
    }

    void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(
            UINT rtvCount, ID3D11RenderTargetView **views,
            ID3D11DepthStencilView **depthStencil, UINT, UINT uavCount,
            ID3D11UnorderedAccessView **uavs) noexcept override
    {
        clearOut(views, rtvCount);
        clearOut(depthStencil, 1);
        clearOut(uavs, uavCount);
    }

    void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState **state,
            FLOAT blendFactor[4], UINT *sampleMask) noexcept override
    {
        clearOut(state, 1);
        if (blendFactor)
        {
            for (int component = 0; component < 4; ++component)
                blendFactor[component] = 1.0f;
        }
        if (sampleMask)
            *sampleMask = 0;
    }

    void STDMETHODCALLTYPE OMGetDepthStencilState(
            ID3D11DepthStencilState **state, UINT *stencilRef) noexcept override
    {
        clearOut(state, 1);
        if (stencilRef)
            *stencilRef = 0;
    }

    void STDMETHODCALLTYPE SOGetTargets(UINT count,
            ID3D11Buffer **targets) noexcept override
    { clearOut(targets, count); }

    void STDMETHODCALLTYPE RSGetState(
            ID3D11RasterizerState **state) noexcept override
    { clearOut(state, 1); }

    void STDMETHODCALLTYPE RSGetViewports(UINT *count,
            D3D11_VIEWPORT *viewports) noexcept override
    {
        /* The public contract lets viewports be null, in which case count is
         * a pure output.  Either way the answer is none. */
        (void)viewports;
        reportStateReadback();
        if (count)
            *count = 0;
    }

    void STDMETHODCALLTYPE RSGetScissorRects(UINT *count,
            D3D11_RECT *) noexcept override
    {
        reportStateReadback();
        if (count)
            *count = 0;
    }

    void STDMETHODCALLTYPE HSGetShaderResources(UINT, UINT count,
            ID3D11ShaderResourceView **views) noexcept override
    { clearOut(views, count); }

    void STDMETHODCALLTYPE HSGetShader(ID3D11HullShader **shader,
            ID3D11ClassInstance **instances,
            UINT *instanceCount) noexcept override
    { getShader(shader, instances, instanceCount); }

    void STDMETHODCALLTYPE HSGetSamplers(UINT, UINT count,
            ID3D11SamplerState **samplers) noexcept override
    { clearOut(samplers, count); }

    void STDMETHODCALLTYPE HSGetConstantBuffers(UINT, UINT count,
            ID3D11Buffer **buffers) noexcept override
    { clearOut(buffers, count); }

    void STDMETHODCALLTYPE DSGetShaderResources(UINT, UINT count,
            ID3D11ShaderResourceView **views) noexcept override
    { clearOut(views, count); }

    void STDMETHODCALLTYPE DSGetShader(ID3D11DomainShader **shader,
            ID3D11ClassInstance **instances,
            UINT *instanceCount) noexcept override
    { getShader(shader, instances, instanceCount); }

    void STDMETHODCALLTYPE DSGetSamplers(UINT, UINT count,
            ID3D11SamplerState **samplers) noexcept override
    { clearOut(samplers, count); }

    void STDMETHODCALLTYPE DSGetConstantBuffers(UINT, UINT count,
            ID3D11Buffer **buffers) noexcept override
    { clearOut(buffers, count); }

    void STDMETHODCALLTYPE CSGetShaderResources(UINT, UINT count,
            ID3D11ShaderResourceView **views) noexcept override
    { clearOut(views, count); }

    void STDMETHODCALLTYPE CSGetUnorderedAccessViews(UINT, UINT count,
            ID3D11UnorderedAccessView **uavs) noexcept override
    { clearOut(uavs, count); }

    void STDMETHODCALLTYPE CSGetShader(ID3D11ComputeShader **shader,
            ID3D11ClassInstance **instances,
            UINT *instanceCount) noexcept override
    { getShader(shader, instances, instanceCount); }

    void STDMETHODCALLTYPE CSGetSamplers(UINT, UINT count,
            ID3D11SamplerState **samplers) noexcept override
    { clearOut(samplers, count); }

    void STDMETHODCALLTYPE CSGetConstantBuffers(UINT, UINT count,
            ID3D11Buffer **buffers) noexcept override
    { clearOut(buffers, count); }

    void STDMETHODCALLTYPE ClearState() noexcept override
    {
        /* Resetting to the documented default state means issuing a bind to
         * every stage, and most of those slots are unpromoted.  A partial
         * reset is worse than none: the application would believe the
         * pipeline was clean. */
        wineD3D11DiagReportOnce(&reportedClearState,
                "d3d11context: ClearState is refused because most of the "
                "binding slots it would have to reset are unpromoted; a "
                "partial reset would leave stale state the application "
                "believes is gone.\n");
    }

    void STDMETHODCALLTYPE Flush() noexcept override
    {
        wineD3D11DiagReportOnce(&reportedFlush,
                "d3d11context: pfnFlush is not a promoted slot, so recorded "
                "work is not submitted here; submission happens when the "
                "caller's D3D12 queue is used.\n");
    }

    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() noexcept override
    {
        /* Truthful without a slot: this object is the immediate context. */
        return D3D11_DEVICE_CONTEXT_IMMEDIATE;
    }

    UINT STDMETHODCALLTYPE GetContextFlags() noexcept override
    {
        /* Zero is the specified value for an immediate context. */
        return 0;
    }

    HRESULT STDMETHODCALLTYPE FinishCommandList(BOOL,
            ID3D11CommandList **commandList) noexcept override
    {
        if (commandList)
            *commandList = nullptr;
        wineD3D11DiagReportOnce(&reportedDeferred,
                "d3d11context: FinishCommandList is unsupported on the "
                "immediate context, and deferred contexts are not yet "
                "created by this host.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

private:
    /* Resolution, wrapped so the kind and the device travel together. */
    HRESULT resolve(IUnknown *object, UINT kind, void **handle) noexcept
    {
        const HRESULT hr = wineD3D11HostResolveHandle(object, kind,
                hostDevice_, handle);

        if (FAILED(hr))
            reportRejectedBinding();
        return hr;
    }

    HRESULT resolveOptional(IUnknown *object, UINT kind,
            void **handle) noexcept
    {
        const HRESULT hr = wineD3D11HostResolveOptionalHandle(object, kind,
                hostDevice_, handle);

        if (FAILED(hr))
            reportRejectedBinding();
        return hr;
    }

    /* The six SetShader methods differ only in which slot they use and which
     * interface they take, and PFND3D10DDI_SETSHADER is one typedef over all
     * six, so they share one body. */
    void setShader(PFND3D10DDI_SETSHADER slot, IUnknown *shader,
            ID3D11ClassInstance *const *instances, UINT instanceCount) noexcept
    {
        void *handle;

        if (!slot)
        {
            reportMissingSlot();
            return;
        }
        if (instanceCount || instances)
        {
            wineD3D11DiagReportOnce(&reportedClassInstances,
                    "d3d11context: a shader was set with class instances, "
                    "which need pfnXsSetShaderWithIfaces; that slot is "
                    "unpromoted, so the shader is not bound rather than "
                    "bound without its interfaces.\n");
            return;
        }
        if (FAILED(resolveOptional(shader, WINE_D3D11_HOST_KIND_SHADER,
                &handle)))
            return;

        D3D10DDI_HSHADER hShader;
        hShader.pDrvPrivate = handle;
        slot(hDevice_, hShader);
    }

    /* Both OMSetRenderTargets overloads, because the DDI has one slot for
     * them and the three-argument form is the seven-argument one with no
     * unordered-access views. */
    void setRenderTargets(UINT viewCount,
            ID3D11RenderTargetView *const *views,
            ID3D11DepthStencilView *depthStencil, UINT uavStartSlot,
            UINT uavCount, ID3D11UnorderedAccessView *const *uavs,
            const UINT *uavInitialCounts) noexcept
    {
        D3D10DDI_HRENDERTARGETVIEW renderTargets[MAX_RENDER_TARGETS];
        D3D11DDI_HUNORDEREDACCESSVIEW
                unorderedAccess[MAX_UNORDERED_ACCESS_VIEWS];
        void *depthStencilHandle = nullptr;

        if (!funcs_->pfnSetRenderTargets)
        {
            reportMissingSlot();
            return;
        }
        if (viewCount > MAX_RENDER_TARGETS
                || uavCount > MAX_UNORDERED_ACCESS_VIEWS)
        {
            reportRejectedBinding();
            return;
        }

        for (UINT index = 0; index < viewCount; ++index)
        {
            void *handle;

            if (FAILED(resolveOptional(views ? views[index] : nullptr,
                    WINE_D3D11_HOST_KIND_RENDERTARGETVIEW, &handle)))
                return;
            renderTargets[index].pDrvPrivate = handle;
        }

        /* No promoted slot can produce a depth-stencil view -- the
         * create-view slots for it are unpromoted -- so a non-null one here
         * cannot have come from this host and will not resolve.  Reported
         * specifically, because "your object is foreign" is misleading when
         * the real reason is that we cannot make one yet. */
        if (depthStencil)
        {
            wineD3D11DiagReportOnce(&reportedDepthStencilView,
                    "d3d11context: a depth-stencil view was bound, but no "
                    "promoted slot can create one, so the render-target "
                    "binding is refused rather than issued without it.\n");
            return;
        }

        for (UINT index = 0; index < uavCount; ++index)
        {
            if (uavs && uavs[index])
            {
                reportUnorderedAccess();
                return;
            }
            unorderedAccess[index].pDrvPrivate = nullptr;
        }

        D3D10DDI_HDEPTHSTENCILVIEW hDepthStencil;
        hDepthStencil.pDrvPrivate = depthStencilHandle;

        /* ClearSlots, UAVRangeStart and UAVRangeSize are passed as zero.
         * The pinned driver marks all three UNREFERENCED_PARAMETER in
         * third_party/D3D11On12/src/view.cpp -- they are hints a runtime may
         * use to narrow what it rebinds, and it ignores them -- so zero is
         * not a guess at a meaning but the value that asks for nothing. */
        funcs_->pfnSetRenderTargets(hDevice_,
                viewCount ? renderTargets : nullptr, viewCount, 0,
                hDepthStencil, uavCount ? unorderedAccess : nullptr,
                uavInitialCounts, uavStartSlot, uavCount, 0, 0);
    }

    template<typename Interface>
    void clearOut(Interface **out, UINT count) noexcept
    {
        reportStateReadback();
        if (!out)
            return;
        for (UINT index = 0; index < count; ++index)
            out[index] = nullptr;
    }

    template<typename Shader>
    void getShader(Shader **shader, ID3D11ClassInstance **instances,
            UINT *instanceCount) noexcept
    {
        clearOut(shader, 1);
        if (instanceCount)
        {
            /* The public contract is that the caller's count is both an
             * input and an output here, so it is cleared rather than read. */
            clearOut(instances, *instanceCount);
            *instanceCount = 0;
        }
    }

    void reportConstantBuffers() noexcept
    {
        wineD3D11DiagReportOnce(&reportedConstantBuffers,
                "d3d11context: pfnXsSetConstantBuffers is not a promoted "
                "slot, so no constant buffer is bound for any stage.\n");
    }

    void reportShaderResources() noexcept
    {
        wineD3D11DiagReportOnce(&reportedShaderResources,
                "d3d11context: pfnXsSetShaderResources is not a promoted "
                "slot, so no shader resource view is bound.\n");
    }

    void reportSamplers() noexcept
    {
        wineD3D11DiagReportOnce(&reportedSamplers,
                "d3d11context: pfnXsSetSamplers is not a promoted slot, so "
                "no sampler is bound.\n");
    }

    void reportInstancedDraw() noexcept
    {
        wineD3D11DiagReportOnce(&reportedInstancedDraw,
                "d3d11context: the instanced draw slots are not promoted, so "
                "the draw is dropped.\n");
    }

    void reportIndirectDraw() noexcept
    {
        wineD3D11DiagReportOnce(&reportedIndirectDraw,
                "d3d11context: the indirect draw slots are not promoted, so "
                "the draw is dropped.\n");
    }

    void reportDispatch() noexcept
    {
        wineD3D11DiagReportOnce(&reportedDispatch,
                "d3d11context: pfnDispatch and pfnDispatchIndirect are not "
                "promoted slots, so compute work is dropped.\n");
    }

    void reportQuery() noexcept
    {
        wineD3D11DiagReportOnce(&reportedQuery,
                "d3d11context: the query slots are not promoted, so queries "
                "neither begin, end, nor return data.\n");
    }

    void reportUnorderedAccess() noexcept
    {
        wineD3D11DiagReportOnce(&reportedUnorderedAccess,
                "d3d11context: the unordered-access view slots are not "
                "promoted, and no promoted slot can create such a view, so "
                "the call is refused.\n");
    }

    void reportResourceMinLOD() noexcept
    {
        wineD3D11DiagReportOnce(&reportedResourceMinLOD,
                "d3d11context: pfnSetResourceMinLOD is not a promoted slot, "
                "so the resource minimum LOD is neither set nor reported.\n");
    }

    void reportStateReadback() noexcept
    {
        wineD3D11DiagReportOnce(&reportedStateReadback,
                "d3d11context: this host does not track bound pipeline "
                "state, and the DDI has no getters, so every Get* method "
                "reports nothing bound.\n");
    }

    void reportMissingSlot() noexcept
    {
        wineD3D11DiagReportOnce(&reportedSlotMissing,
                "d3d11context: the driver left a device function slot null "
                "that this host needs; the call is dropped rather than made "
                "through a null pointer.\n");
    }

    void reportRejectedBinding() noexcept
    {
        wineD3D11DiagReportOnce(&reportedBindingRejected,
                "d3d11context: a binding call was refused because one of its "
                "objects could not be resolved to a driver handle; see the "
                "d3d11host report above for which check failed.\n");
    }

    volatile LONG references_;
    D3DWDDM2_6DDI_DEVICEFUNCS *funcs_;
    D3D10DDI_HDEVICE hDevice_;
    void *hostDevice_;
    ID3D11Device *parentDevice_;
};

}

extern "C" HRESULT wineD3D11CreateImmediateContext(
        const WineD3D11ContextBinding *binding,
        ID3D11DeviceContext **context) noexcept
{
    if (!context)
        return E_POINTER;
    *context = nullptr;

    if (!binding || binding->size != sizeof(*binding) || !binding->deviceFuncs
            || !binding->hostDevice)
        return E_INVALIDARG;

    /* HeapAlloc, not new: this module links no C++ runtime, so there is no
     * operator new to call.  The zeroing matters because the object's vtable
     * pointers are written by the placement construction below and everything
     * else must start empty. */
    void *storage = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            sizeof(HostContext));
    if (!storage)
        return E_OUTOFMEMORY;

    HostContext *object = new (storage) HostContext();
    object->initialize(*binding);
    *context = object;
    return S_OK;
}
