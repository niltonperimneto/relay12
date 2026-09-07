# Clean-room D3D11 DDI declarations

## Purpose

This is the method and specification for authoring the declaration groups in
`relay12-d3d11/ddi/wine_d3d11ddi.h`. The binding rules live in that header and
take precedence over anything here. `docs/D3D11ON12.md` is the macro design and
system roadmap; this document is the single source of truth for how the DDI
declarations get written, in what order, and against which sources.

It exists because the surface is enumerable, the documentation sources are
public, and the declaration sequence is determined strictly by risk: mistakes
that cause silent memory corruption must be addressed before errors that
merely fail at a call site.

## Version negotiation and interface binding

### Pinned driver expectations: WDDM 2.6 tables, 2.7 interface version

The pinned Microsoft D3D11On12 driver advertises exactly two supported DDI
versions in `src/adapter.cpp`:

```cpp\nstatic constexpr UINT64 SupportedVersions[] =\n{\n    D3DWDDM2_6_DDI_SUPPORTED,\n    D3DWDDM2_7_DDI_SUPPORTED,\n};\n```

Consequently, the device function table is `D3DWDDM2_6DDI_DEVICEFUNCS` and the
core-layer callback table is `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS`. Neither
is `D3D11DDI_DEVICEFUNCS` or `D3D11DDI_CORELAYER_DEVICECALLBACKS`, despite the
component name and standard D3D11 initialization literature.

**The table remains the 2.6 structure for both advertised versions.** The
documented `D3D10DDIARG_CREATEDEVICE` union ends at `pWDDM2_6DeviceFuncs`;
there is no `pWDDM2_7DeviceFuncs` arm in the public record, and no published
`D3DWDDM2_7DDI_DEVICEFUNCS`. Reading the driver implementation in
`include/device.hpp` and `src/device.cpp` shows why:

- `DeviceBase` declares `typedef D3DWDDM2_6DDI_DEVICEFUNCS DDITableLatest;`
  and stores one `DDITableLatest *m_pDDITable`;
- `GetDeviceFuncsFromCreateArgs` retrieves `pArgs->pWDDM2_6DeviceFuncs`;
- the `DeviceBase` constructor initializes `m_pCallbacks` from
  `Args.pWDDM2_6UMCallbacks` and `m_pDXGITable` from
  `pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions6_1`;
- with no branch on version above it, the constructor asserts:
  `pArgs->Interface == D3DWDDM2_7_DDI_INTERFACE_VERSION`.

WDDM 2.7 reuses the 2.6-named tables, the union arms the driver reads are
identical either way, and the version expected by the driver's own internal
assertion is 2.7.

**Rule: negotiate the numerically highest advertised word (2.7).** It requires
no declarations beyond 2.6 and satisfies the driver's assertions. (Note: this
rule is sized against the pinned driver revision; if the pin moves, re-read
the device assertions before trusting the rule).

The table size to plan against is 178 slots (1424 bytes), not the 157 slots of
`D3D11_1DDI_DEVICEFUNCS` from earlier estimates.

### Run-time resolution of version literals

The public specification composes DDI interface and supported-version words
in code, but prints minor and build numbers as ellipses. These literals are not
needed at compile time, for three composing reasons:

1. `OpenAdapter_D3D11On12` never inspects `Interface` or `Version`:
   ```cpp\n   pArgs->hAdapter.pDrvPrivate = new D3D11On12::Adapter(pArgs, *pArgs2);\n   return S_OK;\n   ```
2. `GetSupportedVersions` returns the supported-version `UINT64` words to the
   runtime as **data** at run time.
3. The Wine runtime calls `GetSupportedVersions`, selects the highest word,
   and passes its high 32 bits back as `D3D10DDIARG_CREATEDEVICE.Interface`
   using the published arithmetic macros (`WINE_D3D11_DDI_INTERFACE_VERSION`
   and `WINE_D3D11_DDI_SUPPORTED`).

Values travel from the driver to the driver. Neither side needs unreleased
compile-time literals, removing the need for a private WDK header job from
the critical path.

**Residual limit:** The host can *order* advertised versions numerically, but
cannot *name* them. Because both minor and build numbers increase monotonically,
the highest word is WDDM 2.7 on the pinned driver. The host must validate the
created device object rather than blindly trust the selected version.

## Available trees and clean-room boundary

| Source | License | Supplies | Clean-room work needed |
| :--- | :--- | :--- | :--- |
| `D3D11On12/interface/D3D11On12DDI.h` | MIT | `OpenAdapter_D3D11On12`, `SOpenAdapterArgs`, `PrivateCallbacks`, `ID3D11On12DDIDevice` | **None** — use directly |
| `D3D11On12` (remaining) | MIT | Driver implementation | None (driver *consumes* the DDI) |
| `D3D12TranslationLayer` | MIT | Translation layer | None |
| `DirectX-Headers` | MIT | D3D12 public headers | None |
| WineCX | LGPL | `d3dkmdt.h`, `d3dhal.h` | None |
| Clean-room header | GPL-3.0 | `wine_d3d11ddi.h` | Authors all referenced WDK types |

