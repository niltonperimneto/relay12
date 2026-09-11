# Port quality roadmap

This is the single source of truth for relay12 phase and milestone status.
Architecture belongs in `D3D11ON12.md`, clean-room policy in
`CLEANROOM-DDI.md`, and declaration work in `DDI-REMAINING-ROADMAP.md`.
Completion requires committed, green evidence; percentages and local-only
builds are not status evidence.

Status values are `COMPLETE`, `IN PROGRESS`, `BLOCKED`, and `PLANNED`.

## Quality invariants

- Every static gate has a passing fixture and a mutation it rejects.
- Linux CI is authoritative for filename case and MinGW archive claims.
- Unsupported runtime behavior fails intentionally; placeholders never execute.
- ABI, HRESULT, ownership, state, and output bytes are tested as contracts.
- SDK/WDK overlays are build inputs and never enter clean-room DDI compiles.

## Phase 1 — Router and portability foundation

**Objective:** establish stable loading, diagnostics, ownership, and failure
boundaries before enabling a real device.

| Milestone | Status | Evidence | Exit gate / next work |
| --- | --- | --- | --- |
| M1 Router and core fail closed | COMPLETE | Router status and missing-component tests | Preserve explicit errors while replacing the stub core |
| M2 Shared portability contracts | COMPLETE | Native and MinGW ownership, HRESULT, ETW, and struct-return tests | New helpers require positive and negative tests |

**Exit gate:** router failures remain diagnosable and helpers preserve COM
ownership and HRESULT identity.

## Phase 2 — Clean-room DDI surface

**Objective:** expose independently authored, layout-checked declarations
required by the pinned driver and Wine host.

| Milestone | Status | Evidence | Exit gate / next work |
| --- | --- | --- | --- |
| M3 Host-filled tables and negotiation | COMPLETE | C/C++ layout, padding, provenance, negotiation, and negative-signature gates | Re-run on every source-pin change |
| M4 Driver-filled device table | IN PROGRESS | Independent model and mock frame harness cover promoted slots | Promote every host-reachable slot; intentionally fail unsupported paths |
| M5 Overlay isolation | IN PROGRESS | Proprietary filenames rejected from Git | Dependency audit proves DDI harnesses resolve nothing from the overlay |

**Exit gate:** every reachable callback is typed and invoked by a contract
test, and clean-room builds are mechanically isolated from proprietary headers.

## Phase 3 — Upstream portability

**Objective:** build reproducible MinGW archives for DTL and D3D11On12.

| Milestone | Status | Evidence | Exit gate / next work |
| --- | --- | --- | --- |
| M6 DTL source preparation | IN PROGRESS | Ordered patches and high-water inventory | Prepared-tree case and portability gates pass on Linux |
| M7 DTL archives | BLOCKED | CMake reaches the archive build | Green CI produces archives with 31, 4, and 2 unique members |
| M8 D3D11On12 driver DLL | BLOCKED | Source patch series and portability scan exist | Link reproducibly; pass export, import, and dependency audits |

**Active blocker:** the archive lane fails on a case-sensitive local include.
A case-insensitive local build does not satisfy M6 or M7.

## Phase 4 — Wine host and device integration

**Objective:** negotiate the DDI, create the driver device, and connect the
caller's D3D12 device and queue.

| Milestone | Status | Evidence | Exit gate / next work |
| --- | --- | --- | --- |
| M9 Real adapter/device creation | BLOCKED | Router and mock negotiation tests only | Requires M8; pass reachability and unsupported-path tests |

**Exit gate:** return a real D3D11 device without weakening fail-closed routing.

## Phase 5 — Execution correctness

**Objective:** implement lifetime, barriers, submission, wrapped resources,
and synchronization under concurrency.

| Milestone | Status | Evidence | Exit gate / next work |
| --- | --- | --- | --- |
| M10 Lifetime and shared state | PLANNED | Static shared-state audit and core stress foundations | Add shutdown and oversubscription stress per shared object |
| M11 Frame and readback | BLOCKED | Mock frame harness validates order, handles, and expected bytes | Run identical centre/corner assertions against the real host |

**Exit gate:** fault injection, lifetime stress, and byte-for-byte readback pass
against the integrated host.

## Phase 6 — Deployment

**Objective:** package the stack and enable it only when capabilities and
dependencies are present.

| Milestone | Status | Evidence | Exit gate / next work |
| --- | --- | --- | --- |
| M12 Reproducible package | PLANNED | Reproducible source-package gate | Package binaries, licenses, and dependency manifest reproducibly |
| M13 Capability-gated rollout | PLANNED | Machine-readable router status | Whisky selects relay12 only after M9–M12 are green |

**Exit gate:** packaged artifacts pass integrity, dependency, rollback, and
application conformance checks in the supported environment.

## DTL patch order

1. Replace ATL ownership helpers.
2. Replace `_com_error` while preserving HRESULTs.
3. Separate the optional display-kernel present path.
4. Compile out unsupported ETW event sites.
5. Replace MSVC-only language extensions and UUID helpers.
6. Add portable CMake targets and compile every supported translation unit.
7. Correct local include spelling and gate filename case on the prepared tree.
8. Link and audit the archives, then consume them from D3D11On12.

Each step updates `dtl-portability-baseline.json`; increases require an explicit
rationale and decreases land with the patch that caused them.
