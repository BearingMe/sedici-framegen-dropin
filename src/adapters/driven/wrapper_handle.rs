//! The session's swapchain handle and its generation source (ports `runtime/integration.cpp`).
//!
//! The application core's [`crate::application::session`] and the presentation slice's
//! [`Swapchain`] were written against two different seams:
//!
//! * [`crate::application::session::Wrapper`] speaks in FFX-facing handles and owner keys.
//! * [`PresentedContext`] speaks in the wrapper's GPU types.
//!
//! [`WrapperHandle`] and [`SessionDriven`] are the one place that knows both. `WrapperHandle` is
//! the session's view of a wrapped chain; `SessionDriven` is the wrapper's generation source over
//! a session-owned [`crate::application::context::ContextHandle`]. They are the analogue of the
//! C++ `WrapHandle` and `GenerateSink` in one file, because a wrapper needs both to present a
//! generated frame.
//!
//! # Retirement
//!
//! The session's sink can register a submission only against a [`Retirement`] probe, and the
//! wrapper owns its queue fence as a `D::Fence`. The composition root builds a probe over the same
//! queue timeline (`D3d12Retirement`) and installs it on [`SessionDriven`]; without one, admission
//! stays closed (K37) rather than claiming a token the context never saw.
use std::sync::Arc;
use std::time::Duration;

use crate::application::context::{
    ContextHandle, PhaseSpec, Retirement,
};
use crate::application::ffx_model::{self, OpaqueHandle, Resource, ResourceDescription, ResourceKind, SurfaceFormat};
use crate::application::ports::swapchain::NativeSwapchain;
use crate::application::ports::gpu::{
    CommandListHandle, GpuCommandList, GpuDevice, GpuTexture, ResourceState,
};
use crate::application::presentation::presenter::ListLifecycle;
use crate::application::presentation::swapchain::{
    PresentedContext, Swapchain,
};
use crate::application::session::{Wrapper, WrapOrigin};
use crate::application::ports::generator::GeneratePhase;

/// `FFX_API_RESOURCE_STATE_UNORDERED_ACCESS` (`1 << 1`).
pub const FFX_RESOURCE_STATE_UNORDERED_ACCESS: u32 = 1 << 1;

/// The state the wrapper's generated images are created in (`m_generated_state` in the C++, and the
/// per-phase `D3D12_RESOURCE_STATE_COPY_SOURCE` on the "vrr" path). The generator restores what it
/// is given, so this is also the state a session context is told to expect.
pub const GENERATED_OUTPUT_STATE: ResourceState = ResourceState::COPY_SOURCE;

/// A frame generation session's generation source, over the wrapper's device types.
///
/// Every call reaches the session through its [`ContextHandle`]; the textures and lists the
/// wrapper hands over are named by identity, and the adapter that made them resolves those
/// identities back when the context asks. The wrapper's own `D` types are carried only so the
/// wrapper can create its private buffers and command lists on the same device.
pub struct SessionDriven<D: GpuDevice> {
    context: Arc<dyn ContextHandle>,
    retirement: Option<Arc<dyn Retirement>>,
    _device: std::marker::PhantomData<D>,
}

impl<D: GpuDevice> SessionDriven<D> {
    pub fn new(context: Arc<dyn ContextHandle>, retirement: Option<Arc<dyn Retirement>>) -> Self {
        Self { context, retirement, _device: std::marker::PhantomData }
    }
}

