# Clean-room D3D11 DDI declarations

## Purpose

This is the method for authoring the declaration groups in
`gptk-d3d11/ddi/wine_d3d11ddi.h`. The binding rules live in that header and
take precedence over anything here. `docs/D3D11ON12.md` remains the source of
truth for what is and is not done overall; this document covers only how the
DDI declarations get written, in what order, and against what.

It exists because the task looks unbounded and is not. The surface is
enumerable, the sources are public, and the sequence is determined by which
mistakes corrupt memory and which merely fail.

## 1. The tables are WDDM 2.6's, and the version to negotiate is 2.7

The pinned Microsoft D3D11On12 advertises exactly two DDI versions.
`Adapter::GetSupportedVersions` in `src/adapter.cpp` reads:

```cpp
static constexpr UINT64 SupportedVersions[] =
{
    D3DWDDM2_6_DDI_SUPPORTED,
    D3DWDDM2_7_DDI_SUPPORTED,
};
```

So the device function table to declare is `D3DWDDM2_6DDI_DEVICEFUNCS`, and
the core-layer callback table is `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS`.
Neither is `D3D11DDI_DEVICEFUNCS` or `D3D11DDI_CORELAYER_DEVICECALLBACKS`,
despite the component being called D3D11On12 and despite those being the
tables the D3D11 DDI initialization documentation leads with.

**The table is the 2.6 one for both advertised versions.** The documented
`D3D10DDIARG_CREATEDEVICE` union ends at `pWDDM2_6DeviceFuncs`; there is no
`pWDDM2_7DeviceFuncs` arm in the public record, and no published
`D3DWDDM2_7DDI_DEVICEFUNCS`. That first read as a reason to negotiate 2.6 and
leave 2.7 alone, on the grounds that 2.7 would need a table authored from
outside the public record. Reading the driver shows there is no such table to
author. In `include/device.hpp` and `src/device.cpp`:

- `DeviceBase` declares `typedef D3DWDDM2_6DDI_DEVICEFUNCS DDITableLatest;`
  and holds one `DDITableLatest *m_pDDITable`;
- `GetDeviceFuncsFromCreateArgs` returns `pArgs->pWDDM2_6DeviceFuncs`;
- the `DeviceBase` constructor initializes `m_pCallbacks` from
  `Args.pWDDM2_6UMCallbacks` and `m_pDXGITable` from
  `pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions6_1`;
- and then, with no branch on version anywhere above it, it asserts
  `pArgs->Interface == D3DWDDM2_7_DDI_INTERFACE_VERSION`.

So WDDM 2.7 reuses the 2.6-named tables, the arms the driver reads are the
same either way, and the version it was written to be handed is the higher of
the two it advertises.

**Negotiate the numerically highest advertised word**, which is 2.7. It needs
no declaration that 2.6 does not, and it is the only choice the driver's own
assertion accepts. That assertion compiles out under `NDEBUG`, so 2.6 would
very likely also work — but there is no reason to hand a driver a version it
asserts against. The rule is evaluated against the pinned revision's set of
exactly two words; if the pin moves, re-read the assertion before trusting
the rule.

An earlier estimate of this work sized it against `D3D11_1DDI_DEVICEFUNCS` at
157 slots. That was the wrong table, and the correct one is larger. The
number to plan against is 178.

## 2. The elided version literals are not needed at compile time

`docs/D3D11ON12.md` recorded that the public specification prints the DDI
minor and build numbers as ellipses, and concluded that an authorized WDK job
was a prerequisite for freezing a DDI version. That conclusion was too
strong. The literals are not needed at compile time at all, for three reasons
that compose:

1. `OpenAdapter_D3D11On12` never reads them. Its whole body is:

   ```cpp
   pArgs->hAdapter.pDrvPrivate = new D3D11On12::Adapter(pArgs, *pArgs2);
   return S_OK;
   ```

   The adapter constructor fills `pArgs->pAdapterFuncs_2` unconditionally.
   Nothing on the adapter path inspects `Interface` or `Version`.

