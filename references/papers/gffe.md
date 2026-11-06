# GFFE: G-buffer Free Frame Extrapolation for Low-latency Real-time Rendering

| Field | Value |
| --- | --- |
| Authors | Songyin Wu, Deepak Vembar, Anton Sochenov, Selvakumar Panneer, Sungye Kim, Anton Kaplanyan, Ling-Qi Yan |
| Affiliations | University of California, Santa Barbara; Intel Corporation |
| Venue | ACM Transactions on Graphics 43(6), Article 248, 2024 — SIGGRAPH Asia 2024 |
| Identifiers | arXiv:2406.18551 |
| Code | None public; reproducible from the equations and Algo. 1 (Appendix B) |
| Project page | — |
| Source PDF | <https://arxiv.org/pdf/2406.18551> |
| Conversion | Text extracted from the PDF. Figures omitted; figure captions kept, text printed inside artwork kept as a blockquote where legible. |

**Figure 1.** We propose a _G-buffer free_ frame extrapolation framework, GFFE, which introduces no additional latency (unlike interpolation methods) and eliminates the need for additional G-buffers generation of extrapolated frames. Our framework shows better visual quality than previous frame extrapolation method DMVFN [Hu et al. 2023] and interpolation method UPR [Jin et al. 2023] with better performance.

## Abstract

Real-time rendering has been embracing ever-demanding effects, such as ray tracing. However, rendering such effects in high resolution and high frame rate remains challenging. Frame extrapolation methods, which don't introduce additional latency as opposed to frame interpolation methods such as DLSS 3 and FSR 3, boost the frame rate by generating future frames based on previous frames. However, it is a more challenging task because of the lack of information in the disocclusion regions, and recent methods also have a high engine integration cost due to requiring G-buffers as input. We propose a _G-buffer free_ frame extrapolation, GFFE, with a novel heuristic framework and an efficient neural network, to plausibly generate new frames in real-time without introducing additional latency. We analyze the motion of dynamic fragments and different types of disocclusions, and design the corresponding modules of the extrapolation block to handle them. After filling disocclusions, a light-weight shading correction network is used to correct shading and improve overall quality. GFFE achieves comparable or better results compared to previous interpolation as well as G-bufferdependent extrapolation methods, with more efficient performance and easier game integration.

## 1. Introduction

Real-time rendering has advanced significantly in recent years to create more realistic and interactive environments, including the recent trend for real-time path tracing effects in games. Usually, high quality and high frame rates are required for games or virtual reality applications in order to provide a good user experience. However, the cost of rendering such high quality frames is expensive even for the most powerful graphics hardware - naively rendering all frames is not always possible under fixed compute and power budgets. Therefore, in addition to methods that accelerate frame rendering, approaches such as frame super resolution and generation [AMD 2021; Guo et al. 2021, 2022; Intel 2022; Wu et al. 2023a,b; Xiao et al. 2020] are usually implemented in a separate post-processing pass to provide the best quality output within given compute budgets.

Frame generation is one technique that can be used to increase the frame rate for smoother and jitter-free experience. Frame interpolation, including proprietary products DLSS 3 [NVIDIA 2022] and FSR 3 [AMD 2022] and research works [Briedis et al. 2021, 2023; Jin et al. 2023; Kong et al. 2022] try to generate new frames between two rendered frames. These methods increase the key-press-to-display latency of the rendering process since the generated frames rely on availability of both the previous and the next frame.

Frame extrapolation, on the other hand, generates new frames based solely on previous frames, and does not introduce additional

**Figure 2.** An extrapolated frame by directly projecting fragments from previous rendered frame. The right column shows three types of disocclusions: _outof-screen disocclusion_ , _static disocclusion_ and _dynamic disocclusion_ from top to bottom. The thin black lines splatted in the image are due to forward warping.

latency to the rendering process. However, it is a more difficult task and usually generates inferior results due to the missing information from the future frames. Many existing methods, including ExtraNet [Guo et al. 2021], LMV [Wu et al. 2023b] and ExtraSS [Wu et al. 2023a], use G-buffers of generated frames to guide the generation of corresponding final frames. Game-generated G-buffers are not always easily available and the cost of obtaining them from various rendering pipelines is not negligible. Other video extrapolation methods [Hu et al. 2023] do not require G-buffers to generate color frames, however they usually have inferior quality and performance under real-time rendering settings.

Existing methods have shown abilities to generate new frames, but they either introduce latency or require additional G-buffers. Motivated by these problems, we propose a novel method that can generate new frames without introducing latency or requiring G-buffers. Our insight is the missing information of extrapolated frames can be approximately retrieved from previous frames, which are usually discarded in the rendering pipeline. Additionally, the motion of fragments can be plausibly estimated from history frames, so there is no need to render G-buffers for extrapolated frames.

Based on these observations, we propose a G-buffer-free extrapolation framework. First, it uses a heuristic motion estimation method to eliminate the requirement of rendering motion vectors for extrapolated frames. Then, to handle disocclusions in the extrapolated frames, we introduce a background collection module and adaptive rendering window. Lastly, we use a light-weight neural network to further improve the shading and shadow consistency.

We evaluate our framework on various scenes in Unreal Engine [Epic Games 2022] with different types of effects including glossy and translucent materials, complex geometry, and dynamic objects to demonstrate our quality, performance, and robustness. Our method generates smooth and plausible results from 30 FPS to 60FPS. It shows superior quality to G-buffer free extrapolation baseline, and comparable results with G-buffer-dependent baseline and interpolation baselines, with better visual quality and performance.

## 2. Related work

### 2.1. Warping and hole filling

Warping has been used in real time rendering for many years to improve the quality and performance. Mark et al. [1997] proposed a 3D warping method to warp the frame to new frames as a post processing step. But it is difficult for disocclusion areas since such informaiton is not available, where a hole filling algorithm is needed. Didyk et al. [2010] employed additional blur operations to the warped frames to reduce the artifacts for disocclusion areas. Similar to Didyk et al. [2010], Schollmeyer et al. [2017] proposed a hole filling method to fill the disocclusion areas by low pass filtering of them to reduce the artifacts. These methods bring blurring artifacts to the extrapolated frames instead of generating actual details, which is not suitable for modern real-time rendering. Later, Reinert et al. [2016] builds geometry proxies to fill the disocclusion areas but requires pre-computed geometry information and still in low quality since it uses low poly geometries. [Wu et al. 2023a; Zeng et al. 2021] use G-buffers to guide the hole filling process by reusing spatial neighbors' information. However, the G-buffers are not always available in real-time rendering, which limits the usage of these methods.

Besides single frame warping methods which cannot retrieve valid information in disocclusions, there are also some bidirectional methods trying to warp frames from both previous and future frames. Andreev [2010] uses half motion vectors to warp both previous frame and future frame to the current frame to increase the frame rate. Yang et al.[2011] uses an iterative way to find the correspondence from previous and future frames to the current frame. These methods, although fill the disocclusion areas better, introduce additional key-press-to-display latency since new frames reply on future frames and usually the quality is not good enough including lagging shadow and shadings.

### 2.2. Frame Interpolation

Besides pure warping based methods, there are several frame interpolation methods with neural networks achieving better quality. [Briedis et al. 2021, 2023] propose using optical flows or kernel prediction neural network to generate intermediate frames by only given corresponding G-buffers. Although the quality looks promising, these techniques are used for offline rendering, which is difficult to be applied in real time rendering due to low performance. Video interpolation methods [Bao et al. 2019; Huang et al. 2022; Jin et al. 2023; Kong et al. 2022] also generate plausible intermediate frames with neural networks but usually with blurrier results and worse performance since these methods are not designed for rendering pipeline. Commercial solutions including DLSS 3 [NVIDIA 2022] and FSR 3 [AMD 2022] are also proposed to boost frame rate in games but the details of their methods are not released. They are usually running under very high frame rate so that the artifacts become less noticeable. Offline frame interpolatin methods [Reda et al. 2022; Zhang et al. 2023; Zhou et al. 2023] take more than 100ms per frame which is impratical in real-time rendering engine. Nevertheless, frame interpolation methods bring more key-press-to-display latency, making users feel lagging when interacting with the scene,

GFFE: G-buffer Free Frame Extrapolation for Low-latency Real-time Rendering • 3

Table 1. Features and challenges of different frame generation methods.

