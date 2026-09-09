# Relay12 DDI: Remaining Work & Roadmap

While the ABI layout checks, placeholder alias protections, and foundational memory padding scripts have been extensively integrated into the repository and CI, there are still several vital phases required to fully harden and implement the clean-room DDI proxy.

Below is the structured roadmap of what is yet to be done.

## 1. Struct Layout & ABI Hardening

**1.1. Compile-Time Static Asserts**
* **Status:** Done, and the original framing was wrong
* **What happened:** The action here was to migrate the runtime `check_size()`
  and `check_offset()` calls in `d3d11ddilayout.c` into compile-time failures.
  They were already compile-time failures: every size, alignment and field
  offset in `wine_d3d11ddi.h` has carried a `_Static_assert`/`static_assert`
  since the header was written, and `gen_ddi_layout.py --check` already
  required, in both directions, that every modelled field be asserted and every
  assertion be modelled. The runtime walk is a deliberate second pass, which
  the harness's own header comment explains.
* **What was actually missing:** facts that lived only in the runtime test and
  could be constant expressions. Two are now asserted at the declaration:
  `WINE_DDI_ASSERT_SAME_FIELD_TYPE` on the shader-cache addref/release pair and
  on the two sync-token slots, whose shared type no offset assertion can
  express and which the C++ arm of the test previously proved alone.
* **What stays at run time, and why:** escape-flag bit positions (a union
  initialiser is not a constant expression in C11), the write-through of
  `ppfnRetrieveSubObject`, callback callability, and the dirty-padding scan.
  Each is annotated in the test with the reason.

**1.2. Strict Padding Verification (`-Wpadded`)**
* **Status:** Done
* **Action:** `tests/d3d11ddipadding.c` compiles the DDI declarations alone
  under `-Wpadded -Werror` in both C and C++. Its own translation unit, because
  `d3d11ddilayout.c` pads deliberately to drive the dirty-memory trap and would
  need suppressions.
* **The padding:** exactly three structures pad, and all four gaps are now
  named and asserted — `D3D10DDIARG_CREATEDEVICE.WinePad0` at 76,
  `D3DDDICB_ESCAPE.WinePad0`/`WinePad1` at 12 and 28, and
  `D3DDDICB_SYNCTOKEN.WinePad0` at 12. Sizes stay 88/40/24, which is what makes
  naming the bytes a declaration change and not a layout one.
* **Why name them:** `docs/CLEANROOM-DDI.md` already required an offset
  assertion for padding, which anonymous bytes make impossible. A named member
  is also zeroed by an aggregate initialiser, not only by a struct-wide
  `memset`, so a host initialising the structure the ordinary way can no longer
  hand the driver bytes it never wrote.
* **How it stays honest:** `gen_ddi_layout.py` *derives* the gaps from the
  published member list rather than transcribing them, and `check_padding`
  requires one named pad at each derived gap, numbered in offset order, and no
  pad anywhere else. A pad invented at the wrong offset fails rather than being
  ratified by an identical edit on both sides.

**1.3. Calling Convention Stack Guards**
* **Status:** Done, as a tripwire rather than as a test that can fail
* **Action:** `CHECK_STACK` in `tests/d3d11ddilayout.c` reads `%rsp` on both
  sides of every call made through a DDI slot and checks it is unchanged and
  16-byte aligned.
* **Read this before reading it as coverage:** on Win64 x86_64 there is one
  calling convention. `__stdcall`, `__cdecl` and `__fastcall` all name it,
  arguments arrive in registers, and the caller owns the argument area, so the
  stack pointer cannot drift across these calls and no declaration this header
  could carry would make it drift. The guard passes unconditionally on the
  frozen contract. It exists for the case `wine_d3d11ddi.h` describes — an ABI
  that does distinguish conventions must re-derive the declarations — and to
  pin the assumption itself: if it ever stops holding, the claim at the top of
  the header is wrong and everything built on it needs re-reading.

## 2. Concurrency & Thread-Safety

