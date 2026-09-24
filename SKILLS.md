# SKILLS.md — D3D11On12 Procedures, Recipes & Verification

> **Target Audience:** Autonomous Coding Agents, Subagents, and Systems Engineers  
> **Subsystem:** D3D11On12 (Translation Layer & DDI Host)  
> **Prerequisites:** Python 3.10+, a MinGW-w64 cross toolchain (CI pins llvm-mingw 20240619 `ucrt`, i.e. `x86_64-w64-mingw32-clang++`), Wine execution environment  

---

## 1. Skill Matrix Overview

This document provides operational procedures and diagnostic routines for developing and maintaining the D3D11On12 translation layer and clean-room DDI host.

| ID | Skill Name | Primary Components | Typical Scenarios |
| :--- | :--- | :--- | :--- |
| **SK-01** | [Clean-Room DDI Authoring & Provenance](#sk-01-clean-room-ddi-authoring--provenance) | `ddi/wine_d3d11ddi.h`, `check_ddi_header.py` | Adding DDI function tables, callback tables, handle types |
| **SK-02** | [DDI Layout Derivation & Drift Verification](#sk-02-ddi-layout-derivation--drift-verification) | `scripts/gen_ddi_layout.py`, `d3d11ddilayout.c` | Deriving field offsets, validating layout agreement |
| **SK-03** | [PE Exports & Interface Acquisition Audits](#sk-03-pe-exports--interface-acquisition-audits) | `check_pe_audit.py`, `check_interface_acquisition.py` | Auditing export ordinals, enforcing `strictResult()` funnel |
| **SK-04** | [Core Boundary Unit Testing](#sk-04-core-boundary-unit-testing) | `tests/d3d11on12coretest.c`, `d3d11shimstatus.c` | Testing mock COM vtables, fail-closed returns, pointer zeroing |
| **SK-05** | [CI Gates & Test Suite Execution](#sk-05-ci-gates--test-suite-execution) | `tests/test_ci_gates.py`, `package-d3d11on12-source.sh` | Pre-commit sanity checks, source tarball packaging |
| **SK-06** | [Diagnostic Logging & Sinks](#sk-06-diagnostic-logging--sinks) | `wine_d3d11_diag.h`, `ntdll.spec` | Verifying dynamic symbol resolution and error logging |
| **SK-07** | [Conformance Probing & Status Gating](#sk-07-conformance-probing--status-gating) | `tests/d3d11on12probe.c`, `WineD3D11ShimGetStatus` | Checking rendering pipeline flow, reading deployment state |
| **SK-08** | [Verifying Through CI](#sk-08-verifying-through-ci) | `.github/workflows/pull-request.yml`, `gh` | Anything that must compile or run under Wine — **read this before claiming a fix is verified** |

---

## SK-01: Clean-Room DDI Authoring & Provenance

### Context
All DDI structures in [`relay12-d3d11/ddi/wine_d3d11ddi.h`](relay12-d3d11/ddi/wine_d3d11ddi.h) must be clean-room authored using public Microsoft documentation. Proprietary Windows Driver Kit headers are prohibited.

### Procedure

1. **Dual-Source Retrieval:**
   - Retrieve the rendered documentation page:
     `https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d10umddi/<target_page>`
   - Retrieve the GitHub markdown documentation mirror:
     `https://raw.githubusercontent.com/MicrosoftDocs/windows-driver-docs-ddi/staging/wdk-ddi-src/content/d3d10umddi/<target_page>.md`
2. **Cross-Validation:**
   Confirm that member count, member types, and declaration order agree across both documentation sources.
3. **Format Provenance Comment Block:**
   Add a structured comment block with `Specification:` and `Retrieved:` tags to the header:
   ```c
   /*
    * Group: TargetStructureGroup
    * Specification: https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d10umddi/ns-d3d10umddi-target_structure
    * Retrieved: 2026-09-07
    */
   ```
4. **Natural Alignment:**
   Do not include `#pragma pack`. Structures use natural Win64 8-byte alignment.
5. **Execute Gate:**
   ```bash
   python3 scripts/check_ddi_header.py
   ```

---

## SK-02: DDI Layout Derivation & Drift Verification

### Context
Field offsets must match the binary expectation of the D3D11On12 user-mode driver. Offsets and sizes are enforced at compile time using assertions and validated against an independent layout calculation model.

### Recipes

#### 1. Validate Header Offsets Against Python Layout Model
```bash
python3 scripts/gen_ddi_layout.py --check
```
*Expected Output:*
`ddi layout model: 54 structures and 469 fields agree with the header`

The counts grow as DDI groups are authored. Treat a *decrease* as a regression
to explain, not a number to update.

#### 2. Generate C Declarations and Compile-Time Assertions
```bash
python3 scripts/gen_ddi_layout.py --emit <GroupName>
```

#### 3. Run-time Layout Verification in C and C++
Compile and run [`tests/d3d11ddilayout.c`](tests/d3d11ddilayout.c) to confirm that dynamically walked structure offsets match compile-time assertions across both C and C++ compilers.

---

## SK-03: PE Exports & Interface Acquisition Audits

### Context
The router (`d3d11shim.dll`) and core boundary (`d3d11on12core.dll`) must present specific export tables and follow safe COM acquisition patterns.

### Recipes

#### 1. Audit PE Export Tables and Runtime Dependencies
```bash
python3 scripts/check_pe_audit.py d3d11shim.dll d3d11on12core.dll
```
*Validation Rules:*
- `d3d11shim.dll` exports ordinals 1 to 4 (`D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`, `D3D11On12CreateDevice`, `WineD3D11ShimGetStatus`).
- `d3d11on12core.dll` exports ordinals 1 to 18 — the ABI, device, interface and
  adapter entry points at 1 to 4, then the buffer, input-layout, shader and
  Texture2D lifecycles. `AGENTS.md` §4.2 has the table; the gate's copy is
  transcribed from `relay12-d3d11/d3d11on12core.def` and a test pins the two
  together, so a new export fails here until it is added deliberately.
- The whole ordinal table is compared, not just each name's presence, so a
  rename, a renumber or an extra export is rejected.
- Imports are limited to `kernel32.dll` plus one C runtime — `msvcrt.dll`, or
  the seven `api-ms-win-crt-*` stubs under the UCRT-targeted toolchain PR
  validation pins. No dependency on `libstdc++` or `libgcc_s`.

#### 2. Audit COM Interface Acquisitions
```bash
python3 scripts/check_interface_acquisition.py relay12-d3d11
```
*Validation Rule:*
Every occurrence of `->QueryInterface(` or `->GetDevice(` must be wrapped in `strictResult()`.

---

## SK-04: Core Boundary Unit Testing

### Context
Unit testing the core validation boundary does not require an active physical GPU.

### Recipes

#### 1. Run Mock Core Unit Tests
[`tests/d3d11on12coretest.c`](tests/d3d11on12coretest.c) constructs mock COM vtables with designated initializers, leaving unused slots null to verify that only expected interface methods are invoked.

Run via Wine:
```bash
wine d3d11on12coretest.exe
```

#### 2. Test Fail-Closed Router Behavior
[`tests/d3d11shimstatus.c`](tests/d3d11shimstatus.c) loads `d3d11shim.dll` in an environment where `d3d11mt.dll` and `d3d11on12core.dll` are absent:
- Confirms creation functions return `DXGI_ERROR_UNSUPPORTED` (`0x887a0004`).
- Confirms all output pointers are cleared to `NULL`.
- Queries `WineD3D11ShimGetStatus` to confirm machine-readable state reporting.

---

## SK-05: CI Gates & Test Suite Execution

### Context
The gates are themselves tested, in [`tests/test_ci_gates.py`](tests/test_ci_gates.py).
That matters more than it sounds: a gate whose pattern silently stops matching
passes everything from that day on. Two real instances — an export scrape that
broke on a toolchain change and then read zero exports, and an expected-export
table that sat at four entries while the `.def` had grown to eighteen — are why
a new gate lands with both a positive and a negative test.

### Execution Recipe

Run the local verification gates listed in [`AGENTS.md`](AGENTS.md) §5.1. All exit `0` and require no toolchain, Wine, or network.

Gates needing an artifact CI builds or a tree CI materializes:

```bash
python3 scripts/check_pe_audit.py d3d11shim.dll d3d11on12core.dll
python3 scripts/check_wine_d3d11_backend.py WINE_TREE
python3 scripts/check_d3d11on12_port.py SOURCE_DIR
python3 scripts/check_dtl_struct_return.py SOURCE_DIR
python3 scripts/check_dtl_include_case.py SOURCE_DIR [--include-root ROOT]
python3 scripts/check_cleanroom_isolation.py --overlay DIR
```

> `check_secure_code.py` is wired into no workflow yet. It passes, and it
> targets exactly the probabilistic low-level mistakes generated code makes, so
> run it by hand until it is added.

### Applying a Patch Series Locally
The series *can* be validated without a cross compiler — applying it proves the
context still matches, which is most of what breaks:

```bash
git clone --quiet third_party/D3D11On12 /tmp/scratch-d3d11
for p in "$PWD"/patches/d3d11on12/*.patch; do
  git -C /tmp/scratch-d3d11 apply --ignore-space-change --ignore-whitespace "$p" \
    || echo "FAILED: $p"
done
python3 scripts/check_d3d11on12_port.py /tmp/scratch-d3d11
```

Expected: `D3D11On12 portability dependency gate: ok`.

`"$PWD"/` is required, not incidental. `git -C DIR apply` resolves a relative
patch path against `DIR`, so the repo-relative spelling opens none of the
patches and the gate then reports the *unpatched* tree's `CComPtr` and
`_com_error` violations — which reads like a broken series rather than a
broken command.

Upstream stores these sources CRLF while the series is LF, which is why
`--ignore-space-change --ignore-whitespace` is correct here; it does not relax
path, context or hunk-offset matching. When authoring a new patch, generate its
diff from that clone rather than hand-writing hunks — hand-written offsets and
mixed line endings are the usual cause of a series that no longer applies.

#### Source Package Generation
To create a clean-room source archive including submodule licenses:
```bash
./scripts/package-d3d11on12-source.sh
```

---

## SK-06: Diagnostic Logging & Sinks

### Context
Diagnostic logging in [`relay12-d3d11/wine_d3d11_diag.h`](relay12-d3d11/wine_d3d11_diag.h) avoids static linking against internal Wine symbols.

### Operation
- **Dynamic Symbol Resolution:** Queries `__wine_dbg_output` from `ntdll.dll` via `GetProcAddress`. If unavailable, falls back to `OutputDebugStringA` and then to `stderr`.
- **Deduplication:** State latches ensure that recurring failure conditions log once per process, avoiding redundant messages during application polling loops.

---

## SK-07: Conformance Probing & Status Gating

### Context
When a compatible Direct3D 12 environment is available, end-to-end rendering validation is performed with [`tests/d3d11on12probe.c`](tests/d3d11on12probe.c).

### Probe Steps
1. Creates a Direct3D 12 device and direct command queue.
2. Invokes `D3D11On12CreateDevice`.
3. Acquires the `ID3D11On12Device` interface.
4. Wraps a render target resource.
5. Acquires, clears, and releases the resource, flushing work to the command queue.
6. Waits for fence completion.

### Deployment Status Inspection
Callers inspect ordinal 4 (`WineD3D11ShimGetStatus`) to determine whether the D3D11On12 subsystem is ready before selecting a rendering backend.

---

## SK-08: Verifying Through CI

### Context
**Nothing in this repository compiles on a developer Mac.** There is no MinGW
cross toolchain and no Wine. The C and C++ sources, the DDI tests, the patched
DTL and D3D11On12 trees, and every Wine-executed suite exist only inside the
`validate-d3d11on12` job. The gates in SK-05 are necessary and nowhere near
sufficient — they check transcription, layout and annotations, not that a
single line compiles.

The practical consequence: **do not report a compile or runtime fix as
verified when it has not been compiled.** State what was checked, and that the
push is what exercises it.

### Procedure

1. **Run the SK-05 local set first.** Seconds, no setup, and it catches the
   errors that do not need a compiler.
2. **Pre-check what host tooling can reach.** Much can be settled without CI:
   - a header, macro or template question, with a reduced case under host
     `clang -fsyntax-only` using the job's own flags
     (`-std=gnu11 -Wall -Wextra -Werror`);
   - a patch series, by applying it to a throwaway clone (SK-05);
   - a workflow edit, with `ruby -ryaml -e 'YAML.safe_load(File.read(ARGV[0]))'`,
     since a YAML or heredoc-indentation error costs a full run to discover.
3. **Push the branch and read the job.**
   ```bash
   git push origin "$(git branch --show-current)"
   gh run list --branch "$(git branch --show-current)" --limit 5
   gh run view <run-id> --log-failed > /tmp/fail.log
   grep -nE "error:|\[fail\]|##\[error\]" /tmp/fail.log
   ```
   `origin` is the GitHub remote in this standalone repository; run
   `git remote -v` rather than trusting a remembered name (see `AGENTS.md`
   §5.3 — the older nested checkout used `github`).
4. **Read the failure, do not infer it.** Two traps in this log:
   - `[fail]` from a C test is not always a failure. The padding trap fills a
     struct with `0xCC` and *expects* to find its tail padding, so it prints a
     line per byte. Confirm against the run's own verdict line before chasing
     one.
   - A negative test is *supposed* to fail to compile — both
     `tests/*negative.c` (a promoted DDI slot must reject wrong arguments) and
     `tests/relay_*_negative.cpp`. CI inverts their exit code, so their
     `error:` output is the pass condition, and a run where one of them
     *builds* is the real failure.
   When a step exits non-zero with nothing obvious, find the `##[error]` line
   and read the ~30 lines above it; that is where an inline script's traceback
   lands.
5. **Sweep for siblings.** `-Werror` stops at the first translation unit that
   fails, so CI reveals roughly one error class per push. When you fix one, grep
   the tree for the same pattern instead of paying a full run per instance —
   the Clang table in `AGENTS.md` §4.1 lists the classes this tree has hit.

### Expected Timings
`validate-d3d11on12` reaches a verdict in about 4–6 minutes, and fails faster
than it passes. It is the only long job here: the hour-long runtime build now
lives in the separate `winecx-gptk` repository and nothing in this repository
triggers or gates on it.