> ||G-buf Free
> Interp.|G-buf Dependent
> Extrap.|G-buf Free
> Extrap.|
|---|---|---|---|
|Low latency||✓|✓|
|No-extra G-buffers|✓||✓|
|Motion Est.|✓||✓|
|Disocclusion||✓|✓|
|Non-geo Tracking|✓|✓|✓|

and this becomes more severe when input frame rate is low such as boosting 30 FPS to 60 FPS.

### 2.3. Frame Extrapolation

To avoid the extra latency introduced by frame interpolation while increasing frame rate, frame extrapolation methods have been studies these years to generate new frames only based on history frames. ExtraNet [Guo et al. 2021] uses occlusion motion vectors [Zeng et al. 2021] with a neural network to handle both disocclusion areas and lagging shadow and shadings. Learnable motion vector [Wu et al. 2023b] proposes a recurrent framework to optimize motion vectors so that they can handle the motion of shadings and disocclusion areas. ExtraSS [Wu et al. 2023a] uses G-buffers to guide the extrapolation process and uses a flow-based neural network to fix the shading errors. All of these methods require generation of G-buffers for extrapolated frames, which is not always the case in real-time rendering of different engines and platforms such as mobile, cloud gaming and some forward rendering engines. Concurrent work Yang et al. [2024] uses simple warping and hole filling method for extrapolation but fails with large disocclusions and does not consider shading's motion.

Video extrapolation methods [Hu et al. 2023], although do not require G-buffers for extrapolated frames, usually yield much worse quality and performance, which are usually not suitable for realtime rendering. [Li et al. 2022] uses optical flow to predict the future frames but a reshading process is needed for refining extrapolated frames, which is different from our settings.

## 3. Motivation

### 3.1. Problem formulation and design choices

Our G-buffer free extrapolation framework aims to extrapolate new frames to increase the presented frame rate without dependence on G-buffers for extrapolated frames and additional latency. Note that we use the term "G-buffer free" to refer to the absence of G- buffers for extrapolated frames only. The depth buffer and motion vectors for rendered frames are used since they are usually readily available in the rendering engine without additional cost. Unlike previous G-buffer based extrapolation methods [Wu et al. 2023a,b], the G-buffers for extrapolated frames are not available under our setting and some types of G-buffers including albedo, normal and roughness are not available even for rendered frames in forward and in some cases deferred renderers.

We formulate our problem as follows, given a sequence of rendered frames { _It_ } with their corresponding depth buffer { _Dt_ } and

motion vectors { _Vt_ }, our framework generates new frames { _I_¯ _t_ + __ } with their corresponding depth buffer { _D_¯ _t_ + __ } and motion vectors { _V_¯ _t_ + __ }, where __ depends on the number of frames we want to generate for every rendered frame.

In addition to our G-buffer free frame extrapolation, there are two other methods commonly used: G-buffer free frame **interpolation** and G-buffer **dependent** frame extrapolation. The features of these three types of methods are shown in Table 1.

_Latency._ Frame interpolation methods are widely used and have demonstrated good quality as it is easier to find correspondence in either previous or latter frames. The main disadvantage of interpolation methods is the additional latency introduced. As analyzed in previous works [Guo et al. 2021; Wu et al. 2023a,b], the latency of interpolation methods is increased by at least one rendering time interval, which is even higher than the original latency without the frame interpolation method. This leads to worse user experience especially when the low latency is required such as competitive games [Kim et al. 2020] and VR applications. Although mitigation techniques such as NVIDIA Reflex can be used to decrease the latency, they still cannot fully eliminate it.

_G-buffers._ To avoid introducing additional latency, frame extrapolation methods have been proposed [Guo et al. 2021; Wu et al. 2023a,b]. Since it is a more challenging task to achieve similar quality compared to frame interpolation methods, various types of G- buffers from extrapolated frames are required. However, it is not always practical due to the different engine types (forward rendering engine) and G-buffer generation cost. More discussion about this is included in the appendix.

Besides, we also consider our G-buffer free frame extrapolation framework for possible future applications, especially low-latency streaming and cloud gaming on various low end devices. In order to provide immediate response to the user inputs, the frame extrapolation should be done in the client side, where scene information is not available to generate G-buffers. Therefore, our G-buffer free frame extrapolation framework is more suitable for such applications than frame interpolation or G-buffer dependent extrapolation.

### 3.2. Challenges

_3.2.1 Motion estimation._ Our method works under the assumption that the rendering engine doesn't generate any G-buffers for extrapolated frames. Therefore, unlike previous G-buffer dependent extrapolation methods [Guo et al. 2021; Wu et al. 2023a,b], the motion from rendered frame _It_ to extrapolated frame _I_¯ _t_ + __ is unknown. Previous warping methods [Bowles et al. 2012; Lee et al. 2018; Mark et al. 1997] work either only on static scenes or where the objects' motion is given, so no motion estimation is needed.

Motion estimation is challenging since the motion of the objects in the game can be arbitrarily complex. Frame interpolation methods [Jin et al. 2023; Kong et al. 2022] use neural networks to predict the motion between two rendered frames but they are usually slow and unstable sometimes.

Instead of using heavy and slow neural networks to predict the motion, we use a heuristic motion approximation method to estimate the motion for each dynamic fragment for extrapolated frames.

Our goal is to estimate a plausible motion in order to achieve smooth transitions in continuous frames - we do not expect perfectly estimated motion since the future frame's motion can be arbitrary.

_3.2.2 Disocclusions._ Disocclusions, as shown in Fig. 2, are areas that are not shown in the previous frame but visible in the current frame. They are challenging to handle with the frame extrapolation approach due to the lack of information from the next rendered frame, which is used in frame interpolation frameworks. Previous frame extrapolation methods used G-buffers for the extrapolated frame. Although they are not shaded, they provide sufficient information to fill disocclusions. However, under our settings, there is no such information in either the previous frame or G-buffers, which makes it much more difficult to recover this information.

To better understand and deal with the disocclusions, we categorize them into three types: **(1) Out-of-screen disocclusion:** Pixels that are shown in the current frame but are not in the screen space of the previous frame. This type of disoccluded areas are caused by the camera's motion. **(2) Static disocclusion:** Pixels that are shown in the current frame but are not shown in the previous frame due to occlusion from static occluders. These pixels are static in the two consecutive frames and in the screen space of the previous frames. They become visible in the current frame due to the change of the camera's position. **(3) Dynamic disocclusion:** Similar to the static disocclusion, the only difference is that the occluders are dynamic. These areas are usually caused by the motion of the occluders instead of the camera.

Simply using a neural network to fill the disocclusions causes severe artifacts as we don't have any information in those areas and the size1 of the disocclusions areas is usually not small as shown in Fig. 2. Our proposed method uses history information with efficient adaptive rendering windows to handle the disocclusions more plausibly.

_3.2.3 Non-geometric motion tracking._ Frame generation methods usually reuse temporal information, and try to find corresponding pixels in existing frames. However, such correspondence computation is not always accurate. The color in the rendered frames is the combination of lighting information with materials' properties, which may have different directions of motion. Rendered motion vectors only capture the motion of the geometry, but not the motion of the lighting information. Only considering the geometries' motion like Yang et al. [2024] will cause lagging in shading and shadows [Guo et al. 2021; Wu et al. 2023a,b]. For example, shadows move at 30 FPS and other objects move at 60 FPS. These shadows and reflections, although they contribute a small portion to common metrics such as PSNR and SSIM, are crucial for the visual quality. Therefore, a module for tracking such motion is necessary to maintain a high frame rate in all areas and provide smooth transition between frames. To address this, we designed our shading correction network to fix these issues.

## 4. Method

The overview of our framework is shown in Fig. 3. Challenges mentioned in sec. 3.2 are addressed by our different modules: motion estimation (sec. 4.1), background collection for static and dynamic disocclusions (sec. 4.2), adaptive rendering window for out-of-screen disocclusions (sec. 4.3) and shading correction network for nongeometric motion tracking (sec. 4.4). We detail each component in the following sections.

### 4.1. Motion estimation

Frame extrapolation usually re-uses history frames information to generate new frames. Existing extrapolation methods [Guo et al. 2021; Wu et al. 2023a,b] use motion vectors from rendering engines to find the corresponding pixels in the previous frame. These motion vectors are accurate but require full rasterization pipeline for extrapolated frames. We propose a motion estimation module to efficiently predict the motion of fragments for extrapolated frames.

