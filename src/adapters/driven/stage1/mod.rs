//! Stage 1 (Mob-FGSR) compute passes on D3D12.
//!
//! A direct port of `src/generator/stage1.cpp`: the same passes, bindings and dispatch sizes, the
//! same resource-state contract and the same reset-seed behaviour (K53). The C++ kept the
//! descriptor blocks, the pipelines and the target ring in one class; this splits them into
//! [`pipelines`] (root signature, shaders, descriptors) and [`Stage1`] (the recording logic) so the
//! pipeline setup can be read without the pass order in the way.
pub mod pipelines;

use std::cell::Cell;

use windows::Win32::Graphics::Direct3D12::{
    ID3D12DescriptorHeap, ID3D12Resource, ID3D12RootSignature,
};

use crate::application::ports::gpu::{
    GpuCommandList, GpuDevice, GpuTexture, PixelFormat, ResourceState, TextureFlags, TextureInfo,
};
use crate::application::ports::generator::{
    FrameGenerator, GeneratePhase, GeneratorError, HudSource, InputFrame, PassMark, Stage1Pass,
    MAX_PHASES,
};
use crate::domain::frame::{FrameParams, MIDPOINT_ALPHA};

use crate::adapters::driven::d3d12::device::D3d12Device;
use crate::adapters::driven::d3d12::resources::{D3d12Buffer, D3d12CommandList, D3d12Texture};
use pipelines::{
    constant_words, create_root_signature, groups, DescriptorBlocks, Pipelines, HEAP_SLOTS,
    LUT_BYTES, LUT_TEXELS, PHASE_BLOCKS, PREPARE_BLOCKS,
};

/// One intermediate target: the resource plus the state Stage 1 tracks it in.
pub(crate) struct Target {
    pub(crate) texture: Option<D3d12Texture>,
    pub(crate) state: Cell<ResourceState>,
}

impl Default for Target {
    fn default() -> Self {
        Self { texture: None, state: Cell::new(ResourceState::COMMON) }
    }
}

impl Target {
    fn get(&self) -> Option<&D3d12Texture> {
        self.texture.as_ref()
    }

    fn resource(&self) -> Option<&ID3D12Resource> {
        self.texture.as_ref().map(D3d12Texture::raw)
    }

    fn state(&self) -> ResourceState {
        self.state.get()
    }
}

fn create_target(
    device: &D3d12Device,
    format: PixelFormat,
    width: u32,
    height: u32,
    flags: TextureFlags,
) -> Result<Target, GeneratorError> {
    let desc = crate::application::ports::gpu::TextureDesc::texture2d(width, height, format, flags);
    let texture = device.create_texture(&desc)?;
    Ok(Target { texture: Some(texture), state: Cell::new(ResourceState::COMMON) })
}

/// The Stage 1 generator. Not `Sync` (it owns `Cell`-tracked target states and mutable history).
pub struct Stage1 {
    device: D3d12Device,
    pipelines: Option<Pipelines>,
    root_signature: Option<ID3D12RootSignature>,
    heap: Option<ID3D12DescriptorHeap>,
    lut: Option<D3d12Buffer>,
    increment: u32,

    params: FrameParams,
    configured: bool,
    history_valid: bool,
    prepared: bool,
    current: u32,
    composite_weight: f32,
    alpha: f32,

    depth: [Target; 2],
    motion: [Target; 2],
    color: [Target; 2],
    reprojection: Target,
    filled: Target,
    midpoint: Target,
}

impl std::fmt::Debug for Stage1 {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Stage1")
            .field("params", &self.params)
            .field("configured", &self.configured)
            .field("history_valid", &self.history_valid)
            .field("prepared", &self.prepared)
            .finish()
    }
}

