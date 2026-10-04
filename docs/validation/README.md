# Relay12 Validation Archive

This directory stores historical validation reports, experimental trials, and empirical diagnostics produced during the development of Relay12.

For active project scope, completed milestones, and upcoming Phase 2 priorities, consult [docs/ROADMAP.md](../ROADMAP.md).

---

## Chronological Archive

| Date | Milestone / Investigation | Directory | Summary |
| :--- | :--- | :--- | :--- |
| **2026-09-23** | First Frame Rendering | [2026-09-23-first-frame](2026-09-23-first-frame/README.md) | Validation of clear and draw submission through `relay12-d3d11` DDI to `D3D11On12` on macOS / Apple Silicon. |
| **2026-09-24** | Wrapped D3D12 Resources | [2026-09-24-wrapped-resources](2026-09-24-wrapped-resources/README.md) | Verification of wrapped resource interop, command queue fence signaling, and staging readback synchronization. |
| **2026-09-28** | Batch Handoff | [2026-09-28-batch-handoff](2026-09-28-batch-handoff/) | Comparative validation of batch handoff synchronization across standard, ESYNC, and MSYNC modes. |
| **2026-09-28** | Batch Handoff Sweep | [2026-09-28-batch-handoff-sweep](2026-09-28-batch-handoff-sweep/) | Parameterized sweeps across batch sizes to evaluate latency and queue throughput. |
| **2026-09-30** | UMA Memory Management | [2026-09-30-uma-memory](2026-09-30-uma-memory/README.md) | Initial evaluation of staging buffer management and memory pools across unified memory architectures. |
| **2026-10-01** | MSYNC & PEAK Investigation | [2026-10-01-msync-investigation](2026-10-01-msync-investigation/README.md) | MSYNC synchronization deep dive, Steam client DLL placement, and game bootstrap diagnostics for PEAK. |
| **2026-10-01** | UMA Staging & Sampling Followup | [2026-10-01-uma-followup](2026-10-01-uma-followup/README.md) | Extended validation of SRV sampling, staging readback fidelity, and pool behavior under Wine. |
| **2026-10-02** | D3DMetal Coherent UMA Override | [2026-10-02-forced-uma](2026-10-02-forced-uma/README.md) | Verification confirming false UMA capability reports originate at the closed D3DMetal backend boundary. |
| **2026-10-02** | Phase 1 Agent A Findings | [2026-10-02-phase1-agent-a](2026-10-02-phase1-agent-a/README.md) | Architectural analysis of forced host-visible uploads, pixel correctness, and coherency isolation. |
| **2026-10-02** | Phase 1 Agent C Findings | [2026-10-02-phase1-agent-c](2026-10-02-phase1-agent-c/README.md) | Resolution of 32/64-bit Steam DLL placement, environment handoff, and pre-window execution analysis. |
| **2026-10-02** | UMA Burst Policy Hardware Validation | [2026-10-02-uma-burst-policy](2026-10-02-uma-burst-policy/README.md) | Hardware validation of burst memory policies and graceful trimming to eliminate allocation churn. |
| **2026-10-02** | UMA Benchmark Variance & Transfer Trials | [2026-10-02-uma-variance-dispatch](2026-10-02-uma-variance-dispatch/README.md) | Transfer-only benchmark trials decoupling GPU readback latency from CPU staging cache churn. |
| **2026-10-02** | PEAK Direct Startup Diagnostic | [2026-10-02-peak-direct-diagnostic](2026-10-02-peak-direct-diagnostic/README.md) | Diagnostic traces of direct PEAK startup evaluating initialization logs and timeout bounds. |
| **2026-10-02** | PEAK Steam-Ready Live Launch | [2026-10-02-peak-steam-ready](2026-10-02-peak-steam-ready/README.md) | Live authenticated Steam launch with Relay12 rendering the PEAK main menu under MSYNC. |

---

## Archival Policy

- **Permanence**: Reports in this directory are immutable historical artifacts preserving exact telemetry, logs, and findings at specific points in time.
- **Living Status**: Forward-looking implementation plans and active TODOs are tracked exclusively in [docs/ROADMAP.md](../ROADMAP.md).