`D3D11On12`'s `include/pch.hpp` includes WDK headers (`d3d10umddi.h`,
`dxgiddi.h`, `d3dkmthk.h`). WineCX provides none of these. The clean-room
boundary covers strictly what those headers supply that `D3D11On12` references.

`interface/D3D11On12DDI.h` is MIT-licensed and defines the host-driver boundary.
`SOpenAdapterArgs` passes `ID3D12Device1*` and `ID3D12CommandQueue*` directly,
so application D3DMetal objects reach the driver without DDI translation. With
`D3D10DDIARG_OPENADAPTER` and the adapter tables authored, the adapter-level
boundary is complete.

## Dual-source cross-validation

Every declaration group is authored from Microsoft's public reference
documentation, cross-checked across two independent surfaces:

| Surface | Information supplied | Location |
| :--- | :--- | :--- |
| Rendered reference page | Typed syntax block, declaration order | `learn.microsoft.com/.../ddi/d3d10umddi/<page>` |
| Markdown source mirror | Field names and ordering (`### -field`) | `raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/<page>.md` |

The rendered HTML syntax block supplies types and ordering. The markdown source
mirror supplies independent confirmation of member names and order.

**Rule: Both surfaces must agree on member count and order. Any discrepancy
halts authoring until investigated.** Disagreements are never resolved by
arbitrary selection.

### Verified slot counts on primary structures

| Structure | Rendered page | Markdown mirror | Total bytes |
| :--- | :--- | :--- | :--- |
| `D3DWDDM2_6DDI_DEVICEFUNCS` | 178 members, 138 distinct types | 178 fields | 1424 |
| `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS` | 47 members, 46 distinct types | 47 fields | 376 |
| `D3D10DDIARG_CREATEDEVICE` | 23 members | 23 fields | 88 |

## Risk-based ordering and layout strategy

### Layout derivation before signature promotion

A wrong offset causes silent memory corruption across the DLL boundary; a wrong
function signature causes a localized call fault. Layout is cheap to derive and
high risk; signatures are expensive to verify and low risk until invoked:

- **Layout**: Derivable immediately for all 178 device-function slots from the
  syntax block. Slots are initially declared using an explicit placeholder type
  requiring an explicit cast, preventing accidental invocations while preserving
  exact offsets.
- **Signatures**: Promoted incrementally per slot as required by the host, citing
  the corresponding callback documentation page in its provenance block.

### Flow direction dictates risk priority

| Table | Filled by | Called by | Risk profile |
| :--- | :--- | :--- | :--- |
| `D3D10_2DDI_ADAPTERFUNCS` | Driver | Host | Lower: host controls call timing |
| `D3DWDDM2_6DDI_DEVICEFUNCS` | Driver | Host | Lower: unpromoted slots are never called |
| `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS` | **Host** | **Driver** | **Critical**: driver calls host with external arguments |
| `D3DDDI_DEVICECALLBACKS` | **Host** | **Driver** | **Critical**: driver invokes callbacks autonomously |

The tables filled by the host must be signature-complete before runtime execution.
Therefore, the 47-slot core-layer callback table takes priority over the 178-slot
device table.

## Tooling and drift defense

`scripts/gen_ddi_layout.py` serves two distinct roles:

1. **`--emit`**: Generates formatted C declarations and `WINE_DDI_ASSERT_*`
   statements from parsed documentation inputs. Emitted code is committed to
   the repository as reviewed source, never executed dynamically at build time.
2. **`--check`**: Acts as an independent layout verification model and CI gate.

### What `--check` validates vs compiled assertions

Compiled assertions in the header verify that declarations are *self-consistent*
under the Win64 compiler. However, if a field is omitted or typed with an
incorrect size, compiled assertions based on that faulty layout will still pass.
`gen_ddi_layout.py --check` independently models Win64 layout by walking member
lists and checks for:

- Hand-edited or mismatched offsets in either artifact;
- Unasserted fields known to the model (enforcing rule 3);
- Assertions naming fields not derived from specifications (preventing stale
  declarations).

Cross-validating the two public documentation surfaces defends against initial
misinterpretation; `--check` defends against structural *drift* across 178+ slots.

## Authoring procedure and rules

### Step-by-step authoring procedure

1. **Identify structures**: Confirm direct reference in pinned MIT source or
   prerequisite types. No speculative declarations.
2. **Fetch documentation**: Retrieve rendered HTML and raw markdown mirror; record
   the retrieval date.
3. **Cross-validate**: Compare member counts and sequence. Stop on discrepancy.
4. **Draft provenance block**: Include `Specification` (valid HTTPS URL) and
   `Retrieved` (YYYY-MM-DD date) in the group's header comment block for
   `scripts/check_ddi_header.py`.
5. **Document variances**: Explicitly state derived values or omissions. Leave
   unspecified constants undefined.
6. **Declare group**: Keep unauthored types incomplete behind pointers.
7. **Assert layout**: Assert size, alignment, and every member offset. Assert all
   declared union arms at identical offsets.