The motion estimation module consists of three parts: history tracking, position estimation and warping. It first collects the history trajectory in the world space, and then use it to estimate the next world position for extrapolated frame. After that, a warping process is applied to warp fragments to extrapolated frames.

_History tracking._ History tracking happens in rendered frames, which calculate the history trajectory of each fragment in the world space. In high level, our history tracking algorithm works recurrently for each rendered frame to generate _k_ history world position { _Pi_ [ _x_ ]} for each pixel, where current trajectory is updated from corresponded previous trajectory. In order to avoid incorrect correspondences due to disocclusions, we designed a static test algorithm by comparing previous screen space of each pixels by using motion vectors and view projection matrix. If the distance is small than a threshold, the pixel is set as a static pixel, to avoid calculating incorrect history trajectory. The details of algorithm is shown in appendix Algo. 1.

_Position estimation._ History tracking provides history world position { _Pi_ [ _x_ ]} for each pixel. For extrapolated frames, let __ be the extrapolation factor which is calculated by __ = _<u>n</u>_ <u>+</u> _<u>j</u>_ <u>1</u>, where_n_is the number of extrapolated frames per rendered frame, and _j_ refers to the _j_ -th extrapolated frame for a rendered frame. As shown in Fig. 4, the next position _NPt_ -> _t_ + __ is estimated by calculating the linear motion of last two position in the trajectory

NPt -> t + [ x ] = ( P 0 [ x ] - P 1 [ x ]) + P 0 [ x ] (1)

Unlike calculating the motion in the images where linear motions are not reliable due to perspective project, camera rotation and etc., linear motion assumption in the world space efficiently generates plausible next world positions. High order polynomials could be used here but leads to worse results. Please refer to ablation studies for more analysis.

_Warping._ After calculating the next world position, each fragment is projected to the extrapolated frame based on the camera view projection matrix. For multiple fragments projected into the same

1The size of disocclusions depends on the frame rate and objects' motion speed. We target 30 FPS inputs which usually has noticeable disocclusions

GFFE: G-buffer Free Frame Extrapolation for Low-latency Real-time Rendering • 5

> Rendered Frame
> ( It , Dt ,Vt -> t -1 )

> GAE Output Final Extrapolation
> ( I ¯ t GAE + , D ¯ t + , V ¯ t + -> t ) I ¯ t +

**Figure 3.** Our method generates an extrapolated frame _I_¯ _t_ + __ from the rendered frame _It_ and history frames. The left part shows the process of rendered frames including adaptive rendering window , history tracking and background collection, which are prepared for extrapolated frames. The right part shows the process of extrapolating a frame, including geometry aware extrapolation (GAE) and shading correction network (SCN). The depth and motion vectors in extrapolated frames are generated in our framework instead of rendering engine, which can be used for additional post-processing.

**Figure 4.** Our motion estimation module tracks history trajectories and estimate next world positions based on history trajectory.

pixel, we compare the depth value for the projected pixels and keep the fragment with the smallest depth value using atomic operations. Although there are several works[Bowles et al. 2012; Lee et al. 2018] with better ways of warping/projection, our warping method is efficient and simple, which is already sufficient for our pipeline.

### 4.2. Hierarchical Background Collection

With estimated motion and warping in sec. 4.1, an initial extrapolated frame is generated but with invalid regions caused by disocclusions as analyzed in sec. 3.2.2, where we use two modules to handle them. One insight is that static and dynamic disocclusions, although are invisible in the previous frame, may showed in long history frames before. However, naively storing more history frames are impractical due to memory limits and matching the correspondences are also time consuming. Inspired by this, we propose a hierarchical background collection module, to efficiently collect useful information from history frames to fill disocclusions.

This module contains two parts: a background collection for rendered frames to maintain a background buffer { _Bl_ } to collect the fragments of rendered frames as well as the fragments behind

> Render Frame I Cur Layer 0 B 0 Cur Layer 1 B 1
> · · ·
> Static Fragments
> Same Level Fragments · · ·
> Deeper Level Fragments
> Prev Layer 0 B 0 ′ Prev Layer 1 B 1 ′

**Figure 5.** The process of hierarchical background collection. Top row is current rendered frame and updated background buffers, and the bottom row is previous background buffers. Different color arrows show different conditions when updating the background buffers. The size of deeper layers (Layer 1) is only 1/4 as the previous layer (Layer 0).

it without additional rendering cost, and a background projection process for extrapolated frames to fill disocclusions.

_Background collection._ Fig. 5 shows the process of backrgound colleciton. The background buffer _B_ contains _L_ levels with a pair of color buffer and depth buffer for each level, denoting as _B_ = { _Bl_ }, and the size of deeper level is only 1/4 to the previous level. Let ( _It , Dt_ ) be the current rendered frame, and _B_′ = { _Bl_′} be the previous background buffer. The static fragments of rendered frame ( _It , Dt_ ) are filled into the first layer of updated background buffer _B_ 0. For each level _l_ in the previously collected background _Bl_′, there two conditions to update the current background buffer _Bl_ :

- **Case 1 (same level fragments):** If the corresponding position in the same level _Bl_ [ _x_′ ] is invalid, _Bl_′[_x_]isusedfor filling _Bl_ [ _x_′ ].

> Fixed Enlarged Not Enlarged
> Adaptively Enlarged Next View

**Figure 6.** Rendered image under different settings. The yellow rectangle shows displayed areas of the frame. All frames are rendered under the same resolution. Our adaptive strategy not only covers the area we need for the next view, but also contains less redundant information.

- **Case 2 (deeper level fragments):** If the corresponding position in the same level _Bl_ [ _x_′ ] is already valid, and the depth value of _Bl_′[_x_]is larger than it,_B_ _l_′[_x_]is used for next level _Bl_ +1 [ _x_′ ], meaning the deeper fragments of the current layer. If multiple fragments are projected into the same pixel, we keep the fragment with the smallest depth value that satisfies the condition.

Each level represents a layer of geometries in the scene and the higher level contains the deeper fragments. Hence, we can keep track the occluded fragments by updating the background from level 0 to _L_ .

_Background projection._ For extrapolated frames, the collected background buffers are projected to the world space and then back to the extrapolated frames. We only fill the invalid regions of disocclusions in the extrapolated frames.

### 4.3. Adaptive rendering window

The out-of-screen disocclusion, unlike regions that can be handled by background collection, is never shown in history frames such as continuously rotating camera to the right. These disocclusion areas are on the boundary of the frame and enlarging the original rendering viewport could cover those areas.

A naive way to solve it is to enlarge the field-of-view angle for rendered frames, but many redundant information are included, which leads to blurry results for displayed areas under the same rendering cost. Instead, we propose an adaptive rendering window strategy to decrease the area of redundant region as shown in Fig. 6.

Specifically, when rendering a frame _t_ , we estimate the potential areas that will be used for extrapolated frames by two steps: estimate next camera pose and calculate the rendering viewport. Assume camera pose of current frame is _Ct_ and previous rendered frame is _Ct_ -1, where the pose is formed by three vectors ( _v_pos _, v_dir _, v__up_ ). To estimate the camera pose of extrapolated frames _C_¯ _t_ + __ , we use a similar method as our motion estimation by

C ¯ t + = Ct + · ( Ct - Ct -1) (2)

where it calculates each vector component separately.

**Figure 7.** The inputs of shading correction network (SCN). Images from left to right are: the output of GAE module _I_¯ _t_GAE + __, the backward warped result _It__w_ -1-> _t_ + __from frame_t_-1 using motion vectors, the generated depth buffer _D_ ¯ _t_ + __ , and the input mask _Mt_input .

After calculating the estimated camera pose in the next extrapolated frame, the new rendering viewport is approximated based on the union of current camera pose _Ct_ and estimated camera pose _C_ ¯ _t_ + __ rendering areas, which is used for actual rendering. Please refer to appendix for details of calculating actual rendering viewport.

### 4.4. Shading Correction Network

Previous modules handle the motion of geometries, so we call them geometry aware extrapoltion (GAE) module. However, the motion of shadings is not tracked, and simply ignoring it causes shadings move in low frame rate as analysis in sec. 3.2. Thank to our previous efficient modules which handle geometries motion and disocclusions, we introduce a light neural network called shading correction network (SCN) for non-geometric motion tracking and refinement, which is unlike prior works UPR-Net [Jin et al. 2023], IFR-Net [Kong et al. 2022] and DMVFN [Hu et al. 2023] using large neural networks to estimate the flow for the whole image.

