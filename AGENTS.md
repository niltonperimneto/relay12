# AGENTS.md — D3D11On12 Architecture, Invariants & Agent Guidelines

> **Target Audience:** Autonomous Coding Agents, Subagents, and Systems Engineers  
> **Subsystem:** D3D11On12 (Direct3D 11 on Direct3D 12 Translation Layer & DDI Host)  
> **Components:** PE Router (`d3d11shim`), Core Boundary (`d3d11on12core`), Clean-Room DDI Host (`relay12-d3d11/ddi`)  

---

## 1. Mission and Subsystem Overview

This subsystem implements the host environment and driver boundary required to execute Microsoft's open-source D3D11On12 user-mode driver in Wine-compatible environments.

### Core Objectives
1. **PE Router (`d3d11shim.cpp`):** Presents the standard `d3d11.dll` export surface. It forwards standard device creation calls (`D3D11CreateDevice` and `D3D11CreateDeviceAndSwapChain`) to the platform's native D3D11 driver (`d3d11mt.dll`), while routing `D3D11On12CreateDevice` to the core boundary (`d3d11on12core.dll`).
2. **Core Boundary (`d3d11on12core.cpp`):** Validates caller-supplied `ID3D12Device` and `ID3D12CommandQueue` instances, confirms command queue types, enforces two-tier COM identity, and prepares the translation layer context.
3. **Clean-Room WDDM DDI Host (`relay12-d3d11/ddi/wine_d3d11ddi.h`):** Declares the driver callback interfaces (`D3DWDDM2_6DDI_DEVICEFUNCS`, `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS`, `D3D10DDIARG_CREATEDEVICE`) from public documentation without copying proprietary Windows Driver Kit headers.
4. **Standalone Modularity:** Designed to operate as an independent component with its own compilation rules, static audits, test harnesses, and documentation.

---

## 2. Invariants and Constraints

All modifications to this codebase must preserve the following architectural invariants:

### 2.1 Natural 8-Byte Alignment (`/Zp8`)
- Direct3D DDI structures assume standard 64-bit Windows 8-byte natural alignment.
- **`#pragma pack` is strictly prohibited** under [`relay12-d3d11/ddi`](relay12-d3d11/ddi). The presence of packing pragmas is actively checked by [`scripts/check_ddi_header.py`](scripts/check_ddi_header.py).

### 2.2 Fail-Closed Routing Semantics
- **No Mock Success:** The router and core boundary must never return `S_OK` with mock, uninitialized, or partial COM interface pointers.
- **Documented Error Codes:** If dependencies (`d3d11mt.dll`, `d3d11on12core.dll`) or inputs are invalid, entry points must return documented Direct3D error codes (specifically `DXGI_ERROR_UNSUPPORTED`, `0x887a0004`). Non-standard Win32 or NT status codes must be avoided to ensure caller fallback logic functions properly.
- **Output Pointer Zeroing:** On any failure path, all caller-supplied output pointers (`ppDevice`, `ppImmediateContext`) must be set to `NULL` before returning.
- **Device Ownership:** D3D11On12 must submit rendering commands through the caller's supplied `ID3D12Device` and direct `ID3D12CommandQueue`. It must not create an independent or unmanaged device instance.

### 2.3 Strict Interface Acquisition and Identity
- **The `strictResult()` Funnel:** Every `QueryInterface` and `GetDevice` call in [`relay12-d3d11`](relay12-d3d11) must be wrapped in `strictResult()`. If an interface query returns `S_OK` but leaves the output pointer null, `strictResult()` treats the acquisition as failed, preventing subsequent null pointer dereferences. Direct calls that bypass this funnel are rejected by [`scripts/check_interface_acquisition.py`](scripts/check_interface_acquisition.py).
- **Two-Tier Identity Comparison:** To verify that a command queue was created by the provided device, typed `ID3D12Device` pointers are compared first. An `IUnknown` identity query is performed only if typed pointers differ.

