# Sedici refactor decisions

This directory records the refactor discussion and proposed implementation
contracts. It describes a **target**, not the current source layout or shipped
configuration. Creating these documents does not implement or authorize runtime,
deployment, binary redistribution, or default-setting changes.

## Objective

Ship one coherent product containing:

- Sedici's two-stage frame generator: Mob-FGSR analytical generation followed by
  GFFE-inspired residual correction, as specified in
  [the design note](../../references/papers/realtime-fg.md).
- An official, pinned AMD frame-generation **binary** as a fallback backend, not
  a vendored build of AMD's generator source.
- A working overlay for live settings and metrics on both backend paths.
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
| [file-structure.md](file-structure.md) | Target paths, placement rules, and migration from current files |
| [configuration.md](configuration.md) | Proposed TOML, application semantics, defaults, and legacy migration |
| [reliability.md](reliability.md) | Errors, watchdog behavior, diagnostics, and integrated acceptance |
| [audit.md](audit.md) | Current code paths, ownership risks, evidence and prioritized P2.2 handoff |
| [known-issues.md](known-issues.md) | Defects found and deliberately deferred until after the refactor, with evidence and owning milestone |

## Decision status

| Decision | Status |
| --- | --- |
| Two generation stages, AMD fallback, live UI/metrics, and understandable TOML | User-stated product requirements |
| Vendor the official AMD binary, not the full source implementation | User-selected dependency direction |
| Session-owned modular runtime rather than independently patched global services | Recommended architectural baseline |
| Separate public settings requests from acknowledged runtime state | Recommended contract |
| Shared GPU/presentation infrastructure with backend-specific integration | Recommended boundary; AMD integration feasibility remains open |
| Exact binary version, redistribution terms, deployment paths, and loading scheme | Unresolved |
| Seamless mid-session handoff from Sedici to AMD | Not promised; requires a demonstrated recreation/handoff contract |
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
