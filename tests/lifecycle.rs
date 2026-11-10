//! The Alan Wake 2 shape and the repeated-lifecycle scenarios, ported from `fgcheck_routing.cpp`
//! (the "P4.8" block) and driven through the **production** router, swapchain factory, wrapper and
//! Stage 1, not fakes.
//!
//! The C++ scenarios this ports:
//!
//! * the New form builds a 10-bit chain Sedici owns, and the HWND form builds one too; a null
//!   factory is a local error that consumes nothing;
//! * the interpolation list and texture the game asks for are answered, the list stays recordable
//!   across asks, and other swapchain queries stay refused (K131);
//! * a 10-bit HDR-flagged session is accepted and the game's own generation dispatch is
//!   acknowledged;
//! * memory growth over repeated create/query/destroy lifecycles stays small (the C++ bound was
//!   4 MiB of private bytes and 16 MiB of video memory over 40 cycles after 5 warm-up cycles);
//! * the shared present queue count is 0 -> 1 -> 0 across one presenter's life (K140).
//!
//! K138 (the interpolation queries not waiting behind a running Present) is covered at the wrapper
//! level with fakes in `tests/presentation.rs`, where the present path can be held on demand.
//!
//! Everything here requires a real D3D12 adapter and a real hidden window; without either the test
//! skips cleanly with a message, exactly like the smoke test. The game is never launched.
#![allow(unsafe_code)]

use std::sync::Arc;

use windows::core::Interface;
use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory2};

use amd_fidelityfx_framegeneration_dx12::adapters::driven::backend::build_service;
use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::device::D3d12Device;
use amd_fidelityfx_framegeneration_dx12::adapters::driven::dxgi_native::DxgiNative;
use amd_fidelityfx_framegeneration_dx12::application::faults::Faults;
use amd_fidelityfx_framegeneration_dx12::application::ffx_model::{
    self, BackendDx12, CommandListQuery, CreateContextDesc, Dimensions, FfxService,
    FrameGenerationCreate, QueryDesc, QueryReply, SurfaceFormat, SwapchainDesc, SwapchainDesc1,
    SwapchainForHwnd, SwapchainNew, TextureQuery,
};
use amd_fidelityfx_framegeneration_dx12::application::ports::clock::FakeClock;
use amd_fidelityfx_framegeneration_dx12::application::ports::overlay::NoOverlay;
use amd_fidelityfx_framegeneration_dx12::application::presentation::presenter::Presenter;
use amd_fidelityfx_framegeneration_dx12::application::presentation::swapchain::SharedPresentQueues;
use amd_fidelityfx_framegeneration_dx12::domain::config::TearGuard;

const WIDTH: u32 = 480;
const HEIGHT: u32 = 270;

/// A hidden window plus the D3D12 device, queue and factory the game would own.
struct Fixture {
    window: windows::Win32::Foundation::HWND,
    device: windows::Win32::Graphics::Direct3D12::ID3D12Device,
    queue: windows::Win32::Graphics::Direct3D12::ID3D12CommandQueue,
    factory: IDXGIFactory2,
    instance: windows::Win32::Foundation::HINSTANCE,
}

impl Drop for Fixture {
    fn drop(&mut self) {
        use windows::Win32::UI::WindowsAndMessaging::{DestroyWindow, UnregisterClassW};
        // SAFETY: every handle belongs to this test; a fixture is dropped once.
        unsafe {
            let _ = DestroyWindow(self.window);
            let _ = UnregisterClassW(windows::core::w!("SediciRustLifecycleWindow"), Some(self.instance));
        }
    }
}

extern "system" fn default_proc(
    hwnd: windows::Win32::Foundation::HWND,
    msg: u32,
    wparam: windows::Win32::Foundation::WPARAM,
    lparam: windows::Win32::Foundation::LPARAM,
) -> windows::Win32::Foundation::LRESULT {
    // SAFETY: the standard default window procedure for a window this test owns.
    unsafe { windows::Win32::UI::WindowsAndMessaging::DefWindowProcW(hwnd, msg, wparam, lparam) }
}

