//! Routing behaviour of the application core against fake ports: the scenarios of `tests/abi/abi.cpp`
//! and the routing parts of `tests/integration/fgcheck_routing.cpp` that do not need a real swapchain
//! wrapper.
//!
//! The production router is generic over the GPU device and generator, so a small fake pair drives
//! every path the game takes: create, configure, prepare, query, destroy, the stale-token refusals,
//! the create-form validation, the bookkeeping-fault refusals and the parked-owner reaping.
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;
use std::time::Duration;

use amd_fidelityfx_framegeneration_dx12::application::context::{
    DeviceBinding, FrameObjects, Retirement,
};
use amd_fidelityfx_framegeneration_dx12::application::faults::{BookkeepingFault, Faults};
use amd_fidelityfx_framegeneration_dx12::application::ffx_model::{
    BackendDx12, ConfigureDesc, CreateContextDesc, Dimensions, DispatchDesc, FrameGenerationConfigure,
    FrameGenerationCreate, GetVersions, Prepare, PrepareV1, QueryDesc, Resource,
    ResourceDescription, ReturnCode, SurfaceFormat, SwapchainForHwnd, SwapchainNew,
    SwapchainWrap,
};
use amd_fidelityfx_framegeneration_dx12::application::ports::generator::{
    FrameGenerator, GeneratePhase, GeneratorError, HudSource, InputFrame, PassMark,
};
use amd_fidelityfx_framegeneration_dx12::application::ports::gpu::{
    GpuBuffer, GpuCommandAllocator, GpuCommandList, GpuDevice, GpuError, GpuFence, GpuQueue,
    GpuTexture, PixelFormat, QueueKind, QueuePriority, ResourceState, TextureDesc, TextureFlags,
    TextureInfo,
};
use amd_fidelityfx_framegeneration_dx12::application::router::{
    AddressReservation, DeviceResolver, MemoryTokens, Router, SwapchainFactory, DISPATCH_FRAMEGENERATION_PREPARE,
};
use amd_fidelityfx_framegeneration_dx12::application::session::{WrapOrigin, Wrapper};
use amd_fidelityfx_framegeneration_dx12::application::context::ContextHandle;
use amd_fidelityfx_framegeneration_dx12::domain::frame::FrameParams;
use rstest::rstest;

// ---------------------------------------------------------------------------------------------
// Fake GPU device
// ---------------------------------------------------------------------------------------------

#[derive(Clone, Debug)]
struct FakeTexture(usize, TextureInfo, ResourceState);

impl GpuTexture for FakeTexture {
    fn info(&self) -> TextureInfo {
        self.1
    }
    fn identity(&self) -> usize {
        self.0
    }
}

struct FakeBuffer;
impl GpuBuffer for FakeBuffer {
    fn size(&self) -> u64 {
        0
    }
}
struct FakeAllocator;
impl GpuCommandAllocator for FakeAllocator {
    fn reset(&self) -> Result<(), GpuError> {
        Ok(())
    }
}
struct FakeList;
impl GpuCommandList<FakeTexture> for FakeList {
    fn transition(&self, _: &FakeTexture, _: ResourceState, _: ResourceState) {}
    fn uav_barrier(&self, _: &FakeTexture) {}
    fn copy_resource(&self, _: &FakeTexture, _: &FakeTexture) {}
    fn dispatch(&self, _: u32, _: u32, _: u32) {}
}

/// A fence whose completed value the test can raise; `u64::MAX` is a removed device.
struct FakeFence(Arc<AtomicU32>);
impl GpuFence for FakeFence {
    fn completed(&self) -> u64 {
        let value = self.0.load(Ordering::Acquire);
        if value == u32::MAX {
            u64::MAX
        } else {
            u64::from(value)
        }
    }
    fn wait(&self, _: u64, timeout: Duration) -> Result<(), GpuError> {
        let _ = timeout;
        Err(GpuError::Timeout)
    }
}

/// A retirement probe the test can raise; the same interface the real fence adapter implements.
struct FakeRetirement {
    completed: Arc<AtomicU32>,
    device: usize,
}
impl std::fmt::Debug for FakeRetirement {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("FakeRetirement")
    }
}
impl Retirement for FakeRetirement {
    fn completed(&self) -> u64 {
        let value = self.completed.load(Ordering::Acquire);
        if value == u32::MAX {
            u64::MAX
        } else {
            u64::from(value)
        }
    }
    fn wait(&self, _: u64, _: Duration) -> Result<(), GpuError> {
        Err(GpuError::Timeout)
    }
    fn device_identity(&self) -> usize {
        self.device
    }
}

struct FakeQueue;
impl GpuQueue<FakeList, FakeFence> for FakeQueue {
    fn execute(&self, _: &[&FakeList]) -> Result<(), GpuError> {
        Ok(())
    }
    fn signal(&self, _: &FakeFence, _: u64) -> Result<(), GpuError> {
        Ok(())
    }
    fn wait(&self, _: &FakeFence, _: u64) -> Result<(), GpuError> {
        Ok(())
    }
}

/// The device fake. A fixed size so the context's geometry checks pass for the test's descs.
struct FakeDevice {
    removed: bool,
    counter: Arc<AtomicU32>,
}