### 2.4 Clean-Room Boundary and Licensing
- **No Proprietary WDK Headers:** Proprietary Windows Driver Kit headers (`d3d10umddi.h`, `d3d11umddi.h`, `dxgiddi.h`, `d3dkmthk.h`) must not be added to or vendored in this repository.
- **Dual-Source Documentation Validation:** All DDI structures declared in [`relay12-d3d11/ddi/wine_d3d11ddi.h`](relay12-d3d11/ddi/wine_d3d11ddi.h) must be authored from public Microsoft documentation, cross-validated against the GitHub markdown documentation mirror, and documented with provenance comment blocks.
- **License Isolation:**
  - Router, Core, Tests, and Scripts: **GPL-3.0-only**
  - Submodules (`third_party/D3D11On12`, `third_party/D3D12TranslationLayer`, `third_party/DirectX-Headers`): **MIT**

### 2.5 Third-Party Trees Are Never Edited In Place
The pinned submodules stay byte-identical to upstream. Every change to them is
a patch file in [`patches/d3d11on12/`](patches/d3d11on12) or
[`patches/dtl/`](patches/dtl), applied by `scripts/prepare-d3d11on12-source.sh`
and `scripts/prepare-dtl-source.sh` into a throwaway clone. Those scripts
assert the expected upstream revision first and refuse to run against a
different one, so editing a submodule directly is both undone by the next CI
run and invisible in review. A patch needs a body explaining *why* upstream is
wrong for this target, not just what it changes.

### 2.6 Function Tables Use Designated Initializers
Any table of function pointers crossing a version or backend boundary —
notably both `d3d11_backend_ops` tables in the Wine frontend patches — must be
initialized by member name, never positionally.

This is not style. A positional table silently rebinds every entry after an
inserted member: adding `create_texture2d`/`destroy_texture2d` after
`destroy_buffer` shifted the WineD3D table by one pair and bound
`destroy_vertex_shader` to `wined3d_backend_set_vertex_shader`, so releasing a
vertex shader on the ordinary WineD3D path would have called a binding
function through a destroy signature. Enforced by
`test_positional_wined3d_backend_table_is_rejected`.

Members a backend does not implement stay **absent**, not `NULL`-padded: C
zero-initializes them, and a null op is how a backend says "use the WineD3D
path", which the call sites already test for.

### 2.7 Resource Destruction Is Idempotent
Destroy entry points reach the driver exactly once and are a no-op afterwards.
A repeat returns `S_OK` and leaves the caller's structure inert; it must not
return `E_INVALIDARG` for a stale handle. Rejecting one would turn a late
destroy after device teardown into a failure rather than the double-free guard
it exists to be, and every resource kind must agree. The contract is stated in
[`docs/D3D11ON12.md`](docs/D3D11ON12.md) as "destruction reaching the driver
once, idempotent afterwards".

Using a destroyed handle is the opposite case and *is* rejected with
`E_INVALIDARG`, before any DDI dispatch.

### 2.8 Shared State Is Guarded and Annotated
Process-wide mutable state in [`relay12-d3d11`](relay12-d3d11) must be
initialized through `InitOnceExecuteOnce` and carry an annotation naming the
`INIT_ONCE` that guards it. Unannotated or unguarded shared state is rejected
by [`scripts/check_shared_state.py`](scripts/check_shared_state.py), which
follows a wrapped `InitOnceExecuteOnce` through its helper.

---

## 3. Subsystem Architecture

See [`README.md`](README.md) for the Mermaid architecture flowchart and
[`docs/D3D11ON12.md`](docs/D3D11ON12.md) for the detailed text-form pipeline.
The summary: Application → Router (`d3d11shim.dll`) → Core Boundary
(`d3d11on12core.dll`) → Clean-Room DDI Host → Microsoft D3D11On12 Driver →
D3D12TranslationLayer → Caller's `ID3D12Device`.

### Component Breakdown

Paths below are repo-relative. Do not reintroduce absolute `file:///Users/...`
links: they break for every other checkout and on GitHub's web view.