fn fixture() -> Option<Fixture> {
    use windows::Win32::Foundation::HINSTANCE;
    use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
    use windows::Win32::Graphics::Direct3D12::{
        D3D12CreateDevice, ID3D12CommandQueue, ID3D12Device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_QUEUE_DESC, D3D12_COMMAND_QUEUE_FLAG_NONE,
    };
    use windows::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows::Win32::UI::WindowsAndMessaging::{
        CreateWindowExW, RegisterClassW, CW_USEDEFAULT, WS_POPUP, WINDOW_EX_STYLE, WINDOW_STYLE,
        WNDCLASSW,
    };

    // SAFETY: every call uses initialised locals and checks its result; a failure returns `None`,
    // which skips the test instead of failing it.
    unsafe {
        let mut device: Option<ID3D12Device> = None;
        if D3D12CreateDevice(None::<&windows::core::IUnknown>, D3D_FEATURE_LEVEL_11_0, &mut device)
            .is_err()
        {
            eprintln!("lifecycle: D3D12CreateDevice failed");
            return None;
        }
        let device = device?;
        let queue = match device.CreateCommandQueue::<ID3D12CommandQueue>(&D3D12_COMMAND_QUEUE_DESC {
            Type: D3D12_COMMAND_LIST_TYPE_DIRECT,
            Priority: 0,
            Flags: D3D12_COMMAND_QUEUE_FLAG_NONE,
            NodeMask: 0,
        }) {
            Ok(queue) => queue,
            Err(error) => {
                eprintln!("lifecycle: CreateCommandQueue failed {error:?}");
                return None;
            }
        };
        let factory: IDXGIFactory2 = match CreateDXGIFactory1() {
            Ok(factory) => factory,
            Err(error) => {
                eprintln!("lifecycle: CreateDXGIFactory1 failed {error:?}");
                return None;
            }
        };
        let instance = match GetModuleHandleW(None) {
            Ok(instance) => instance,
            Err(error) => {
                eprintln!("lifecycle: GetModuleHandleW failed {error:?}");
                return None;
            }
        };
        let class = WNDCLASSW {
            lpfnWndProc: Some(default_proc),
            hInstance: HINSTANCE(instance.0),
            lpszClassName: windows::core::w!("SediciRustLifecycleWindow"),
            ..Default::default()
        };
        RegisterClassW(&class);
        let window = match CreateWindowExW(
            WINDOW_EX_STYLE(0),
            windows::core::w!("SediciRustLifecycleWindow"),
            windows::core::w!("Sedici lifecycle"),
            WINDOW_STYLE(WS_POPUP.0),
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            320,
            180,
            None,
            None,
            Some(HINSTANCE(instance.0)),
            None,
        ) {
            Ok(window) => window,
            Err(error) => {
                eprintln!("lifecycle: CreateWindowExW failed {error:?}");
                return None;
            }
        };
        Some(Fixture { window, device, queue, factory, instance: HINSTANCE(instance.0) })
    }
}

/// `DXGI_FORMAT_R10G10B10A2_UNORM` = 24; ditto the desc1 form.
fn new_desc(window: windows::Win32::Foundation::HWND) -> SwapchainDesc {
    SwapchainDesc {
        buffer_count: 2,
        width: WIDTH,
        height: HEIGHT,
        format: 24,
        refresh_numerator: 60,
        refresh_denominator: 1,
        // `DXGI_USAGE_RENDER_TARGET_OUTPUT`.
        usage: 0x20,
        // `DXGI_SWAP_EFFECT_FLIP_DISCARD`.
        swap_effect: 4,
        // `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`. The C++ also set ALLOW_MODE_SWITCH, which this
        // test desktop's windowed chain refuses (measured: E_INVALIDARG from CreateSwapChain), so
        // the fixture keeps the one flag DXGI accepts here; the flag set is not what the scenario
        // checks.
        flags: 0x2,
        windowed: true,
        output_window: window.0 as usize,
    }
}

