# Documentation improvement plan

> **Update: the AMD backend was removed.** The owner decided to drop the AMD
> fallback entirely as long as Sedici stays a drop-in replacement. Text below that
> proposes or describes an AMD backend, a bundled AMD binary, `.orig` forwarding or
> backend fallback is kept as design history and no longer applies. The last commit
> with that code is the tag `pre-amd-removal`; [amd-binary.md](../amd-binary.md) keeps
> what was learned about the binary.

Status: planned documentation work before implementing the refactor. This records
the review scope; it does not settle open architectural choices, change shipped
behavior, or authorize implementation, deployment, or game launches.

The existing [refactor documents](../README.md) capture the direction, but need
more precise contracts before they can guide incremental implementation. Improve
those documents rather than adding duplicate explanations.

## 1. Clarify decision status

- Label requirements and decisions consistently as **decided**, **proposed**, or
  **unresolved**.
- Separate user-selected requirements from architectural recommendations.
- Identify prerequisites and evidence needed to close each unresolved question.
- Do not present suggested defaults, module boundaries, or fallback capabilities
  as already approved or implemented.

Primary location: [decision status](../README.md#decision-status), with local
status notes where a reader could otherwise mistake a proposal for a contract.

## 2. Specify ownership and lifecycle

- Identify owners of FFX contexts, swapchains, callbacks, frame resources,
  descriptors, command allocators, and submissions.
- Describe creation, history warmup, FG off/on, resize, window replacement,
  failure, draining, and shutdown.
- For each transition, specify accepted work, outstanding ownership, observable
  results, and safe resource reuse/release conditions.
- Make thread ownership, callback lifetime/reentrancy, and handoff obligations
  explicit without prematurely choosing every helper class or container.

Primary location: [architecture.md](../architecture.md).

## 3. Define the AMD backend contract

- Distinguish the bundled official AMD fallback binary from the game's `.orig`
  compatibility provider.
- Record the binary version, ABI, loading/dependency, provenance, and redistribution
  questions that must be answered before integration.
- Establish which supported generation and composition interfaces are available,
  including whether AMD can feed Sedici's presentation infrastructure.
- Distinguish pre-ownership selection, real-frame degradation, recreation-boundary
  fallback, and any future seamless handoff. Do not promise the last implicitly.
- Specify which unknown/unsupported calls can be forwarded to which valid owner.

Primary location: [architecture.md](../architecture.md), with dependency placement
and packaging details in [file-structure.md](../file-structure.md).

## 4. Make the TOML proposal reviewable

- For every proposed public setting, document its meaning, units, type, default,
  validation, backend applicability, and application boundary.
- Specify requested versus acknowledged/applied state, rejection, pending changes,
  and recreation/restart requirements.
- Define save, cancel, external-edit conflicts, legacy migration, and any retained
  environment override precedence.
- Account for existing pacing opt-outs, fractional/adaptive work, and compatibility
  semantics; do not silently remove or reinterpret settings.
- Leave unresolved choices explicitly unresolved rather than inventing an answer
  to make the example look complete.

Primary location: [configuration.md](../configuration.md). Keep its candidate
schema clearly separate from the currently supported root `sedici.toml`.

## 5. Document incremental implementation

Prepare a dedicated implementation-plan document after the contracts above are
sufficiently clear. It should describe small migration slices, not a giant rewrite
checklist or a mandate to move every file first.

For each slice, record:

1. Responsibility being migrated and its destination owner.
2. Prerequisites, unresolved details, and scope boundaries.
3. Observable behavior that must remain unchanged.
4. Existing failures intentionally addressed.
5. Characterization and product acceptance tests.
6. Acceptance evidence, safe rollback, and removal of the superseded path.

Start with production-path integration coverage and coherent session identity/status
as the proposed first milestone. Investigate AMD feasibility early. Refine later
slices from evidence rather than deciding every class upfront. Do not keep two
authoritative settings stores or lifecycle managers during migration.

## 6. Separate tests, evidence, and requirements

- Distinguish verified current behavior, reported/known failures, and future
  acceptance requirements.
- Separate characterization tests protecting valid compatibility behavior from
  acceptance tests establishing intended integrated behavior.
- Prioritize lifecycle transitions, failure branches, settings acknowledgements,
  backend ownership, and genuinely in-flight GPU resources over an arbitrary
  line-coverage target.
- Define combined coverage for generation, overlay, live cap changes, metrics,
  resize, FG toggles, and teardown through production entry points.
- Keep deterministic policy checks and the focused offline DX12 host; avoid a broad
  GPU Cartesian matrix. Recorded failures must not become silently green tests.

Primary location: [reliability.md](../reliability.md). Link historical evidence in
[findings](../../findings/) rather than rewriting it as current acceptance.

## 7. Reconcile existing documentation

- Review stale or conflicting statements in the root
  [README.md](../../../README.md), [MILESTONES.md](../../../MILESTONES.md), and
  [AGENTS.md](../../../AGENTS.md).
- Distinguish current implementation facts from refactor targets and historical
  measurements.
- Resolve `.orig` fallback, bundled-runtime deployment, defaults, and safety-rule
  conflicts explicitly before changing those contracts.
- Keep one delivery source of truth. Do not create competing milestone copies or
  duplicate ownership/configuration rules across documents.
- Preserve historical findings and third-party attribution/license obligations.

## Documentation acceptance gate

A developer implementing one slice must be able to identify its owner, allowed
dependencies, observable contract, failure behavior, and acceptance tests without
guessing.

Before implementation, the documents should make clear what is settled, what still
requires a decision, and what evidence will resolve it. Links and examples should
be checked, proposed paths must not be described as existing code, and documentation
work must not silently change runtime defaults or safety restrictions.