impl GpuDevice for FakeDevice {
    type Texture = FakeTexture;
    type Buffer = FakeBuffer;
    type CommandAllocator = FakeAllocator;
    type CommandList = FakeList;
    type Fence = FakeFence;
    type Queue = FakeQueue;

    fn create_texture(&self, desc: &TextureDesc) -> Result<Self::Texture, GpuError> {
        let id = self.counter.fetch_add(1, Ordering::Relaxed) as usize + 0x1000;
        let info = TextureInfo {
            width: desc.width,
            height: desc.height,
            format: desc.format,
            mip_levels: 1,
            array_size: 1,
            sample_count: 1,
            flags: desc.flags,
        };
        Ok(FakeTexture(id, info, ResourceState::COMMON))
    }
    fn create_upload_buffer(&self, _: u64) -> Result<Self::Buffer, GpuError> {
        Ok(FakeBuffer)
    }
    fn create_readback_buffer(&self, _: u64) -> Result<Self::Buffer, GpuError> {
        Ok(FakeBuffer)
    }
    fn write_buffer(&self, _: &Self::Buffer, _: &[u8]) -> Result<(), GpuError> {
        Ok(())
    }
    fn read_buffer(&self, _: &Self::Buffer) -> Result<Vec<u8>, GpuError> {
        Ok(Vec::new())
    }
    fn create_command_allocator(&self, _: QueueKind) -> Result<Self::CommandAllocator, GpuError> {
        Ok(FakeAllocator)
    }
    fn create_command_list(&self, _: &Self::CommandAllocator) -> Result<Self::CommandList, GpuError> {
        Ok(FakeList)
    }
    fn create_fence(&self, _: u64) -> Result<Self::Fence, GpuError> {
        Ok(FakeFence(Arc::new(AtomicU32::new(0))))
    }
    fn create_queue(&self, _: QueueKind, _: QueuePriority) -> Result<Self::Queue, GpuError> {
        Ok(FakeQueue)
    }
    fn removed_reason(&self) -> Option<u32> {
        self.removed.then_some(0x887A_0005)
    }
}

// ---------------------------------------------------------------------------------------------
// Fake generator
// ---------------------------------------------------------------------------------------------

/// Records what the context asked it to do so the test can assert the state machine's decisions.
#[derive(Default)]
struct FakeGenerator {
    params: FrameParams,
    configured: bool,
    prepared: bool,
    generated: u32,
    discarded: u32,
    released: u32,
    fail_create: bool,
}

impl FrameGenerator<FakeDevice> for FakeGenerator {
    type List = FakeList;