fn hwnd_desc() -> SwapchainDesc1 {
    SwapchainDesc1 {
        width: WIDTH,
        height: HEIGHT,
        format: 24,
        stereo: false,
        sample_count: 1,
        sample_quality: 0,
        usage: 0x20,
        buffer_count: 2,
        scaling: 0,
        swap_effect: 4,
        alpha_mode: 0,
        flags: 0,
    }
}

fn new_create(fixture: &Fixture) -> CreateContextDesc {
    // A real stack slot the router would write through the ABI edge; the router itself returns the
    // handle, so the test reads it from `CreatedContext::swapchain`.
    let mut slot: usize = 0;
    let slot = &mut slot as *mut usize;
    CreateContextDesc {
        swapchain_new: Some(SwapchainNew {
            swapchain_slot: slot as usize,
            desc: Some(new_desc(fixture.window)),
            dxgi_factory: Interface::as_raw(&fixture.factory) as usize,
            game_queue: Interface::as_raw(&fixture.queue) as usize,
        }),
        ..Default::default()
    }
}

fn hwnd_create(fixture: &Fixture, factory: usize) -> CreateContextDesc {
    let mut slot: usize = 0;
    let slot = &mut slot as *mut usize;
    CreateContextDesc {
        swapchain_for_hwnd: Some(SwapchainForHwnd {
            swapchain_slot: slot as usize,
            hwnd: fixture.window.0 as usize,
            desc: Some(hwnd_desc()),
            fullscreen_desc: 0,
            dxgi_factory: factory,
            game_queue: Interface::as_raw(&fixture.queue) as usize,
        }),
        ..Default::default()
    }
}

fn ten_bit_frame_generation() -> FrameGenerationCreate {
    FrameGenerationCreate {
        // HDR | async workloads, exactly the AW2 create flags.
        flags: (1 << 5) | (1 << 0),
        display_size: Dimensions { width: WIDTH, height: HEIGHT },
        max_render_size: Dimensions { width: WIDTH, height: HEIGHT },
        back_buffer_format: SurfaceFormat(17), // FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM
    }
}