impl<D> PresentedContext for SessionDriven<D>
where
    D: GpuDevice + Send + Sync + 'static,
    D::Texture: GpuTexture + Send + Sync,
    D::CommandList: CommandListHandle,
{
    type Texture = D::Texture;
    type List = D::CommandList;

    fn prepared(&self) -> bool {
        self.context.prepared()
    }

    fn generate_phases(
        &self,
        list: &Self::List,
        real: &Self::Texture,
        phases: &[GeneratePhase<'_, Self::Texture>],
        reset_history: bool,
    ) -> bool {
        if reset_history {
            self.context.abandoned();
        }
        let specs: Vec<PhaseSpec> = phases
            .iter()
            .map(|phase| PhaseSpec {
                output: phase.output.identity() as OpaqueHandle,
                output_state: phase.output_state,
                alpha: phase.alpha,
            })
            .collect();
        self.context.record_phases(
            list.object_handle() as OpaqueHandle,
            real.identity() as OpaqueHandle,
            &specs,
        )
    }

    fn note_ordered_submission(&self) {
        // A session sink notes ordering when it records (`ContextSink::record_phases`), before the
        // generator ever sees the list; calling `record_phases` would generate, so nothing is done.
    }

    fn track_submission(&self, fence_value: u64) -> bool {
        match &self.retirement {
            Some(retirement) => self.context.submitted(Arc::clone(retirement), fence_value),
            None => false,
        }
    }

    fn abandon_generation(&self) {
        self.context.abandoned();
    }

    fn has_untracked_work(&self) -> bool {
        self.context.needs_submission()
    }

    fn wait_for_retirement(&self, timeout: Duration) -> bool {
        self.context.wait_for_retirement(timeout)
    }

    fn output_state(&self) -> ResourceState {
        GENERATED_OUTPUT_STATE
    }
}

/// The session's handle over one wrapped chain.
///
/// The same bounds the [`Swapchain`] itself needs; they are repeated because a struct bound cannot
/// be written as a type alias in Rust.
pub struct WrapperHandle<D, N>
where
    D: GpuDevice + Send + Sync + 'static,
    D::Texture: GpuTexture + Clone + Send + Sync + 'static,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator>
        + GpuCommandList<D::Texture>
        + CommandListHandle
        + Send
        + Sync
        + 'static,
    D::CommandAllocator: Send + Sync + 'static,
    D::CommandList: Clone,
    D::Fence: Send + Sync + 'static,
    D::Queue: Send + Sync + 'static,
    N: NativeSwapchain<Buffer = D::Texture> + Send + Sync + 'static,
{
    swapchain: Arc<
        Swapchain<D, N, SessionDriven<D>, crate::adapters::driven::overlay_ui::OverlayHost>,
    >,
}

impl<D, N> WrapperHandle<D, N>
where
    D: GpuDevice + Send + Sync + 'static,
    D::Texture: GpuTexture + Clone + Send + Sync + 'static,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator>
        + GpuCommandList<D::Texture>
        + CommandListHandle
        + Send
        + Sync
        + 'static,
    D::CommandAllocator: Send + Sync + 'static,
    D::CommandList: Clone,
    D::Fence: Send + Sync + 'static,
    D::Queue: Send + Sync + 'static,
    N: NativeSwapchain<Buffer = D::Texture> + Send + Sync + 'static,
{
    pub fn new(
        swapchain: Arc<
            Swapchain<D, N, SessionDriven<D>, crate::adapters::driven::overlay_ui::OverlayHost>,
        >,
        chain_handle: usize,
    ) -> Self {
        let _ = chain_handle;
        Self { swapchain }
    }

    pub fn inner(&self) -> &Arc<
        Swapchain<D, N, SessionDriven<D>, crate::adapters::driven::overlay_ui::OverlayHost>,
    > {
        &self.swapchain
    }
}

impl<D, N> Wrapper for WrapperHandle<D, N>
where
    D: GpuDevice + Send + Sync + 'static,
    D::Texture: GpuTexture + Clone + Send + Sync + 'static,
    D::CommandList: ListLifecycle<Allocator = D::CommandAllocator>
        + GpuCommandList<D::Texture>
        + CommandListHandle
        + Send
        + Sync
        + 'static,
    D::CommandAllocator: Send + Sync + 'static,
    D::CommandList: Clone,
    D::Fence: Send + Sync + 'static,
    D::Queue: Send + Sync + 'static,
    N: NativeSwapchain<Buffer = D::Texture> + Send + Sync + 'static,
{
    fn set_generator(&self, owner: usize, sink: Arc<dyn ContextHandle>) -> bool {
        // The wrapper's generation source is a `SessionDriven` over this sink. The retirement probe
        // was installed when the wrapper was built, so it is carried through.
        let driven = Arc::new(SessionDriven::<D>::new(sink, self.swapchain.sink_retirement()));
        self.swapchain.set_generator(Some(
            crate::application::presentation::swapchain::GeneratorSink {
                context: driven,
                owner,
                lease: Some(Arc::new(owner)),
            },
        ))
    }

    fn clear_generator_if_owner(&self, owner: usize) -> bool {
        self.swapchain.clear_generator_if_owner(owner)
    }

    fn close_generator(&self) {
        self.swapchain.close_generator()
    }

    fn interpolation_list(&self) -> Option<OpaqueHandle> {
        self.swapchain
            .interpolation_list()
            .ok()
            .map(|list| list.object_handle() as OpaqueHandle)
    }

    fn interpolation_texture(&self) -> Option<Resource> {
        let texture = self.swapchain.interpolation_texture().ok()?;
        let info = texture.info();
        Some(Resource {
            handle: texture.identity(),
            description: texture_resource(&info),
            state: FFX_RESOURCE_STATE_UNORDERED_ACCESS,
        })
    }
}

/// The FFX resource description of a wrapper texture, as the log and the ABI edge print it.
fn texture_resource(
    info: &crate::application::ports::gpu::TextureInfo,
) -> ResourceDescription {
    ResourceDescription {
        kind: ResourceKind::Texture2D,
        // `FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM` is the enum value 10 and
        // `R10G10B10A2_UNORM` is 17; `ColorFormat` owns that mapping.
        format: SurfaceFormat(
            crate::domain::frame::ColorFormat::from(info.format).to_ffx_surface_format(),
        ),
        width: info.width,
        height: info.height,
        depth: 1,
        mip_count: u32::from(info.mip_levels),
        flags: info.flags.0,
        usage: 0,
    }
}

/// The origin a swapchain create form names, from the create desc.
pub fn origin_of(desc: &ffx_model::CreateContextDesc) -> WrapOrigin {
    if desc.swapchain_wrap.is_some() {
        WrapOrigin::Wrapped
    } else {
        WrapOrigin::Created
    }
}