*As detailed in `DDI-CONCURRENCY-TESTING.md`*

**2.1. ThreadSanitizer (TSAN) Integration**
* **Status:** Impossible for this target; replaced
* **Finding:** `clang: error: unsupported option '-fsanitize=thread' for target
  'x86_64-w64-windows-gnu'`. Compiler-rt ships no TSAN runtime for Windows and
  GCC no `libtsan` for mingw. Building the sources natively as ELF behind a
  fake `windows.h` was rejected: neither module is portable, so TSAN would be
  instrumenting a shim.
* **Replacement:** `scripts/check_shared_state.py`, which requires every
  namespace-scope mutable in `relay12-d3d11/*.cpp` to be an `INIT_ONCE`, a
  `volatile LONG` moved only through `Interlocked*`, or annotated as published
  through a named `INIT_ONCE` with the barrier ordered before every touch.

**2.2. Multi-threaded Stress Suite (`ddi_thread_stress.c`)**
* **Status:** Done, against the code that has shared state
* **Action:** 12 threads × 400 iterations over the three exported core entry
  points and all seven diagnostic latches, with a bounded join so a deadlock
  is a named failure rather than a hung runner. The CI step captures stderr and
  requires each latch to have reported exactly once.
* **Not the device function table.** 173 of its 178 slots are still
  non-callable placeholders, so a test hammering them would race its own stubs
  and pass regardless of the eventual implementation.

**2.3. GPU/CPU Memory Barrier Compliance**
* **Status:** Blocked on §3
* **Action:** Guarantee that asynchronous Wait/Signal logic appropriately syncs
  deferred command execution without prematurely firing state changes on the
  immediate context. Not writable yet: the sync-object slots it would test are
  among the 63 kernel-callback slots holding an offset and nothing else.

## 3. Core DDI Function Implementations

**3.1. Removing Placeholders**
* **Status:** Ongoing — 56 of 138 PFN typedefs promoted, covering 72 of 178 slots
* **Done:** the command-list family — `pfnAbandonCommandList`,
  `pfnCommandListExecute`,
  `pfnDestroyCommandList`, `pfnRecycleCommandList`, and
  `pfnRecycleDestroyCommandList`, plus `pfnCalcPrivateCommandListSize`,
  `pfnCreateCommandList`, and `pfnRecycleCreateCommandList`.
  The latter three are unblocked by the authored
  `D3D11DDIARG_CREATECOMMANDLIST` and `D3D11DDI_HRTCOMMANDLIST`.
* **Done:** the deferred-context creation family —
  `pfnCheckDeferredContextHandleSizes`, `pfnCalcDeferredContextHandleSize`,
  `pfnCalcPrivateDeferredContextSize`, `pfnCreateDeferredContext`, and
  `pfnRecycleCreateDeferredContext`. These are unblocked by the authored
  `D3D11DDI_HANDLETYPE`, `D3D11DDI_HANDLESIZE`,
  `D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE`, and
  `D3D11DDIARG_CREATEDEFERREDCONTEXT` declarations.
* **Done:** the resource creation and shared-resource opening family —
  `pfnCalcPrivateResourceSize`, `pfnCalcPrivateOpenedResourceSize`,
  `pfnCreateResource`, `pfnOpenResource`, and `pfnDestroyResource`. The
  `D3D10DDIARG_CREATERESOURCE`, `D3D11DDIARG_CREATERESOURCE`, and
  `D3D10DDIARG_OPENRESOURCE` layouts are independently modelled and the
  promoted signatures are exercised by the layout harness and a negative
  compile test.
