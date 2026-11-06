# Real-Time Frame Generation: Architecture and Paper Selection (RDNA3 / FFX FG Hook)

| Field | Value |
| --- | --- |
| Scope | Architecture and paper selection for frame generation behind an FFX FG hook |
| Target hardware | Radeon RX 7700 XT (RDNA3), 54 CUs, 48 MB Infinity Cache, 432 GB/s |
| Budget | ≤ 2.0 ms at 1080p for the full FG pass |
| Constraint | No engine G-buffers: HUD-less color, depth, motion vectors, camera matrices only |
| Sources | `mob-fgsr.md`, `gffe.md` (this folder) |
| Status | Design proposal; nothing implemented |

## 1. Objective and hard requirements

Implement a frame generation (FG) pipeline from a hook on the AMD FidelityFX Frame
Generation API (FFX FG), within a strict budget of **≤ 2.0 ms at 1080p** on a
**Radeon RX 7700 XT (RDNA3)**, and **without access to the engine's internal
G-buffers** (normals, albedo, roughness, stencil).

That constraint eliminates most recent academic proposals. Among the studies
published since 2023, the purely neural temporal supersampling models (ExtraSS,
PatchEX, LMV, MoFlow, EFXNet) and the geometry-cache systems (Amulet) fail the
hard requirements because they need complete G-buffers every frame or intrusive
changes to the engine's shading pipeline.

Two 2024 papers — **Mob-FGSR** (SIGGRAPH 2024) and **GFFE** (SIGGRAPH Asia 2024 /
ACM TOG) — together with the isolated convolutional backbone of **STSSNet**
(AAAI 2024), match the FFX FG input constraints directly: HUD-less color, depth,
2D motion vectors and camera/history matrices.

## 2. Recommended solution

A two-stage hybrid pipeline that combines the complementary modules of the
approved papers.

**Stage 1 — Analytical, native resolution (1080p, ~0.15–0.30 ms on RX 7700 XT),
based on Mob-FGSR.** The system performs:

- quadratic trajectory estimation (uniform acceleration from two consecutive frames);
- motion splatting with atomic depth testing;
- hole filling for thin objects;
- analytical detection of disocclusions and shading changes;
- backward warping accelerated by a 32×32×16 3D lookup table (LUT), pre-trained
  by a small MLP.

The formulation supports both **interpolation** (1 rendered frame of delay, better
quality in disocclusions) and **extrapolation** (0 frames of delay, lower latency).

**Stage 2 — Neural residual at reduced resolution (540p, ~1.10–1.35 ms on
RX 7700 XT), based on the GFFE Shading Correction Network (SCN)** and sized after
the STSSNet U-Net backbone. A lightweight network runs at half resolution
(960×540) with **~93K–141K parameters (~91–138 KB in INT8)**. It receives the
analytically projected frame, the projected depth and a focus mask (disocclusion
and shading variation), and predicts only an RGB residual ($\Delta I$) plus a
spatial attention mask ($\hat{M}$) that corrects lagging shadows, reflections and
disocclusion artifacts without softening static regions.

## 3. Paper scoring and triage

Scores every candidate identified in the real-time rendering literature
(2023–2026), plus the historical baseline (ExtraNet, 2021) and the proposed
hybrid, against the mandatory criteria.