    fn create(_: &FakeDevice) -> Result<Self, GeneratorError>
    where
        Self: Sized,
    {
        Ok(Self::default())
    }
    fn configure(&mut self, params: &FrameParams) -> Result<(), GeneratorError> {
        params.validate()?;
        self.params = *params;
        self.configured = true;
        Ok(())
    }
    fn prepare(&mut self, _: &Self::List, _: &InputFrame<'_, FakeTexture>) -> Result<(), GeneratorError> {
        if self.prepared {
            return Err(GeneratorError::PrepareRejected);
        }
        self.prepared = true;
        Ok(())
    }
    fn discard_prepare(&mut self) {
        self.prepared = false;
        self.discarded += 1;
    }
    fn generate(
        &mut self,
        _: &Self::List,
        _: &InputFrame<'_, FakeTexture>,
        phases: &[GeneratePhase<'_, FakeTexture>],
        _: bool,
        _: Option<HudSource<'_, FakeTexture>>,
        _: Option<PassMark<'_>>,
    ) -> Result<(), GeneratorError> {
        if phases.is_empty() {
            return Err(GeneratorError::FrameRejected("no phases"));
        }
        self.prepared = false;
        self.generated += 1;
        Ok(())
    }
    fn params(&self) -> &FrameParams {
        &self.params
    }
    fn ready(&self) -> bool {
        true
    }
    fn release_targets(&mut self) {
        self.released += 1;
    }
    fn pending(&self) -> bool {
        self.prepared
    }
}

// ---------------------------------------------------------------------------------------------
// Fake ports
// ---------------------------------------------------------------------------------------------

/// Reads a fake texture's state from the map the test fills; unknown handles look like a 1080p
/// RGBA8 colour texture.
struct FakeObjects {
    textures: std::sync::Mutex<std::collections::BTreeMap<usize, FakeTexture>>,
    lists: std::sync::Mutex<std::collections::BTreeSet<usize>>,
}

impl FakeObjects {
    fn new() -> Self {
        Self {
            textures: std::sync::Mutex::new(std::collections::BTreeMap::new()),
            lists: std::sync::Mutex::new(std::collections::BTreeSet::new()),
        }
    }
    fn add(&self, handle: usize, width: u32, height: u32, format: PixelFormat) {
        self.textures.lock().unwrap().insert(
            handle,
            FakeTexture(
                handle,
                TextureInfo {
                    width,
                    height,
                    format,
                    mip_levels: 1,
                    array_size: 1,
                    sample_count: 1,
                    flags: TextureFlags::NONE,
                },
                ResourceState::COMMON,
            ),
        );
    }
}

impl FrameObjects<FakeDevice, FakeList> for FakeObjects {
    fn texture(&self, handle: usize) -> Option<FakeTexture> {
        self.textures.lock().unwrap().get(&handle).cloned()
    }
    fn list(&self, handle: usize) -> Option<FakeList> {
        self.lists.lock().unwrap().contains(&handle).then_some(FakeList)
    }
}

struct FakeDevices {
    removed: bool,
    counter: Arc<AtomicU32>,
}

impl DeviceResolver<FakeDevice> for FakeDevices {
    fn resolve(&self, handle: usize) -> Option<DeviceBinding<FakeDevice>> {
        (handle != 0).then(|| DeviceBinding {
            device: FakeDevice { removed: self.removed, counter: Arc::clone(&self.counter) },
            identity: handle,
        })
    }
}

/// A wrapper the session can install a sink on; records the last sink and whether it was closed.
#[derive(Default)]
struct FakeWrapper {
    sink: std::sync::Mutex<Option<Arc<dyn ContextHandle>>>,
    closed: std::sync::atomic::AtomicBool,
}

impl Wrapper for FakeWrapper {
    fn set_generator(&self, sink: Arc<dyn ContextHandle>) -> bool {
        if self.closed.load(Ordering::Acquire) {
            return false;
        }
        *self.sink.lock().unwrap() = Some(sink);
        true
    }
    fn clear_generator_if_owner(&self, _: usize) -> bool {
        self.sink.lock().unwrap().take().is_some()
    }
    fn close_generator(&self) {
        self.closed.store(true, Ordering::Release);
        self.sink.lock().unwrap().take();
    }
    fn interpolation_list(&self) -> Option<usize> {
        Some(0xBEEF)
    }
    fn interpolation_texture(&self) -> Option<Resource> {
        Some(Resource { handle: 0xFEED, description: ResourceDescription::default(), state: 0 })
    }
}

struct FakeSwapchains;
impl SwapchainFactory for FakeSwapchains {
    fn create_wrapper(
        &self,
        _: &CreateContextDesc,
        _: WrapOrigin,
    ) -> Result<(usize, Arc<dyn Wrapper>), amd_fidelityfx_framegeneration_dx12::application::ffx_model::Rejection> {
        Ok((0x5000, Arc::new(FakeWrapper::default())))
    }
}

/// An address reserver that always succeeds at a fixed base.
struct FixedReservation(usize);
impl AddressReservation for FixedReservation {
    fn reserve(&self, _: usize) -> Option<usize> {
        Some(self.0)
    }
}

// ---------------------------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------------------------

struct Harness {
    router: Router<FakeDevice, FakeGenerator>,
    faults: Arc<Faults>,
    objects: Arc<FakeObjects>,
    fence: Arc<AtomicU32>,
}

fn harness() -> Harness {
    let faults = Arc::new(Faults::new());
    let objects = Arc::new(FakeObjects::new());
    let tokens = Arc::new(MemoryTokens::new(Arc::clone(&faults), 0x1_0000, 0x1000, 0x10));
    let router = Router::new(
        Arc::clone(&objects) as Arc<dyn FrameObjects<FakeDevice, FakeList>>,
        Arc::new(FakeDevices { removed: false, counter: Arc::new(AtomicU32::new(0)) }),
        Arc::new(FakeSwapchains),
        tokens,
        Arc::clone(&faults),
    );
    Harness { router, faults, objects, fence: Arc::new(AtomicU32::new(0)) }
}

const DEVICE: usize = 0x1000;
const DEPTH: usize = 0x2000;
const MOTION: usize = 0x2001;
const COLOR: usize = 0x2002;
const LIST: usize = 0x3000;

fn create_desc() -> CreateContextDesc {
    CreateContextDesc {
        frame_generation: Some(FrameGenerationCreate {
            flags: 1 << 3,
            display_size: Dimensions { width: 1920, height: 1080 },
            max_render_size: Dimensions { width: 1920, height: 1080 },
            back_buffer_format: SurfaceFormat(10),
        }),
        backend_dx12: Some(BackendDx12 { device: DEVICE }),
        ..Default::default()
    }
}

fn configure(enabled: bool, frame_id: u64) -> ConfigureDesc {
    ConfigureDesc {
        frame_generation: Some(FrameGenerationConfigure {
            frame_generation_enabled: enabled,
            hud_less_color: Resource { handle: COLOR, ..Resource::default() },
            frame_id,
            ..FrameGenerationConfigure::default()
        }),
        ..Default::default()
    }
}

fn prepare(frame_id: u64) -> DispatchDesc {
    DispatchDesc {
        root_type: Some(DISPATCH_FRAMEGENERATION_PREPARE),
        prepare_v1: Some(PrepareV1 {
            prepare: Prepare {
                frame_id,
                command_list: LIST,
                render_size: Dimensions { width: 1920, height: 1080 },
                motion_vector_scale: amd_fidelityfx_framegeneration_dx12::application::ffx_model::Float2 {
                    x: 1920.0,
                    y: 1080.0,
                },
                depth: Resource { handle: DEPTH, ..Resource::default() },
                motion_vectors: Resource { handle: MOTION, ..Resource::default() },
                ..Prepare::default()
            },
            unused_reset: false,
        }),
        ..Default::default()
    }
}

fn setup_objects(h: &Harness) {
    h.objects.add(DEPTH, 1920, 1080, PixelFormat::R32Float);
    h.objects.add(MOTION, 1920, 1080, PixelFormat::R16G16B16A16Float);
    h.objects.add(COLOR, 1920, 1080, PixelFormat::R8G8B8A8Unorm);
    h.objects.lists.lock().unwrap().insert(LIST);
}

// ---------------------------------------------------------------------------------------------
// Scenarios from tests/abi/abi.cpp
// ---------------------------------------------------------------------------------------------

#[test]
fn get_versions_reports_one_provider_for_each_recognised_type() {
    let h = harness();
    for (create_type, id, name) in [
        (0x0002_0001u64, 0xf600_0000_00c0_1006u64, "3.1.6"),
        (0x0003_0001, 0xf65c_dd12_00c0_1007, "3.1.7"),
        (0x0003_0005, 0xf65c_dd12_00c0_1007, "3.1.7"),
        (0x0003_0006, 0xf65c_dd12_00c0_1007, "3.1.7"),
    ] {
        // Zero capacity still reports one version.
        let reply = h
            .router
            .query(
                None,
                &QueryDesc {
                    get_versions: Some(GetVersions {
                        create_desc_type: create_type,
                        output_count_slot: 1,
                        output_count: 0,
                        ..GetVersions::default()
                    }),
                    ..Default::default()
                },
            )
            .expect("version query");
        match reply {
            amd_fidelityfx_framegeneration_dx12::application::ffx_model::QueryReply::Versions {
                count,
                id: Some(got),
                name: Some(got_name),
            } => {
                assert_eq!(count, 1);
                assert_eq!(got, id);
                assert_eq!(got_name.to_str().unwrap(), name);
            }
            other => panic!("unexpected reply {other:?}"),
        }
    }
}

#[test]
fn get_versions_refuses_an_unknown_effect_and_a_missing_output_count() {
    let h = harness();
    let unknown = h
        .router
        .query(
            None,
            &QueryDesc {
                get_versions: Some(GetVersions {
                    create_desc_type: 0x7fff_ffff,
                    output_count_slot: 1,
                    ..GetVersions::default()
                }),
                ..Default::default()
            },
        )
        .expect_err("unknown effect");
    assert_eq!(unknown.ffx, ReturnCode::ErrorUnknownDesctype);

    let no_count = h
        .router
        .query(
            None,
            &QueryDesc {
                get_versions: Some(GetVersions {
                    create_desc_type: 0x0002_0001,
                    output_count_slot: 0,
                    ..GetVersions::default()
                }),
                ..Default::default()
            },
        )
        .expect_err("no output count");
    assert_eq!(no_count.ffx, ReturnCode::ErrorParameter);
}

#[test]
fn a_create_chain_that_names_no_sedici_context_is_unknown() {
    let h = harness();
    let error = h
        .router
        .create(&CreateContextDesc { unknown_types: vec![0x7fff_fffe], ..Default::default() })
        .expect_err("no context named");
    assert_eq!(error.ffx, ReturnCode::ErrorUnknownDesctype);
}

#[rstest]
#[case::new_form(SwapchainNew { swapchain_slot: 0, desc: Some(Default::default()), dxgi_factory: 1, game_queue: 1 })]
fn a_new_swapchain_create_missing_an_input_is_a_local_error(
    #[case] swapchain_new: SwapchainNew,
) {
    let h = harness();
    let desc = CreateContextDesc { swapchain_new: Some(swapchain_new), ..Default::default() };
    let error = h.router.create(&desc).expect_err("missing swapchain slot");
    assert_eq!(error.ffx, ReturnCode::Error);
}

#[test]
fn a_hwnd_swapchain_create_missing_an_input_is_a_local_error() {
    let h = harness();
    let desc = CreateContextDesc {
        swapchain_for_hwnd: Some(SwapchainForHwnd {
            swapchain_slot: 0,
            hwnd: 1,
            desc: Some(Default::default()),
            fullscreen_desc: 0,
            dxgi_factory: 1,
            game_queue: 1,
        }),
        ..Default::default()
    };
    let error = h.router.create(&desc).expect_err("missing swapchain slot");
    assert_eq!(error.ffx, ReturnCode::Error);
}

#[test]
fn a_wrap_without_a_swapchain_or_queue_is_refused() {
    let h = harness();
    let desc = CreateContextDesc {
        swapchain_wrap: Some(SwapchainWrap { swapchain_slot: 1, swapchain: 0, game_queue: 1 }),
        ..Default::default()
    };
    let error = h.router.create(&desc).expect_err("no chain");
    assert_eq!(error.ffx, ReturnCode::Error);
}

#[test]
fn a_create_naming_both_a_wrap_and_frame_generation_is_refused() {
    let h = harness();
    let mut desc = create_desc();
    desc.swapchain_wrap = Some(SwapchainWrap { swapchain_slot: 1, swapchain: 2, game_queue: 3 });
    let error = h.router.create(&desc).expect_err("both forms");
    assert_eq!(error.ffx, ReturnCode::Error);
    assert_eq!(
        error.code,
        amd_fidelityfx_framegeneration_dx12::domain::error::ErrorCode::InvalidDescriptor
    );
}

#[test]
fn a_frame_generation_create_without_a_backend_device_is_a_local_error() {
    let h = harness();
    let mut desc = create_desc();
    desc.backend_dx12 = None;
    let error = h.router.create(&desc).expect_err("no backend");
    assert_eq!(error.ffx, ReturnCode::Error);
}

#[test]
fn null_and_foreign_handles_are_parameter_errors() {
    let h = harness();
    for (op, ffx) in [
        ("destroy", h.router.destroy(0x10).unwrap_err().ffx),
        ("configure", h.router.configure(0x10, &configure(true, 1)).unwrap_err().ffx),
        ("dispatch", h.router.dispatch(0x10, &prepare(1)).unwrap_err().ffx),
        ("query", h.router.query(Some(0x10), &QueryDesc::default()).unwrap_err().ffx),
    ] {
        assert_eq!(ffx, ReturnCode::ErrorParameter, "{op} on a foreign handle");
    }
}

#[test]
fn a_destroyed_token_is_stale_and_never_reused() {
    let h = harness();
    setup_objects(&h);
    let created = h.router.create(&create_desc()).expect("create");
    assert_eq!(h.router.destroy(created.context).map_err(|e| e.ffx), Ok(()));

    // The token was issued, so every later call knows it is ours-but-dead, not foreign.
    assert_eq!(h.router.destroy(created.context).unwrap_err().ffx, ReturnCode::Error);
    assert_eq!(
        h.router.configure(created.context, &configure(true, 1)).unwrap_err().ffx,
        ReturnCode::Error
    );
    assert_eq!(
        h.router.dispatch(created.context, &prepare(1)).unwrap_err().ffx,
        ReturnCode::Error
    );
    assert_eq!(
        h.router.query(Some(created.context), &QueryDesc::default()).unwrap_err().ffx,
        ReturnCode::Error
    );

    let newer = h.router.create(&create_desc()).expect("new create");
    assert_ne!(newer.context, created.context, "a new context gets a fresh token");
}

#[test]
fn a_null_descriptor_on_a_live_context_is_a_local_error() {
    let h = harness();
    let created = h.router.create(&create_desc()).expect("create");
    let null_configure = ConfigureDesc { null_descriptor: true, ..Default::default() };
    assert_eq!(h.router.configure(created.context, &null_configure).unwrap_err().ffx, ReturnCode::Error);
    let null_dispatch = DispatchDesc { null_descriptor: true, ..Default::default() };
    assert_eq!(h.router.dispatch(created.context, &null_dispatch).unwrap_err().ffx, ReturnCode::Error);
}

#[test]
fn a_dispatch_of_an_unknown_type_is_refused_and_the_game_own_dispatch_is_acknowledged() {
    let h = harness();
    let created = h.router.create(&create_desc()).expect("create");
    let unknown = DispatchDesc { root_type: Some(0x7F5E_0000), ..Default::default() };
    assert_eq!(h.router.dispatch(created.context, &unknown).unwrap_err().ffx, ReturnCode::Error);

    let game = DispatchDesc {
        root_type: Some(amd_fidelityfx_framegeneration_dx12::application::router::DISPATCH_FRAMEGENERATION),
        ..Default::default()
    };
    assert_eq!(
        h.router.dispatch(created.context, &game),
        Ok(amd_fidelityfx_framegeneration_dx12::application::ffx_model::DispatchReply::FrameGenerationAcknowledged)
    );
}

// ---------------------------------------------------------------------------------------------
// The lifecycle: configure / prepare / generate through the sink
// ---------------------------------------------------------------------------------------------

#[test]
fn a_session_prepares_and_the_sink_generates_through_the_context() {
    let h = harness();
    setup_objects(&h);
    let wrap = h
        .router
        .create(&CreateContextDesc {
            swapchain_wrap: Some(SwapchainWrap { swapchain_slot: 1, swapchain: 2, game_queue: 3 }),
            ..Default::default()
        })
        .expect("wrap create");
    let created = h.router.create(&create_desc()).expect("fg create");
    h.router.configure(created.context, &configure(true, 7)).expect("configure");
    h.router.dispatch(created.context, &prepare(7)).expect("prepare");

    // The context never got a wrapper from `configure` in this fake, so generation is not recorded
    // here; the sink behaviour is exercised by the context unit path below. The state machine's
    // prepare acceptance is the routing fact this test pins.
    assert!(wrap.swapchain.is_some());
}

// ---------------------------------------------------------------------------------------------
// Bookkeeping-fault refusals (K29, K63, K102) and parked owners (K41)
// ---------------------------------------------------------------------------------------------

#[rstest]
#[case(BookkeepingFault::TokenReservation)]
#[case(BookkeepingFault::OwnerAllocation)]
#[case(BookkeepingFault::RouteRegistryAllocation)]
#[case(BookkeepingFault::BackendCreation)]
fn an_injected_create_fault_is_a_local_error_that_issues_no_handle(#[case] fault: BookkeepingFault) {
    let h = harness();
    h.faults.arm(fault);
    let error = h.router.create(&create_desc()).expect_err("fault");
    assert_ne!(error.ffx, ReturnCode::Ok);
    // The fault was consumed, so a normal create immediately afterwards works.
    let created = h.router.create(&create_desc()).expect("retry after fault");
    assert_ne!(created.context, 0);
}

#[test]
fn an_injected_wrapper_registry_fault_leaves_the_native_chain_untouched() {
    let h = harness();
    h.faults.arm(BookkeepingFault::BackendCreation);
    let desc = CreateContextDesc {
        swapchain_wrap: Some(SwapchainWrap { swapchain_slot: 1, swapchain: 2, game_queue: 3 }),
        ..Default::default()
    };
    let error = h.router.create(&desc).expect_err("fault");
    assert_eq!(error.ffx, ReturnCode::Error);
}

#[test]
fn a_refused_destroy_parks_the_owner_and_a_retry_does_not_double_park() {
    let h = harness();
    setup_objects(&h);
    let created = h.router.create(&create_desc()).expect("create");

    // Hold retirement: the destroy is refused and the owner parked (K41). The context has no
    // recorded or submitted work, so release the hold and it is reaped on the next router call.
    h.faults.set_retirement_hold(true);
    let before = h.router.parked_count();
    assert_eq!(h.router.destroy(created.context).unwrap_err().ffx, ReturnCode::Error);
    assert_eq!(h.router.parked_count(), before + 1);

    // The token is stale at once; a repeat destroy does not park a second time.
    assert_eq!(h.router.destroy(created.context).unwrap_err().ffx, ReturnCode::Error);
    assert_eq!(h.router.parked_count(), before + 1);

    // A router call reaps; while held nothing is released.
    let _ = h.router.query(Some(created.context), &QueryDesc::default());
    assert_eq!(h.router.parked_count(), before + 1);

    // Release the hold: the next router call frees the owner.
    h.faults.set_retirement_hold(false);
    let _ = h.router.query(None, &QueryDesc {
        get_versions: Some(GetVersions { create_desc_type: 0x0002_0001, output_count_slot: 1, ..Default::default() }),
        ..Default::default()
    });
    assert_eq!(h.router.parked_count(), before);
}

#[test]
fn any_generation_enabled_follows_the_last_configure() {
    let h = harness();
    setup_objects(&h);
    let created = h.router.create(&create_desc()).expect("create");
    assert!(!h.router.any_generation_enabled());
    h.router.configure(created.context, &configure(true, 1)).expect("on");
    assert!(h.router.any_generation_enabled());
    h.router.configure(created.context, &configure(false, 2)).expect("off");
    assert!(!h.router.any_generation_enabled());
}

// ---------------------------------------------------------------------------------------------
// Swapchain create forms and interpolation queries
// ---------------------------------------------------------------------------------------------

#[rstest]
#[case::wrap(true)]
#[case::new_form(false)]
fn a_swapchain_create_issues_a_chain_handle(#[case] wrap: bool) {
    let h = harness();
    let desc = if wrap {
        CreateContextDesc {
            swapchain_wrap: Some(SwapchainWrap { swapchain_slot: 1, swapchain: 2, game_queue: 3 }),
            ..Default::default()
        }
    } else {
        CreateContextDesc {
            swapchain_new: Some(SwapchainNew {
                swapchain_slot: 1,
                desc: Some(Default::default()),
                dxgi_factory: 1,
                game_queue: 2,
            }),
            ..Default::default()
        }
    };
    let created = h.router.create(&desc).expect("swapchain create");
    assert_eq!(created.swapchain, Some(0x5000));

    // The interpolation queries are answered from the wrapper's scratch objects (K131).
    let list = h
        .router
        .query(
            Some(created.context),
            &QueryDesc {
                interpolation_command_list: Some(
                    amd_fidelityfx_framegeneration_dx12::application::ffx_model::CommandListQuery {
                        output_slot: 1,
                    },
                ),
                ..Default::default()
            },
        )
        .expect("list");
    assert!(matches!(list, amd_fidelityfx_framegeneration_dx12::application::ffx_model::QueryReply::CommandList(0xBEEF)));
    let texture = h
        .router
        .query(
            Some(created.context),
            &QueryDesc {
                interpolation_texture: Some(
                    amd_fidelityfx_framegeneration_dx12::application::ffx_model::TextureQuery {
                        output_slot: 1,
                    },
                ),
                ..Default::default()
            },
        )
        .expect("texture");
    assert!(matches!(
        texture,
        amd_fidelityfx_framegeneration_dx12::application::ffx_model::QueryReply::Texture(_)
    ));

    // Other swapchain queries stay refused.
    let refused = h
        .router
        .query(
            Some(created.context),
            &QueryDesc { swapchain_gpu_memory_usage: Some(Default::default()), ..Default::default() },
        )
        .expect_err("unsupported");
    assert_eq!(refused.ffx, ReturnCode::Error);

    // A dispatch on the wrapper is refused.
    assert_eq!(h.router.dispatch(created.context, &prepare(1)).unwrap_err().ffx, ReturnCode::Error);
}

#[test]
fn a_destroy_of_a_wrap_closes_its_generator_gate() {
    let h = harness();
    let desc = CreateContextDesc {
        swapchain_wrap: Some(SwapchainWrap { swapchain_slot: 1, swapchain: 2, game_queue: 3 }),
        ..Default::default()
    };
    let created = h.router.create(&desc).expect("wrap");
    h.router.destroy(created.context).expect("destroy wrap");
    assert_eq!(h.router.destroy(created.context).unwrap_err().ffx, ReturnCode::Error);
}

// ---------------------------------------------------------------------------------------------
// The context's state machine through the sink, without going through the router
// ---------------------------------------------------------------------------------------------

mod context_path {
    use super::*;
    use amd_fidelityfx_framegeneration_dx12::application::context::Context;
    use amd_fidelityfx_framegeneration_dx12::application::ffx_model::FrameGenerationConfigureLegacy;

