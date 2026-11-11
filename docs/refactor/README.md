# Sedici refactor decisions

> **Update: the AMD backend was removed.** The owner decided to drop the AMD
> fallback entirely as long as Sedici stays a drop-in replacement. Text below that
> proposes or describes an AMD backend, a bundled AMD binary, `.orig` forwarding or
> backend fallback is kept as design history and no longer applies. The last commit
> with that code is the tag `pre-amd-removal`; [amd-binary.md](amd-binary.md) keeps
> what was learned about the binary.

> **Status (2026-11-09): decision record.** The AMD rows in the decision status are
> superseded. Phase 2 hardened the flat modules in place; Phase 3 (done) moved them into
> the layout in [file-structure.md](file-structure.md) version 2 without changing behaviour. The
> current tree is in [docs/dev/layout.md](../dev/layout.md).
> [audit.md](audit.md) is historical.

This directory records the refactor discussion and proposed implementation
contracts. It describes a **target**, not the shipped configuration (the source layout part of
the target was built in Phase 3). Creating these documents does not implement or authorize runtime,
deployment, binary redistribution, or default-setting changes.

## Objective

Ship one coherent product containing:

- Sedici's two-stage frame generator: Mob-FGSR analytical generation followed by
  GFFE-inspired residual correction, as specified in
  [the design note](../../references/papers/realtime-fg.md).
- No AMD backend: Sedici is the only implementation behind the drop-in DLL.
- A working overlay for live settings and metrics.
- One understandable TOML settings surface, without hidden experimental switches
  or monitor-specific defaults.
- Explicit ownership, meaningful errors, bounded diagnostics and recovery, and
  tests of the components operating together.

The design note's GPU budgets are estimates, not acceptance measurements. Training
and model/LUT export remain offline, outside the shipped runtime.

## Document map

| Document | Owns |
| --- | --- |
| [architecture.md](architecture.md) | Module boundaries, dependency direction, session and GPU ownership |
| [file-structure.md](file-structure.md) | The Phase 3 module layout (version 2) and the file-by-file migration map |
| [configuration.md](configuration.md) | Proposed TOML, application semantics, defaults, and legacy migration |
| [reliability.md](reliability.md) | Errors, watchdog behavior, diagnostics, and integrated acceptance |
| [stage2-contract.md](stage2-contract.md) | Draft interface a residual network must meet: tensor layout, phase, colour, reset, padding, provenance, scratch and retirement |
| [audit.md](audit.md) | Historical static review (f95b54d) and the P2.2 handoff |
| [known-issues.md](../archive/known-issues.md) | Archived: the K1-K147 defect list (open ones moved to GitHub issues on 2026-11-11) |
| [feature-requests.md](../archive/feature-requests.md) | Archived: the FR-01 to FR-05 requests (FR-03 moved to a GitHub issue) |

## Decision status

| Decision | Status |
| --- | --- |
| Two generation stages, live UI/metrics, and understandable TOML (the AMD fallback was dropped by a later owner decision) | User-stated product requirements |
| ~~Vendor the official AMD binary, not the full source implementation~~ | Superseded: the AMD backend was removed |
| Session-owned modular runtime rather than independently patched global services | Recommended architectural baseline |
| Separate public settings requests from acknowledged runtime state | Recommended contract |
| Shared GPU/presentation infrastructure with backend-specific integration | Recommended boundary; AMD integration feasibility remains open |
| ~~Exact binary version, redistribution terms, deployment paths, and loading scheme~~ | Not applicable: no AMD binary is loaded or shipped |
| ~~Seamless mid-session handoff from Sedici to AMD~~ | Not applicable |
| Exact TOML spelling, public options, and future defaults | Proposed in configuration.md; not a shipped schema |

## Alternatives considered

1. **Session-owned modules:** one runtime owns lifecycle and settings application;
   concrete backend and presentation components perform work. Recommended for its
   explicit integration owner and incremental migration path.
2. **Backend vertical slices:** AMD and Sedici each own most of their integration.
   Strong isolation, but UI, limiter, and metrics behavior can drift or duplicate.
3. **Pure policy core with adapters:** very testable decisions, but wrapping every
   COM operation would add unnecessary indirection. Use pure policy functions
   where they have real independent inputs and outputs.
4. **Frame-pipeline organization:** strongest resource/pass boundaries for the two
   stages, but insufficient alone for FFX contexts, UI, and configuration. Use an
   explicit pass sequence inside the Sedici backend, not a generic render graph.
5. **Message-owned runtime:** clear thread ownership, but synchronous FFX results
   and borrowed command lists prevent blindly deferring all work. Use bounded,
   typed handoffs where asynchronous ownership actually exists.

The recommendation is not to combine five frameworks. Start with ordinary C++
objects, explicit dependencies, pure scheduling decisions, and one bounded
presenter handoff. Split files and introduce interfaces only for concrete needs.

## Implementation sequence

1. Settle fallback scope, AMD binary compatibility, public settings, and ownership
   contracts before changing behavior.
2. Establish a combined acceptance scenario that exposes the integration failures.
3. Introduce the session owner while retaining verified shaders and algorithms.
4. Route settings application, status, limiter integration, and lifecycle through
   that owner; make the UI a client rather than a subsystem coordinator.
5. Establish explicit GPU retirement and post-generation composition boundaries.
6. Accept both backends through the same product scenarios.
7. Add the Stage 2 feature contract, model validation, and inference implementation.

P2.1 is audit and documentation; P2.2 establishes coverage before structural
migration. The integrated product gate is generation, overlay, live cap changes,
resize, backend selection/fallback, and teardown working together through
production entry points with truthful diagnostics; it is not a file move.

## Relationship to existing instructions

[AGENTS.md](../../AGENTS.md) remains the current safety/workflow authority, and
[MILESTONES.md](../../MILESTONES.md) remains the delivery source of truth until an
explicitly approved update. These notes do not silently override current pacing
defaults, opt-outs, forwarding rules, or deploy restrictions.

In particular, the bundled-runtime proposal requires reconciling today's `.orig`
fallback rule and deploy allowlist before implementation. Do not add a second
milestone document or copy historical findings here. Evidence remains in
[findings](../findings/), and license obligations remain in their attribution files.