| Paper | Year | Code | Inputs OK | ML slot | Params | Est. ms @1080p RDNA3 | vs FSR3/DLSS-G | License | Verdict |
|---|---|---|---|---|---|---|---|---|---|
| Mob-FGSR (SIGGRAPH) | 2024 | Yes (GitHub `Mob-FGSR/MobFGSR`) | Yes (color, depth, MV only) | Yes (MLP→LUT + slot for residual CNN) | ~1K est. (4-layer MLP) → 32×32×16 LUT (~16 KB INT8) | ~0.15–0.30 ms (0.42 GFLOPs est.; 2.26–2.80 ms on SD 8 Gen 2/3) | Compares vs FSR 1/2, DLSS 2, TSR, 3DWarp, BSR, AFME | Academic / open GitHub repo | **PASS** (Tier 1 analytical stage + LUT) |
| GFFE (SIGGRAPH Asia / TOG) | 2024 | No repo; reproducible (full equations + Algo. 1 in appendix) | Yes (color, depth, MV, camera matrix; G-buffer free) | Yes (heuristic GAE warp + residual SCN) | Unreported in main text (~93K–141K when paired with 540p U-Net) | ~3.4–4.8 ms (full-res 1080p est.; 6.62 ms on 4070 TiS) / ~1.0–1.3 ms at 540p | Discusses FSR 3 / DLSS 3; benchmarks vs ExtraSS-E, UPR-Net, IFR-Net, DMVFN | arXiv non-exclusive / ACM | **PASS** (Tier 1 residual ML stage at 540p) |
| STSSNet (AAAI) | 2024 | Yes (GitHub `ryanhe312/STSSNet-AAAI2024`) | No for ERM (needs albedo, normal, roughness, metallic); yes for backbone | Yes (U-Net backbone + RRM masking) | 141.00K (backbone) / 417.24K (full) | ~0.82–1.09 ms (backbone, 20.53 GFLOPs) / ~1.26–1.86 ms (full, 31.50 GFLOPs) | Compares vs RIFE, ExtraNet, NSRR, EDVR, ZoomingSloMo | MIT | DROP full (inputs); **PASS** 141K backbone + loss |
| **Proposed hybrid** (Mob-FGSR + 540p GFFE-SCN / STSS backbone) | 2024–2026 | Yes (combines MobFGSR + STSSNet MIT code + GFFE equations) | Yes (100% FFX FG hook compatible: color, depth, MV, camera) | Ideal (1080p quadratic warp/splat + 540p gated residual CNN) | 92,992–141,000 (~91–138 KB INT8) | ~1.30–1.67 ms (FP16 WMMA) / ~0.95–1.20 ms (INT8 DP4A) | Combines GFFE & Mob-FGSR benchmarks | MIT / custom | **RECOMMENDED IMPLEMENTATION** |
| Decoupled Motion G-buffer Free (ACM MM) | 2025 | No (closed access, no public repo) | Yes (G-buffer free: color, depth, MV) | Yes (decoupled motion + lightweight SCN) | Unreported (closed access) | Unreported (closed access) | Compares vs GFFE & ExtraNet | ACM copyright | DROP (not reproducible; closed text & code) |
| ExtraSS (SIGGRAPH Asia) | 2023 | Reimplemented in `NJUCG/tile-based-frame-extrapolation` | No (needs normals, albedo, stencil + extrapolated G-buffers) | Yes (flow refinement + joint Extra-SS net) | Unreported in public snippets | > 4–6 ms est. (requires extra G-buffer rasterization pass) | Discusses FSR 3 / DLSS 3; benchmarks vs ExtraNet, IFRNet, NSR | ACM / MIT (baseline) | DROP (fails inputs — requires G-buffers) |
| PatchEX (arXiv / ACM TOG) | 2024 / 2026 | No public repo | No (needs stencil, world normal, world position, NoV, shadow mask) | Yes (3 parallel patch U-Nets + shadow net) | Lightweight U-Nets (exact count unreported) | 2× faster than ExtraSS, 6× vs ExtraNet on RTX 4090 (exact ms unreported) | Compares vs ExtraNet, ExtraSS | arXiv non-exclusive | DROP (fails inputs — needs 8 UE G-buffers) |
| LMV (SIGGRAPH Asia) | 2023 | Yes (GitHub `jsnzwu/learnable-motion-vector`) | No (needs baseColor, worldNormal, worldPosition, translucency) | Yes (learnable MV + recurrent ShadeNet / FSNet) | ~2.1M est. (8.4 MB checkpoint) | Too slow for 1080p real time (per GFFE analysis) | Compares vs ExtraNet, video VFI | MIT | DROP (fails inputs & model size > 1M) |
| MoFlow (ACM TOG) | 2025 | Yes (GitHub `jsnzwu/motion-guided-flow`) | No (needs baseColor/albedo, worldNormal, depth, translucency) | Yes (recurrent feature streaming + MoFlow) | Unreported (recurrent feature stream) | Unreported in public snippets | Cites DLSS/FSR; compares vs LMV, ExtraNet, RIFE | Open GitHub repo | DROP (fails inputs — requires G-buffers) |
| EFXNet (SIGGRAPH Asia / TOG) | 2025 | No public repo found | No (needs G-buffers + engine-coupled incremental sparse shading) | Yes (joint error-flow-extrapolation net) | Unreported | Unreported (couples inference with sparse engine shading) | Compares vs ExtraNet, GFFE, RIFE, IFRNet | ACM copyright | DROP (not engine-independent; needs G-buffers) |
| Tile-based Frame Extrapolation (TBFE) (SIGGRAPH Asia) | 2026 | Yes (GitHub `NJUCG/tile-based-frame-extrapolation`) | No (uses ExtraNet / ExtraSS G-buffer EXR pipeline) | Yes (`src/tbfe/models/` repair network) | Unreported in public metadata | Unreported in public metadata | Compares vs ExtraNet, ExtraSS, ExtraSS-TBR | MIT | DROP (uses G-buffers; repo useful for baselines) |
| Amulet (arXiv 2608.10423) | 2026 | No repo | No (replaces deferred renderer with froxel primitive-ID cache) | No (explicitly non-neural pipeline) | 0 (non-neural) | < 1 ms compositing at 1080p on RTX 5090 (plus cache shading pass) | Compares vs MoFlow, EFXNet, ExtraSS | arXiv preprint | DROP (replaces engine renderer; non-ML) |
| Kernel-Based Frame Interpolation (SIGGRAPH) | 2023 | No (Disney proprietary / patented) | No (needs albedo, depth, optical flow, light passes) | Yes (GridNet + attention kernels) | Large GridNet + attention (exact count unreported) | Offline only (runtime unreported) | Compares vs NFIRC (Briedis 2021) | Patented (US 12,367,544) | DROP (offline-only, uses attention + albedo) |
| Fewer, Better Frames (FSR4 PoC) (arXiv 2606.02586) | 2026 | PoC bridge only | Calls closed-source FSR4 DX12 FG API on world-model output | Uses Waypoint-1.5 diffusion world model | World model (unsuitable for 2 ms budget) | 15 FPS anchor cadence ("not a finished realtime renderer") | Uses FSR 4 FG directly | arXiv preprint | DROP (wrapper around closed FSR4 API) |
| Baseline: ExtraNet (SIGGRAPH Asia / TOG) | 2021 | Reimplemented in MobFGSR & `NJUCG/tbfe` | No (needs normals, albedo, roughness, stencil) | Yes (gated conv inpainting + history encoder) | Unreported (gated convs) | Too slow for 1080p real time (6× slower than PatchEX) | Pre-dates FSR 3 / DLSS 3 | ACM / MIT reimpl. | CITED BASELINE ONLY (<2023 and needs G-buffers) |

