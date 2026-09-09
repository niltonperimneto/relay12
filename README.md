# relay12: Direct3D 11 to 12 Translation Layer & UMD Host

```text
 ┌──────────────────────────────────────────────────────────────┐
 │                      Application (Game)                      │
 │       (e.g., Unity 6, Hybrid D3D11 Media / D3D12 3D)         │
 └──────────────┬───────────────────────────────┬───────────────┘
                │ D3D11 API Calls               │ D3D12 API Calls
                │ (Media, UI, 2D)               │ (Main 3D Pipeline)
                ▼                               │
 ┌──────────────────────────────┐               │
 │        relay12 (UMD)         │               │
 │       (d3d11shim.dll)        │               │
 │      (d3d11on12core.dll)     │               │
 │                              │               │
 │   Clean-Room DDI Interface   │               │
 └──────────────┬───────────────┘               │
                │ DDI Calls                     │
                ▼                               │
 ┌──────────────────────────────┐               │
 │      Microsoft D3D11On12     │               │
 │    & D3D12TranslationLayer   │               │
 └──────────────┬───────────────┘               │
                │ D3D12 API Calls               │
                ▼                               ▼
 ┌──────────────────────────────────────────────────────────────┐
 │             Direct3D 12 Host Implementation                  │
 ├─────────────────────┬──────────────────────┬─────────────────┤
 │ Native Windows      │ D3DMetal / GPTK      │ VKD3D-Proton    │
 │ (Hardware Drivers)  │ (macOS / Apple Sil.) │ (Linux)         │
 └─────────────────────┴──────────────────────┴─────────────────┘
```

---

## Purpose of this Project

This project implements the host environment and driver infrastructure required to run Microsoft's open-source D3D11On12 user-mode driver in Wine-based and compatible environments.

Effectively functioning as a User-Mode Graphics Driver (UMD) within the Windows/Wine graphics stack, `relay12` maps the WDDM Device Driver Interface (DDI) directly to the Translation Layer without requiring upstream driver modifications. This is especially critical for modern games that utilize a **hybrid rendering pipeline**—using D3D12 for their core 3D engine, while relying on D3D11 for UI, 2D elements, or video playback (like Windows Media Foundation). `relay12` ensures these D3D11 contexts can seamlessly share resources with the D3D12 queue.

1. **PE Router (`d3d11shim.dll`):** A replacement for `d3d11.dll` that exports standard entry points (`D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`, and `D3D11On12CreateDevice`). It forwards standard creation calls to native implementations (such as `d3d11mt.dll`) and routes `D3D11On12CreateDevice` to the core boundary.
2. **Core Validation Boundary (`d3d11on12core.dll`):** Validates caller-supplied `ID3D12Device` and `ID3D12CommandQueue` pointers, confirms command queue types, enforces two-tier COM identity checks, and initializes the DDI host.
3. **Clean-Room WDDM DDI Host (`relay12-d3d11/ddi/wine_d3d11ddi.h`):** Reconstructs the necessary Windows Driver Model (WDDM 2.6 and 2.7) structures and function tables from public specifications without copying proprietary Windows Driver Kit headers.
4. **Verification Framework:** Provides automated layout checks, mock-object unit tests, and static code audits to prevent interface mismatches and ABI drift.

---

## Architectural Philosophy

`relay12` was deliberately split from the main `winecx` environment (specifically the fork maintained in the `dappermint` repository). Treating this project as an independent, standalone User-Mode Driver provides several critical architectural advantages:

- **Isolated Commit History:** D3D12 translation requires precise iteration. By decoupling `relay12` from Wine's massive codebase, the commit history remains highly focused on DDI implementations and ABI validation, making regressions trivial to isolate and bisect.
- **Upstream Agnostic:** It avoids the substantial technical debt of resolving merge conflicts during `winecx` upstream syncs. The `winecx` base can be updated independently of this translation layer.
- **API Boundary Enforcement:** Separating the build environments enforces a strict boundary between the Wine core and the `relay12` translation layer, ensuring no accidental interdependencies or unstable internal headers are referenced.
- **Platform Agnostic Output:** Since `relay12` targets standard D3D12, it is not intrinsically tied to macOS or D3DMetal. It can bridge D3D11On12 into native Windows implementations, D3DMetal/GPTK, or VKD3D-Proton equally well.

---

## Architecture and Components

### 1. The PE Router (`relay12-d3d11/d3d11shim.cpp`)

Applications link against `d3d11.dll` to access Direct3D 11 functionality. The router fulfills this role while maintaining exact export compatibility:

| Export Ordinal | Function Name | Routing Behavior |
| ---: | :--- | :--- |
| 1 | `D3D11CreateDevice` | Forwards to `d3d11mt.dll` |
| 2 | `D3D11CreateDeviceAndSwapChain` | Forwards to `d3d11mt.dll` |
| 3 | `D3D11On12CreateDevice` | Validates inputs and routes to `d3d11on12core.dll` |
| 4 | `WineD3D11ShimGetStatus` | Reports component readiness and initialization state |

If dependencies (`d3d11mt.dll` or `d3d11on12core.dll`) are unavailable, the router returns `DXGI_ERROR_UNSUPPORTED` (`0x887a0004`) and clears all output pointers. This ensures calling applications can detect unsupported configurations and select alternative rendering paths.