| Directory | Responsibilities |
| :--- | :--- |
| [`relay12-d3d11/`](relay12-d3d11) | Implementation: `d3d11shim.cpp`/`.h`, `d3d11on12core.cpp`/`.h`, `wine_d3d11_diag.h`, `wine_d3d11ddi_negotiate.h`, `ddi/wine_d3d11ddi.h`, and the `.def` export contracts. |
| [`scripts/`](scripts) | Fourteen `check_*.py` gates plus `gen_ddi_layout.py` and `inventory_dtl_portability.py`; the `prepare-*.sh` scripts that materialize the patched third-party trees and the SDK overlay; `package-d3d11on12-source.sh` and `build-wine-d3d11-host.sh`; the `run-*.py` hardware and PEAK smoke runners and `summarize_overhead.py`. See §5 for which run with no arguments. |
| [`tests/`](tests) | ~50 files: the Python gate suite (`test_ci_gates.py`), the DDI layout and negotiation tests, the mock driver (`d3d11on12mockdriver.c`) and the lifecycle suite that drives it (`d3d11on12openadapter.c`), per-group promoted-slot negative tests, `compat/` unit tests, and probes. [`docs/TESTS.md`](docs/TESTS.md) is the inventory and the rationale. |
| [`docs/`](docs) | [`D3D11ON12.md`](docs/D3D11ON12.md) (design, readiness, milestones — source of truth for what is done), [`CLEANROOM-DDI.md`](docs/CLEANROOM-DDI.md) (DDI authoring roadmap), [`TESTS.md`](docs/TESTS.md) (test inventory, strategy and concurrency), [`DDI-REMAINING-ROADMAP.md`](docs/DDI-REMAINING-ROADMAP.md) (remaining DDI groups), [`PERFORMANCE-RESEARCH-ROADMAP.md`](docs/PERFORMANCE-RESEARCH-ROADMAP.md) (performance plan), [`RELAY12-IMPLEMENTATION-PLAN.md`](docs/RELAY12-IMPLEMENTATION-PLAN.md) (Wine patch milestones), [`FIRST-FRAME-VALIDATION.md`](docs/FIRST-FRAME-VALIDATION.md), [`WRAPPED-RESOURCE-VALIDATION.md`](docs/WRAPPED-RESOURCE-VALIDATION.md), [`dtl-portability-baseline.json`](docs/dtl-portability-baseline.json) (golden record for `inventory_dtl_portability.py`), and [`validation/`](docs/validation) (dated validation evidence, including the MSYNC investigation). |
| [`third_party/`](third_party) | Pinned submodules (MIT): `D3D11On12`, `D3D12TranslationLayer`, `DirectX-Headers`. Never edited in place — see §2.5. |
| [`patches/`](patches) | The reviewable portability series. `patches/d3d11on12/` and `patches/dtl/` are applied to the pinned submodules by `scripts/prepare-*.sh`; `patches/*.patch` at the top level are the Wine frontend patches. |

---

## 4. Compilation and Toolchain Rules

### 4.1 MinGW-w64 Toolchain Configuration
- PE binaries are compiled with `x86_64-w64-mingw32-clang++` in C++17 mode. PR
  validation pins llvm-mingw 20240619 (the `ucrt` asset); it is Clang, not GCC,
  and rejects a number of constructs GCC accepted.
- Linkage against `libstdc++` and `libgcc_s` is prohibited to ensure runtime
  independence. PE binaries must import only `kernel32.dll` plus one C runtime:
  `msvcrt.dll` under an msvcrt-targeted toolchain, or the seven
  `api-ms-win-crt-*` stubs under a UCRT-targeted one. Nothing else — a graphics
  or `ntdll` import means an entry point stopped being resolved at run time.
- Code must be compiled with `-fno-exceptions -fno-rtti`.
- Every compile in `validate-d3d11on12` uses `-Wall -Wextra -Werror`, so a
  warning is a build failure.

#### Clang rejections to expect
The toolchain moved from apt's GCC to llvm-mingw, and Clang refuses a number of
things GCC accepted. These are the classes that have actually broken this tree,
and are worth checking before pushing because none of them can be reproduced
locally (§5):

| Symptom | Cause |
| :--- | :--- |
| `member access into incomplete type` in a template | Clang binds non-dependent parts of a template body at parse time. A method whose body touches a type only forward-declared at that point must be defined out of line, after the definition. |
| `discards qualifiers` | A `const` DDI pointer assigned to a non-`const` variable. Fix the variable; the qualifier is upstream's and correct. |
| `missing exception specification '__attribute__((nothrow))'` | mingw-w64 expands `STDMETHOD` with `COM_DECLSPEC_NOTHROW` but `STDMETHODIMP_` without it, so an out-of-class definition needs explicit `noexcept`. |
| `unused-but-set-variable` on an entry-point macro | `OPEN_TRYCATCH` declares a result the matching `CLOSE_TRYCATCH` reads; an unpaired macro leaves it set and unread. |
| tautological comparison | A bound that is unreachable on a 64-bit target. Prefer `static_assert` so the guard is still checked and a 32-bit target fails loudly. |
| `unknown warning option` | A `-Wno-*` name that exists in GCC but not Clang. Suppressions must be spelled for the pinned compiler. |
| unknown SAL macro, e.g. `_Maybenull_` | The WDK overlay annotates fields with SAL that this include chain never defines. Shim it empty next to the existing `__in`/`__nullterminated` shims in the port series. |