/// The New form builds a 10-bit chain, the interpolation queries are answered, and the destroy
/// leaves the device healthy. This is the whole AW2 create flow, through the real factory.
#[test]
fn the_new_form_builds_a_ten_bit_chain_and_answers_the_interpolation_queries() {
    let Some(fixture) = fixture() else {
        eprintln!("SKIP lifecycle: no D3D12 adapter or hidden window available");
        return;
    };
    let service = build_service(Arc::new(Faults::new())).expect("production service");
    let created = match service.create(&new_create(&fixture)) {
        Ok(created) => created,
        Err(rejection) => {
            // A windowless CI desktop can refuse the chain; that is the host, not Sedici.
            eprintln!("SKIP lifecycle: the factory refused the chain ({rejection})");
            return;
        }
    };
    assert_ne!(created.context, 0, "a context token was issued");
    let handle = created.swapchain.expect("the game receives a chain handle");
    assert_ne!(handle, 0, "the chain handle is not null");
    // SAFETY: the create produced one reference to the wrapper COM object; adopting and dropping
    // it releases exactly that reference.
    let chain = unsafe {
        windows::Win32::Graphics::Dxgi::IDXGISwapChain4::from_raw(handle as *mut std::ffi::c_void)
    };
    // SAFETY: a pure query on the chain this test owns.
    let desc: windows::Win32::Graphics::Dxgi::DXGI_SWAP_CHAIN_DESC1 =
        unsafe { chain.GetDesc1() }.expect("GetDesc1");
    assert_eq!(desc.Format.0, 24, "the New form built an R10G10B10A2 chain");

    // K131: the interpolation scratch queries, through the router.
    let mut list_slot: usize = 0;
    let first_list = match service
        .query(
            Some(created.context),
            &QueryDesc {
                interpolation_command_list: Some(CommandListQuery {
                    output_slot: &mut list_slot as *mut usize as usize,
                }),
                ..Default::default()
            },
        )
        .expect("interpolation list")
    {
        QueryReply::CommandList(handle) => handle,
        other => panic!("interpolation list: {other:?}"),
    };
    assert_ne!(first_list, 0, "the game receives a command list");
    // Asking again answers the same recordable list, not a closed one.
    let mut again_slot: usize = 0;
    match service
        .query(
            Some(created.context),
            &QueryDesc {
                interpolation_command_list: Some(CommandListQuery {
                    output_slot: &mut again_slot as *mut usize as usize,
                }),
                ..Default::default()
            },
        )
        .expect("interpolation list again")
    {
        QueryReply::CommandList(handle) => assert_eq!(handle, first_list, "the same list is returned"),
        other => panic!("second list: {other:?}"),
    }
    // The interpolation list handle is borrowed: the wrapper owns the list for the game, so the
    // test reads it with `from_raw_borrowed` and never releases it (a `from_raw` would take a
    // reference and a later drop would double-release it). The list was left open by the wrapper's
    // reset, which is what "recordable" means.
    // SAFETY: a live list the wrapper created and keeps open across this query.
    let raw_list: *mut std::ffi::c_void = first_list as *mut _;
    let list = unsafe {
        windows::Win32::Graphics::Direct3D12::ID3D12GraphicsCommandList::from_raw_borrowed(&raw_list)
    }
    .expect("the list handle is live");
    // SAFETY: `GetDevice` is a pure query on that live list, so a broken handle would be caught
    // here rather than put a reference behind the wrapper's back.
    let mut owner: Option<windows::Win32::Graphics::Direct3D12::ID3D12Device> = None;
    unsafe { list.GetDevice(&mut owner) }.expect("the list handle is a live command list");

    let mut texture_slot: usize = 0;
    match service
        .query(
            Some(created.context),
            &QueryDesc {
                interpolation_texture: Some(TextureQuery {
                    output_slot: &mut texture_slot as *mut usize as usize,
                }),
                ..Default::default()
            },
        )
        .expect("interpolation texture")
    {
        QueryReply::Texture(resource) => {
            assert_ne!(resource.handle, 0, "the game receives an interpolation texture");
            assert_eq!(resource.description.width, WIDTH);
            assert_eq!(resource.description.height, HEIGHT);
            assert_eq!(resource.description.format, SurfaceFormat(17), "the texture is 10-bit");
        }
        other => panic!("interpolation texture: {other:?}"),
    }
    // Anything else stays a refusal.
    assert!(
        service
            .query(
                Some(created.context),
                &QueryDesc { swapchain_gpu_memory_usage: Some(Default::default()), ..Default::default() },
            )
            .is_err(),
        "other swapchain queries stay refused"
    );

    assert!(service.destroy(created.context).is_ok(), "destroy");
    // SAFETY: a pure query on the test's live device.
    let removal = unsafe { fixture.device.GetDeviceRemovedReason() }.map_err(|error| error.message());
    assert_eq!(removal, Ok(()), "the device stays healthy");
    drop(chain);
}