_Non-geometric motion detection._ To make SCN only focus on the non-geometric motion and shadings, we generate a focus mask to identify the areas that need to be refined and exclude the areas that are already plausible. The focus mask is calculated by the following formula:

> M focus [ x ] = x ′ ∈min N ( x ) s ( I GAE [ x ] , I gt [ x ′]) > 0 . 5 (3)
> ∧( M ˆ dyn [ x ] = 0)

where _s_ (· _,_ ·) refers to symmetric mean absolute percentage error (SMAPE), _N_ ( _x_ ) is the set of 9 neighborhood pixels and _M_ˆdyn represents whether a pixel is dynamic. This mask will ignore subtle difference and pixels shifting to only focus on the shading changes.

_Shading correction network._ After calculating the focus mask, it guides the shading correction network to only focus on the nongeometric motion. Specifically, the inputs of the network contain: the output of GAE module _I_¯ _t_GAE + __, the corresponding projected depth buffer _D_¯ _t_ + __ , the warped frame _It__w_ -1-> _t_ + __from rendered frame_t_-1 to _t_ + __ where ghosting areas are replaced with the correponding areas in _I_¯ _t_GAE + __, and the input mask_M_input. An example of the shading correction network's inputs is shown in Fig. 7.

The input mask is generated by our GAE module, where white region indicates dynamic areas, black region indicates disocclusion areas and grey region indicates remaining areas. Warped frame from _t_ - 1 provides a different shading condition comparing to _I_¯ _t_GAE + __in

> , Vol. 1, No. 1, Article . Publication date: June 2024.

GFFE: G-buffer Free Frame Extrapolation for Low-latency Real-time Rendering • 7

Table 2. Scene configuration for training and testing. All frames are captured in 1080p/30fps for inputs and 1080p/60fps for outputs. Our dataset contains less training data and more diverse testing data comparing to previous works [Guo et al. 2021; Wu et al. 2023a,b].

the other one measures the video perceptual quality. We encourage readers to combine quantitative comparison with image/video qualitative comparison for better understanding.

## 5. Experiments

### 5.1. Datasets

To demonstrate effectiveness and robustness of our method, we collect 8 difference scenes with different characteristics from Unreal Engine [Epic Games 2022]. To demonstrate the generalization ability of our method, we use 4 scenes for training and test on all scenes, where 4 scenes are never shown during training. The details of the dataset are shown in Table. 2. Our collected test scenes are more diverse than previous works [Guo et al. 2021; Wu et al. 2023a,b] with few training data in order to show our robustness and generalization ability.

> |Scenes|Training
> Sequences|Testing
> Sequences|Training
> Frames|Testing
> Frames|
|---|---|---|---|---|
|Bunker|2|1|2000|720|
|Park|2|1|2000|720|
|Future|2|1|2000|720|
|City|2|1|2000|720|
|Town|0|1|0|720|
|Forest|0|1|0|720|
|Factory|0|1|0|720|
|Infiltrator|0|1|0|720|
### 5.2. Quantitative Metrics

We evaluate our method with both quantitative metrics and qualitative images/videos to show the comparison. Four metrics are included to show various aspects of our quality: peak signal-to-noise ratio (PSNR), structural similarity index (SSIM), perceptual similarity (LPIPS) [Zhang et al. 2018], and FovVideoVDP (FvVDP) [Mantiuk et al. 2021]. However, we notice that PNSR and SSIM are less sensitive to blurriness, distortion and temporal flickering since they measure local similarity. LPIPS and FvVDP are more suitable in our case with one measures the whole image perceptual similarity and

### 5.3. Comparison against Baselines

To demonstrate the effectiveness of our method, we compare our method with state-of-the-art baselines under three different settings. Note that this is not a fair comparison since frame interpolation and G-buffer dependent extrapolation methods are under easier settings, which means they don't need to handle either disocclusions or motion estimation while our method needs to handle both. Even though, our method still achieves comparable or better results than baselines in general.

UPR-Net [Jin et al. 2023] and IFR-Net [Kong et al. 2022] are SOTA video interpolation methods which use optical flow like methods to generate intermediate frames. Offline video interpolation methods [Reda et al. 2022; Zhang et al. 2023; Zhou et al. 2023] take more than 100 ms per frame, which is too slow to be used in real-time rendering and irrelevant to our task. DLSS 3.0 and FSR 3 are commercial frameworks where the code and details of implementation are unavailable and it is difficult to obtain the intermediate results for comparison.

ExtraSS [Wu et al. 2023a] is a joint framework for super resolution and frame extrapolation in real-time rendering with G-buffers for corresponding extrapolated frame. We use ExtraSS-E modules for comparison, which is the extrapolation part of ExtraSS. We choose ExtraSS-E as our baseline instead of LMV [Wu et al. 2023b] because the latter one requires even more additional G-buffers for rendered and extrapolated frames, which even far away from our goal of G-buffer free frame extrapolation.

DMVFN [Hu et al. 2023] is a video future prediction method which uses current and previous frames to predict the future frame and can be considered as a G-buffer free frame extrapolation baseline.

UPR-Net, IFR-Net and DMVFN are trained on a large scale video dataset and we fine-tune their pre-trained models on our datasets with learning rate 10-4 for 50 epochs. ExtraSS-E is trained on our datasets from scratch with the same settings as ours.

_5.3.1 Qualitative comparison._ The qualitative comparison is shown in Fig. 8 (trained scenes) and Fig. 9 (not trained scenes). DMVFN generates highly distorted results when motion is large and can not generate correct results for the areas that don't have corresponding information in prior two frames. Frame interpolation methods UPRNet and IFR-Net cannot track the motion of thin geometries, so thin geometries are usually missing in this case. Besides, their estimated optical flows are not accurate enough to generate clear results so their results are usually over-blurred or even severely distorted in some cases. ExtraSS-E uses ground truth G-buffers to guide the generation of extrapolated frames, which is usually more stable and contains more details. However, it fails in translucent materials (windows in City) and generates flickering results without its own super sampling module. Our method generates more stable frames with sharper details and less distortions.

_5.3.2 Quantitative comparison._ Table. 3 shows the quantitative comparison against baselines. As discussed in sec. 5.2, PSNR and SSIM

> Ours Full Frame IFR-Net UPR-Net ExtraSS-E DMVFN Ours GT
> Bunker
> Park
> Future
> City

**Figure 8.** Qualitative comparison in **trained** scenes between our method and baseline methods including DMVFN [Hu et al. 2023], UPR-Net[Jin et al. 2023], IFR-Net [Kong et al. 2022] and ExtraSS-E [Wu et al. 2023a]. DMVFN generates distorted results and cannot generate correct results if the information is missing from two given images. UPR-Net generates over-blurred results and misses thin geometries. ExtraSS-E generates overall good results but fails in translucent materials (windows in the second row). Our method generate detailed extrapolated frames closer to the ground truth with correct geometries and shadings.

metrics are not always reliable to evaluate the quality of the generated frames. UPR-Net and IFR-Net shows severe distortion and missing geometries as shown in previous qualitative comparison, although they have higher PSNR and SSIM metrics in some scenes. ExtraSS-E shows marginaly better results in scenes with complex geometries such as Forest and Park since G-buffers provide strong clues for the generation of extrapolated frames but the time of generating G-buffer in those scenes is usually long. Besides, it fails with scenes with translucent materials like City and Future. DMVFN

shows significant lower PSNR and SSIM metrics than other methods since it struggles in disocclusion areas and generates severelt distorted results.

Besides PSNR and SSIM metrics, our method shows better results in LPIPS and FvVDP metrics which are more perceptual and suitable for evaluating the quality of the generated frames, and more consistent with qualitative image and video comparison. This is because our method generates more stable and plausible frames than other baselines, which is important in real-time rendering applications

> GFFE: G-buffer Free Frame Extrapolation for Low-latency Real-time Rendering • 9
> Ours Full Frame IFR-Net UPR-Net ExtraSS-E DMVFN Ours GT
> Town
> Forest
> Factory
> Infiltrator

**Figure 9.** Qualitative comparison in **test** scenes between our method and baseline methods including DMVFN [Hu et al. 2023], UPR-Net[Jin et al. 2023], IFR-Net [Kong et al. 2022] and ExtraSS-E [Wu et al. 2023a]. Our method still shows comparable or better visual quality with less distortion, blurriness and artifacts even though there are some big gap between training scenes and the test scenes.