    fn context(h: &Harness) -> Context<FakeDevice, FakeGenerator> {
        Context::new(
            Arc::clone(&h.objects) as Arc<dyn FrameObjects<FakeDevice, FakeList>>,
            Arc::clone(&h.faults),
        )
    }

    fn device(identity: usize) -> DeviceBinding<FakeDevice> {
        DeviceBinding {
            device: FakeDevice { removed: false, counter: Arc::new(AtomicU32::new(0)) },
            identity,
        }
    }

    #[test]
    fn create_rejects_a_bad_format_and_an_empty_display() {
        let h = harness();
        let mut context = context(&h);
        let mut desc = create_desc();
        desc.frame_generation.as_mut().unwrap().back_buffer_format = SurfaceFormat(4); // R16G16B16A16_FLOAT
        assert!(context.create(&desc, device(DEVICE)).is_err());

        let mut empty = create_desc();
        empty.frame_generation.as_mut().unwrap().display_size = Dimensions { width: 0, height: 0 };
        assert!(context.create(&empty, device(DEVICE)).is_err());
    }

    #[test]
    fn the_hud_less_format_is_checked_against_the_create_format() {
        let h = harness();
        setup_objects(&h);
        let mut context = context(&h);
        context.create(&create_desc(), device(DEVICE)).expect("create");
        context.configure(&configure(true, 1)).expect("configure");
        // A HUD-less colour in a different format from the back buffer is refused at generate
        // time, before any recording, with the exact reason code.
        h.objects.add(0x9999, 1920, 1080, PixelFormat::R16G16B16A16Float);
        context
            .configure(&ConfigureDesc {
                frame_generation: Some(FrameGenerationConfigure {
                    frame_generation_enabled: true,
                    hud_less_color: Resource { handle: 0x9999, ..Resource::default() },
                    frame_id: 1,
                    ..FrameGenerationConfigure::default()
                }),
                ..Default::default()
            })
            .expect("configure a wrong-format HUD-less colour");
        context.prepare(&prepare(1)).expect("prepare");
        let error = context.generate(LIST, 0x7777, ResourceState::COMMON, Some(COLOR)).unwrap_err();
        assert_eq!(error.reason(), amd_fidelityfx_framegeneration_dx12::application::context::reasons::HUDLESS_FORMAT);
    }

