# Relay12 roadmap

Use this index for current scope, remaining work, and the evidence required to
promote it. The linked subsystem documents own their detailed contracts; dated
validation reports describe only their recorded source revisions and hardware.

| Area | Current source of truth |
| --- | --- |
| Architecture, supported operations, and readiness | [D3D11On12](D3D11ON12.md) |
| Remaining DDI groups and promotion requirements | [DDI roadmap](DDI-REMAINING-ROADMAP.md) |
| Wine frontend integration milestones | [Implementation plan](RELAY12-IMPLEMENTATION-PLAN.md) |
| Clean-room declaration provenance | [Clean-room boundary](CLEANROOM-DDI.md) |
| Test commands and acceptance conditions | [Tests](TESTS.md) |
| Memory ownership and profiles | [UMA memory](UMA-MEMORY.md) |
| Performance hypotheses and measurements | [Performance research](PERFORMANCE-RESEARCH-ROADMAP.md) |
| Deferred backend creation and activation | [Lazy initialization](LAZY-INITIALIZATION.md) |
| Recorded runtime results | [Validation reports](validation/) |
| Isolated historical measurements | [Opt-in research](../research/README.md) |

The older consolidation branches `codex/port-roadmap-consolidation` and
`feat/uma-memory` proposed moving several documents into one roadmap. This
index retains their navigation intent without deleting existing links or
copying superseded milestone and performance claims. Completing an interface,
passing a mock test, producing a GPU readback, rendering video, and validating
a game are separate acceptance results. Each remains attached to the relevant
subsystem document and the exact tested source/artifact.