since people notice the flickering and distortion more than pixel level differences.

Based on our quantitative and qualitative evaluation, GFFE is significantly better than G-buffer free baselines in all aspects, and shows comparable results with G-buffer dependent and interpolation methods with better perceptual quality in LPIPS and FvVDP metrics.

our method generates stable and plausible results in all scenes. This is because our hybrid modules are robust to different scenes and can handle disocclusions and motion estimation well, instead of using a single neural network to handle all problems which requires a large scale dataset for training. Therefore, we consider our method has good generalization ability and robustness to various different scenes.

### 5.4. Generalization

Our framework is trained on 4 scenes and tested on 8 scenes, where 4 scenes are never shown during training. Despite the limited dataset,

### 5.5. Performance

Performance is an important factor for real-time rendering applications. We used a machine with NVIDIA RTX 4070Ti Super GPU and

Table 3. Quantitative comparison with UPR-Net [Jin et al. 2023], IFR-Net [Kong et al. 2022], DMVFN [Hu et al. 2023] and ExtraSS-E [Wu et al. 2023a] under PSNR, SSIM, LPIPS, and FvVDP. Our method shows comparable quality with interpolation methods UPR-Net and IFR-Net, and G-buffer dependent extrapolation method ExtraSS-E under PSNR and SSIM metrics. Besides, our method shows better perceptual quality than interpolation methods and G-buffer dependent method since these baselines are over-blurred, distorted or flickering as anaylzed in qualitative comparison. Our method also outperforms G-buffer free extrapolation baseline DMVFN in all aspects. The values of SSIM and LPIPS are scaled by 102 .

||||Trai|ned|||No|t trained|||
|---|---|---|---|---|---|---|---|---|---|---|
|Type|Method|Bunker|Park|Future|City|Town|Forest|Factory|Infiltrator|Average|
|Inter|IFR|25.55|18.37|28.66|26.16|27.41|19.62|22.40|25.78|24.24|
> |↑
> p.|UPR|26.35|18.52|**28.75**|27.99|**27.75**|19.60|23.87|**26.12**|**24.87**|
> |NR
> G-buf Extrap.|ExSS-E|26.44|**24.75**|25.77|24.38|27.45|**21.71**|23.10|23.53|24.64|
> |PS
> Et|DMVFN|23.79|16.89|23.69|24.77|24.44|17.00|20.38|22.84|21.73|
|xrap.|Ours|**27.84**|17.04|26.33|**29.77**|26.09|18.21|**24.50**|23.78|24.20|
|I|IFR|86.78|71.43|**94.82**|84.84|91.16|70.23|84.66|91.21|84.39|
> |↑
> nterp.|UPR|88.21|72.44|94.65|88.26|90.58|67.55|86.16|**91.77**|84.95|
> |IM
> G-buf Extrap.|ExSS-E|91.17|**86.79**|91.68|88.92|**92.32**|**78.57**|88.45|85.51|**87.93**|
> |SS
> E|DMVFN|82.39|61.18|87.46|81.43|81.63|51.97|76.25|86.39|76.09|
|xtrap.|Ours|**93.50**|73.80|93.46|**93.49**|89.04|65.25|**89.55**|89.37|85.93|
|It|IFR|15.62|24.19|10.67|22.86|11.89|28.00|17.95|12.88|18.01|
> |↓
> nerp.|UPR|22.49|42.44|17.15|24.05|22.99|52.81|26.15|19.19|28.41|
> |IPS
> G-buf Extrap.|ExSS-E|12.22|14.63|14.73|17.38|**7.79**|**17.78**|15.72|24.26|15.56|
> |LP
> Et|DMVFN|16.89|28.12|12.83|20.06|16.65|35.44|20.79|15.42|20.78|
|xrap.|Ours|**6.68**|**14.02**|**5.74**|**7.24**|7.98|16.98|**9.88**|**8.81**|**9.67**|
|I|IFR|7.98|6.77|5.36|7.20|8.35|7.07|6.84|7.77|7.17|
> |P↑
> nterp.|UPR|8.52|6.85|**5.40**|8.45|**8.75**|7.16|7.24|8.05|7.55|
> |VD
> G-buf Extrap.|ExSS-E|8.21|**7.40**|5.30|5.96|8.11|**7.38**|7.39|7.78|7.19|
> |Fv
> E|DMVFN|7.25|6.57|5.37|6.85|7.88|6.43|6.49|7.56|6.80|
|xtrap.|Ours|**8.65**|6.97|5.37|**8.58**|8.65|7.05|**7.87**|**8.09**|**7.65**|

Table 4. Runtime (ms) for all methods to generate 1080p frames, + means not including the time of generating G-buffers, see Table. 6.

Table 6. G-buffer generation time (ms) under 1080p for different scenes. For non-experimented scenes in products, the time may even exceed 10 ms.

|UPRNet|IFRNet|DMVFN|ExSS-E|Ours-Full|
|---|---|---|---|---|
|43.04|19.50|20.57|4.18+|**6.62**|

Table 5. Runtime (ms) breakdown for our framework under different resolutions. Misc mainly includes adjusting display window and maintaining correct motion vectors.

||540p|720p|1080p|
|---|---|---|---|
|BG Collection|0.34|0.54|1.13|
|History Track|0.27|0.49|1.04|
|Misc|0.09|0.14|0.37|
|BG Projection|0.16|0.32|0.76|
|Position Pred.|0.07|0.11|0.22|
|Warp|0.51|0.76|0.80|
|SCN|0.90|1.30|2.30|
|Total|2.34|3.66|6.62|

||Bunker|Park|Future|City|
|---|---|---|---|---|
|Time|0.35|8.23|2.85|0.40|
||Town|Forest|Factory|Infiltrator|
|Time|2.02|2.61|1.91|0.96|

Ryzen 9 5900X CPU for inference. The non-neural modules (GAE) of our method are implemented in NVIDIA Falcor [Kallweit et al. 2022] renderer. All neural networks including baselines are trained under PyTorch framework and converted into TensorRT [NVIDIA 2021] with FP16 precision for inference.

Table. 5 shows the break down run time of our method under different resolutions. Note that our method is designed to be applied in the post-processing stage and the performance is not affected by complexity of the scene.

Table. 4 shows the run times for all methods. Previous frame interpolation methods UPR-Net and IFR-Net and extrapolation method DMVFN are much slower than our method, since neural networks are usually slow comparing to heuristic methods. ExtraSS-E is faster than our method since it uses G-buffers to guide the generation of

GFFE: G-buffer Free Frame Extrapolation for Low-latency Real-time Rendering • 11

Table 7. Ablation study on our designed modules. Numbers are averaged for all scenes. SSIM and LPIPS numbers are scaled by 102 . ME = Motion Estimation, BGC = Hierarchical Background Collection, AW = Adaptive Render Window, SCN = Shading Correction Network, FM = Focus Mask

||PSNR↑|SSIM↑|LPIPS↓|FvVDP↑|
|---|---|---|---|---|
|w/o ME|23.44|85.21|10.36|7.57|
|w/o BGC|24.11|85.91|9.79|7.62|
|w/o AW|24.15|85.91|9.71|7.62|
|w/o SCN|23.93|85.86|**9.23**|7.60|
|w/o FM|23.86|81.39|35.63|7.49|
|Ours Full|**24.20**|**85.93**|9.67|**7.65**|

w/o ME 5 Order Est. Ours GT

**Figure 10.** Ablation study of the motion estimation (ME) module. Without this module, the geometry will move. With higher order polynomials to estimate the motion, it diverges and is unstable. Our approach estimates the motion for dynamic objects more plausibly.

extrapolated frames. However, the time of generating G-buffers is not included in the runtime of ExtraSS-E, which varies depending on the complexity of the scene. Table. 6 shows the time of generating G-buffers for different scenes, where complex geometries scene like Forest and Park takes longer time to generate G-buffers. Note that the scenes that ExtraSS-E is better than ours are usually the scenes with complex geometries.

Although there are some dedicated hardware or software optimizations could be applied to accelerate the runtime performance, all methods are tested under the same environment and settings without dedicated optimizations, so any optimizations applied to baselines can also be applied to our method to achieve better performance.

## 6. Ablation study

Our framework is a complete pipeline that consists of several modules for G-buffer free frame extrapolation. In this section, we analyze the effectiveness and importance of each module in our framework to demonstrate the necessity of each module. Table. 7 show the quantitative metrics of removing our designed modules, and more qualititative results will included in following sections and the supplementary video.

