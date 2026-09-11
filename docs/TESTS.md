# Testing strategy

This document separates contracts enforced today from validation that depends
on future host integration. Project milestone status belongs in
`PORT-QUALITY-ROADMAP.md`.

## Implemented gates

### Clean-room declarations

- `check_ddi_header.py` enforces declaration-group provenance and rejects
  forbidden packing.
- `gen_ddi_layout.py --check` independently derives member layout and detects
  missing, stale, or hand-edited assertions.
- The C and C++ layout builds check size, alignment, offsets, and callable
  promoted slots under MinGW.
- `-Wpadded -Werror` requires every ABI gap to be explicit.
- Negative translation units prove placeholders and promoted signatures reject
  invalid arguments.
- Compiler dependency files prove clean-room tests resolve no SDK/WDK overlay
  header.

### Behavioural contracts

- `d3d11ddi_triangle.c` drives the promoted mock table through create, bind,
  clear, draw, copy, map, verify, unmap, and teardown. It checks call order,
  handle identity, and expected centre/corner bytes.
- `d3d11ddinegotiate.c` checks version-buffer sizing and selection behavior.
- Router and core tests cover missing components, status reporting, interface
  identity, output slots, and fail-closed behavior.
- Shared-state and oversubscribed core stress tests cover concurrency that
  exists in the current implementation.

The mock frame harness proves table behavior; it does not render or execute a
Metal pipeline.

### Upstream portability

- `inventory_dtl_portability.py` records category counts, locations, and
  acknowledged high-water increases.
- Prepared-tree gates cover struct-return wrappers and exact filename case for
  project-local includes.
- Native and MinGW/Wine contract tests cover COM ownership, HRESULT identity,
  telemetry no-ops, and structure-return ABI selection.
- The Linux CMake lane compiles DTL translation units and verifies archive
  member counts. Only a green run is evidence that this contract is satisfied.
- Reproducible source packaging and PE import/export audits protect the
  deliverable boundary.

## Planned validation

### Integrated host and graphics output

- Run `e2e_d3d11_triangle.cpp` against the real Wine host and D3D11On12 driver.
- Compare centre and corner pixels byte-for-byte with the mock harness.
- Exercise adapter/device creation, callback reachability, wrapped resources,
  barriers, queue synchronization, and teardown.

### Fault injection and memory safety

- Inject allocation, interface, and state-transition failures at each new
  boundary and assert exact cleanup and HRESULT behavior.
- Add native ASan/UBSan targets for platform-independent lifetime and state
  algorithms when those components exist.
- Add dirty-memory fixtures for structures whose producer must initialize every
  byte consumed across the ABI.

### Fuzzing and concurrency

- Add structured fuzz targets only after real parsing or state-machine entry
  points exist; seed them with valid captured contract fixtures.
- Extend shutdown, reference-lifetime, and oversubscription stress whenever a
  new object becomes shared.
- Treat GPU completion and CPU visibility as separate assertions once real
  command submission is available.

## Contribution rule

Every new gate needs a passing fixture and a mutation it rejects. Every planned
test becomes an implemented claim only when its command runs in required CI and
the failure signal has been demonstrated.