impl Stage1 {
    /// Creates the root signature, descriptor heap, LUT and pipelines (ports `Stage1::initialize`).
    pub fn initialize(device: &D3d12Device) -> Result<Self, GeneratorError> {
        let root_signature = create_root_signature(device)?;
        let heap = device.create_descriptor_heap(HEAP_SLOTS)?;
        let increment = device.descriptor_increment();

        // The LUT is a 128x128 FLOAT atlas uploaded once. `LUT_TEXELS * 4` bytes.
        let lut = device.create_upload_buffer(LUT_BYTES.len() as u64)?;
        debug_assert_eq!(LUT_BYTES.len(), LUT_TEXELS as usize * 4);
        device.write_buffer(&lut, LUT_BYTES)?;

        let pipelines = Pipelines::compile(device, &root_signature)?;

        Ok(Self {
            device: device.clone(),
            pipelines: Some(pipelines),
            root_signature: Some(root_signature),
            heap: Some(heap),
            lut: Some(lut),
            increment,
            params: FrameParams::default(),
            configured: false,
            history_valid: false,
            prepared: false,
            current: 0,
            composite_weight: 0.0,
            alpha: MIDPOINT_ALPHA,
            depth: Default::default(),
            motion: Default::default(),
            color: Default::default(),
            reprojection: Target::default(),
            filled: Target::default(),
            midpoint: Target::default(),
        })
    }

    /// Applies the frame geometry; recreates targets on a size change. Refused while a prepare
    /// snapshot is unconsumed.
    pub fn configure(&mut self, params: &FrameParams) -> Result<(), GeneratorError> {
        if !self.ready() {
            return Err(GeneratorError::NotInitialized);
        }
        // Do not replace targets for an unconsumed snapshot.
        if self.prepared {
            return Err(GeneratorError::SnapshotOutstanding);
        }
        params.validate()?;

        let resize = !self.configured
            || params.render_width != self.params.render_width
            || params.render_height != self.params.render_height
            || params.display_width != self.params.display_width
            || params.display_height != self.params.display_height;

        self.params = *params;
        if resize {
            self.create_targets()?;
            self.current = 0;
        }
        self.configured = true;
        Ok(())
    }

    fn create_targets(&mut self) -> Result<(), GeneratorError> {
        self.release_targets();

        let uav = TextureFlags::ALLOW_UNORDERED_ACCESS;
        let read_only = TextureFlags::NONE;
        let (rw, rh) = (self.params.render_width, self.params.render_height);
        let (dw, dh) = (self.params.display_width, self.params.display_height);
        let color = PixelFormat::from(self.params.color_format);

        for index in 0..2 {
            self.depth[index] = create_target(&self.device, PixelFormat::R32Float, rw, rh, uav)?;
            self.motion[index] = create_target(&self.device, PixelFormat::R16G16Float, rw, rh, uav)?;
            self.color[index] = create_target(&self.device, color, dw, dh, read_only)?;
        }
        self.reprojection = create_target(&self.device, PixelFormat::R32Uint, rw, rh, uav)?;
        self.filled = create_target(&self.device, PixelFormat::R32Uint, rw, rh, uav)?;
        // The warp writes the midpoint here and the composite pass finishes it, so HUD composition
        // never has to run inside the fusion pass.
        self.midpoint = create_target(&self.device, color, dw, dh, uav)?;
        Ok(())
    }

    fn transition(&self, list: &D3d12CommandList, target: &Target, after: ResourceState) {
        let Some(texture) = target.get() else { return };
        if target.state() == after {
            return;
        }
        list.transition(texture, target.state(), after);
        target.state.set(after);
    }

    fn transition_external(
        &self,
        list: &D3d12CommandList,
        texture: &D3d12Texture,
        before: ResourceState,
        after: ResourceState,
    ) {
        if before == after {
            return;
        }
        list.transition(texture, before, after);
    }

    fn uav_barrier(&self, list: &D3d12CommandList, target: &Target) {
        if let Some(texture) = target.get() {
            list.uav_barrier(texture);
        }
    }

    /// Copies one target to another and leaves both in NON_PIXEL_SHADER_RESOURCE, as the C++ does.
    fn copy_target(&self, list: &D3d12CommandList, source: &Target, destination: &Target) {
        let (Some(source_texture), Some(destination_texture)) = (source.get(), destination.get())
        else {
            return;
        };
        self.transition(list, source, ResourceState::COPY_SOURCE);
        self.transition(list, destination, ResourceState::COPY_DEST);
        list.copy_resource(destination_texture, source_texture);
        self.transition(list, destination, ResourceState::NON_PIXEL_SHADER_RESOURCE);
        self.transition(list, source, ResourceState::NON_PIXEL_SHADER_RESOURCE);
    }