### 6.1. Motion Estimation

Motion estimation tracks the motion of dynamic fragments and project to extrapolated frames. Linear motion in the world space is used for estimate the next world position in the extrapolated frames. Higher order polynomials could be used but with worse quality in our experiments. As shown in Fig. 10, dynamic objects

w/o BC 1 Layer 2 Layers GT

**Figure 11.** Ablation study of the hierarchical background collection module. Without background collection, the disocclusion areas are not handled at all. With one layer background, it only captures background behind dynamic objects. With our two-layers background collection, it not only recovers disocclusion behind dynamic objects, but also static disocclusions behind static objects.

Not Enlarged Fixed-size Adaptive GT

**Figure 12.** Ablation study of adaptive rendering windows. Invalid region appears at the boundary of the image without the adaptive windows. Fixed-size enlarged rendering window contains more redundant and less useful information for extrapolation. Our adaptive strategy can adjust rendering window dynamically for better extrapolation.

are not moving without motion estimation module. Higher order polynomials estimation diverges and generates artifacts. Our module efficiently generates plausible motions for dynamic objects.

### 6.2. Background Collection

Background collection addresses static disocclusions and dynamic disocclusions. Without such module and directly to guess what is in the disocclusion areas, the results are usually in lower quality. Our hierarchical background collection module collects multiple layers background to handle different levels disocclusions. Fig. 11 shows the comparison between results with and without background collection. We can see that our complete background fixes not only disocclusions behind dynamic objects but also for the disocclusions behind the static objects due to camera motion.

### 6.3. Adaptive Rendering Window

We compare our adaptive strategy with fixed enlarged window and not enlarged window. Fig. 12 shows the comparison between these three methods, and our method covers more disocclusions since our rendering window is adaptively adjusted based on camera motion.

### 6.4. Shading Correction Network

Our SCN module mainly fixes the lagging issue of non-geometries motion including shadows and reflections. As discuss in previous

w/o SCN w/o FM Ours GT

**Figure 13.** Ablation study of the shading correction network (SCN). FM = Focus Mask. Without SCN, non-geometric motions are not tracked, so shadows are not moving in extrapolated frame. Without the focus mask, directly predicting the final refined frame will blurs the whole image. Our SCN with focus mask not only fix non-geometric motion, but also keeps sharp details.

Ours-DLSS UPR-DLSS GT-DLSS

**Figure 14.** Results of integrating DLSS 2 with ours and UPR [Jin et al. 2023]. Our results contain more details while UPR with DLSS 2 still generates blurred results.

work [Guo et al. 2021], although such effects have small impact in metrics or even slightly worse (LPIPS), it is noticeable in human perception and important for high quality rendering.

Fig. 13 shows the comparison between not using SCN, without focus mask, and with our full SCN module. Without SCN, the shadows and reflections are not moving due to missing motion, leading low frame rate feeling in those areas. Without focus mask, the neural network tries to refine the whole image, which blurs the overall details. Our full SCN module can detect the areas that need to be refined and only refine those areas and do not blur other areas. For better visualization and comparison of this ablation study, please refer to the supplementary video to see how it affects the final results for continuous frames.

## 7. Discussion

### 7.1. Anti-aliasing and Super Resolution

Our framework, unlike UPR-Net, IFR-Net and DMVFN, generates not only extrapolated shaded frames, but also the corresponding depth buffer and motion vectors between the extrapolated frames and rendered frames. This indicates that the generated frames can

Ours GT Ours GT Ours GT Ours GT

**Figure 15.** Failure cases of our framework includes uncollected disocclusions, effects without depth, shading changes in disocclusions, and imperfect shading correction.

be considered the same as other rendered frames to apply additional anti-aliasing or super resolution techniques.

Super sampling techniques, including DLSS[Liu 2020], XeSS[Intel 2022], FSR[AMD 2021], have shown high quality results in generating higher resolution frames from lower resolution frames efficiently which are widely used in real-time rendering to improve the visual quality. Our method with generated depth and motion vectors can be easily integrated with such super resolution techniques to generate higher quality frames. Fig. 14 shows the comparison between our method and UPR [Jin et al. 2023] of using DLSS on Forest with complex geometries. Our results contains more details and UPR tends to over-blur them. Ground truth depth and motion vectors are used for baselines.

### 7.2. Practical Choice

We show breakdown performance and ablation studies in previous section to demonstrate the usage to each module. Each module in our framework is relatively independent and can be removed or replaced by better modules in future if needed. For example, for low end devices such as mobiles, neural network module SCN could be removed since the shading changes are usually simpler, so the integration is easier and performance is better with some degradation in quality. Our framework is flexible to be adjusted in various applications based on needs.

### 7.3. Limitations

As noted throughout the paper, our method being G-buffer free extrapolation, has much fewer inputs compared to G-buffer dependent extrapolation (missing G-buffers), and interpolation (missing future frames). Therefore, although with comparable quality overall, our method still has limitations. We analyze them below and show corresponding artifacts in Fig. 15.

_Uncollected disocclusions._ Our background collection module tries to find information from previous frames to fill in the disocclusions. However, it fails when the disocclusion areas have never been shown before and are not the out-of-screen areas (Fig. 15 the first column).

_Effects without depth._ Our framework relies on depth to calculate correct motions and projection. Some effects, including UI and particles, do not have such information, so our framework does not

GFFE: G-buffer Free Frame Extrapolation for Low-latency Real-time Rendering • 13

attempt to calculate correct positions in extrapolated frames (Fig. 15 the second column). One possible solution could be separating these effects into other passes and combining them with our extrapolated frames.

_Shading changes in disocclusions._ As shown in the third column of Fig. 15, the shading of background collected fragments can be incorrect due to view direction changes, dynamic lighting, and so on. We currently do not specifically train our shading correction network to deal with this and leave it for future work.

_Imperfect shading correction._ Since our method lacks information from G-buffers and future frames compared to the other two types of methods, estimating refined shadings such as shadows are more complicated. As a result, the outcomes of such refined shadings are sometimes blurred (Fig. 15, the fourth column). A better shading correction module is left for future work.

## 8. Conclusion

We have presented a G-buffer free extrapolation method, GFFE, for low-latency real-time rendering. We addressed three challenges of G-buffer free extrapolation tasks by our designed modules: motion estimation, background collection, adaptive rendering windows and shading correction network.

We evaluated GFFE on diverse scenes and show high quality extrapolation results that demonstrate robustness and generality. The proposed modules provide efficient frame generation without additional latency and extra G-buffers in real-time rendering context. Our framework outperforms G-buffer free extrapolation baselines, and is comparable with frame generation methods including frame interpolation and G-buffer dependent frame extrapolation, with better performance.

In the future, apart from improving the aforementioned limitations, GFFE may be worth exploring in the context of VR/AR and streaming applications. It can also be extended to perform multiple frame extrapolation by passing an extrapolation factor __ to the shading correction network to refine the shading motion in different magnitudes to further boost the performance.

## References

AMD. 2021. AMD FidelityFX™Super Resolution. https://www.amd.com/en/ technologies/fidelityfx-super-resolution Accessed: 2023-05-23. AMD. 2022. AMD FidelityFX™Super Resolution 3. https://gpuopen.com/fidelityfxsuper-resolution-3/ https://gpuopen.com/fidelityfx-super-resolution-3/ [Accessed: 2024-01-24].

- Dmitry Andreev. 2010. Real-time frame rate up-conversion for video games: or how to get from 30 to 60 fps for" free". In _ACM SIGGRAPH Talks_ . 1-1.

- Wenbo Bao, Wei-Sheng Lai, Chao Ma, Xiaoyun Zhang, Zhiyong Gao, and Ming-Hsuan Yang. 2019. Depth-aware video frame interpolation. In _Proceedings of the IEEE/CVF conference on computer vision and pattern recognition_ . 3703-3712.

- Huw Bowles, Kenny Mitchell, Robert W Sumner, Jeremy Moore, and Markus Gross. 2012. Iterative image warping. In _Computer graphics forum_ , Vol. 31. Wiley Online Library, 237-246.

- Karlis Martins Briedis, Abdelaziz Djelouah, Mark Meyer, Ian McGonigal, Markus Gross, and Christopher Schroers. 2021. Neural frame interpolation for rendered content. _ACM Trans. Graph._ 40, 6 (2021), 1-13.