2. `GetSupportedVersions` hands the runtime the supported-version words as
   **data**, at run time.

3. We are the runtime. We call `GetSupportedVersions`, choose one of the
   returned `UINT64` values, and pass its high 32 bits back as
   `D3D10DDIARG_CREATEDEVICE.Interface`. The decomposition is the published
   arithmetic already captured in the header as
   `WINE_D3D11_DDI_INTERFACE_VERSION` and `WINE_D3D11_DDI_SUPPORTED`, and the
   layout harness already checks that a supported-version word round-trips
   with its interface version in the high 32 bits.

The value therefore travels from the driver to the driver. Neither side needs
a literal we do not have, and the authorized WDK job comes off the critical
path.

**The residual weakness, stated plainly.** We can *order* the advertised
versions but we cannot *name* them. The selection rule is to take the
numerically highest advertised word, which for the pinned driver is WDDM 2.7,
because both the minor and the build number increase with version. That is an
ordering argument, not an identification: if a future driver advertised a
third, higher version with a table of its own, we would select it silently and
believe it was 2.7. So the host must validate the device it got rather than
trust the selection, and if the literals ever do become available the selection
should be replaced with an equality check. The header records the gap; this is
what depends on it.

## 3. What the MIT and WineCX trees already supply

| Source | License | Supplies | Clean-room work needed |
| --- | --- | --- | --- |
| `D3D11On12/interface/D3D11On12DDI.h` | MIT | `OpenAdapter_D3D11On12`, `SOpenAdapterArgs`, `PrivateCallbacks`, `ID3D11On12DDIDevice` | **None** — use directly |
| `D3D11On12` (rest) | MIT | The driver implementation | None; it *consumes* the DDI |
| `D3D12TranslationLayer` | MIT | Translation layer | None |
| `DirectX-Headers` | MIT | D3D12 public headers | None |
| WineCX | LGPL | `d3dkmdt.h`, `d3dhal.h` | None for those |

`D3D11On12`'s `include/pch.hpp` includes the WDK's `d3d10umddi.h`,
`dxgiddi.h`, and `d3dkmthk.h`, and its headers reference `D3D10DDI_HDEVICE`
and `D3D10DDI_HRESOURCE`. It consumes the DDI; it does not declare it.
WineCX ships neither `d3dkmthk.h`, `dxgiddi.h`, `d3dumddi.h`, nor
`d3d10umddi.h`. So the clean-room surface is exactly what those three WDK
headers provide that D3D11On12 actually references — no more, and no less.

The one genuinely free win is `interface/D3D11On12DDI.h`. It is the boundary
between our host and the MIT driver, it is MIT-licensed, and it needs no
authoring. Its entry point is:

```cpp
extern "C" HRESULT WINAPI OpenAdapter_D3D11On12(
    D3D10DDIARG_OPENADAPTER *pArgs, D3D11On12::SOpenAdapterArgs *pArgs2);
```

`SOpenAdapterArgs` carries `ID3D12Device1*` and `ID3D12CommandQueue*`
directly, so the application's D3DMetal device and queue reach the driver
through an MIT structure rather than through the DDI. Combined with the
already-authored `D3D10DDIARG_OPENADAPTER` and both adapter function tables,
**the adapter-level boundary is complete.**

## 4. Two sources, cross-validated

Every group is authored from Microsoft's published reference documentation.
Two independent surfaces carry it, and both are used:

| Surface | Gives | Location |
| --- | --- | --- |
| Rendered reference page | The typed syntax block, in declaration order | `learn.microsoft.com/.../ddi/d3d10umddi/<page>` |
| Documentation source mirror | `### -field` entries, in declaration order | `raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/<page>.md` |

The rendered page's syntax block is present in the raw HTML, so it parses
without a browser. The markdown mirror carries field names and order but not
types, so it cannot replace the rendered page — it independently corroborates
it.