    fn blocks(&self) -> DescriptorBlocks<'_> {
        DescriptorBlocks::new(
            self.device.raw(),
            self.heap.as_ref().expect("heap exists once initialized"),
            self.increment,
        )
    }

    /// Binds the heap, root signature and pipeline, writes the root constants (with the HUD weight)
    /// and dispatches. Ports `Stage1::dispatch`.
    #[allow(clippy::too_many_arguments)]
    fn dispatch(
        &self,
        list: &D3d12CommandList,
        pass: Stage1Pass,
        state: &ID3D12RootSignature,
        heap: &ID3D12DescriptorHeap,
        pipeline: &windows::Win32::Graphics::Direct3D12::ID3D12PipelineState,
        block: u32,
        groups_x: u32,
        groups_y: u32,
        mark: &mut Option<PassMark<'_>>,
    ) {
        let mut params = self.params;
        params.alpha = self.alpha;
        let words = constant_words(&params.constants(), self.composite_weight);

        let srv = self.blocks().gpu_srv_table(block);
        let uav = self.blocks().gpu_uav_table(block);

        // SAFETY: the list is open and recording; every object bound here outlives the recording
        // and the descriptors referenced by the tables are written before this call.
        unsafe {
            list.raw().SetDescriptorHeaps(&[Some(heap.clone())]);
            list.raw().SetComputeRootSignature(state);
            list.raw().SetPipelineState(pipeline);
            list.raw().SetComputeRoot32BitConstants(
                0,
                pipelines::ROOT_CONSTANTS,
                words.as_ptr().cast(),
                0,
            );
            list.raw().SetComputeRootDescriptorTable(1, srv);
            list.raw().SetComputeRootDescriptorTable(2, uav);
        }
        list.dispatch(groups_x, groups_y, 1);
        if let Some(hook) = mark {
            hook(pass);
        }
    }

    /// Records the prepare pass on the game's command list (ports `Stage1::capture_prepare`).
    fn capture_prepare(
        &mut self,
        list: &D3d12CommandList,
        frame: &InputFrame<'_, D3d12Texture>,
        mark: &mut Option<PassMark<'_>>,
    ) -> Result<(), GeneratorError> {
        if !self.configured || self.pipelines.is_none() {
            return Err(GeneratorError::NotConfigured);
        }
        let (Some(depth), Some(motion)) = (frame.depth, frame.motion) else {
            return Err(GeneratorError::PrepareRejected);
        };
        if self.prepared {
            return Err(GeneratorError::PrepareRejected);
        }
        for texture in [depth, motion] {
            let info = texture.info();
            if !info.is_single_sample_2d()
                || info.width != self.params.render_width
                || info.height != self.params.render_height
            {
                return Err(GeneratorError::PrepareRejected);
            }
        }

        let current = self.current as usize;
        let depth_target = &self.depth[current];
        let motion_target = &self.motion[current];

        self.transition_external(
            list,
            depth,
            frame.depth_state,
            ResourceState::NON_PIXEL_SHADER_RESOURCE,
        );
        self.transition_external(
            list,
            motion,
            frame.motion_state,
            ResourceState::NON_PIXEL_SHADER_RESOURCE,
        );
        self.transition(list, depth_target, ResourceState::UNORDERED_ACCESS);
        self.transition(list, motion_target, ResourceState::UNORDERED_ACCESS);

        let (Some(depth_target_texture), Some(motion_target_texture)) =
            (depth_target.resource(), motion_target.resource())
        else {
            return Err(GeneratorError::PrepareRejected);
        };

        let mut blocks = self.blocks();
        blocks.begin(0);
        blocks.write_srv(0, depth.raw(), frame.depth_format);
        blocks.write_srv(1, motion.raw(), frame.motion_format);
        blocks.write_uav(0, depth_target_texture, PixelFormat::R32Float);
        blocks.write_uav(1, motion_target_texture, PixelFormat::R16G16Float);

        // Borrow the long-lived objects rather than AddRef/Release them on every generated frame.
        let state = self.root_signature.as_ref().ok_or(GeneratorError::NotInitialized)?;
        let heap = self.heap.as_ref().ok_or(GeneratorError::NotInitialized)?;
        let prepare = &self.pipelines.as_ref().ok_or(GeneratorError::NotInitialized)?.prepare.state;
        self.dispatch(
            list,
            Stage1Pass::Prepare,
            state,
            heap,
            prepare,
            0,
            groups(self.params.render_width),
            groups(self.params.render_height),
            mark,
        );

        self.uav_barrier(list, depth_target);
        self.uav_barrier(list, motion_target);
        self.transition(list, depth_target, ResourceState::NON_PIXEL_SHADER_RESOURCE);
        self.transition(list, motion_target, ResourceState::NON_PIXEL_SHADER_RESOURCE);
        self.transition_external(
            list,
            depth,
            ResourceState::NON_PIXEL_SHADER_RESOURCE,
            frame.depth_state,
        );
        self.transition_external(
            list,
            motion,
            ResourceState::NON_PIXEL_SHADER_RESOURCE,
            frame.motion_state,
        );
        self.prepared = true;
        Ok(())
    }

    /// Produces one HUD-less analytical phase into `midpoint`. HUD composition is a separate
    /// recording step so a future correction stage can run at this boundary.
    #[allow(clippy::too_many_arguments)]
    fn synthesize(
        &mut self,
        list: &D3d12CommandList,
        current: usize,
        previous: usize,
        alpha: f32,
        block: u32,
        mark: &mut Option<PassMark<'_>>,
    ) -> Result<(), GeneratorError> {
        self.alpha = alpha;
        let state = self.root_signature.as_ref().ok_or(GeneratorError::NotInitialized)?;
        let heap = self.heap.as_ref().ok_or(GeneratorError::NotInitialized)?;
        let pipelines = self.pipelines.as_ref().ok_or(GeneratorError::NotInitialized)?;
        let color = PixelFormat::from(self.params.color_format);
        let (gw, gh) = (groups(self.params.render_width), groups(self.params.render_height));

        // 3. Clear, splat, fill.
        self.transition(list, &self.reprojection, ResourceState::UNORDERED_ACCESS);
        let mut blocks = self.blocks();
        blocks.begin(block);
        blocks.write_uav(
            0,
            self.reprojection.resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R32Uint,
        );
        self.dispatch(
            list,
            Stage1Pass::Clear,
            state,
            heap,
            &pipelines.clear.state,
            block,
            gw,
            gh,
            mark,
        );
        self.uav_barrier(list, &self.reprojection);

        let mut blocks = self.blocks();
        blocks.begin(block + 1);
        blocks.write_srv(
            0,
            self.depth[current].resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R32Float,
        );
        blocks.write_srv(
            1,
            self.motion[current].resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R16G16Float,
        );
        blocks.write_srv(
            2,
            self.motion[previous].resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R16G16Float,
        );
        blocks.write_uav(
            0,
            self.reprojection.resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R32Uint,
        );
        self.dispatch(
            list,
            Stage1Pass::Reproject,
            state,
            heap,
            &pipelines.reproject.state,
            block + 1,
            gw,
            gh,
            mark,
        );
        self.uav_barrier(list, &self.reprojection);
        self.transition(list, &self.reprojection, ResourceState::NON_PIXEL_SHADER_RESOURCE);

        self.transition(list, &self.filled, ResourceState::UNORDERED_ACCESS);
        let mut blocks = self.blocks();
        blocks.begin(block + 2);
        blocks.write_srv(
            0,
            self.reprojection.resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R32Uint,
        );
        blocks.write_uav(
            0,
            self.filled.resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R32Uint,
        );
        self.dispatch(
            list,
            Stage1Pass::Fill,
            state,
            heap,
            &pipelines.fill.state,
            block + 2,
            gw,
            gh,
            mark,
        );
        self.uav_barrier(list, &self.filled);
        self.transition(list, &self.filled, ResourceState::NON_PIXEL_SHADER_RESOURCE);

        // 4. Warp both real frames to the midpoint and fuse into a private target.
        self.transition(list, &self.midpoint, ResourceState::UNORDERED_ACCESS);
        let mut blocks = self.blocks();
        blocks.begin(block + 3);
        blocks.write_lut_srv(
            self.lut.as_ref().map(D3d12Buffer::raw).ok_or(GeneratorError::NotInitialized)?,
        );
        blocks.write_srv(
            0,
            self.filled.resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R32Uint,
        );
        blocks.write_srv(
            1,
            self.color[current].resource().ok_or(GeneratorError::NotConfigured)?,
            color,
        );
        blocks.write_srv(
            2,
            self.color[previous].resource().ok_or(GeneratorError::NotConfigured)?,
            color,
        );
        blocks.write_srv(
            3,
            self.depth[current].resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R32Float,
        );
        blocks.write_srv(
            4,
            self.depth[previous].resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R32Float,
        );
        blocks.write_srv(
            5,
            self.motion[current].resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R16G16Float,
        );
        blocks.write_srv(
            6,
            self.motion[previous].resource().ok_or(GeneratorError::NotConfigured)?,
            PixelFormat::R16G16Float,
        );
        blocks.write_uav(
            0,
            self.midpoint.resource().ok_or(GeneratorError::NotConfigured)?,
            color,
        );
        self.dispatch(
            list,
            Stage1Pass::Warp,
            state,
            heap,
            &pipelines.warp.state,
            block + 3,
            groups(self.params.display_width),
            groups(self.params.display_height),
            mark,
        );
        self.uav_barrier(list, &self.midpoint);
        self.transition(list, &self.midpoint, ResourceState::NON_PIXEL_SHADER_RESOURCE);
        Ok(())
    }

    /// Composes the HUD layer onto the analytical midpoint. Without a HUD source this is a copy.
    fn composite_hud(
        &mut self,
        list: &D3d12CommandList,
        current: usize,
        phase: &GeneratePhase<'_, D3d12Texture>,
        hud: Option<HudSource<'_, D3d12Texture>>,
        block: u32,
        mark: &mut Option<PassMark<'_>>,
    ) -> Result<(), GeneratorError> {
        let state = self.root_signature.as_ref().ok_or(GeneratorError::NotInitialized)?;
        let heap = self.heap.as_ref().ok_or(GeneratorError::NotInitialized)?;
        let pipelines = self.pipelines.as_ref().ok_or(GeneratorError::NotInitialized)?;
        let color = PixelFormat::from(self.params.color_format);

        let hud_texture = hud.as_ref().map(|source| source.real);
        self.composite_weight = if hud_texture.is_some() { 1.0 } else { 0.0 };
        if let Some(source) = &hud {
            self.transition_external(
                list,
                source.real,
                source.real_state,
                ResourceState::NON_PIXEL_SHADER_RESOURCE,
            );
        }
        self.transition_external(
            list,
            phase.output,
            phase.output_state,
            ResourceState::UNORDERED_ACCESS,
        );

        let midpoint = self.midpoint.resource().ok_or(GeneratorError::NotConfigured)?;
        let hud_resource = hud_texture.map(D3d12Texture::raw).unwrap_or(midpoint);
        let mut blocks = self.blocks();
        blocks.begin(block);
        blocks.write_srv(0, midpoint, color);
        blocks.write_srv(1, hud_resource, color);
        blocks.write_srv(
            2,
            self.color[current].resource().ok_or(GeneratorError::NotConfigured)?,
            color,
        );
        blocks.write_uav(0, phase.output.raw(), color);
        self.dispatch(
            list,
            Stage1Pass::Composite,
            state,
            heap,
            &pipelines.composite.state,
            block,
            groups(self.params.display_width),
            groups(self.params.display_height),
            mark,
        );

        self.transition_external(
            list,
            phase.output,
            ResourceState::UNORDERED_ACCESS,
            phase.output_state,
        );
        if let Some(source) = &hud {
            self.transition_external(
                list,
                source.real,
                ResourceState::NON_PIXEL_SHADER_RESOURCE,
                source.real_state,
            );
        }
        self.composite_weight = 0.0;
        Ok(())
    }

    /// The core of `generate`: validates the frame, then either seeds history (K53) or runs one
    /// synthesis per phase.
    fn generate_impl(
        &mut self,
        list: &D3d12CommandList,
        frame: &InputFrame<'_, D3d12Texture>,
        phases: &[GeneratePhase<'_, D3d12Texture>],
        reset_history: bool,
        hud: Option<HudSource<'_, D3d12Texture>>,
        mark: &mut Option<PassMark<'_>>,
    ) -> Result<(), GeneratorError> {
        if !self.configured || self.pipelines.is_none() {
            return Err(GeneratorError::NotConfigured);
        }
        let Some(color) = frame.color else {
            return Err(GeneratorError::FrameRejected("generate without a colour resource"));
        };
        if phases.is_empty() || phases.len() > MAX_PHASES as usize {
            return Err(GeneratorError::FrameRejected("generate with an unsupported phase count"));
        }

        let display = (self.params.display_width, self.params.display_height);
        if !matches_size(color.info(), display.0, display.1) {
            return Err(GeneratorError::FrameRejected("colour resource has the wrong geometry"));
        }
        if color.info().format != PixelFormat::from(self.params.color_format) {
            return Err(GeneratorError::FrameRejected("colour resource has the wrong format"));
        }

        for (index, phase) in phases.iter().enumerate() {
            let info = phase.output.info();
            if !matches_size(info, display.0, display.1) {
                return Err(GeneratorError::FrameRejected("output has the wrong geometry"));
            }
            if info.format != PixelFormat::from(self.params.color_format)
                || !info.flags.contains(TextureFlags::ALLOW_UNORDERED_ACCESS)
            {
                return Err(GeneratorError::FrameRejected("output cannot be written as a UAV"));
            }
            let identity = phase.output.identity();
            if [frame.color, frame.depth, frame.motion]
                .iter()
                .flatten()
                .any(|texture| texture.identity() == identity)
            {
                return Err(GeneratorError::FrameRejected("output aliases an input"));
            }
            if hud.as_ref().is_some_and(|source| source.real.identity() == identity) {
                return Err(GeneratorError::FrameRejected("output aliases the HUD source"));
            }
            if !(phase.alpha > 0.0 && phase.alpha < 1.0) {
                return Err(GeneratorError::FrameRejected("phase alpha outside (0, 1)"));
            }
            if phases[..index].iter().any(|earlier| earlier.output.identity() == identity) {
                return Err(GeneratorError::FrameRejected("two phases share an output"));
            }
        }
        if let Some(source) = &hud {
            let info = source.real.info();
            if !matches_size(info, display.0, display.1)
                || !info.is_single_sample_2d()
                || info.format != PixelFormat::from(self.params.color_format)
            {
                return Err(GeneratorError::FrameRejected("HUD source has the wrong shape or format"));
            }
        }

        if !self.prepared {
            self.capture_prepare(list, frame, mark)?;
        }

        let current = self.current as usize;
        let previous = current ^ 1;

        // 1. Keep a private copy of the HUD-less color: the game's resource is recycled as soon as
        //    the frame has been consumed.
        self.transition_external(list, color, frame.color_state, ResourceState::COPY_SOURCE);
        self.transition(list, &self.color[current], ResourceState::COPY_DEST);
        list.copy_resource(
            self.color[current].get().ok_or(GeneratorError::NotConfigured)?,
            color,
        );
        self.transition(list, &self.color[current], ResourceState::NON_PIXEL_SHADER_RESOURCE);
        self.transition_external(list, color, ResourceState::COPY_SOURCE, frame.color_state);

        if reset_history || !self.history_valid {
            self.copy_target(list, &self.depth[current], &self.depth[previous]);
            self.copy_target(list, &self.motion[current], &self.motion[previous]);
            self.copy_target(list, &self.color[current], &self.color[previous]);
            // There is no temporal pair yet. Seed history and emit the real frame; warping two
            // copies of it would manufacture motion on a reset. With a HUD source the game's own
            // frame is the exact frame to show, so the seed is not HUD-less (K53).
            self.transition(list, &self.color[current], ResourceState::COPY_SOURCE);
            for phase in phases {
                self.transition_external(
                    list,
                    phase.output,
                    phase.output_state,
                    ResourceState::COPY_DEST,
                );
                match &hud {
                    Some(source) => {
                        self.transition_external(
                            list,
                            source.real,
                            source.real_state,
                            ResourceState::COPY_SOURCE,
                        );
                        list.copy_resource(phase.output, source.real);
                        self.transition_external(
                            list,
                            source.real,
                            ResourceState::COPY_SOURCE,
                            source.real_state,
                        );
                    }
                    None => list.copy_resource(
                        phase.output,
                        self.color[current].get().ok_or(GeneratorError::NotConfigured)?,
                    ),
                }
                self.transition_external(
                    list,
                    phase.output,
                    ResourceState::COPY_DEST,
                    phase.output_state,
                );
            }
            self.transition(list, &self.color[current], ResourceState::NON_PIXEL_SHADER_RESOURCE);
            self.current = previous as u32;
            self.history_valid = true;
            self.prepared = false;
            return Ok(());
        }

        // 2. One synthesis per phase. Each owns its descriptor blocks and its alpha in the root
        //    constants; the scratch targets are reused in order, which the transitions inside
        //    serialise on the GPU.
        for (index, phase) in phases.iter().enumerate() {
            let block = PREPARE_BLOCKS + PHASE_BLOCKS * index as u32;
            self.synthesize(list, current, previous, phase.alpha, block, mark)?;
            self.composite_hud(list, current, phase, hud.as_ref().copied(), block + 4, mark)?;
        }
        self.alpha = self.params.alpha;
        self.current = previous as u32;
        self.history_valid = true;
        self.prepared = false;
        Ok(())
    }
}