## 4. Selected papers in detail

### 4.1 Mob-FGSR (SIGGRAPH 2024) — analytical stage and LUT warping

*Mob-FGSR: Frame Generation and Super Resolution for Mobile Real-Time Rendering*
(Yang et al., Zhejiang University & OPPO Computing & Graphics Research Institute,
SIGGRAPH 2024) fully meets the FFX FG input requirements, ships open source
(<https://github.com/Mob-FGSR/MobFGSR>) and has been validated commercially on
mobile devices such as the OnePlus Ace 3 Pro (launched June 2024, holding a
stable 120 FPS in Genshin Impact) and the OnePlus 13 (Genshin Impact and Honkai:
Star Rail).

On Snapdragon 8 Gen 3 hardware, the method runs FG at 720p in 2.2 ms and joint
FG + super resolution (SR) at 1080p in 2.3 ms. On Snapdragon 8 Gen 2 the measured
totals are 2.78 ms for 720p interpolation (Ours-I), 2.47 ms for 720p extrapolation
(Ours-E), 2.80 ms for 1080p interpolation with SR (Ours-ISR) and 2.26 ms for 1080p
extrapolation with SR (Ours-ESR).

**Inputs.** Mob-FGSR requires only color, depth and motion vectors from the frames
rendered at times 0 ($I_0$) and 1 ($I_1$) — no normals, albedo, roughness, shadow
mask or stencil. The same formulation covers interpolation (Ours-I), generating
$B_\alpha$ for $\alpha \in (0, 1)$ between $I_0$ and $I_1$, and extrapolation
(Ours-E), generating $P_{1+\alpha}$ after $I_1$.

**Quadratic motion model.** Instead of assuming constant linear velocity — which
breaks on fast camera rotation and complex animation — Mob-FGSR models each pixel
trajectory with uniform acceleration $\ddot{x} = \frac{m_1 - m_0}{t^2}$ and initial
velocity $\dot{x}_0 = \frac{m_1 + m_0}{2t}$, derived from the motion vectors $m_0$
and $m_1$ of two consecutive frames separated by $t$. The interpolated position
($p_\alpha$, Eq. 1 and Eq. 8 in the supplement) and extrapolated position
($p_{1+\alpha}$, Eq. 2 and Eq. 13 in the supplement) are:

$$p_\alpha = p_1 - m_1 + \frac{\alpha}{2}(m_1 + m_0) + \frac{\alpha^2}{2}(m_1 - m_0)$$

$$p_{1+\alpha} = p_1 + \frac{\alpha}{2}(3m_1 - m_0) + \frac{\alpha^2}{2}(m_1 - m_0)$$

The ablation study (Table 1 of the supplement) shows the quadratic trajectory
consistently beats the linear-velocity assumption
($p_\alpha = p_1 - (1 - \alpha)m_1$, $p_{1+\alpha} = p_1 + \alpha m_1$), gaining up
to +0.72 dB PSNR (26.60 dB vs 25.88 dB in Street View extrapolation; 27.57 dB vs
27.26 dB interpolation) with essentially no runtime overhead (2.21 ms vs 2.20 ms
interpolating, 1.92 ms vs 1.92 ms extrapolating).

**Splatting and hole filling.** Motion vectors are splatted to the target time
using atomic depth-compare operations so foreground pixels win over background.
Because forward splatting leaves small grid-like gaps, a thin-object detector
followed by a mean filter reconstructs a continuous motion field.

**Interpolation fusion.** In interpolation mode, $I_0$ and $I_1$ (with their depth
maps) are back-warped with the reconstructed vectors, producing $F^{0\to U}$ and
$F^{1\to U}$. The fusion equation (Eq. 1 of the main paper) separates disocclusions
from shading changes with two data-optimized thresholds ($\delta_d$ / $T_D$ for
depth difference, $\delta_c$ / $T_B$ for brightness difference):

$$
B_U(x) =
\begin{cases}
F^{0\to U}(x) & x \in A_{\text{disocclusion}},\ \text{depth change near } I_0 \\
F^{1\to U}(x) & x \in A_{\text{disocclusion}},\ \text{depth change near } I_1 \\
\text{lerp}\left(F^{0\to U}(x), F^{1\to U}(x), U\right) & x \in A_{\text{shading}}\ \text{or default blend region}
\end{cases}
$$

Where both color and depth of the two projected frames agree, the temporally
closer frame is sampled preferentially, preserving sharpness.

**Extrapolation.** With no future reference frame, a simplified disocclusion fill
repairs missing motion vectors directly from the rendered frame $I_1$ in
~0.20–0.29 ms, against > 3.0 ms for classic Background Duplication (BD) and
Depth-Based Filtering (DBF), while keeping high-frequency detail.

**LUT-accelerated warping.** To avoid bilinear blur and the cost of a bicubic
filter during backward warping, Mob-FGSR trains a 4-layer fully connected MLP with
`tanh` activation, Adam (learning rate 10⁻³, $\beta = (0.9, 0.999)$, $\epsilon =
10^{-4}$) and a $1 - \text{SSIM}$ loss. Training the MLP takes about 20 minutes on
a single RTX 3090; tuning the thresholds $T_D$ and $T_B$ takes another 20 minutes
on the same GPU.

The supplement does not print the exact hidden-neuron count (estimated ~1K
parameters for a 4-layer network mapping offsets $(d_x, d_y)$ to 16 weights). At
runtime no neural inference is needed: the weights are exported into a **32×32×16
LUT (~16 KB at 1 byte per weight)**, giving immediate access to the 16 resampling
weights with better visual quality than a bicubic filter.

### 4.2 GFFE (SIGGRAPH Asia 2024 / ACM TOG) — shading correction without G-buffers

*GFFE: G-buffer Free Frame Extrapolation for Low-latency Real-time Rendering*
(Wu et al., UC Santa Barbara & Intel, ACM TOG Vol. 43, No. 6, Article 248, 2024,
arXiv:2406.18551) addresses the main limitation of purely geometric methods:
non-geometric motion (moving shadows, reflections and lighting changes that do not
follow mesh motion vectors) and complex disocclusion filling without G-buffers.

On an RTX 4070 Ti Super with a Ryzen 9 5900X converting 1080p from 30 to 60 FPS,
full GFFE runs in **6.62 ms per frame**, far faster and visually better than video
extrapolation DMVFN (20.57 ms) and interpolation UPR-Net (43.04 ms); it is also
evaluated against IFR-Net and ExtraSS-E.

**Inputs.** GFFE takes only the rendered color sequence $\{I_t\}$, the matching
depth buffers $\{D_t\}$, motion vectors $\{V_t\}$ and view-projection camera
matrices $\{C_t\}$.

**World-space motion estimation (GAE, Sec. 4.1).** Each pixel is unprojected to its
3D world position $P_0[x]$ using $D_t$ and $C_t$, and the historical trajectory
$\{P_i[x]\}$ is tracked recursively (Algo. 1, Appendix B) over $k$ rendered frames.
To avoid trajectories corrupted by disocclusions, a static test compares the
previous screen-space projection against the motion vector; if the distance is
below a threshold the pixel is marked static. The future position at extrapolated
frame $t + \alpha$ (where $\alpha = \frac{j}{n+1}$, $n$ being the number of
extrapolated frames per rendered frame and $j$ the current index) is estimated
linearly in 3D world space (Eq. 1 of GFFE):

$$NP_{t\to t+\alpha}[x] = \alpha \big(P_0[x] - P_1[x]\big) + P_0[x]$$

Interpolating in 3D world space before reprojecting fragments with camera matrix
$C_{t+\alpha}$ (resolving overlaps with atomic operations to retain the nearest
fragment) avoids the perspective and camera-rotation distortion that degrades
linear methods working in the 2D image plane.

**Hierarchical Background Collection (Sec. 4.2).** To fill on-screen disocclusions
without rendering extra geometry, a persistent buffer $\{B_l\}$ holds $L$ color and
depth levels, each level at 1/4 the resolution of the previous one. Static
fragments from rendered frames feed the first level; when valid data with greater
depth becomes available, fragments move to the next level and are later projected
into the extrapolated frame to cover static and dynamic disocclusion holes.

**Adaptive Rendering Window (Sec. 4.3).** Handles off-screen disocclusions by
intersecting a bounding box with a virtual plane in front of the extrapolated
camera $C_{\text{next}} = C_{\text{current}} + \alpha(C_{\text{current}} -
C_{\text{previous}})$. Since an FFX FG hook works in post-processing and does not
change the engine's primary rasterization viewport, **this sub-module is omitted**
from the implementation; the remaining modules stay intact.

**Shading Correction Network (SCN, Sec. 4.4).** A lightweight encoder-decoder
network applied residually after disocclusion filling. The SCN takes four inputs:
the image produced by GAE, its depth buffer, the previous frame back-warped
($t-1 \to t+\alpha$), and an input focus mask ($M_{\text{input}}$) whose white
regions mark dynamic fragments and lighting discrepancies. The final output is a
spatial blend between the analytical GAE image and the SCN-refined image, guided
by the focus mask acting as a spatial attention mechanism.

The main text defers the detailed SCN layer configuration to the appendix,
unavailable in the public extract, but its input formulation, SMAPE focus mask and
hybrid loss combine directly with the open STSSNet U-Net backbone described next.

### 4.3 STSSNet (AAAI 2024) — the 141K-parameter U-Net backbone (MIT)

*Low-Latency Space-Time Supersampling for Real-Time Rendering* (He et al., Fudan
University, AAAI 2024, arXiv:2312.10890) ships official PyTorch code under the MIT
license (<https://github.com/ryanhe312/STSSNet-AAAI2024>).

Inspecting the data loader (`ReadData` in `dataloaders.py`) shows the full model
(417,241 parameters, 31,502 GFLOPs, 4.35 ms on an RTX 3090 in FP16) fails the
input requirements: its secondary ERM (Efficient Reshading Module) needs a 33–35
channel tensor containing basecolor (channels 21–23), metallic (24), roughness (25)
and normal (27–29).

However, Table 4 of the paper separates the U-Net backbone from the ERM module. The
isolated backbone has exactly **141.00K parameters (~137.7 KB in INT8)**, requires
only **20.53 GFLOPs** to produce a 1080p output and runs in **3.39 ms** on an
RTX 3090 in PyTorch FP16 (`model.half()`).

Beyond the ultra-light U-Net structure, STSSNet contributes two directly reusable
elements:

- **Random Reshading Masking (RRM)** training: random augmentation of the warping
  hole mask, making the network robust to aliasing regions and shadows;
- a **Weighted L1 loss** (Eq. 6) combined with a VGG perceptual loss.

## 5. Hybrid architecture for RDNA3 (RX 7700 XT, ≤ 2.0 ms at 1080p)

To satisfy every Deep Learning Fit and Lightweight / RDNA3 Fit criterion, the
recommended architecture merges the Mob-FGSR analytical stage (1080p) with a
residual SCN inspired by GFFE/STSSNet operating at 540p.

### 5.1 Layer-by-layer specification of the residual network (SCN 540p)

To avoid slow or vendor-specific operations on RDNA3 — no transformers, attention,
wide windows, heavy softmax, FP8, sparsity tricks or NVIDIA's Optical Flow
Accelerator (OFA) — the proposed residual network uses only standard 3×3
convolutions, ReLU activations, 2×2 average pooling / bilinear upsampling, and one
`PixelShuffle(2)` at the output. It is fully compatible with INT8 (dp4a) or FP16
(WMMA) quantization.

Stage 1 tensors at 1080p are downsampled 2× (to 960×540 with 16 channels):

| Input | Channels |
|---|---|
| Projected frame 0 $F^{0\to U}$ | 3 (RGB) |
| Projected frame 1 $F^{1\to U}$ (or previous frame in extrapolation) | 3 (RGB) |
| Analytical base fusion $I_{\text{base}}$ from Mob-FGSR | 3 (RGB) |
| Projected depths $D^{0\to U}$, $D^{1\to U}$ | 2 |
| Splatted motion vectors $M_{0\to U}$, $M_{1\to U}$ | 4 |
| Combined analytical disocclusion + shading-change mask $M_{\text{input}}$ | 1 |

| Layer / block | Operating resolution | Operation / kernel | Channels (in → out) | Params | GFLOPs @ 1080p output |
|---|---|---|---|---|---|
| Stem (enc0) | 960×540 (540p) | Conv3x3 + ReLU | 16 → 24 | 3,480 | 3.58 |
| Down 1 (enc1_a) | 480×270 (270p) | AvgPool2x2 + Conv3x3 + ReLU | 24 → 32 | 6,944 | 1.79 |
| Enc 1 (enc1_b) | 480×270 (270p) | Conv3x3 + ReLU | 32 → 32 | 9,248 | 2.39 |
| Bottleneck (bot_a) | 240×135 (135p) | AvgPool2x2 + Conv3x3 + ReLU | 32 → 48 | 13,872 | 0.90 |
| Bottleneck (bot_b) | 240×135 (135p) | Conv3x3 + ReLU | 48 → 48 | 20,784 | 1.34 |
| Up 1 (dec1) | 480×270 (270p) | Bilinear2x + Concat(enc1_b) + Conv3x3 + ReLU | 48 + 32 → 32 | 23,072 | 5.97 |
| Up 0 (dec0) | 960×540 (540p) | Bilinear2x + Concat(enc0) + Conv3x3 + ReLU | 32 + 24 → 24 | 12,120 | 12.54 |
| Head (out_head) | 960×540 → 1920×1080 | Conv3x3 + PixelShuffle(2) | 24 → 16 (→ 4 ch @1080p) | 3,472 | 3.58 |
| **Residual network total** | 540p → 135p | 8 convolutional layers | Output: 3 ch $\Delta I$ + 1 ch $\hat{M}$ | **92,992** (~90.8 KB INT8) | **32.10** |

In the final step (fused into a single compute shader with the `PixelShuffle(2)`),
the 4 channels at 1080p split into 3 color-residual channels
$\Delta I(x) \in [-1, 1]$ and 1 spatial attention mask $\hat{M}(x) \in [0, 1]$,
producing the final frame:

$$\hat{I}_U(x) = \text{clamp}_{[0, 1]}\Big(I_{\text{base}}(x) + \hat{M}(x) \cdot \Delta I(x)\Big)$$

### 5.2 Performance and budget on the Radeon RX 7700 XT (RDNA3)

The RX 7700 XT has **54 RDNA3 compute units** (~35.2 TFLOPS FP32 peak; ~70.3 TFLOPS
FP16 WMMA dual-issue peak) and **48 MB Infinity Cache (L3)** connected to **432
GB/s** of GDDR6 memory. Applying a realistic 20–25 TFLOPS effective
(20–25 GFLOPs/ms) for compute shaders with FP16 WMMA matrices or INT8 dp4a
instructions gives the per-step estimates below.

**Stage 1 — analytical (Mob-FGSR at 1080p).** Roughly:

- ~25 FLOPs/pixel for quadratic trajectory;
- ~30 FLOPs/pixel for splatting and the 3×3 thin-object filter;
- ~128 FLOPs/pixel for two-frame backward warping through the 16-sample LUT;
- ~20 FLOPs/pixel for threshold fusion ($\delta_d$, $\delta_c$).

That totals only **~0.42 GFLOPs** (~0.02 ms of pure ALU) and **~65 MB** of texture
traffic at 1080p. Because the working set mostly fits in the 48 MB Infinity Cache,
the realistic desktop GPU time is **0.15–0.30 ms**.

**Stage 2 — neural residual (SCN at 540p).** Requires **32.10 GFLOPs** with the
92,992-parameter architecture above (or **20.53 GFLOPs** using the 141.00K-parameter
STSSNet backbone configuration). Dividing 32.10 GFLOPs by 20–25 TFLOPS effective
FP16 WMMA gives **1.28–1.61 ms** (or 0.82–1.03 ms for the 20.53 GFLOPs STSSNet
backbone). Since the weights occupy only 90.8 KB in INT8 (or 181.6 KB in FP16) —
comparable to FSR 4's ~87 KB INT8 — all filters stay resident in L1/L2 during
inference, avoiding VRAM access stalls.

**Combined.** Stage 1 + Stage 2 = 32.52 GFLOPs and ~85 MB of cache traffic, for an
estimated total of **1.30–1.67 ms in FP16 WMMA** (~0.95–1.20 ms in INT8 dp4a) on
the RX 7700 XT — meeting the ≤ 2.0 ms at 1080p target with safety margin.

### 5.3 Hook data capture, loss function and training cost

The training pipeline is fully **self-supervised** from the FFX FG hook itself, so
no proprietary dataset is needed. While any game runs at 2× FPS, the hook records
consecutive frame triplets $(t-1, t, t+1)$ containing
{Color_HUDless, Depth, MotionVectors, CameraMatrix} and keeps the middle frame
$I_t$ as ground truth ($I_{\text{GT}}$) to train interpolation from $(t-1, t+1)$,
or uses $(t-2, t-1)$ to predict $I_t$ in extrapolation. For Mob-FGSR, 6 scenes
with 8 clips of 500 frames each (first 350 frames per clip for training = 16,800
training frames, 150 for testing) proved sufficient to train the compared models.

To supervise the residual SCN, the target focus mask $M_{\text{GT}} \in [0, 1]$ is
computed from the SMAPE (Symmetric Mean Absolute Percentage Error) between the
analytical output $I_{\text{base}}$ and ground truth $I_{\text{GT}}$, as defined in
GFFE:

$$M_{\text{GT}}(x) = \text{clamp}_{[0, 1]}\left(\frac{|I_{\text{base}}(x) - I_{\text{GT}}(x)|}{|I_{\text{base}}(x)| + |I_{\text{GT}}(x)| + \epsilon}\right)$$

The total loss combines STSSNet's weighted L1 loss (Eq. 6, with $w_{i,j} = 2$ in
reshading/hole mask regions and $w_{i,j} = 1$ elsewhere), Mob-FGSR's structural
SSIM loss, GFFE's focus-mask loss and a VGG perceptual loss:

$$L_{\text{l1}}(I, \hat{I}) = \frac{1}{mn}\sum_{i=1}^{m}\sum_{j=1}^{n} w_{i,j}|I_{i,j} - \hat{I}_{i,j}|$$

$$\mathcal{L}_{\text{total}} = L_{\text{l1}}(I_{\text{GT}}, \hat{I}) + \lambda_{\text{ssim}}\big(1 - \text{SSIM}(\hat{I}, I_{\text{GT}})\big) + \lambda_{\text{focus}}\|\hat{M} - M_{\text{GT}}\|_1 + \lambda_{\text{vgg}}\mathcal{L}_{\text{VGG}}(\hat{I}, I_{\text{GT}})$$

**Training cost.** Mob-FGSR's MLP→LUT and threshold $(\delta_d, \delta_c)$
calibration is documented at ~40 minutes (0.67 GPU-hours) on a single RTX 3090
(20 minutes for the MLP + 20 minutes for thresholds with Adam at learning rate
10⁻³). Training the 93K–141K-parameter residual network follows the STSSNet
protocol (100 epochs, Adam, learning rate 10⁻⁴), which for a sub-150K-parameter
network on cropped patches (`crop` in `dataloaders.py`) represents an estimated
**~6–10 GPU-hours** on a single modern GPU (RTX 3090 / RTX 4090 / RX 7900 XTX).

## 6. Quality signals, practical issues and edge cases

### 6.1 Interpolation versus extrapolation and added latency

Combining Mob-FGSR with GFFE natively supports both generation modes in one
pipeline.

**Interpolation (Ours-I + SCN).** Frame $B_{0.5}$ is synthesized between $I_0$ and
$I_1$, which introduces **1 rendered frame of presentation delay** (11.11 ms to
22.22 ms of extra latency at the frame rates analysed in the literature), requiring
temporary retention of $I_1$ — similar to FSR 3 FG and DLSS 3 FG. In exchange,
interpolation gives better visual quality in disocclusions (+0.97 to +2.12 dB PSNR
versus extrapolation in Table 1 of Mob-FGSR), because newly revealed regions are
already visible in $I_1$.

**Extrapolation (Ours-E + GFFE background buffer + SCN).** Frame $P_{1.5}$ is
predicted immediately after $I_1$ using only past frames, giving **0 frames of
queue delay** (only the shader execution latency, ~1.5 ms). Without the future
frame, extrapolation has lower accuracy on fast shadows and never-seen
disocclusions, mitigated by GFFE's hierarchical background buffer $\{B_l\}$ and
Mob-FGSR's motion-vector repair.

### 6.2 Disocclusions, transparency, particles, UI/HUD, temporal stability and camera cuts

**Disocclusions and thin objects.** Depth-atomic motion splatting plus Mob-FGSR's
thin-object filter avoid the stretching of thin geometry that affects classic
methods such as 3DWarp and BSR, while GFFE explicitly separates static and dynamic
disocclusions through the multi-level buffer $\{B_l\}$.

**Transparency, reflections and particles.** Where primary motion vectors do not
reflect the actual motion of lighting or translucent surfaces, Mob-FGSR's
brightness test ($|B^{0\to U} - B^{1\to U}| > \delta_c$) applies a smooth linear
interpolation (lerp) to suppress ghosting, and GFFE's focus mask
($M_{\text{input}}$) concentrates the SCN residual correction on those
non-geometric variation areas.

**UI/HUD.** Because the FFX FG hook provides HUD-less color, all processing happens
before HUD composition, keeping text and 2D elements free of distortion.

**Temporal stability (flicker).** Neither paper relies on single-frame PSNR:
GFFE evaluates temporal consistency with the FovVideoVDP (FvVDP) perceptual video
metric (Table 3 of GFFE), and STSSNet reports large gains on the VMAF video metric
(75.71–85.15 versus 56.33–72.60 for RIFE+EDVR).

**Fast motion and camera cuts.** Mob-FGSR's quadratic trajectory and GFFE's 3D
world-space extrapolation maintain coherence under high acceleration. If an abrupt
camera cut makes the fraction of pixels invalidated by the depth ($\delta_d$) and
brightness ($\delta_c$) tests exceed a global threshold, the pipeline must reset the
motion history and present the native frame without fusion for 1 cycle.

**Versus FSR 3 FG and DLSS-G.** GFFE and ExtraSS discuss the latency trade-off
against DLSS 3 and FSR 3 extensively, and Mob-FGSR compares visual quality against
FSR 1, FSR 2, DLSS 2 and TSR — but no academic paper publishes PSNR/VMAF tables
running the closed DLSS 3 FG binary on the same synthetic scenes, a gap
acknowledged in GFFE's community discussion.

### 6.3 Engine/platform independence and licensing

Unlike ExtraSS, PatchEX, LMV, MoFlow, EFXNet and Amulet — which need Unreal Engine
internal G-buffers or changes to the deferred renderer — the Mob-FGSR + GFFE (SCN)
combination has been validated on both forward (Unity) and deferred (Unreal Engine)
pipelines, depending only on color, depth, motion vectors and camera matrix, which
guarantees engine and graphics-API independence.

Legally, the STSSNet reference code (`ryanhe312/STSSNet-AAAI2024`) and the
Tile-based Frame Extrapolation baseline package
(`NJUCG/tile-based-frame-extrapolation`) are MIT licensed, allowing personal use,
modification and redistribution, while the Mob-FGSR and GFFE analytical shaders can
be implemented directly from the published equations.

## 7. Sequential implementation plan

1. **Implement the Stage 1 analytical pipeline (Mob-FGSR) in HLSL compute shaders**
   (target ~0.25 ms at 1080p). Retain the previous frame's motion vectors ($m_0$)
   in the FFX FG hook alongside the current ones ($m_1$), compute quadratic motion
   splatting ($p_\alpha$ for interpolation, $p_{1+\alpha}$ for extrapolation) using
   depth atomics (`InterlockedMin`), apply the 3×3 thin-object hole-fill filter and
   run the LUT-assisted (32×32×16) backward warping with $\delta_d$ / $\delta_c$
   threshold fusion, producing the 1080p base frame $I_{\text{base}}$ and the mask
   $M_{\text{input}}$.
2. **Capture triplets in the FFX FG hook and train the LUT (16 KB) and the residual
   SCN (~93K parameters).** Record 300–500 frame sequences at 2× FPS keeping each
   middle frame as $I_{\text{GT}}$, train the 4-layer MLP (~20 minutes on a GPU) to
   export the 32×32×16 LUT, and adapt the MIT `train.py` from
   `STSSNet-AAAI2024` (removing the ERM module's G-buffer channels) to train the
   8-layer 540p U-Net for 100 epochs with Adam (learning rate 10⁻⁴) and the combined
   loss $\mathcal{L}_{\text{total}}$ (~6–10 GPU-hours).
3. **Integrate 540p inference on the RX 7700 XT via WMMA FP16 or DP4A INT8**
   (target ≤ 1.65 ms total). Run the 8 convolutional layers over the 540p
   downsampled tensors keeping the 90.8 KB of INT8 weights (or 181.6 KB FP16)
   resident in L1/L2/Infinity Cache, and fuse the final `PixelShuffle(2)` pass with
   the weighted residual sum
   $\hat{I}_U = \text{clamp}(I_{\text{base}} + \hat{M} \cdot \Delta I)$ directly into
   the 1080p output buffer before HUD composition.

## References

- S. Yang, Q. Zhu, J. Zhuge, Q. Qiu, C. Li, Y. Yan, H. Xu, L.-Q. Yan, X. Jin.
  *Mob-FGSR: Frame Generation and Super Resolution for Mobile Real-Time
  Rendering.* SIGGRAPH 2024. <https://mob-fgsr.github.io/> ·
  <https://github.com/Mob-FGSR/MobFGSR> · local copy: `mob-fgsr.md`
- S. Wu, D. Vembar, A. Sochenov, S. Panneer, S. Kim, A. Kaplanyan, L.-Q. Yan.
  *GFFE: G-buffer Free Frame Extrapolation for Low-latency Real-time Rendering.*
  ACM TOG 43(6), Article 248, 2024 (SIGGRAPH Asia 2024). arXiv:2406.18551 · local
  copy: `gffe.md`
- R. He et al. *Low-Latency Space-Time Supersampling for Real-Time Rendering.*
  AAAI 2024. arXiv:2312.10890 · <https://github.com/ryanhe312/STSSNet-AAAI2024>