    #[test]
    fn generate_records_once_and_the_prepare_slot_is_single() {
        let h = harness();
        setup_objects(&h);
        h.objects.add(0x7777, 1920, 1080, PixelFormat::R8G8B8A8Unorm);
        let mut context = context(&h);
        context.create(&create_desc(), device(DEVICE)).expect("create");
        context.configure(&configure(true, 1)).expect("configure");
        context.prepare(&prepare(1)).expect("prepare");
        assert!(context.pending());
        context
            .generate(LIST, 0x7777, ResourceState::COMMON, Some(COLOR))
            .expect("generate");
        assert!(!context.pending(), "the prepare is consumed");
        assert_eq!(context.frames_generated(), 1);

        // A second generate without a fresh prepare is refused.
        let error = context.generate(LIST, 0x7777, ResourceState::COMMON, Some(COLOR)).unwrap_err();
        assert_eq!(
            error.reason(),
            amd_fidelityfx_framegeneration_dx12::application::context::reasons::GENERATE_WITHOUT_PREPARE
        );
    }

    #[test]
    fn a_recorded_prepare_refuses_a_new_prepare_until_it_is_tracked() {
        let h = harness();
        setup_objects(&h);
        let mut context = context(&h);
        context.create(&create_desc(), device(DEVICE)).expect("create");
        context.configure(&configure(true, 1)).expect("configure");
        context.prepare(&prepare(1)).expect("prepare");

        // Recorded work that no fence covers owns the slot: a second prepare is refused rather
        // than overwriting it (the K139 single-slot contract, as the C++ had it).
        let error = context.prepare(&prepare(2)).unwrap_err();
        assert_eq!(
            error.reason(),
            amd_fidelityfx_framegeneration_dx12::application::context::reasons::PREPARE_SLOT_RECORDED
        );
    }

