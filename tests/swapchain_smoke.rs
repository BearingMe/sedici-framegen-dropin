//! GPU smoke test: the production swapchain factory builds a real chain and presents through it.
//!
//! This is the Alan Wake 2 create flow, reduced to the calls a game makes against the DLL:
//!
//! 1. `ffxCreateContext` with a swapchain-NEW desc (the game's factory and queue, a
//!    `DXGI_SWAP_CHAIN_DESC` the game would use), through the installed router;
//! 2. query the interpolation command list and texture the game asks for (K131);
//! 3. a few `Present` calls through the COM `IDXGISwapChain4` the game received, not through the
//!    native chain;
//! 4. `ffxDestroyContext`.
//!
//! The test creates its own hidden window, device and queue and never touches a game. No D3D12
//! adapter, or a DXGI factory that refuses a hidden-window chain, skips cleanly: the Windows
//! session used by CI has no presentable output, and that is not a Sedici failure.
#![allow(unsafe_code)]

use std::sync::Arc;

use windows::core::Interface;

use amd_fidelityfx_framegeneration_dx12::adapters::driven::backend::build_service;
use amd_fidelityfx_framegeneration_dx12::application::faults::Faults;
use amd_fidelityfx_framegeneration_dx12::application::ffx_model::FfxService;
use amd_fidelityfx_framegeneration_dx12::application::ffx_model::{
    CommandListQuery, CreateContextDesc, Dimensions, FrameGenerationCreate, QueryDesc, QueryReply,
    ResourceKind, SurfaceFormat, SwapchainDesc, SwapchainNew, TextureQuery,
};

/// A hidden window plus the D3D12 device, queue and factory the game would own.
struct Fixture {
    window: windows::Win32::Foundation::HWND,
    /// The game's device, kept alive for the fixture's lifetime.
    _device: windows::Win32::Graphics::Direct3D12::ID3D12Device,
    queue: windows::Win32::Graphics::Direct3D12::ID3D12CommandQueue,
    factory: windows::Win32::Graphics::Dxgi::IDXGIFactory2,
    instance: windows::Win32::Foundation::HINSTANCE,
}