8. **Extend run-time test**: Add layout validation in `tests/d3d11ddilayout.c`
   compiled in both C and C++.
9. **Verify locally**: Model layout locally with `gen_ddi_layout.py --check`.
10. **Validate via CI**: Pass the `validate-d3d11on12` GitHub Actions job.

### Strict prohibitions

- **No vendored WDK headers**: Reject `d3d10umddi.h`, `d3d11umddi.h`, or other
  proprietary headers by filename.
- **No binary disassembly reconstruction**: Declarations must stem solely from
  public specifications.
- **No unattributed values**: Never invent values or cite pages lacking them.
- **No `#pragma pack`**: MinGW and MSVC use `/Zp8` natural alignment; packing
  is prohibited and enforced by CI.
- **No speculative versions**: Declare only the negotiated WDDM 2.6/2.7 structures.
- **No unasserted fields**: Every struct field, union arm, and padding must have
  an explicit offset assertion.

## Declaration groups: status and roadmap

### Authored and verified groups

| Group | Key structures and contents | Total bytes |
| :--- | :--- | :--- |
| Object handles | `D3D10DDI_HADAPTER`, `HRTADAPTER`, `HRESOURCE`, `HRTRESOURCE` | 8 each |
| Adapter tables | `D3D10DDI_ADAPTERFUNCS`, `D3D10_2DDI_ADAPTERFUNCS`, `D3D10DDIARG_OPENADAPTER`, 6 `PFN` typedefs | 24 / 40 / 40 |
| Version arithmetic | `D3D11_DDI_MAJOR_VERSION`, composition and extraction macros | n/a |
| Device creation | `D3D10DDI_HDEVICE`, `HRTDEVICE`, `HRTCORELAYER`, `PFND3D10DDI_RETRIEVESUBOBJECT`, `DXGI_DDI_BASE_ARGS`, `D3D10DDIARG_CREATEDEVICE`, flag constants | 8 each / 16 / 88 |

#### Key constraints in `D3D10DDIARG_CREATEDEVICE` (88 bytes)

| Field | Offset | Notes |
| :--- | :--- | :--- |
| `hRTDevice` | 0 | Runtime handle |
| `Interface` | 8 | Negotiated interface version (high 32 bits of supported word) |
| `Version` | 12 | Driver version |
| `pKTCallbacks` | 16 | Kernel callbacks pointer |
| Device funcs union | 24 | Contains `pWDDM2_6DeviceFuncs` (offset 24) |
| `hDrvDevice` | 32 | Driver private handle |
| `DXGIBaseDDI` | 40 | **Embedded by value** (`DXGI_DDI_BASE_ARGS`, 16 bytes: offsets 40 and 48) |
| `hRTCoreLayer` | 56 | Runtime core-layer handle |
| Callbacks union | 64 | Contains `pWDDM2_6UMCallbacks` (offset 64) |
| `Flags` | 72 | 4 bytes, followed by 4 bytes of unnamed padding (must zero entire struct) |
| `ppfnRetrieveSubObject` | 80 | **Pointer to function pointer**; storage owned by host, written by driver |

Per rule 4, only the union arms read by the pinned driver are declared.
`gen_ddi_layout.py` models both published and declared arms and ensures exact
offset agreement.

### Remaining groups roadmap

| # | Group | Scope | Rationale and dependencies |
| :--- | :--- | :--- | :--- |
| 1 | Core-layer callbacks | `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS`, signature-complete | 47 slots (376 bytes). Urgent: host fills this, driver invokes it |
| 2 | Kernel callbacks | `D3DDDI_DEVICECALLBACKS` | Only members invoked by D3D11On12. Host fills |
| 3 | Device function table | `D3DWDDM2_6DDI_DEVICEFUNCS`, full layout, placeholder slots | 178 slots (1424 bytes). Driver fills, host calls |
| 4 | Surface discovery | MIT tree test build against clean-room header | Turns remaining clean-room work into a measurable compiler worklist |
| 5 | Signature promotion | Promote slots in `D3DWDDM2_6DDI_DEVICEFUNCS` | 138 distinct callback types, prioritized by host call sites |
| 6 | DXGI DDI interop | `DXGI_DDI_BASE_CALLBACKS`, `DXGI1_6_1_DDI_BASE_FUNCTIONS` | Required by `DXGIBaseDDI` pointers in creation arguments |

## Open questions

- **Extent of `d3dkmthk.h` types in host signatures**: `D3D11On12`'s `pch.hpp`
  includes `d3dkmthk.h`, but it is uncertain whether any types cross the host-driver
  boundary. Group 4 (Surface discovery) will provide the compiler-verified answer.
- **`D3DDDI_DEVICECALLBACKS` active call set**: Establishing the exact subset of
  kernel callbacks invoked by the pinned driver to avoid declaring unused slots.
- **Value of `D3D11DDI_CREATEDEVICE_FLAG_IS_XBOX`**: Checked by the driver in
  `IsXboxCreateFlags`, but omitted from public documentation. Left undefined as
  the host never targets Xbox execution.
