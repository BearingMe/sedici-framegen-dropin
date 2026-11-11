# Stage 2 feature and model contract

> **Status:** draft for the P2.11 handoff. The "as built" statements were checked against
> `src/` on 2026-11-09. K47 and K77 are closed, and the retirement rule they describe is
> implemented.

Status: draft, written for P2.11 (2026-11-08). **Owner decision 2026-11-08: all AI
integration is postponed until the current system is fully refactored and stable.**
Nothing here is implemented and no Stage 2 work starts before that. It fixes the interface a residual network must meet so that adding it
does not reopen Stage 1, the presenter or the retirement rules.

Each item is marked **as built** (taken from the code today), **from the paper**
(`references/papers/realtime-fg.md`, a design estimate, not measured) or **decision**
(not settled; the owner or the training plan has to choose before code is written).

## 1. Where Stage 2 sits

- **As built:** `Stage1::generate()` has a `synthesize()` step that writes the
  midpoint into `m_midpoint`, then `composite_hud()` writes the output. The reset
  seed path copies a real frame and never calls `synthesize()` (K53).
- **Contract:** the network runs between `synthesize()` and `composite_hud()`, on the
  HUD-less image. It never sees the HUD and never runs on a reset seed. The final
  frame is `clamp(I_base + M * dI)` and is what `composite_hud()` receives as its
  midpoint, so the HUD logic does not change.
- **Failure:** AGENTS.md says a failed generator returns a defined error for that
  call and there is no fallback implementation. Stage 2 follows that: if the network
  cannot be recorded for a frame, the call fails with the defined error and a log
  line. Emitting the Stage 1 base instead would be a fallback and needs an explicit
  owner decision to change the rule.

## 2. Inputs (tensor layout)

**From the paper:** 16 channels at half display resolution (960x540 at 1080p):

| Channels | Content | Source today |
| --- | --- | --- |
| 3 | previous real frame warped to the target time | Stage 1 warp of `m_color[previous]` |
| 3 | current real frame warped to the target time | Stage 1 warp of `m_color[current]` |
| 3 | `I_base` | `m_midpoint` |
| 2 | warped depths | `m_depth[*]` (R32_FLOAT, decoded) |
| 4 | splatted motion vectors, both directions | `m_motion[*]` (R16G16_FLOAT, decoded) |
| 1 | analytical disocclusion and shading-change mask | **not produced today**: per the code, the fusion thresholds are applied inside the warp pass and no mask is stored |

- The two warped colour images and the mask are not separate resources in Stage 1
  today; Stage 1 would have to write them. **Decision:** whether to add the extra
  Stage 1 outputs (cost: one 8-bit mask at display size, two warped frames) or to
  feed the network the unwarped frames and the vectors and let it learn the warp.
- **Layout decision:** tensors are channel-last (NHWC) FP16 in a typed buffer, with
  channel count padded to a multiple of 8 so every 3x3 tap reads aligned vectors.
  A 16-channel input and 24/32/48-channel layers already satisfy this. Textures are
  kept only at the boundary (Stage 1 outputs, final residual).
- Half-resolution inputs come from 2x2 box averaging of the display-resolution
  Stage 1 tensors, done in one compute pass.

## 3. Phase and time

- **As built:** a generated frame has `alpha` in (0, 1), the time between the previous
  real frame (0) and the current one (1); `kMaxPhases = 2` phases can be recorded for
  one real pair, each into its own output, and history advances once.
- **Contract:** `alpha` is an input of the network, not a constant. It is passed as
  one extra constant-valued channel (so 17 channels, padded to 24) or as a root
  constant added to the first layer's bias; **decision**, to be fixed by training.
  The training data must cover the alphas the scheduler produces (the offline
  checks use 1/3, 1/2 and 2/3).
- Each phase needs its own network run. At 2x with two phases per real frame the cost
  is twice the single-frame estimate; the paper's 1.30-1.67 ms figure is for one frame.

## 4. Colour normalisation

- **As built:** all colour is `R8G8B8A8_UNORM` in whatever encoding the game hands
  over; Stage 1 does no transfer-function conversion, and which encoding each game
  uses has not been verified in this repository.
- **Contract:** the network input is `color / 255` in the same encoding the game
  presents (no sRGB-to-linear step), in [0, 1]. `dI` is in [-1, 1] and `M` in [0, 1];
  the output is clamped to [0, 1] and quantised once, at the end. HDR or 10-bit
  back buffers are outside this contract (K14: wrapping already requires 8-bit).
- Depth and motion keep the units Stage 1 already normalises to (depth after
  `depthScale`/`depthBias`, motion in pixels of the render size). The model is trained
  with those exact conventions, including reverse-Z.

## 5. Reset and history

- **As built:** `resetHistory` or an invalid history makes every output the real frame
  (`generate()` seed path), and discarding the prepare snapshot forces a seed.
- **Contract:** the network has no recurrent state, only the two real frames Stage 1
  already holds. A reset therefore needs nothing extra in Stage 2: it is skipped on a
  seed, exactly like `synthesize()`. If a later model adds temporal state,
  that state is reset by the same flag and is covered by the retirement rule in section 8.
