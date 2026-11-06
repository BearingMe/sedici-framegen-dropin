# Mob-FGSR attribution

Sedici's analytical interpolation shaders adapt the Mob-FGSR authors' GLSL
implementation to HLSL/DX12, with input normalization and explicit edge bounds.
The resampling LUT is an unchanged FLOAT-weight export of their EXR asset.

- Work: *Mob-FGSR: Frame Generation and Super Resolution for Mobile Real-Time
  Rendering* (SIGGRAPH 2024).
- Authors: Sipeng Yang, Qingchuan Zhu, Junhao Zhuge, Qiang Qiu, Chen Li,
  Yuzhong Yan, Huihui Xu, Ling-Qi Yan and Xiaogang Jin.
- Source: <https://github.com/Mob-FGSR/MobFGSR>.
- Commit: `df7ed207ed66d53dc44518ce12572cd49015c125`.
- Adapted source: `resources/FrameGeneration/`, `resources/Preprocessing/Dilate.comp`,
  `resources/Sample.glsl`, `resources/Packing.glsl`.
- Exported asset: `resources/lut.exr`; SHA256 is recorded in
  `src/fg_lut_data.h`. Regenerate with `tools/export_mobfgsr_lut.py`.
- License: **Creative Commons Attribution-NonCommercial 4.0 International**,
  <https://creativecommons.org/licenses/by-nc/4.0/legalcode>.
  The upstream license text is in `references/sources/mobfgsr/LICENSE`.

This material is not MIT licensed. Distribution of builds containing these
adaptations and weights must retain attribution, indicate the adaptations, link
the license, and comply with its noncommercial restriction. This notice does not
claim any endorsement by the authors or relicense their work.