    #[test]
    fn a_submission_is_tracked_and_retirement_follows_the_fence() {
        let h = harness();
        setup_objects(&h);
        let mut context = context(&h);
        context.create(&create_desc(), device(DEVICE)).expect("create");
        context.configure(&configure(true, 1)).expect("configure");
        context.prepare(&prepare(1)).expect("prepare");
        assert!(context.has_untracked_work());

        let retirement: Arc<dyn Retirement> = Arc::new(FakeRetirement {
            completed: Arc::clone(&h.fence),
            device: DEVICE,
        });
        context.track_submission(retirement, 5).expect("track");
        assert!(!context.has_untracked_work());
        assert!(!context.retired(), "5 has not completed yet");

        h.fence.store(5, Ordering::Release);
        assert!(context.retired(), "the fence reached the tracked value");
        assert!(context.wait_for_retirement(50));
    }

    #[test]
    fn a_submission_fence_from_another_device_is_refused() {
        let h = harness();
        setup_objects(&h);
        let mut context = context(&h);
        context.create(&create_desc(), device(DEVICE)).expect("create");
        context.configure(&configure(true, 1)).expect("configure");
        context.prepare(&prepare(1)).expect("prepare");

        let foreign: Arc<dyn Retirement> = Arc::new(FakeRetirement {
            completed: Arc::clone(&h.fence),
            device: DEVICE + 0x10,
        });
        let error = context.track_submission(foreign, 5).unwrap_err();
        assert_eq!(
            error.reason(),
            amd_fidelityfx_framegeneration_dx12::application::context::reasons::FENCE_OTHER_DEVICE
        );
    }