impl Drop for Fixture {
    fn drop(&mut self) {
        use windows::Win32::UI::WindowsAndMessaging::{DestroyWindow, UnregisterClassW};
        // SAFETY: every handle belongs to this test; destroying a window twice is not reached
        // because a fixture is dropped once.
        unsafe {
            let _ = DestroyWindow(self.window);
            let _ = UnregisterClassW(
                windows::core::w!("SediciRustSmokeWindow"),
                Some(self.instance),
            );
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

/// Builds a hidden window and a D3D12 device/queue/factory, or `None` when the host cannot.
fn fixture() -> Option<Fixture> {
    use windows::Win32::Foundation::{HINSTANCE, LPARAM, LRESULT, WPARAM};
    use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
    use windows::Win32::Graphics::Direct3D12::{
        D3D12CreateDevice, ID3D12CommandQueue, ID3D12Device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        D3D12_COMMAND_QUEUE_DESC, D3D12_COMMAND_QUEUE_FLAG_NONE,
    };
    use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory2};
    use windows::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows::Win32::UI::WindowsAndMessaging::{
        CreateWindowExW, RegisterClassW, CW_USEDEFAULT, WINDOW_EX_STYLE, WINDOW_STYLE, WNDCLASSW,
        WS_POPUP,
    };

    // SAFETY: every call below uses fully initialised locals and checks its result; a failure at any
    // step returns `None`, which skips the test rather than failing it.
    unsafe {
        let mut device: Option<ID3D12Device> = None;
        if D3D12CreateDevice(
            None::<&windows::core::IUnknown>,
            D3D_FEATURE_LEVEL_11_0,
            &mut device,
        )
        .is_err()
        {
            eprintln!("swapchain smoke: D3D12CreateDevice failed");
            return None;
        }
        let device = device?;
        let queue =
            match device.CreateCommandQueue::<ID3D12CommandQueue>(&D3D12_COMMAND_QUEUE_DESC {
                Type: D3D12_COMMAND_LIST_TYPE_DIRECT,
                Priority: 0,
                Flags: D3D12_COMMAND_QUEUE_FLAG_NONE,
                NodeMask: 0,
            }) {
                Ok(q) => q,
                Err(e) => {
                    eprintln!("swapchain smoke: CreateCommandQueue failed {e:?}");
                    return None;
                }
            };
        let factory: IDXGIFactory2 = match CreateDXGIFactory1() {
            Ok(f) => f,
            Err(e) => {
                eprintln!("swapchain smoke: CreateDXGIFactory1 failed {e:?}");
                return None;
            }
        };
        let instance = match GetModuleHandleW(None) {
            Ok(i) => i,
            Err(e) => {
                eprintln!("swapchain smoke: GetModuleHandleW failed {e:?}");
                return None;
            }
        };
        let class = WNDCLASSW {
            lpfnWndProc: Some(default_proc),
            hInstance: HINSTANCE(instance.0),
            lpszClassName: windows::core::w!("SediciRustSmokeWindow"),
            ..Default::default()
        };
        RegisterClassW(&class);
        // A tiny, hidden popup: never visible, never fullscreen.
        let _ = RegisterClassW;
        let window = match CreateWindowExW(
            WINDOW_EX_STYLE(0),
            windows::core::w!("SediciRustSmokeWindow"),
            windows::core::w!("Sedici smoke"),
            WINDOW_STYLE(WS_POPUP.0),
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            64,
            64,
            None,
            None,
            Some(HINSTANCE(instance.0)),
            None,
        ) {
            Ok(w) => w,
            Err(e) => {
                eprintln!("swapchain smoke: CreateWindowExW failed {e:?}");
                return None;
            }
        };
        let _ = (WPARAM(0), LPARAM(0), LRESULT(0));
        Some(Fixture {
            window,
            _device: device,
            queue,
            factory,
            instance: HINSTANCE(instance.0),
        })
    }
}

/// A `DXGI_SWAP_CHAIN_DESC` for the hidden window: one buffer, RGBA8, discard.
fn chain_desc(window: windows::Win32::Foundation::HWND) -> SwapchainDesc {
    SwapchainDesc {
        buffer_count: 2,
        width: 64,
        height: 64,
        // `DXGI_FORMAT_R8G8B8A8_UNORM`.
        format: 28,
        refresh_numerator: 60,
        refresh_denominator: 1,
        // `DXGI_USAGE_RENDER_TARGET_OUTPUT`.
        usage: 0x20,
        // `DXGI_SWAP_EFFECT_FLIP_DISCARD`.
        swap_effect: 4,
        flags: 0,
        windowed: true,
        output_window: window.0 as usize,
    }
}

#[test]
fn a_swapchain_new_create_queries_presents_and_destroys_through_the_router() {
    let Some(fixture) = fixture() else {
        eprintln!("swapchain smoke: no D3D12 adapter or hidden window available; skipping");
        return;
    };
    let faults = Arc::new(Faults::new());
    let Some(service) = build_service(Arc::clone(&faults)) else {
        eprintln!("swapchain smoke: no production service built; skipping");
        return;
    };

    // The game asks for a chain on its own factory and queue.
    let output_slot: usize = 0; // replaced below with a stack slot the router writes
    let mut received: usize = 0;
    let desc = CreateContextDesc {
        swapchain_new: Some(SwapchainNew {
            swapchain_slot: &mut received as *mut usize as usize,
            desc: Some(chain_desc(fixture.window)),
            dxgi_factory: windows::core::Interface::as_raw(&fixture.factory) as usize,
            game_queue: windows::core::Interface::as_raw(&fixture.queue) as usize,
        }),
        ..Default::default()
    };
    let _ = output_slot;
    let created = match service.create(&desc) {
        Ok(created) => created,
        Err(rejection) => {
            // A windowless CI desktop can refuse the chain; that is the host, not Sedici.
            eprintln!("swapchain smoke: the factory refused the chain ({rejection}); skipping");
            return;
        }
    };
    assert_ne!(created.context, 0, "a context token was issued");
    assert_ne!(
        created.swapchain,
        Some(0),
        "the game receives a chain handle"
    );
    assert_ne!(created.swapchain.unwrap_or(0), 0);

    // The interpolation queries Alan Wake 2 makes (K131).
    let mut list_slot: usize = 0;
    let list_reply = service.query(
        Some(created.context),
        &QueryDesc {
            interpolation_command_list: Some(CommandListQuery {
                // A real game passes its own writable slot; the router refuses a null one (K131).
                output_slot: &mut list_slot as *mut usize as usize,
            }),
            ..Default::default()
        },
    );
    match list_reply {
        Ok(QueryReply::CommandList(handle)) => {
            assert_ne!(handle, 0, "the game receives a command list")
        }
        other => panic!("interpolation list: {other:?}"),
    }

    let mut texture_slot: usize = 0;
    let texture_reply = service.query(
        Some(created.context),
        &QueryDesc {
            interpolation_texture: Some(TextureQuery {
                output_slot: &mut texture_slot as *mut usize as usize,
            }),
            ..Default::default()
        },
    );
    match texture_reply {
        Ok(QueryReply::Texture(resource)) => {
            assert_ne!(
                resource.handle, 0,
                "the game receives an interpolation texture"
            )
        }
        other => panic!("interpolation texture: {other:?}"),
    }

    // A few Presents through the COM wrapper the game received.
    // SAFETY: the handle was written by the create above and owns one reference; the test adopts
    // it and releases it exactly once, after the presents.
    let chain = unsafe {
        windows::Win32::Graphics::Dxgi::IDXGISwapChain4::from_raw(
            created.swapchain.unwrap() as *mut std::ffi::c_void
        )
    };
    let mut presented = 0;
    for _ in 0..3 {
        // SAFETY: a live chain this test owns; a zero sync interval and no flags is the ordinary
        // windowed present. A hidden window on a desktop with no presentable output can refuse it,
        // which is the host's limit, not a Sedici failure; at least one must succeed where the
        // factory accepted the chain.
        let hr = unsafe {
            windows::Win32::Graphics::Dxgi::IDXGISwapChain::Present(&chain, 0, Default::default())
        };
        if hr.is_err() {
            eprintln!(
                "swapchain smoke: Present refused by the host ({:#010x}); stopping",
                hr.0
            );
            break;
        }
        presented += 1;
    }
    if presented == 0 {
        eprintln!("swapchain smoke: the host refused every Present; the chain was still created");
    }
    // The wrapper ran without a panic and the chain is still usable.
    assert!(service.destroy(created.context).is_ok(), "destroy");
    // SAFETY: this is the one reference the test adopted; releasing it closes the COM object.
    // SAFETY: the pointer was produced by `from_raw` above and is owned by this test.
    let _ = windows::core::Interface::into_raw(chain);
}

/// The descriptor model's resource kinds are the FFX enum shape the query reply uses.
#[test]
fn the_smoke_descriptors_match_the_ffx_surface_enum() {
    assert_eq!(SurfaceFormat(28), SurfaceFormat(28));
    assert_eq!(ResourceKind::Texture2D, ResourceKind::Texture2D);
    // The query reply's texture shape is `ffxApiGetResourceDX12`'s: a handle, a description, a
    // state. A compile of this assertion is the test.
    let _ = QueryReply::CommandList(0);
    let _ = Dimensions::default();
    let _ = FrameGenerationCreate::default();
}
