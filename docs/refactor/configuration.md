# TOML and live configuration contract

Status: proposed refactor schema and behavior. **Do not copy this example over the
current `sedici.toml`: the current parser does not implement these names.** Existing
defaults and opt-outs remain unchanged until an explicitly approved migration.
Current defaults and direct environment reads are inventoried in the
[audit](audit.md#configuration-and-diagnostic-observations); the shipped file and
`src/config.cpp` remain authoritative for implemented parsing.

## Design decisions

- TOML describes supported product intent, not every internal timing experiment.
- File ingestion, UI edits, validation, application, and save share one typed
  settings model; they do not maintain independent lists of settings/side effects.
- Runtime status distinguishes requested, applied, pending, rejected, and
  recreation/restart-required settings.
- No hidden environment reads in GPU, backend, or presentation code. If overrides
  remain supported, ingest and report them centrally with the same validation.
- Every public setting has documented units, bounds, backend applicability,
  default, and application timing. Unsupported settings are not silently ignored.

## Candidate minimal file

```toml
[frame_generation]
backend = "fsr3"       # "fsr3" or "sedici"; recreation/restart required

[limiter]
real_fps = 0           # 0 disables the real-frame cap; fractions are supported

[presentation]
sync = "game"         # Preserve game policy; explicit vsync override is proposed

[ui]
mode = "off"          # "off", "mini", or "full" startup preference
hotkey = "alt+f10"
scale = "auto"        # Or a validated numeric scale

[logging]
level = "info"        # Proposed: "error", "info", "debug"
```

The example is intentionally minimal, not a finalized exhaustive schema. The
initial integrated baseline is 2x interpolation. Additional rate controls require
the capability and latency contract below, rather than hidden implementation knobs.

## Proposed semantics

All entries below are proposals, not parser guarantees. The example values are
candidate defaults; numeric bounds and migration details remain unresolved rather
than being silently inferred from current controls.

| Key | Meaning | Application boundary |
| --- | --- | --- |
| `frame_generation.backend` | Requested implementation, not the game's FG on/off state | Validated session creation/recreation; pending changes remain visible |
| `limiter.real_fps` | Maximum real-frame admission rate, independent of generated output count | Next acknowledged safe real-frame boundary; reset schedule coherently |
| `presentation.sync` | User synchronization intent, resolved against backend/display capability | Safe presentation boundary or recreation if needed; report actual result |
| `ui.mode` | Startup menu mode; runtime opening/closing is separate view state | Startup preference; opening the menu does not rewrite it |
| `ui.hotkey` | Validated input binding | Acknowledged input-owner update; resolve capture safely |
| `ui.scale` | Automatic or validated fixed UI scale | UI-owner frame boundary |
| `logging.level` | Diagnostic verbosity, not feature enablement | Diagnostic sink update; never reset generation |

| Candidate | Type / proposed default | Validation and applicability still to decide |
| --- | --- | --- |
| `frame_generation.backend` | enum string / `fsr3` | Allowed backends, binary availability and relationship to legacy `.orig`; P2.3/P2.6. |
| `limiter.real_fps` | finite number in real frames/s / `0` | Upper bound and applicable backend paths; `0` disables. |
| `presentation.sync` | enum string / `game` | Exact supported choices and backend/display capabilities. |
| `ui.mode` | enum string / `off` | `off`, `mini`, `full`; backend overlay support must be verified. |
| `ui.hotkey` | binding string / `alt+f10` | Accepted syntax, conflicts and window/input applicability. |
| `ui.scale` | `auto` or finite number / `auto` | Numeric bounds and supported renderers. |
| `logging.level` | enum string / `info` | Final level mapping, precedence and sink capabilities. |

Every candidate also requires an explicit legacy-alias and override-precedence
decision before implementation; none changes the effective startup configuration.

Recommended defaults are FSR3 while the combined Sedici path remains unaccepted,
limiter off, and UI off until requested. `sync = "game"` is a proposed predictable
default, **not a claim of tear-free presentation**; game tearing behavior remains
visible. Final sync policy and overrides need backend/display acceptance first.
Do not hardcode a 75 Hz panel, 36.5 FPS cap, or an undisclosed refresh margin.

Backend selection does not force the game's own FG setting on. A selected backend
can legitimately be active with generation disabled by the game.

## Settings transaction

```text
TOML/defaults/documented overrides or UI request
  -> validated complete candidate
  -> runtime request revision N
  -> apply at component-safe boundaries
  -> publish acknowledgement and applied status
  -> optional persistence of the acknowledged snapshot
```

- Validate cross-setting/backend constraints before application, not only scalar
  types. Numeric inputs must be finite; FPS must be nonnegative and within the
  approved supported range. Final bounds and units must be part of the schema.
- Capture coherent immutable snapshots with revisions; independent relaxed
  atomics are not a transaction across related fields.
- UI controls submit requests. They never directly reset limiter timers, call
  backend configure functions, or manage a presenter lifecycle.
- A component failure must not publish a partially applied candidate as successful.
  Keep prior applied values or report field-level pending/rejected outcomes.
- Preserve old resources until the change's required GPU retirement completes.
- Backend-inapplicable controls are unavailable with an explanation; do not show
  a successful slider change that the selected implementation cannot apply.
- TOML is read at startup. Automatic file watching/hot reload is not a requirement.
  Any future explicit reload must use this same transaction path.

## Save, cancel, and file errors

- Live edits request runtime application. **Save** persists the acknowledged
  snapshot; **Close** does not imply persistence.
- **Cancel** submits restoration of the captured applied snapshot through the same
  runtime path. It is not a sequence of direct global writes. Concurrent edits or
  newer revisions require an explicit conflict policy, not a blind rollback.
- The UI requests persistence through runtime/settings services; disk writes and
  file inspection do not execute in the output rendering callback.
- Preserve comments/unrelated content where safely supported. Refuse an unsafe
  patch or external-edit conflict rather than overwrite user work.
- Use a safe replacement strategy. A save failure retains the previous valid file
  and active runtime settings and reports the failure to UI and diagnostics.
- Missing files use defaults. Syntax errors use safe documented defaults; invalid
  individual values and unknown/obsolete keys produce actionable diagnostics.
  Validate the resulting complete candidate before application.
- Bad configuration is non-fatal to game calls. Do not silently rewrite the file
  merely because parsing or migration reported a problem.

## Legacy settings and experimental controls

| Existing surface | Refactor treatment to settle before implementation |
| --- | --- |
| `fg.mode = "amd"` | Currently means game `.orig`, not the bundled binary. Do not silently reinterpret it as the new `fsr3` backend. |
| `fg.present`, `fg.tear_guard` | Replace implementation choices with explicit presentation intent/capability policy; preserve or warn during migration. |
| `fg.multiplier`, `fg.target_fps` | Current fractional/adaptive work must be accounted for, not deleted by omission from the minimal example. |
| `limiter.fps` | Candidate rename to `limiter.real_fps`; preserve fractional units/meaning and disabled behavior. |
| `pacing.*`, `async.mode`, `limiter.spin_us` | Separate supported user intent from internal tuning; current pacing-on default and opt-outs remain required until approved changes. |
| `log.level`, `MFSRFG_LOG` | Candidate migration to named logging levels, with explicit precedence/deprecation. |
| Direct A/B environment switches | Centralize and document any retained override; do not hide normal product behavior behind debug variables. |

Do not solve option sprawl by moving the same undocumented settings from TOML to
environment variables. Internal experiments belong in clearly identified developer
builds or the offline host, not the normal user surface. Compatibility guards such
as third-party overlay detection are reported decisions, not invisible mode changes.

If fractional/adaptive output is exposed, specify backend support, multiplier vs
output-target semantics, real-frame omission, latency, and overload behavior. Apply
it coherently with the cap and sync policy; do not expose combinations known not
to work. Do not promise that requested output FPS equals displayed FPS.

## Open decisions

- Final key names, numeric bounds, schema/version migration, and alias lifetime.
- Which legacy overrides remain supported and their precedence over live edits.
- Final sync choices/defaults and how unsupported combinations are surfaced.
- Public multiplier/adaptive controls and their AMD backend applicability.
- Which legacy pacing/async controls remain product settings versus experiments.
- Exact backend recreation boundary; seamless live backend switching is not assumed.

Resolve these before changing the shipped file, parser, UI, persistence, or defaults.