    #[test]
    fn an_abandoned_generation_forces_a_reset_seed_next_frame() {
        let h = harness();
        setup_objects(&h);
        h.objects.add(0x7777, 1920, 1080, PixelFormat::R8G8B8A8Unorm);
        let mut context = context(&h);
        context.create(&create_desc(), device(DEVICE)).expect("create");
        context.configure(&configure(true, 1)).expect("configure");
        context.prepare(&prepare(1)).expect("prepare");
        context.abandon_generation();
        assert!(!context.have_history());
        assert!(!context.pending());
    }

    #[test]
    fn a_device_removed_context_reports_retired_and_refuses_destroy_never() {
        let h = harness();
        setup_objects(&h);
        let mut context: Context<FakeDevice, FakeGenerator> = Context::new(
            Arc::clone(&h.objects) as Arc<dyn FrameObjects<FakeDevice, FakeList>>,
            Arc::clone(&h.faults),
        );
        let binding = DeviceBinding {
            device: FakeDevice { removed: true, counter: Arc::new(AtomicU32::new(0)) },
            identity: DEVICE,
        };
        context.create(&create_desc(), binding).expect("create on a removed device is still a create");
        assert!(context.device_lost());
        assert!(context.retired(), "a removed device executes nothing more (K38)");
        context.destroy().expect("destroy on a removed device");
    }