- A camera cut is a reset. The cut signal is still an open question (see AGENTS.md);
  Stage 2 does not decide it.

## 6. Dimensions and padding

- The network runs at half display resolution; display sizes that are odd in either
  dimension are rounded up, and the extra row or column is edge-replicated on input and
  cropped on output.
- 3x3 convolutions use zero padding at the border for the first version (the
  training must use the same). Pooling uses 2x2 with the same round-up rule at each
  level; the 135p level of a 1080p frame is 240x135, and an odd size is replicated.
- Render size and display size are independent in Stage 1 today (render may be smaller
  than display). The network consumes Stage 1's display-resolution products, so render
  size does not enter the network contract; `maxRenderSize` is only a Stage 1 matter.
- A resolution change recreates all Stage 2 resources in `configure()` together with the
  Stage 1 targets, under the same retirement wait.

## 7. Provenance and licensing

- Weights are delivered as a versioned file beside the DLL (**decision**: or embedded
  in the DLL as `src/generator/lut_data.h` is today). Whichever is chosen, it carries: model
  version, training data description, training code revision, input conventions of
  sections 2-4, tensor layout and quantisation, and a checksum the loader verifies.
- A weights file that is missing, fails the checksum or declares another contract version
  disables Stage 2 for the session with one log line; it never fails the game call.
- **Owner decision 2026-11-08 (licence):** use the most open licence that stays aligned
  with Mob-FGSR's. Mob-FGSR is CC BY-NC 4.0 and has no ShareAlike clause, so Sedici's
  own original code may carry a more open licence (proposed: MIT, as the vendored
  Dear ImGui and toml++ are), while everything adapted from Mob-FGSR (the Stage 1
  shaders, the LUT, and any weights trained on Mob-derived outputs) stays CC BY-NC 4.0
  with attribution. A binary that contains the adapted parts is therefore
  noncommercial-only. See NOTICE.md. This is a plan, not legal advice.
- Origin of any adapted code is recorded in `docs/` before it is vendored: the GFFE
  and STSSNet architecture notes are in `references/papers/`; Mob-FGSR is CC BY-NC 4.0
  (`docs/mobfgsr-attribution.md`), and a trained model derived from its outputs inherits that
  question. **Decision** for the owner before any shipped weights.
- Training and export happen offline, outside this repository's build.

## 8. Scratch memory and retirement

**Activation memory, from the paper's layer table (not measured).** Counting every
layer output at its own resolution (input 16 ch and stem 24 ch at 960x540; 32 ch twice
at 480x270; 48 ch twice at 240x135; decoder 32 ch at 480x270, 24 ch at 960x540; head
16 ch at 960x540) gives 57.0 million elements: about **114 MB in FP16 (57 MB INT8)
per in-flight network run** if nothing is reused. Liveness reuse brings that down, but
the skip connections (stem and enc1_b outputs) must stay alive until the decoder reads
them. Weights are small (92,992 parameters, 182 KB FP16).

**Stage 1 today, as built (from `create_targets()`):** about 44 bytes per pixel when render
size equals display size: decoded depth and motion 8, depth/motion/colour history pairs
24, reprojection and filled 8, midpoint 4. That is roughly **91 MB at 1920x1080**, one
single in-flight slot. The presenter's snapshot ring and its three generated-image slots
(K15) are on top of that and are not yet measured.

- **Frame slots:** Stage 1 is a single in-flight slot and `kMaxPhases = 2` outputs are
  recorded per real frame. Stage 2 scratch is owned per slot, never shared between slots
  that can be in flight together. With two phases per real frame, either the scratch is
  reused phase after phase on the GPU timeline (barriers, one activation set) or it is
  doubled. **Decision**, to be taken from a measured activation peak.
- **Retirement:** Stage 2 work is recorded on the same command list as Stage 1's
  `generate()` and covered by the same fence value that `retirable()` and the
  `Context::retired()` rule already use. Scratch, weights and descriptors are released
  only after that fence completes; `configure()`, `release_targets()` and context destroy
  wait for it with the existing bounded wait (K47/K77 apply unchanged).
- **Budget:** the combined 1.30-1.67 ms (FP16) is a design estimate. Stage 1 alone was
  measured at 0.61 ms at 1080p on the RX 7700 XT (after merging decode and dilate; 0.64-0.69 ms
  before) against the paper's 0.15-0.30 ms (K117, resolved in P4.4). With Stage 2 at 1.10-1.35 ms the sum is
  1.71-1.96 ms against 2.0 ms; rebuild the budget from measured numbers once Stage 2 exists.
  Compare timing runs without the debug layer (`--no-debug-layer`); validation runs
  are for correctness only.

## 9. Gate to start Stage 2

1. K117 resolved (P4.4): per-pass Stage 1 timings are in `docs/archive/known-issues.md` (K117); the decision is to accept 0.61 ms.
2. The decisions marked above are made (failure answer, extra Stage 1 outputs, alpha
   input, weight delivery, licence question, scratch reuse).
3. A training plan exists (data capture from the FFX hook, loss, export to the layout of
   section 2); it is its own deliverable and is not part of this repository's build.