- Karlis Martins Briedis, Abdelaziz Djelouah, Raphaël Ortiz, Mark Meyer, Markus Gross, and Christopher Schroers. 2023. Kernel-Based Frame Interpolation for SpatioTemporally Adaptive Rendering. In _ACM Trans. Graph. (SIGGRAPH)_ . 1-11.

- Pierre Charbonnier, Laure Blanc-Feraud, Gilles Aubert, and Michel Barlaud. 1994. Two deterministic half-quadratic regularization algorithms for computed imaging. In _Proc. international conference on image processing_ , Vol. 2. 168-172.

- Piotr Didyk, Elmar Eisemann, Tobias Ritschel, Karol Myszkowski, and Hans-Peter Seidel. 2010. Perceptually-motivated real-time temporal upsampling of 3D content for high-refresh-rate displays. In _Comp. Graph. Forum_ , Vol. 29. 713-722.

- Epic Games. 2022. _Unreal Engine_ . https://www.unrealengine.com Jie Guo, Xihao Fu, Liqiang Lin, Hengjun Ma, Yanwen Guo, Shiqiu Liu, and Ling-Qi Yan. 2021. ExtraNet: Real-Time Extrapolated Rendering for Low-Latency Temporal Supersampling. _ACM Trans. Graph._ , Article 278 (2021).

- Yu-Xiao Guo, Guojun Chen, Yue Dong, and Xin Tong. 2022. Classifier Guided Temporal Supersampling for Real-time Rendering. In _Computer Graphics Forum_ , Vol. 41. Wiley Online Library, 237-246.

- Xiaotao Hu, Zhewei Huang, Ailin Huang, Jun Xu, and Shuchang Zhou. 2023. A dynamic multi-scale voxel flow network for video prediction. In _Proc. IEEE CVPR_ . 6121-6131.

- Zhewei Huang, Tianyuan Zhang, Wen Heng, Boxin Shi, and Shuchang Zhou. 2022. Real-time intermediate flow estimation for video frame interpolation. In _European Conference on Comp. Vision_ . 624-642.

- Intel. 2022. Intel(R) Arc™- Xe Super Sampling. https://www.intel.com/content/www/ us/en/products/docs/discrete-gpus/arc/technology/xess.html Accessed: 2023-05-23.

- Xin Jin, Longhai Wu, Jie Chen, Youxin Chen, Jayoon Koo, and Cheul-hee Hahm. 2023. A Unified Pyramid Recurrent Network for Video Frame Interpolation. In _Proc. IEEE CVPR_ . 1578-1587.

- Nima Khademi Kalantari, Ravi Ramamoorthi, et al. 2017. Deep high dynamic range imaging of dynamic scenes. _ACM Trans. Graph._ 36, 4 (2017), 144-1.

- Simon Kallweit, Petrik Clarberg, Craig Kolb, Tom'aš Davidovič, Kai-Hwa Yao, Theresa Foley, Yong He, Lifan Wu, Lucy Chen, Tomas Akenine-Möller, Chris Wyman, Cyril Crassin, and Nir Benty. 2022. The Falcor Rendering Framework. https://github. com/NVIDIAGameWorks/Falcor https://github.com/NVIDIAGameWorks/Falcor.

- Joohwan Kim, Pyarelal Knowles, Josef Spjut, Ben Boudaoud, and Morgan Mcguire. 2020. Post-render warp with late input sampling improves aiming under high latency conditions. _Proc. ACM Comp. Graph. and Interactive Techniques_ 3, 2 (2020), 1-18.

- Diederik P Kingma and Jimmy Ba. 2014. Adam: A method for stochastic optimization. _arXiv preprint arXiv:1412.6980_ (2014).

- Lingtong Kong, Boyuan Jiang, Donghao Luo, Wenqing Chu, Xiaoming Huang, Ying Tai, Chengjie Wang, and Jie Yang. 2022. Ifrnet: Intermediate feature refine network for efficient frame interpolation. In _Proc. IEEE CVPR_ . 1969-1978.

- Sungkil Lee, Younguk Kim, and Elmar Eisemann. 2018. Iterative Depth Warping. _ACM Trans. Graph._ 37, 5, Article 177 (oct 2018), 13 pages. https://doi.org/10.1145/3190859

- Zhan Li, Carl S Marshall, Deepak S Vembar, and Feng Liu. 2022. Future Frame Synthesis for Fast Monte Carlo Rendering. In _Graph. Interface_ .

- Edward Liu. 2020. DLSS 2.0-Image reconstruction for real-time rendering with deep learning. In _Nvidia GPU Tech. Conf. (GTC)_ .

- Rafał K. Mantiuk, Gyorgy Denes, Alexandre Chapiro, Anton Kaplanyan, Gizem Rufo, Romain Bachy, Trisha Lian, and Anjul Patney. 2021. FovVideoVDP: a visible difference predictor for wide field-of-view video. _ACM Trans. Graph._ , Article 49 (jul 2021), 19 pages. https://doi.org/10.1145/3450626.3459831

- William R Mark, Leonard McMillan, and Gary Bishop. 1997. Post-rendering 3D warping. In _Proc. Symp. Interactive 3D Graphics_ . 7-ff.

- Simon Meister, Junhwa Hur, and Stefan Roth. 2018. Unflow: Unsupervised learning of optical flow with a bidirectional census loss. In _Proc. AAAI conference on artificial intelligence_ , Vol. 32.

- NVIDIA. 2021. NVIDIA TensorRT. https://developer.nvidia.com/tensorrt NVIDIA. 2022. NVIDIA DLSS 3: AI-Powered Performance Multiplier Boosts Frame Rates By Up To 4X. https://www.nvidia.com/en-us/geforce/news/dlss3-ai-poweredneural-graphics-innovations/ https://www.nvidia.com/en-us/geforce/news/dlss3ai-powered-neural-graphics-innovations/ [Accessed: 2024-01-24].

- Fitsum Reda, Janne Kontkanen, Eric Tabellion, Deqing Sun, Caroline Pantofaru, and Brian Curless. 2022. Film: Frame interpolation for large motion. In _European Conference on Computer Vision_ . Springer, 250-266.

- Bernhard Reinert, Johannes Kopf, Tobias Ritschel, Eduardo Cuervo, David Chu, and Hans-Peter Seidel. 2016. Proxy-guided image-based rendering for mobile devices. In _Comp. Graph. Forum_ , Vol. 35. 353-362.

- Andre Schollmeyer, Simon Schneegans, Stephan Beck, Anthony Steed, and Bernd Froehlich. 2017. Efficient hybrid image warping for high frame-rate stereoscopic rendering. _IEEE Trans. Vis. and Comp. Graph._ 23, 4 (2017), 1332-1341.

- Songyin Wu, Sungye Kim, Zheng Zeng, Deepak Vembar, Sangeeta Jha, Anton Kaplanyan, and Ling-Qi Yan. 2023a. ExtraSS: A Framework for Joint Spatial Super Sampling and Frame Extrapolation. In _ACM Trans. Graph. (SIGGRAPH Asia)_ . Article 92.

- Zhizhen Wu, Chenyu Zuo, Yuchi Huo, Yazhen Yuan, Yifan Peng, Guiyang Pu, Rui Wang, and Hujun Bao. 2023b. Adaptive Recurrent Frame Prediction with Learnable Motion Vectors. In _ACM Trans. Graph. (SIGGRAPH Asia)_ . Article 10.

- Lei Xiao, Salah Nouri, Matt Chapman, Alexander Fix, Douglas Lanman, and Anton Kaplanyan. 2020. Neural supersampling for real-time rendering. _ACM Transactions on Graphics (TOG)_ 39, 4 (2020), 142-1.

- Lei Yang, Yu-Chiu Tse, Pedro V Sander, Jason Lawrence, Diego Nehab, Hugues Hoppe, and Clara L Wilkins. 2011. Image-based bidirectional scene reprojection. In _ACM Trans. Graph. (SIGGRAPH Asia)_ . 1-10.

- Sipeng Yang, Qingchuan Zhu, Junhao Zhuge, Qiang Qiu, Chen Li, Yuzhong Yan, Huihui Xu, Ling-Qi Yan, and Xiaogang Jin. 2024. Mob-FGSR: Elevating Mobile Real-Time Rendering with Fast Frame Generation and Super Resolution. _ACM Trans. Graph._ (2024).