### 4.2 PE Export Tables
[`scripts/check_pe_audit.py`](scripts/check_pe_audit.py) enforces exact export tables and ordinals:

#### `d3d11shim.dll` (Router)
| Ordinal | Symbol | Purpose |
| ---: | :--- | :--- |
| 1 | `D3D11CreateDevice` | Forwards to `d3d11mt.dll` |
| 2 | `D3D11CreateDeviceAndSwapChain` | Forwards to `d3d11mt.dll` |
| 3 | `D3D11On12CreateDevice` | Routes to `d3d11on12core.dll` |
| 4 | `WineD3D11ShimGetStatus` | Reports module status |

#### `d3d11on12core.dll` (Core Boundary)
Ordinals are part of the ABI and are never reused or renumbered. The
authoritative list is [`relay12-d3d11/d3d11on12core.def`](relay12-d3d11/d3d11on12core.def);
`scripts/check_pe_audit.py` compares the built DLL's whole ordinal table
against its own transcription of that `.def`, and a gate test pins the
transcription to the file, so this table is a reading aid and the `.def` is the
contract.

| Ordinal | Symbol | Purpose |
| ---: | :--- | :--- |
| 1 | `WineD3D11On12GetABIVersion` | Returns `WINE_D3D11ON12_ABI_VERSION` |
| 2 | `WineD3D11On12CreateDeviceV1` | Validated device creation entry point |
| 3 | `WineD3D11On12GetInterface` | Returns interface function table |
| 4 | `WineD3D11On12OpenAdapterV1` | Opens the driver and creates the DDI device |
| 5, 6 | `WineD3D11On12{Create,Destroy}BufferV1` | Owned buffer lifecycle |
| 7, 8 | `WineD3D11On12Set{VertexBuffers,IndexBuffer}V1` | Input-assembler buffer binding |
| 9-11 | `WineD3D11On12{Create,Destroy,Set}InputLayoutV1` | Element-layout lifecycle and binding |
| 12-16 | `WineD3D11On12{CreateVertex,CreatePixel,Destroy,SetVertex,SetPixel}Shader*V1` | Vertex and pixel shader lifecycle and binding |
| 17, 18 | `WineD3D11On12{Create,Destroy}Texture2DV1` | Owned Texture2D lifecycle |
| 19, 20 | `WineD3D11On12{Create,Destroy}RenderTargetViewV1` | Owned render-target-view lifecycle |
| 21-23 | `WineD3D11On12{SetRenderTarget,SetViewport,ClearRenderTarget}V1` | First-frame output state and clear |
| 24-26 | `WineD3D11On12{CopyTexture2D,MapTexture2D,UnmapTexture2D}V1` | Copy and staging readback |
| 27 | `WineD3D11On12CheckFrameSupportV1` | Experimental first-frame readiness check |
| 28, 29 | `WineD3D11On12{CreateWrappedTexture2D,SetWrappedOwnership}V1` | Wrapped Texture2D interop |
| 30, 31 | `WineD3D11On12{Create,Destroy}DeferredContextV1` | Deferred-context lifecycle |
| 32, 33 | `WineD3D11On12{Create,Destroy}CommandListV1` | Finish and destroy a command list |
| 34 | `WineD3D11On12ExecuteCommandListV1` | Execute a command list on the immediate context |
| 35-37 | `WineD3D11On12{CreateBlend,CreateDepthStencil,CreateRasterizer}StateV1` | Blend, depth-stencil and rasterizer state lifecycle |
| 38 | `WineD3D11On12DestroyPipelineStateV1` | Unified pipeline state destruction |
| 39-41 | `WineD3D11On12Set{Blend,DepthStencil,Rasterizer}StateV1` | Pipeline state binding |

