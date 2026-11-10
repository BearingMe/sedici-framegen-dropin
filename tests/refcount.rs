//! Reference balance at the FFX resource seam, porting the lifetime that `fgcheck_quality.cpp`'s
//! contract checks took for granted: Sedici only *borrows* the game's textures and command lists,
//! so a create/prepare/generate does not change the game's COM reference count.
//!
//! K165: the Rust `FrameObjects` adopted each FFX handle with an owning `from_raw` and then
//! `D3d12Texture::borrow`/`D3d12CommandList::borrow` AddRef'd it again, so the second borrow of the
//! same game resource ran on freed memory. This test drives the real adapter for the same reason
//! `fgcheck` drove the real `Context`: fakes cannot show a COM reference count.
//!
//! A regression here is a use-after-free, so it aborts the test process rather than failing an
//! assertion. That is the loudest signal available and the one the C++ would have shown as an
//! access violation.
#![allow(unsafe_code)]

use windows::core::Interface;

use amd_fidelityfx_framegeneration_dx12::adapters::driven::backend::D3d12FrameObjects;
use amd_fidelityfx_framegeneration_dx12::adapters::driven::d3d12::device::D3d12Device;
use amd_fidelityfx_framegeneration_dx12::application::context::FrameObjects;
use amd_fidelityfx_framegeneration_dx12::application::ports::gpu::{
    GpuDevice, PixelFormat, QueueKind, TextureDesc, TextureFlags,
};

#[test]
fn borrowing_a_game_texture_three_times_keeps_it_alive() {
    let Some(device) = D3d12Device::create_default().ok().flatten() else {
        eprintln!("SKIP refcount: no D3D12 adapter on this machine");
        return;
    };
    let desc = TextureDesc::texture2d(64, 64, PixelFormat::R8G8B8A8Unorm, TextureFlags::NONE);
    // The game owns exactly this one reference; Sedici's adapter sees only its raw pointer.
    let owned = device.create_texture(&desc).expect("texture");
    let handle = Interface::as_raw(owned.raw()) as usize;
    let objects = D3d12FrameObjects;
    for round in 0..3 {
        let borrowed = objects.texture(handle).unwrap_or_else(|| panic!("round {round}: the adapter resolves the game's texture"));
        // SAFETY: a pure query through the borrowed reference.
        let width = unsafe { borrowed.raw().GetDesc() }.Width;
        assert_eq!(width, 64, "round {round}: the borrowed texture is the game's");
        drop(borrowed);
    }
    // SAFETY: the game still owns its reference; if the adapter consumed it, this is the fault the
    // test is meant to surface.
    let width = unsafe { owned.raw().GetDesc() }.Width;
    assert_eq!(width, 64, "the game's texture is still alive after three borrows");
}

#[test]
fn borrowing_a_game_command_list_three_times_keeps_it_alive() {
    let Some(device) = D3d12Device::create_default().ok().flatten() else {
        eprintln!("SKIP refcount: no D3D12 adapter on this machine");
        return;
    };
    let allocator = device.create_command_allocator(QueueKind::Direct).expect("allocator");
    // The game owns this list for the length of a prepare; Sedici is handed its address.
    let owned = device.create_command_list(&allocator).expect("list");
    let handle = Interface::as_raw(owned.raw()) as usize;
    let objects = D3d12FrameObjects;
    for round in 0..3 {
        let borrowed = objects
            .list(handle)
            .unwrap_or_else(|| panic!("round {round}: the adapter resolves the game's command list"));
        // SAFETY: a pure query through the borrowed reference.
        let mut owner: Option<windows::Win32::Graphics::Direct3D12::ID3D12Device> = None;
        unsafe { borrowed.raw().GetDevice(&mut owner) }
            .unwrap_or_else(|error| panic!("round {round}: the borrowed list is live ({error:?})"));
        drop(borrowed);
    }
    // SAFETY: the game still owns its reference.
    let mut owner: Option<windows::Win32::Graphics::Direct3D12::ID3D12Device> = None;
    unsafe { owned.raw().GetDevice(&mut owner) }.expect("the game's command list is still alive");
}