- Zheng Zeng, Shiqiu Liu, Jinglei Yang, Lu Wang, and Ling-Qi Yan. 2021. Temporally Reliable Motion Vectors for Real-time Ray Tracing. In _Comp. Graph. Forum_ , Vol. 40. 79-90.

- Guozhen Zhang, Yuhan Zhu, Haonan Wang, Youxin Chen, Gangshan Wu, and Limin Wang. 2023. Extracting motion and appearance via inter-frame attention for efficient video frame interpolation. In _Proceedings of the IEEE/CVF Conference on Computer Vision and Pattern Recognition_ . 5682-5692.

- Richard Zhang, Phillip Isola, Alexei A Efros, Eli Shechtman, and Oliver Wang. 2018. The Unreasonable Effectiveness of Deep Features as a Perceptual Metric. In _Proc. IEEE CVPR_ .

- Kun Zhou, Wenbo Li, Xiaoguang Han, and Jiangbo Lu. 2023. Exploring motion ambiguity and alignment for high-quality video frame interpolation. In _Proceedings of the IEEE/CVF Conference on Computer Vision and Pattern Recognition_ . 22169-22179.

## Appendix A. Limitation of using g-buffers in extrapolated frames

As discussed in sec. 3.1, G-buffers are not available or become the bottleneck under following cases:

- **Availability:** Some types of G-buffers used in previous extrapolation methods including albedo, roughness and metallic are only available for deferred rendering pipeline. Forward rendering pipeline, which is widely used in smartphone, console and even personal computer platform, doesn't provide such G-buffers.

- **Complexity:** The generation of G-buffers is the bottleneck in some real-time application. Simulation heavy games require complex simulation process so generating G-buffers is quite time consuming. Besides, some modern game generates high quality G-buffers with low quality shading and then modulate the shading with the G-buffers to render final detailed images, where the generation of G-buffers consumes majority of the time.

- **Memory requirements** Even if the generation of G-buffers is not the bottleneck, it still requires additional memory store them with additional cost in multiple aspects.

In these cases, the G-buffer dependent methods [Guo et al. 2021; Wu et al. 2023a,b] are limited.

## Appendix B. History tracking algorithm

Here are the details of history collection algorithm. _M_dyn is the dynamic mask where 1 refers to pixels are dynamic fragments.

## Appendix C. Adaptive rendering window

After obtaining current camera pose _Ct_ and estimated next camera pose _C_¯ _t_ + __ , a virtual plane will be put in front of the current camera along the lookat direction with distance _d_ . By calculating the intersections of four corners of camera _Ct_ 's view frustum, we get the coordinates of four intersections and corresponding 2D axis-aligned bounding box of them on the plane, denoted as _r_ = ( _x_ min _,y_ min _,x_ max _,y_ max). Similarly, we calculate the axis-aligned bounding box of estimated camera _C_¯ _t_ + __ on the same virtual plane, denoting as _r_ ¯ = ( ¯ _x_ min _,_ ¯ _y_ min _,_ ¯ _x_ max _,_ ¯ _y_ max). Then we can calculate the enlarged size of rendering windows based on relative sizes of _r_ and ¯ _r_ . Assume the original rendering window is the rectangle (-1 _,_ -1 _,_ 1 _,_ 1), the adaptive window of current frame ( _u_ 0 _, v_ 0 _,u_ 1 _, v_ 1) is calculated

|**ALGORITHM 1:**History tracking with static test|
|---|
> |**Data:**Pixel position_x_, Current and frame depth {_Dt_}, Current
> motion vector_Vt_->_t_-1, Current and previous camera pose {_Ct_,
> _Ct_-1}, previous history trajectory_P_′, length of history
> trajectory_k_
> **Result:**History trajectory_P_, Dynamic Mask_M_dyn
> _t_|
> |_p_<-unproject(_x, Dt,Ct_);// cur world position
> ˆ_x_<-project(_p,Ct_-1);// previous position
> _x_′ =_x_+_Vt_->_t_-1[_x_];// previous position
> **if** ∥ˆ_x_-_x_′∥2 _> _**then**// static test
> **for** _i = 1_**to**_k-1_**do**
> _Pi_[_x_] =_P_′
> _i_-1[_x_′] ;
> **end**
> _P_0[_x_] =_p_;
> _M_dyn
> _t_
> [_x_] <-1 ;
> **end**|
> |**else**// fragment is static
> **for**_i = 0_**to**_k-1_**do**
> _Pi_[_x_] =_p_;
> **end**
> _M_dyn
> _t_
> [_x_] <-0 ;
> **end**|

> WARP
> WARP
> WARP
> 12 12 16 16 24 24 D3 16 D2 12 D1 4 D0
> Down/Up Sampling Data Flow Concat Add
> Conv + Activation + Down Sampling Skip Connnection
> BLEND

**Figure 16.** The network structure of shading correction network. The input is down-sampled at first to improve performance.

by:

> |
> _u_0 =|min(-1_,_-¯_x_min/_x_min)||
|---|---|---|
> |
> _v_0 =
> _u_1 =|min(-1_,_-¯_y_min/_y_min)
> max(1_,_¯_x_max/_x_max)|(5)|
> |
> _v_1 =|max(1_,_¯_y_max/_y_max)||

Note that the virtual plane is put in front of current camera, so the bounding box of current camera on the virtual plane always satisfies _x_ min = - _x_ max _<_ 0 and _y_ min = - _y_ max _<_ 0.

## Appendix D. Shading correction network

_Network structure._ SCN is a flow-based network with gradually predicted flows to warp intermediate features. The structure of SCN is shown in Fig. 16. The output contains a predicted focus mask and

GFFE: G-buffer Free Frame Extrapolation for Low-latency Real-time Rendering • 15

a refined image, and the final output is the blending between the refined image and the input GAE image.

_Loss Functions._ To train our SCN, we use the following loss functions to cover various aspects of the output.

Intermediate feature loss L _f_ constrains the intermediate features to better align the non-geometric flows from coarse to fine levels. It is defined as:

> 3
> L f = ∑︁ L cen ( ¯ k,k ) (6)
> k =1

where Lcen is the census loss [Meister et al. 2018] and __¯ _k_ and _k_ are the intermediate features _k_ of extrapolated frames and ground truth frames from the encoder.

Focus mask loss is the key part of our SCN module to predict a correct focus mask. It is defined as:

Lfocus = M ¯focus - M focus2 (7)

The reconstruction loss Lrecon is calculated by Charbonnier loss [Charbonnier et al. 1994] between final predicted image and the ground the truth image. The VGG perceptual loss Lvgg is used to keep the details of extrapolated frames. The final loss function is formulated as

where we set _f_ = 0 _._ 01 _, _ focus = 1 _._ 0 _, _ vgg = 0 _._ 01 in our experiments.

_Data Preparation._ During the training process, we crop the original images into 256 x 256 patches to train the network. Since our GAE module provides almost correct geometries, the majority areas of extrapolated frames are correct, which are less useful for training the network. Therefore, we first randomly crop 106 patches from the training dataset, and then sort the crops based on the areas of focus mask _M_ focus. We keep top 15% patches and randomly select other 3% patches for training. We still evaluate on full resolution images during the inference process. All color image in the linear space will be first tone-mapped by __ -Law [Kalantari et al. 2017] tone-mapper before feeding into the network and the final output will be inverse tone-mapped to the linear space. All losses are calculated in the tone-mapped space.

_Training._ We train our model on the cropped dataset with batch size 256 for 300 epochs. We use Adam [Kingma and Ba 2014] optimizer with learning rate starting from 10-4 and gradually decay to 10-5 during the training. We use PyTorch to implement our network and train it on four NVIDIA A6000 GPUs.

L = Lrecon + f L f + focusLfocus + vggLvgg (8)

## Publication notice

Authors' addresses: Songyin Wu, s_wu975@ucsb.edu, University of California, Santa
Barbara, USA; Deepak Vembar, deepak.s.vembar@intel.com, Intel Corporation, USA; Anton
Sochenov, anton.sochenov@intel.com, Intel Corporation, USA; Selvakumar Panneer,
selvakumar.panneer@intel.com, Intel Corporation, USA; Sungye Kim, sungyekim@gmail.com,
Intel Corporation (now AMD), USA; Anton Kaplanyan, anton.kaplanyan@intel.com, Intel
Corporation, USA; Ling-Qi Yan, lingqi@cs.ucsb.edu, University of California, Santa
Barbara, USA.
