# Port quality and technical-debt roadmap

This roadmap turns porting debt into observable contracts. An item advances
only when its named gate is committed and green; source volume or an apparently
successful local build is not evidence by itself.

## Quality invariants

- Each upstream portability concern occupies its own ordered patch.
- Every source pin is checked before patches are applied.
- Every static gate has a passing fixture and a mutation that it rejects.
- Expected compile failures identify one exact boundary and become positive
  compile tests as soon as that boundary is removed.
- Compatibility helpers are shared only after their ownership and error
  semantics agree; coincidentally similar types are not merged prematurely.
- Unsupported runtime behavior returns an intentional error and has a negative
  test. It never falls through a placeholder callback.
- ABI, HRESULT, reference ownership, resource state, and output bytes are tested
  as contracts rather than inferred from implementation structure.

## Measurable milestones

| Area | Current evidence | Exit gate |
| --- | --- | --- |
| Clean-room DDI | Independent layout model, C/C++ padding builds, promoted-signature negatives, 72-slot frame/readback harness | Every host-reachable callback promoted and invoked; all unsupported callbacks fail intentionally |
| D3D12TranslationLayer source | Shared ownership removes all 15 `CComPtr`, all 3 `CComHeapPtr`, and the controlled ATL include | Prepared inventory reaches zero except explicitly retained public-SDK paths |
| D3D12TranslationLayer compiler | Ownership and ETW contracts pass under native C++ and MinGW/Wine; the dependency edge preprocesses to completion with warnings as errors | Every translation unit compiles with C++17 and warnings as errors |
| D3D12TranslationLayer linker | Three static archives contain the expected 37 unique translation-unit objects; standard CI now links them into D3D11On12 | Static library links reproducibly without ATL, telemetry, PIX, or MSVC runtime imports |
| D3D11On12 source | Pinned patch series; `_com_error` and `CComPtr` removed; COM lifetime test | Prepared tree passes its portability inventory and clean-room header scan |
| D3D11On12 compiler/linker | Portable Linux/MinGW link and export/import audit added to standard CI; self-hosted EWDK remains an optional second lane | Driver DLL compiles, links reproducibly, and passes export/import audit |
| Wine host | Router and fail-closed core tests only | Real adapter/device creation, callback reachability audit, and explicit unsupported-path tests |
| Frame correctness | DDI mock readback agrees with application test expectations | Both tests pass against the real host, with centre/corner pixels compared byte-for-byte |
| Failure behavior | Interface, router, negative-compile, and concurrency tests | Allocation/interface/state-transition fault injection at every new boundary |
| Concurrency | Shared-state audit and oversubscribed core stress | Every newly shared object adds lifetime and shutdown stress before integration |

## Patch order

The D3D12TranslationLayer port proceeds in dependency order:

1. Replace ATL pointer and heap ownership helpers with tested, minimal types.
2. Replace `_com_error` while preserving HRESULTs at every catch boundary.
3. Compile out ETW, Microsoft telemetry, and PIX without removing error paths.
4. Replace isolated MSVC extensions and UUID helpers.
5. Separate public-SDK functionality from optional WDK/DXBC functionality.
6. Make CMake configure and compile every supported translation unit with
   MinGW-w64 GCC C++17.
7. Link and audit the static library, then consume it from D3D11On12.

Each step updates `docs/dtl-portability-baseline.json`; unexplained count
increases fail CI. A decrease is reviewed alongside the patch that caused it.