**The two must agree on member count and order. A disagreement stops the
group and gets investigated, never reconciled by choosing one.** This is the
cheapest real check available against a transcription error in a file where a
transcription error is silent memory corruption.

Worked confirmation of the method, on the two largest tables:

| Structure | Rendered page | Markdown mirror | Bytes |
| --- | --- | --- | --- |
| `D3DWDDM2_6DDI_DEVICEFUNCS` | 178 members, 138 distinct types | 178 fields | 1424 |
| `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS` | 47 members, 46 distinct types | 47 fields | 376 |
| `D3D10DDIARG_CREATEDEVICE` | 23 members | 23 fields | 88 |

## 5. Layout before signature

A wrong offset is silent memory corruption. A wrong signature is a bad call
at a known site. These cost very different amounts to establish, so they are
separated:

- **Layout** for all 178 device-function slots is derivable today from one
  published syntax block, because order is all that is required and every
  slot is a pointer. It is cheap, and it is the part that corrupts memory.
- **Signatures** are 138 distinct callback pages for that table alone, each
  dragging in its own argument structures and enumerations. It is expensive,
  and it is only needed for slots the host actually calls.

So each function table lands in one group with its complete layout and every
offset asserted, with unpromoted slots typed as a deliberate placeholder.
Individual slots are then promoted to real signatures in later groups, driven
by what the host implements.

The placeholder must require an explicit cast to fill, so that filling a slot
is a deliberate act and an unpromoted slot cannot be called by accident. A
bare `void *` would not do: it would accept anything silently, which is the
failure this is meant to prevent. A promoted slot's provenance block cites
its own callback page.

This is not a shortcut around rule 3 of the header. Every field's offset,
size and alignment is asserted from the start. What is deferred is the claim
to know a signature, and deferring that claim is more honest than guessing it.

## 6. Direction of flow sets the risk order

| Table | Filled by | Called by | A wrong signature means |
| --- | --- | --- | --- |
| `D3D10_2DDI_ADAPTERFUNCS` | driver | host | Host calls wrong; host controls when |
| `D3DWDDM2_6DDI_DEVICEFUNCS` | driver | host | Host calls wrong; unpromoted slots never called |
| `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS` | **host** | **driver** | Driver calls us with mismatched arguments |
| `D3DDDI_DEVICECALLBACKS` | **host** | **driver** | As above |

The tables the host fills are the dangerous ones. The driver will call them
on its own schedule with arguments we do not control, and there is no site to
guard. Those get signature-complete treatment before the host runs at all.
The tables the driver fills are safer, because the host decides when to call
and an unpromoted slot is simply never called.

This inverts the intuitive order: the 47-entry callback table is more urgent
than the 178-entry device table, despite being smaller and appearing later in
the initialization sequence.

## 7. Procedure for one group

1. Identify the exact structures the group covers, and confirm each is
   referenced by the pinned MIT source or required by a structure that is.
   Do not declare speculatively.
2. Fetch both documentation surfaces. Record the retrieval date.
3. Cross-validate member count and order. Stop on disagreement.
4. Write the provenance block in the form the header mandates, with
   `Specification` and `Retrieved` within three lines of the `Group:` marker,
   because that is the window CI inspects.
5. Where a member's content is derived rather than quoted, say so in the
   block. Where a required value is not publicly specified, record the gap
   and do not fill it.
6. Declare the group. Types not yet authored stay incomplete, used only
   behind pointers.
7. Assert size, alignment, and every field offset. Assert both arms of every
   union at the same offset.
8. Extend `tests/d3d11ddilayout.c` to walk the new group against a real
   object at run time, in both C and C++.
9. Model the layout locally first. Everything in these groups is pointers and
   4-byte integers, so host `clang` reproduces the Win64 offsets exactly and
   catches an arithmetic error before a CI cycle.
10. Verify through CI. There is no local MinGW; the `validate-d3d11on12` job
    is the only real check.

Step 9 is worth keeping. It has already caught the difference between a
computed layout and an asserted one at no cost.