### 4.3 Diagnostic Logging
Diagnostic logging is handled via [`relay12-d3d11/wine_d3d11_diag.h`](relay12-d3d11/wine_d3d11_diag.h):
1. Dynamically queries `__wine_dbg_output` from `ntdll.dll` via `GetProcAddress`.
2. Falls back to `OutputDebugStringA`.
3. Falls back to standard error via the C runtime.

Deduplication latches ensure repeated failure conditions log only once per process.

---

## 5. Agent Verification Protocol

### 5.1 What you can verify locally

Run these before every push. They need no toolchain, no Wine, and no network,
and they take seconds. All must exit `0`.

```bash
python3 -m unittest discover -s tests -p "test_*.py"   # 104 tests
python3 scripts/gen_ddi_layout.py --check              # 54 structures, 469 fields
python3 scripts/check_ddi_header.py                    # 21 declaration groups
python3 scripts/check_interface_acquisition.py         # strictResult() funnel
python3 scripts/check_shared_state.py                  # InitOnce annotations
python3 scripts/check_adapter_args.py                  # adapter-arg transcription
python3 scripts/check_secure_code.py relay12-d3d11     # refcount/alloc lints
```

The remaining gates need an artifact or a tree that only CI materializes, and
are listed here so their absence from the block above is not mistaken for them
not existing:

| Gate | Needs |
| :--- | :--- |
| `check_pe_audit.py MODULE…` | built `d3d11shim.dll` / `d3d11on12core.dll` |
| `check_wine_d3d11_backend.py WINE_TREE` | a patched WineCX checkout |
| `check_d3d11on12_port.py SOURCE_DIR` | the prepared D3D11On12 clone |
| `check_dtl_struct_return.py SOURCE_DIR` | the prepared DTL clone |
| `check_dtl_include_case.py SOURCE_DIR` | the prepared DTL clone |
| `check_cleanroom_isolation.py --overlay DIR` | the licensed SDK/WDK overlay |

> `check_secure_code.py` is currently wired into no workflow. It passes, and it
> exists specifically to catch probabilistic errors in generated code, so run it
> by hand until it is added to CI.

### 5.2 What you cannot verify locally — and what to do instead

**Nothing in this repository compiles on a developer Mac.** There is no MinGW
cross toolchain and no Wine, so the C and C++ sources, the DDI tests, the
patched DTL and D3D11On12 trees, and the entire Wine-executed suite exist only
inside `validate-d3d11on12`. Treat green local gates as necessary and far from
sufficient.

The consequence for how you work:

1. Run §5.1 first. It is cheap and catches transcription, layout and annotation
   errors without waiting on CI.
2. For anything that must actually compile, push the branch and read the job.
   Do not claim a compile fix is verified when it has not been compiled — say
   what was checked and what the push is for.
3. Pre-check what host tooling can reach. A header or macro question can often
   be settled with host `clang -fsyntax-only` on a reduced case, and a patch
   series can be applied to a throwaway clone of the pinned submodule to prove
   it still applies and produces the tree you expect.
4. Read failures from the job log, not from a guess:
   ```bash
   gh run list --branch "$(git branch --show-current)" --limit 5
   gh run view <run-id> --log-failed > /tmp/fail.log
   grep -nE "error:|\[fail\]|##\[error\]" /tmp/fail.log
   ```
   `[fail]` in a C test's output is not always a failure — the padding trap
   reports expected findings. Confirm against the run's own verdict line before
   chasing one.
5. Expect the loop to reveal one error class at a time, since `-Werror` stops
   at the first translation unit that fails. When you fix one, sweep the tree
   for its siblings rather than waiting for CI to surface them one per push.

### 5.3 Repository Location and Pushing

`relay12` is a **standalone repository**; run every command below from the root
of your own checkout.
It is no longer nested inside the Whisky checkout and is no longer referenced
by Whisky as a submodule — see `chore(repo): decouple winecx, winecx-gptk, and
relay12 into standalone repositories` on the Whisky side.

`origin` is the GitHub remote (`niltonperimneto/relay12`):

```bash
git remote -v                                   # confirm before pushing
git push origin "$(git branch --show-current)"
```

## 6. Agent Identity and Commit Guidelines

When contributing to this repository or executing git commands, autonomous agents must adhere to the following rules regarding identity and commit provenance:

- **Required Sign-Off:** Every commit must include a Signed-off-by line. You must always use `git commit -s`.
- **Identity Representation:** Agents must adopt the default local git user identity for the `Signed-off-by` trait and commit author.