fn matches_size(info: TextureInfo, width: u32, height: u32) -> bool {
    info.is_single_sample_2d() && info.width == width && info.height == height
}

impl FrameGenerator<D3d12Device> for Stage1 {
    type List = D3d12CommandList;

    fn create(device: &D3d12Device) -> Result<Self, GeneratorError> {
        Self::initialize(device)
    }

    fn configure(&mut self, params: &FrameParams) -> Result<(), GeneratorError> {
        Stage1::configure(self, params)
    }

    fn prepare(
        &mut self,
        list: &Self::List,
        frame: &InputFrame<'_, D3d12Texture>,
    ) -> Result<(), GeneratorError> {
        self.capture_prepare(list, frame, &mut None)
    }

    fn discard_prepare(&mut self) {
        self.prepared = false;
        self.history_valid = false;
    }

    fn generate(
        &mut self,
        list: &Self::List,
        frame: &InputFrame<'_, D3d12Texture>,
        phases: &[GeneratePhase<'_, D3d12Texture>],
        reset_history: bool,
        hud: Option<HudSource<'_, D3d12Texture>>,
        pass_mark: Option<PassMark<'_>>,
    ) -> Result<(), GeneratorError> {
        let mut mark = pass_mark;
        self.generate_impl(list, frame, phases, reset_history, hud, &mut mark)
    }

    fn params(&self) -> &FrameParams {
        &self.params
    }

    fn ready(&self) -> bool {
        self.pipelines.is_some() && self.root_signature.is_some() && self.heap.is_some()
    }

    fn release_targets(&mut self) {
        self.depth = Default::default();
        self.motion = Default::default();
        self.color = Default::default();
        self.reprojection = Target::default();
        self.filled = Target::default();
        self.midpoint = Target::default();
        self.configured = false;
        self.history_valid = false;
        self.prepared = false;
    }

    fn pending(&self) -> bool {
        self.prepared
    }
}