## 8. What to script, and what not to

Transcribing 178 members by hand is the single most likely source of a silent
error in this work, so the declaration bodies and their assertion blocks
should be generated: parse both surfaces, cross-check, emit the struct and one
assertion per field.

What is generated is the *committed source*, not a build step. The header
stays a reviewable artifact with no generation at build time, because a
generator in the build would mean the offsets are whatever the docs said today
rather than what was reviewed. The generator is a tool for producing a patch,
and its output is read before it lands.

`scripts/gen_ddi_layout.py` is that tool, and it has a second mode that is
worth more than the first. `--emit` prints a group's declaration and
assertions; `--check` compares the model against the committed header and is a
CI gate. The distinction it exists for: the compiled assertions only prove the
header is *self-consistent*. Transcribe a member list wrongly, assert the
offsets that follow from it, and the compiler, the run-time harness and both
languages all agree. The model derives its offsets by walking member lists
instead of asserting numbers, so a divergence between the two is visible. It
catches three things:

- a hand-edited offset in either the header or the model;
- a field the model knows and the header does not assert, which is rule 3's
  "an unasserted field is not acceptable", enforced rather than remembered;
- an assertion naming a field nothing derives from the specification, which is
  how a stale declaration would outlive the group it belonged to.

It is not independent authorship, so it does not defend against reading the
specification wrong once and writing it into both. Only cross-validating the
two documentation surfaces does that, and that stays a discipline. What
`--check` defends against is *drift* — the two artifacts ceasing to agree
after the day they were written, which over 178 slots is the likelier failure.

The provenance blocks stay hand-written. They carry judgment — what was
quoted, what was derived, what is missing — and that is not mechanizable.

## 9. Groups: authored and remaining

Authored, asserted, and CI-verified:

| Group | Contents | Bytes |
| --- | --- | --- |
| Object handles | `D3D10DDI_HADAPTER`, `HRTADAPTER`, `HRESOURCE`, `HRTRESOURCE` | 8 each |
| Adapter tables | `D3D10DDI_ADAPTERFUNCS`, `D3D10_2DDI_ADAPTERFUNCS`, `D3D10DDIARG_OPENADAPTER`, 6 `PFN` typedefs | 24 / 40 / 40 |
| Version arithmetic | `D3D11_DDI_MAJOR_VERSION`, composition macros | n/a |
| Device creation | `D3D10DDI_HDEVICE`, `HRTDEVICE`, `HRTCORELAYER`, `PFND3D10DDI_RETRIEVESUBOBJECT`, `DXGI_DDI_BASE_ARGS`, `D3D10DDIARG_CREATEDEVICE`, flag constants | 8 each / 16 / 88 |

The device-creation group is the whole device-level boundary in one
declaration, and it landed with every offset asserted and walked at run time:

| Field | Offset |
| --- | --- |
| `hRTDevice` | 0 |
| `Interface` | 8 |
| `Version` | 12 |
| `pKTCallbacks` | 16 |
| device funcs union | 24 |
| `hDrvDevice` | 32 |
| `DXGIBaseDDI` | 40 |
| `hRTCoreLayer` | 56 |
| callbacks union | 64 |
| `Flags` | 72 |
| `ppfnRetrieveSubObject` | 80 |

Three things about it are worth carrying forward. `DXGI_DDI_BASE_ARGS` is
embedded **by value**, so it had to be declared first; if it ever became a
pointer, the structure would shrink by eight bytes and everything after it
would move. `Flags` is four bytes at 72 followed by four bytes of padding no
member names, so the host must zero the structure rather than assign member by
member. And `ppfnRetrieveSubObject` is a pointer to a function pointer whose
storage the *host* owns and the driver writes, which is why the harness checks
that a write through it lands in the host's slot.

Only the union arms the pinned driver reads are declared, per rule 4. The
provenance block records the full published arm lists so the omission reads as
a choice rather than a transcription loss, and `scripts/gen_ddi_layout.py`
models both arm sets and fails if they disagree on any offset.