/// Memory over repeated lifecycles: the AW2 shape (a New chain, the two interpolation queries, a
/// 10-bit HDR session, destroy) repeated. The C++ allowed 4 MiB of private bytes over 40 cycles
/// after 5 warm-up cycles and 16 MiB of video memory; this reads the same two counters.
#[test]
fn repeated_alan_wake_2_lifecycles_do_not_grow_memory() {
    let Some(fixture) = fixture() else {
        eprintln!("SKIP lifecycle: no D3D12 adapter or hidden window available");
        return;
    };
    let service: std::sync::Arc<dyn FfxService> =
        std::sync::Arc::from(build_service(Arc::new(Faults::new())).expect("production service"));

    let cycle = |service: &dyn FfxService| -> Result<(), String> {
        let created = service
            .create(&new_create(&fixture))
            .map_err(|rejection| format!("create: {rejection}"))?;
        let handle = created.swapchain.ok_or_else(|| "no chain handle".to_owned())?;
        // SAFETY: the create produced one reference; adopting and dropping it releases that one.
        let chain = unsafe {
            windows::Win32::Graphics::Dxgi::IDXGISwapChain4::from_raw(handle as *mut std::ffi::c_void)
        };
        let mut list_slot: usize = 0;
        let mut texture_slot: usize = 0;
        service
            .query(
                Some(created.context),
                &QueryDesc {
                    interpolation_command_list: Some(CommandListQuery {
                        output_slot: &mut list_slot as *mut usize as usize,
                    }),
                    ..Default::default()
                },
            )
            .map_err(|rejection| format!("list query: {rejection}"))?;
        service
            .query(
                Some(created.context),
                &QueryDesc {
                    interpolation_texture: Some(TextureQuery {
                        output_slot: &mut texture_slot as *mut usize as usize,
                    }),
                    ..Default::default()
                },
            )
            .map_err(|rejection| format!("texture query: {rejection}"))?;
        let session = service
            .create(&CreateContextDesc {
                frame_generation: Some(ten_bit_frame_generation()),
                backend_dx12: Some(BackendDx12 {
                    device: Interface::as_raw(&fixture.device) as usize,
                }),
                ..Default::default()
            })
            .map(|created| created.context)
            .map_err(|rejection| format!("session: {rejection}"))?;
        drop(chain);
        service.destroy(session).map_err(|rejection| format!("session destroy: {rejection}"))?;
        service.destroy(created.context).map_err(|rejection| format!("swapchain destroy: {rejection}"))?;
        Ok(())
    };

    const WARMUP: u32 = 5;
    const MEASURED: u32 = 40;
    for index in 0..WARMUP {
        if let Err(reason) = cycle(service.as_ref()) {
            panic!("warm-up cycle {index}: {reason}");
        }
    }
    let before = private_bytes();
    let adapter = adapter_memory(&fixture);
    let video_before = adapter.as_ref().and_then(video_bytes);
    for index in 0..MEASURED {
        if let Err(reason) = cycle(service.as_ref()) {
            panic!("measured cycle {index}: {reason}");
        }
    }
    let grown = private_bytes() as i64 - before as i64;
    eprintln!("memory over {MEASURED} create/destroy cycles: private bytes grew by {} KiB", grown / 1024);
    assert!(
        grown < 4 * 1024 * 1024,
        "private bytes grew by {grown} over {MEASURED} cycles (limit 4 MiB)"
    );
    match (adapter.as_ref(), video_before) {
        (Some(adapter), Some(before)) => match video_bytes(adapter) {
            Some(after) => {
                let video_grown = after as i64 - before as i64;
                eprintln!("video memory over {MEASURED} cycles: grew by {} KiB", video_grown / 1024);
                assert!(
                    video_grown < 16 * 1024 * 1024,
                    "video memory grew by {video_grown} (limit 16 MiB)"
                );
            }
            None => eprintln!("NOTE video memory query unavailable; GPU growth not checked"),
        },
        _ => eprintln!("NOTE video memory query unavailable; GPU growth not checked"),
    }
}

fn private_bytes() -> u64 {
    use windows::Win32::System::ProcessStatus::{
        GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS, PROCESS_MEMORY_COUNTERS_EX,
    };
    let mut counters = PROCESS_MEMORY_COUNTERS_EX {
        cb: std::mem::size_of::<PROCESS_MEMORY_COUNTERS_EX>() as u32,
        ..Default::default()
    };
    // SAFETY: the out-struct is owned and sized; the call only fills it.
    let ok = unsafe {
        GetProcessMemoryInfo(
            windows::Win32::System::Threading::GetCurrentProcess(),
            &mut counters as *mut PROCESS_MEMORY_COUNTERS_EX as *mut PROCESS_MEMORY_COUNTERS,
            counters.cb,
        )
    }
    .is_ok();
    if ok {
        counters.PrivateUsage as u64
    } else {
        0
    }
}