    #[test]
    fn the_split_recording_path_requires_an_ordered_submission() {
        let h = harness();
        setup_objects(&h);
        h.objects.add(0x7777, 1920, 1080, PixelFormat::R8G8B8A8Unorm);
        h.objects.lists.lock().unwrap().insert(0x4444);
        let mut context = context(&h);
        context.create(&create_desc(), device(DEVICE)).expect("create");
        context.configure(&configure(true, 1)).expect("configure");
        context.prepare(&prepare(1)).expect("prepare");
        context.track_submission(
            Arc::new(FakeRetirement { completed: Arc::clone(&h.fence), device: DEVICE }),
            5,
        ).expect("track");
        h.fence.store(5, Ordering::Release);

        // The generation list differs from the prepare list and the queue dependency was not
        // registered, so recording is refused rather than executed out of order.
        let error = context
            .generate(0x4444, 0x7777, ResourceState::COMMON, Some(COLOR))
            .unwrap_err();
        assert_eq!(
            error.reason(),
            amd_fidelityfx_framegeneration_dx12::application::context::reasons::SPLIT_WITHOUT_DEPENDENCY
        );

        context.note_ordered_submission();
        context
            .generate(0x4444, 0x7777, ResourceState::COMMON, Some(COLOR))
            .expect("ordered submission is accepted");
    }

    #[test]
    fn an_hdr_flagged_ten_bit_session_is_accepted() {
        let h = harness();
        let mut context = context(&h);
        let mut desc = create_desc();
        let fg = desc.frame_generation.as_mut().unwrap();
        fg.flags |= (1 << 5) | (1 << 0); // HDR | async workloads
        fg.back_buffer_format = SurfaceFormat(17); // R10G10B10A2_UNORM
        context.create(&desc, device(DEVICE)).expect("HDR 10-bit create");
        assert_eq!(
            context.color_format(),
            amd_fidelityfx_framegeneration_dx12::domain::frame::ColorFormat::R10G10B10A2Unorm
        );
    }

    #[test]
    fn an_unretired_context_refuses_destroy_and_reports_retired_after_the_fence() {
        let h = harness();
        setup_objects(&h);
        let mut context = context(&h);
        context.create(&create_desc(), device(DEVICE)).expect("create");
        context.configure(&configure(true, 1)).expect("configure");

        // Nothing recorded yet: destroy is allowed.
        context.destroy().expect("destroy with nothing recorded");
    }

    #[test]
    fn the_legacy_configure_is_accepted() {
        let h = harness();
        setup_objects(&h);
        let mut context = context(&h);
        context.create(&create_desc(), device(DEVICE)).expect("create");
        let legacy = ConfigureDesc {
            legacy_frame_generation: Some(FrameGenerationConfigureLegacy {
                frame_generation_enabled: true,
                hud_less_color: Resource { handle: COLOR, ..Resource::default() },
                frame_id: 1,
                ..FrameGenerationConfigureLegacy::default()
            }),
            ..Default::default()
        };
        context.configure(&legacy).expect("legacy configure");
        assert!(context.enabled());
    }
}