Remaining, in the order they should land:

| # | Group | Scope | Notes |
| --- | --- | --- | --- |
| 1 | Core-layer callbacks | `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS`, signature-complete | 47 slots; host fills these |
| 2 | Kernel callbacks | `D3DDDI_DEVICECALLBACKS`, only the members D3D11On12 calls | Host fills these |
| 3 | Device function table | `D3DWDDM2_6DDI_DEVICEFUNCS`, full layout, placeholder slots | 178 slots, 1424 bytes |
| 4 | Surface discovery | CI job compiling the MIT tree against the header | Turns the rest into a closed worklist |
| 5 | Signature promotion | Promote slots by priority from group 4's output | 138 distinct types, worked in priority order |
| 6 | DXGI DDI interop | `DXGI_DDI_BASE_CALLBACKS`, `DXGI1_6_1_DDI_BASE_FUNCTIONS` | Extent set by group 4 |

The core-layer table is `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS` and it has
**47** members, not the 40 of `D3D11DDI_CORELAYER_DEVICECALLBACKS`. An earlier
revision of this document named the D3D11 one. That was wrong: `DeviceBase`
holds a `const D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS *` and initializes it
from `Args.pWDDM2_6UMCallbacks`, so the 2.6 table is the one the host fills and
the driver calls. Both documentation surfaces agree on 47 fields. This is the
most urgent group by section 6's rule, and it is 18% larger than the estimate
it replaces.

Group 4 is the one that changes the character of the work. Compiling the
pinned MIT tree against the clean-room header and collecting unresolved
identifiers replaces an estimate with a measurement, and gives the effort a
termination condition instead of a guess. It should not be deferred to the end
merely because it appears late in the dependency order — the device-creation
group has landed, so it can be stood up now, and it will be wrong in useful
ways before the tables above it exist.

Group 6's extent is already partly known: the driver reads
`pDXGIBaseCallbacks` and `pDXGIDDIBaseFunctions6_1` out of the structure that
just landed, and both are incomplete types behind pointers until that group
authors them.

## 10. Prohibitions

These restate the header, which governs:

- Never copy or vendor a WDK header; CI rejects `d3d10umddi.h` and
  `d3d11umddi.h` by filename.
- Never reconstruct a declaration from a binary.
- Never write a value from recollection behind a provenance block citing a
  page that does not contain it. CI cannot catch this — the gate only checks
  that the citation lines exist — so it is a discipline, not a control.
- No `#pragma pack` under `gptk-d3d11/ddi`; CI rejects it.
- Declare only the one selected DDI version. No speculative versions.
- An unasserted field is not acceptable.

## 11. Open questions

- **Does D3D11On12 require `d3dkmthk.h` types in any signature the host
  touches?** Its `pch.hpp` includes the header, but the host may never see
  those types across the boundary. Group 4 answers this; until then the
  extent of the D3DKMT surface is unknown, and it is the largest remaining
  uncertainty in the total.
- **Which `D3DDDI_DEVICECALLBACKS` members does the driver actually call?**
  Filling the whole table is more work than the host needs and more risk than
  it should carry. The MIT source's call sites bound this, and reading them
  is cheap.
- **What is `D3D11DDI_CREATEDEVICE_FLAG_IS_XBOX`?** The driver tests it in
  `IsXboxCreateFlags`, and its value is in no public document. The header
  records the gap and does not define it. Nothing the host does needs it —
  it would never set the flag — but a bit of `Flags` is therefore unaccounted
  for, alongside the three the 3-D pipeline level mask covers.

Answered since the first revision, kept because the reasoning is load-bearing:

- **Does WDDM 2.7 differ from 2.6 in the device table?** No. `DDITableLatest`
  is `D3DWDDM2_6DDI_DEVICEFUNCS` for both, and the driver reads the 2.6 union
  arms while asserting the 2.7 interface version. Section 1 records what that
  costs and what it buys.