fn adapter_memory(fixture: &Fixture) -> Option<windows::Win32::Graphics::Dxgi::IDXGIAdapter3> {
    use windows::Win32::Graphics::Dxgi::{IDXGIAdapter3, IDXGIFactory4};
    // SAFETY: a pure query on the test's live device.
    let luid = unsafe { fixture.device.GetAdapterLuid() };
    let factory: IDXGIFactory4 = fixture.factory.cast().ok()?;
    // SAFETY: a pure enum query on the test's factory.
    unsafe { factory.EnumAdapterByLuid::<IDXGIAdapter3>(luid) }.ok()
}

fn video_bytes(adapter: &windows::Win32::Graphics::Dxgi::IDXGIAdapter3) -> Option<u64> {
    use windows::Win32::Graphics::Dxgi::{
        DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_QUERY_VIDEO_MEMORY_INFO,
    };
    let mut info = DXGI_QUERY_VIDEO_MEMORY_INFO::default();
    // SAFETY: the out-struct is owned; the call only fills it.
    unsafe { adapter.QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mut info) }
        .ok()
        .map(|()| info.CurrentUsage)
}

/// K140: the shared present queue is created with the first presenter on a device and released with
/// the last one, so a device recreated by the game is not kept alive forever. The C++ check read
/// `shared_present_queue_count()` before, during and after one presenter's life and required
/// `0 -> 1 -> 0`. This drives a real presenter over a real chain and reads the registry's own count.
#[test]
fn the_shared_present_queue_is_released_with_its_last_presenter() {
    use windows::Win32::Graphics::Dxgi::{
        IDXGISwapChain1, DXGI_SCALING_STRETCH, DXGI_SWAP_CHAIN_DESC1,
        DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_USAGE_RENDER_TARGET_OUTPUT,
    };

    let Some(fixture) = fixture() else {
        eprintln!("SKIP lifecycle: no D3D12 adapter or hidden window available");
        return;
    };
    let host_desc = DXGI_SWAP_CHAIN_DESC1 {
        Width: WIDTH,
        Height: HEIGHT,
        Format: windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_R8G8B8A8_UNORM,
        Stereo: false.into(),
        SampleDesc: windows::Win32::Graphics::Dxgi::Common::DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
        BufferUsage: DXGI_USAGE_RENDER_TARGET_OUTPUT,
        BufferCount: 3,
        Scaling: DXGI_SCALING_STRETCH,
        SwapEffect: DXGI_SWAP_EFFECT_FLIP_DISCARD,
        AlphaMode: Default::default(),
        Flags: 0,
    };
    // SAFETY: the queue and window are the test's live ones; the desc is a fully initialised local.
    let created: windows::core::Result<IDXGISwapChain1> = unsafe {
        fixture.factory.CreateSwapChainForHwnd(
            &fixture.queue,
            fixture.window,
            &host_desc,
            None,
            None::<&windows::Win32::Graphics::Dxgi::IDXGIOutput>,
        )
    };
    let Ok(chain) = created else {
        eprintln!("SKIP lifecycle: the factory refused the chain");
        return;
    };
    let Ok(chain4) = chain.cast::<windows::Win32::Graphics::Dxgi::IDXGISwapChain4>() else {
        eprintln!("SKIP lifecycle: the chain is not IDXGISwapChain4");
        return;
    };

    let device = Arc::new(D3d12Device::from_raw(&fixture.device));
    let native = Arc::new(DxgiNative::for_test(&chain4, &fixture.queue, &fixture.device));
    let queues: Arc<SharedPresentQueues<D3d12Device>> = Arc::new(SharedPresentQueues::new(false));
    let clock = Arc::new(FakeClock::new(1_000_000, 1_000_000));
    let device_key = Arc::as_ptr(&device) as usize;
    let before = queues.len();
    let mut why = String::new();
    let presenter = Presenter::create(
        Arc::clone(&device),
        Arc::clone(&queues),
        device_key,
        Arc::clone(&native),
        clock,
        TearGuard::Off,
        None,
        0.00025,
        Arc::new(NoOverlay),
        &mut why,
    );
    let Some(presenter) = presenter else {
        eprintln!("SKIP lifecycle: presenter unavailable ({why}); present queue release not checked");
        return;
    };
    let during = queues.len();
    drop(presenter);
    let after = queues.len();
    eprintln!("present queue counts: {before} before, {during} with a presenter, {after} after");
    assert_eq!(during, before + 1, "the presenter created one shared queue");
    assert_eq!(after, before, "the last presenter released it");
}