* **Done:** the shader resource view and render target view creation family —
  `pfnCalcPrivateShaderResourceViewSize`, `pfnCreateShaderResourceView`,
  `pfnDestroyShaderResourceView`, `pfnCalcPrivateRenderTargetViewSize`,
  `pfnCreateRenderTargetView`, and `pfnDestroyRenderTargetView`. Depth-stencil
  and unordered-access views are not part of this family and remain
  unpromoted; per `docs/D3D11ON12-SKIPPABLE-ELEMENTS.md` the MVP path needs
  only the SRV/RTV pair. `D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW` has no
  published WDDM 2.0-named page of its own (see the header's provenance note);
  its fields are the cross-validated base `D3D10DDIARG_CREATERENDERTARGETVIEW`
  ones, and its type name follows the pinned driver's own call-site signature.
* **Done:** the vertex and pixel shader creation pair —
  `pfnCalcPrivateShaderSize`, `pfnCreateVertexShader`, `pfnCreatePixelShader`,
  and `pfnDestroyShader`. The latter two are shared by every shader stage, not
  only the two promoted here; geometry, hull, domain, and compute shader
  creation stay behind placeholders until their own argument types (stream
  output, tessellation) are authored. `D3D11_1DDIARG_STAGE_IO_SIGNATURES` is
  the other type these callbacks take, but only ever behind a pointer no
  promoted slot dereferences, so — per the resource group's precedent for
  helper types like `D3D10DDI_MIPINFO` — it stays an incomplete forward
  declaration rather than an unverified layout.
* **Done:** the base pipeline state creation family — blend state,
  depth-stencil state, rasterizer state, and sampler state each have their
  private-size, create, and destroy callbacks promoted. `pfnCreateSampler`
  is included explicitly, and the layout harness exercises all twelve slots
  with distinct state handles.
* **Done:** the element-layout, binding, and draw group — the callbacks that
  turn the objects above into a frame. `pfnCalcPrivateElementLayoutSize`,
  `pfnCreateElementLayout`, `pfnDestroyElementLayout`, `pfnIaSetInputLayout`,
  `pfnIaSetVertexBuffers`, `pfnIaSetTopology`, `pfnSetRenderTargets`,
  `pfnSetViewports`, `pfnSetBlendState`, `pfnSetDepthStencilState`,
  `pfnSetRasterizerState`, `pfnClearRenderTargetView`, and `pfnDraw`, plus
  `PFND3D10DDI_SETSHADER`. Fourteen typedefs, nineteen slots: the SetShader
  typedef covers all six stages, because its page gives one parameter list for
  all of them. That promotes `pfnGsSetShader`, `pfnHsSetShader`,
  `pfnDsSetShader` and `pfnCsSetShader` as a consequence of the shared type
  and not as a scope decision — *creation* for those stages still takes
  argument types this header has not authored, and stays behind placeholders.
  Three variances are recorded at the group in `wine_d3d11ddi.h`: the
  ClearRenderTargetView page transposes its parameter names against its own
  Syntax block, SetBlendState publishes an unnamed `const FLOAT[4]`, and
  `D3D10_DDI_PRIMITIVE_TOPOLOGY` publishes enumerator names with no values, so
  it is declared as a transport typedef and names no constant.
* **Done:** readback — `PFND3D10DDI_RESOURCEMAP`,
  `PFND3D10DDI_RESOURCEUNMAP`, and `PFND3D10DDI_RESOURCECOPY`. These three
  typedefs cover thirteen slots. In particular, the Dynamic*Map/Unmap family
  comes along with the shared map and unmap typedefs; that is a consequence of
  typedef-keyed promotion, not a scope choice. `D3D10_DDI_MAP` is a transport
  typedef with no named constants because its page publishes names without
  numeric values, while `D3D10DDI_MAPPED_SUBRESOURCE` is fully modelled because
  the frame harness reads `pData`, `RowPitch`, and `DepthPitch`.
* **Not promoted with that group, deliberately:** `PFNWDDM2_0DDI_FLUSH`'s
  WDDM 2.0-named page is a 404 and the base `PFND3D10DDI_FLUSH` page's
  one-parameter list cannot be attributed to the WDDM 2.0-named typedef the
  table holds. `PFND3D10DDI_SETSCISSORRECTS` would need `D3D10_DDI_RECT` and
  the frame does not require a scissor.
* **The mechanism the pilot established, and which every later promotion
  reuses:**
  * `PROMOTED_SLOTS` in `gen_ddi_layout.py` holds the promoted typedefs, and
    `--check` requires each to have an authored signature and every other slot
    to remain a placeholder alias. Both directions: a promotion that did not
    land fails, and so does one nobody recorded. Nothing else can see it — a
    promoted pointer and a placeholder pointer are both eight bytes at the same
    offset.
  * `tests/d3d11ddilayout.c` assigns a stub with the declared signature into
    each promoted slot and calls it through the table.
  * `tests/d3d11ddipromotednegative.c` must fail to compile, proving a promoted
    slot rejects the wrong arguments — the counterpart to
    `d3d11ddiplaceholdernegative.c`, and the only thing that catches a
    parameter list transcribed wrongly.
  * The table's size assertion stays at 1424 bytes, unchanged. That is the
    pilot's headline result.

**3.2. What gates the remaining 82 typedefs**

A slot can be promoted when every type in its parameter list is declared. That
makes the worklist a dependency order on structure groups, not a list of slots:

| Unblocked by | Slot families waiting on it |
| :--- | :--- |
| Command list handle — **done** | `pfnAbandonCommandList`, `pfnCommandListExecute`, `pfnDestroyCommandList`, `pfnRecycleCommandList`, `pfnRecycleDestroyCommandList` |
| `D3D11DDIARG_CREATECOMMANDLIST` — **done** | `pfnCalcPrivateCommandListSize`, `pfnCreateCommandList`, `pfnRecycleCreateCommandList` |
| `D3D11DDIARG_CREATEDEFERREDCONTEXT`, `D3D11DDI_HANDLESIZE` — **done** | `pfnCalcPrivateDeferredContextSize`, `pfnCreateDeferredContext`, `pfnRecycleCreateDeferredContext`, `pfnCheckDeferredContextHandleSizes`, `pfnCalcDeferredContextHandleSize` |
| Resource structures (`D3D10DDIARG_CREATERESOURCE`, `D3D11DDIARG_CREATERESOURCE`, `D3D10DDIARG_OPENRESOURCE`) — **done** | `pfnCalcPrivateResourceSize`, `pfnCalcPrivateOpenedResourceSize`, `pfnCreateResource`, `pfnOpenResource`, `pfnDestroyResource` |
| `D3D10_DDI_MAP`, `D3D10DDI_MAPPED_SUBRESOURCE` — **done** | all thirteen `pfn*ResourceMap`/`Unmap` slots and `pfnResourceCopy` — **done**; `pfnResourceCopyRegion`, `pfnResourceUpdateSubresourceUP`, `pfnDiscard`, and `pfnResourceConvert*` still require their own argument types |
| SRV/RTV creation arguments (`D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW`, `D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW`) — **done** | `pfnCalcPrivateShaderResourceViewSize`, `pfnCreateShaderResourceView`, `pfnDestroyShaderResourceView`, `pfnCalcPrivateRenderTargetViewSize`, `pfnCreateRenderTargetView`, `pfnDestroyRenderTargetView` |
| DSV/UAV creation arguments | every remaining `pfnCalcPrivate*ViewSize`/`pfnCreate*View`/`pfnDestroy*View`, `pfnClearRenderTargetView`, `pfnClearDepthStencilView`, `pfnClearView`, `pfnClearUnorderedAccessView*` |
| Vertex/pixel shader creation (`D3D10DDI_H(RT)SHADER`) — **done** | `pfnCalcPrivateShaderSize`, `pfnCreateVertexShader`, `pfnCreatePixelShader`, `pfnDestroyShader` |
| Stream-output and tessellation shader structures | `pfnCreateGeometryShader`, `pfnCalcPrivateGeometryShaderWithStreamOutput`, `pfnCreateGeometryShaderWithStreamOutput`, `pfnCreateHullShader`, `pfnCreateDomainShader`, `pfnCreateComputeShader`, `pfnCalcPrivateTessellationShaderSize`, `pfnCreateElementLayout`, the `pfn*SetShaderWithIfaces` family, `pfnRetrieveShaderComment`, `pfnAssignDebugBinary` |
| State structures (blend, depth-stencil, rasterizer, sampler) — **done** | `pfnSetBlendState`, `pfnSetDepthStencilState`, `pfnSetRasterizerState` — **done**; `pfnPsSetSamplers` and the rest of the `pfn*SetSamplers` family remain, an untextured frame not needing them |
| Element layout and input assembly — **done** | `pfnCalcPrivateElementLayoutSize`, `pfnCreateElementLayout`, `pfnDestroyElementLayout`, `pfnIaSetInputLayout`, `pfnIaSetVertexBuffers`, `pfnIaSetTopology` |
| Depth-stencil and unordered-access view *handles* — **done** | `pfnSetRenderTargets`. The handles are declared; nothing promoted can create either kind of view, which is the MVP state |
| Shader binding (`D3D10DDI_HSHADER`) — **done** | `pfnVsSetShader`, `pfnPsSetShader`, `pfnGsSetShader`, `pfnHsSetShader`, `pfnDsSetShader`, `pfnCsSetShader` — one typedef |
| Query structures and `D3D10DDI_QUERY` | `pfnCalcPrivateQuerySize`, `pfnCreateQuery`, `pfnDestroyQuery`, `pfnQueryBegin`, `pfnQueryEnd`, `pfnQueryGetData`, `pfnSetPredication` |
| Tiled-resource structures | `pfnUpdateTileMappings`, `pfnCopyTileMappings`, `pfnCopyTiles`, `pfnUpdateTiles`, `pfnTiledResourceBarrier`, `pfnGetMipPacking`, `pfnResizeTilePool` |
| GetCaps group (`D3D11DDI_THREADING_CAPS`, `D3D11DDI_3DPIPELINELEVEL`) | nothing in this table directly, but it is what tells the runtime the command-list slots above may be called at all |
| DXGI DDI interop (`DXGI_DDI_BASE_CALLBACKS`, `DXGI1_6_1_DDI_BASE_FUNCTIONS`) | `pfnCheckDirectFlipSupport`, `pfnQueryScanoutCaps`, `pfnPrepareScanoutTransformation` |

Two slots can never be promoted from the current public set:
`PFND3DWDDM2_2DDI_SHADERCACHE_GET_VALUE_CB` and
`PFND3DWDDM2_6DDI_QUERY_SCANOUT_CAPS_CB` are named by the core-layer callback
structure's page but linked from nowhere, and both surfaces 404.
`pfnRecycleDestroyCommandList` has no page of its own either; it is promoted
only because the DestroyCommandList page states the two members may take one
implementation.

**3.3. WDDM Interface Translation**
* **Status:** Ongoing
* **Action:** Finalize the state/pipeline mapping mechanisms across `hs`, `ds`,
  `ps`, and `vs` shaders natively against macOS translation limits.

**3.4. CI & End-to-End Test Harness**
* **Status:** Framework Drafted
* **Action:** The fundamental E2E integration boundaries are now tested using `tests/e2e_d3d11_harness.cpp`. The CI triggers a full compute pipeline dispatch (`D3D11 -> relay12 -> D3D12 -> GPTK4 -> Metal`) locally inside MinGW, proving strict deterministic output. As DDI translation implementations for `hs`, `ds`, `ps`, and `vs` are solidified, add rigorous asserting functions in the harness to lock down regressions.

**3.5. Context handle types: closed, and not as expected**
* A deferred context has no handle type of its own. The runtime reuses
  `D3D10DDI_HDEVICE`, passed as the `hDrvContext` member of
  `D3D11DDIARG_CREATEDEFERREDCONTEXT`. The only new context handle was
  `D3D11DDI_HCOMMANDLIST`, now declared. Nobody may add a
  `D3D11DDI_HDEFERREDCONTEXT`: no specification names one, so it would be an
  invented type behind a provenance block, which rule 1 forbids.

---
*Created as a living document to track the DDI Cleanroom implementation progress.*