### 2. The Core Boundary (`relay12-d3d11/d3d11on12core.cpp`)

The core module receives Direct3D 12 device pointers and configuration flags from the caller. Before initializing the translation driver, it applies several safety verifications:

- **Command Queue Verification:** Inspects the caller-supplied `ID3D12CommandQueue` description to confirm it is a direct queue (`D3D12_COMMAND_LIST_TYPE_DIRECT`). Compute or copy queues cannot serve as primary queues for Direct3D 11 presentation.
- **Two-Tier COM Identity:** Verifies that the command queue was created by the supplied device. It compares typed `ID3D12Device` pointers first, only querying `IUnknown` identity if the typed pointers differ.
- **Strict Acquisition Funnel:** All interface queries funnel through `strictResult()`. If a query returns `S_OK` but leaves the output pointer null, the call is treated as a failure to avoid subsequent null pointer dereferences.

### 3. The Clean-Room DDI Interface (`relay12-d3d11/ddi/wine_d3d11ddi.h`)

Microsoft's D3D11On12 driver communicates with the host through the user-mode driver interface (DDI). Because proprietary driver development headers cannot be used, all structures are clean-room declared from public documentation:

- **Version Negotiation:** The driver negotiates WDDM 2.7 while utilizing `D3DWDDM2_6DDI_DEVICEFUNCS` (178 function slots) and `D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS` (47 callback slots).
- **Dual-Source Cross-Validation:** Every structure is verified against Microsoft Learn documentation pages and corresponding GitHub markdown documentation mirrors to confirm field order, type sizes, and member counts.
- **Layout Model Verification:** Structure layouts are checked against an independent Python layout generator (`scripts/gen_ddi_layout.py`) to ensure offsets and alignments match 64-bit Windows natural alignment rules (`/Zp8`).

---

## Directory Structure

| Path | Purpose |
| :--- | :--- |
| `relay12-d3d11/` | Implementation of the router (`d3d11shim.cpp`), core boundary (`d3d11on12core.cpp`), ABI header (`wine_d3d11on12.h`), diagnostic logging (`wine_d3d11_diag.h`), and clean-room DDI headers (`ddi/wine_d3d11ddi.h`). |
| `scripts/` | Static verification tools and compliance gates: `check_ddi_header.py`, `gen_ddi_layout.py`, `check_pe_audit.py`, `check_interface_acquisition.py`. |
| `tests/` | Test suite: Python CI gate tests (`test_ci_gates.py`), mock core unit tests (`d3d11on12coretest.c`), DDI layout validation (`d3d11ddilayout.c`), and router status checks (`d3d11shimstatus.c`). |
| `docs/` | Technical specifications: `D3D11ON12.md` (system architecture), `CLEANROOM-DDI.md` (DDI authoring), `DDI-CONCURRENCY-TESTING.md` (thread-safety plans), and `DDI-REMAINING-ROADMAP.md` (roadmap), and `D3D11ON12-SKIPPABLE-ELEMENTS.md` (MVP scoping). |
| `third_party/` | Pinned submodules: `D3D11On12`, `D3D12TranslationLayer`, and `DirectX-Headers`. |

---

## Verification and Safety Gates

To maintain stability and prevent regression across compiler toolchains, four static audits and unit test harnesses are executed:

| Gate Command | Purpose |
| :--- | :--- |
| `python3 -m unittest discover -s tests -p "test_*.py"` | Verifies that all CI static gate scripts and verification rules function as expected. |
| `python3 scripts/gen_ddi_layout.py --check` | Compares clean-room DDI header field offsets against an independent layout calculation model. |
| `python3 scripts/check_ddi_header.py` | Validates documentation provenance blocks (URLs and retrieval dates) and verifies that `#pragma pack` is not used. |
| `python3 scripts/check_interface_acquisition.py relay12-d3d11` | Confirms that every `QueryInterface` and `GetDevice` call routes through `strictResult()`. |

---

## Further Reading

- **[docs/D3D11ON12.md](docs/D3D11ON12.md):** Complete architectural design, component boundaries, failure modes, and rollout checklist.
- **[docs/CLEANROOM-DDI.md](docs/CLEANROOM-DDI.md):** Clean-room WDDM DDI authoring guidelines, version negotiation proof, and implementation worklist.
- **[docs/DDI-CONCURRENCY-TESTING.md](docs/DDI-CONCURRENCY-TESTING.md):** Strategy for testing race conditions, thread safety violations, and missing CPU/GPU memory barriers.
- **[docs/DDI-REMAINING-ROADMAP.md](docs/DDI-REMAINING-ROADMAP.md):** Structured roadmap governing upcoming ABI hardening, placeholder eradication, and concurrency deployments.
- **[AGENTS.md](AGENTS.md):** Architecture invariants, safety rules, and guidelines for automated coding agents.
- **[SKILLS.md](SKILLS.md):** Step-by-step procedures for layout generation, compilation checks, and test execution.

---

## Licensing

- **Router, Core, Tests, and Tools:** Licensed under the [GNU General Public License v3.0](LICENSE) (`GPL-3.0-only`).
- **Submodules (`D3D11On12`, `D3D12TranslationLayer`, `DirectX-Headers`):** Licensed under the [MIT License](THIRD_PARTY_NOTICES.md).