/// A 10-bit HDR-flagged session is accepted and the game's own generation dispatch is acknowledged
/// (the P4.8 half of the C++ block, without a chain).
#[test]
fn a_ten_bit_hdr_session_is_accepted_and_the_game_dispatch_is_acknowledged() {
    let Some(fixture) = fixture() else {
        eprintln!("SKIP lifecycle: no D3D12 adapter or hidden window available");
        return;
    };
    let service = build_service(Arc::new(Faults::new())).expect("production service");
    let created = service
        .create(&CreateContextDesc {
            frame_generation: Some(ten_bit_frame_generation()),
            backend_dx12: Some(BackendDx12 { device: Interface::as_raw(&fixture.device) as usize }),
            ..Default::default()
        })
        .expect("10-bit HDR session");
    let game_dispatch = ffx_model::DispatchDesc {
        root_type: Some(
            amd_fidelityfx_framegeneration_dx12::application::router::DISPATCH_FRAMEGENERATION,
        ),
        ..Default::default()
    };
    assert_eq!(
        service.dispatch(created.context, &game_dispatch),
        Ok(ffx_model::DispatchReply::FrameGenerationAcknowledged)
    );
    assert!(service.destroy(created.context).is_ok(), "destroy");
}

/// The HWND form builds a 10-bit chain; a null factory is a local error that consumes nothing. This
/// is the other half of the C++ P4.8 block.
#[test]
fn the_hwnd_form_builds_a_ten_bit_chain_and_a_null_factory_is_a_local_error() {
    let Some(fixture) = fixture() else {
        eprintln!("SKIP lifecycle: no D3D12 adapter or hidden window available");
        return;
    };
    let service = build_service(Arc::new(Faults::new())).expect("production service");
    let factory = Interface::as_raw(&fixture.factory) as usize;
    let created = match service.create(&hwnd_create(&fixture, factory)) {
        Ok(created) => created,
        Err(rejection) => {
            eprintln!("SKIP lifecycle: the factory refused the HWND chain ({rejection})");
            return;
        }
    };
    let handle = created.swapchain.expect("the game receives a chain handle");
    // SAFETY: the create produced one reference to the wrapper COM object; adopting and dropping
    // it releases exactly that reference.
    let chain = unsafe {
        windows::Win32::Graphics::Dxgi::IDXGISwapChain4::from_raw(handle as *mut std::ffi::c_void)
    };
    // SAFETY: a pure query on the chain this test owns.
    let desc: windows::Win32::Graphics::Dxgi::DXGI_SWAP_CHAIN_DESC1 =
        unsafe { chain.GetDesc1() }.expect("GetDesc1");
    assert_eq!(desc.Format.0, 24, "the HWND form built a 10-bit chain");
    assert!(service.destroy(created.context).is_ok(), "destroy");
    drop(chain);

    // A null factory is a local error that issues no context and no chain.
    let refused = service.create(&hwnd_create(&fixture, 0));
    assert!(refused.is_err(), "a null factory is refused");
}
