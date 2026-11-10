//! D3D12 resources: textures, buffers, allocators and command lists.
//!
//! Each type owns its COM reference. The `unsafe` blocks are the D3D12 calls; the exposed API is
//! safe and the resource-state bookkeeping that Stage 1 relies on is kept in the texture wrapper.
use std::cell::Cell;

use windows::Win32::Graphics::Direct3D12::{
    ID3D12CommandAllocator, ID3D12GraphicsCommandList, ID3D12Resource, D3D12_RESOURCE_BARRIER,
    D3D12_RESOURCE_BARRIER_0, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
    D3D12_RESOURCE_BARRIER_FLAG_NONE, D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
    D3D12_RESOURCE_BARRIER_TYPE_UAV, D3D12_RESOURCE_DESC,
    D3D12_RESOURCE_STATES, D3D12_RESOURCE_TRANSITION_BARRIER, D3D12_RESOURCE_UAV_BARRIER,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT, D3D12_SUBRESOURCE_FOOTPRINT,
    D3D12_TEXTURE_COPY_LOCATION, D3D12_TEXTURE_COPY_LOCATION_0,
    D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    D3D12_TEXTURE_DATA_PITCH_ALIGNMENT,
};

use crate::application::ports::gpu::{
    GpuBuffer, GpuCommandAllocator, GpuCommandList, GpuError, GpuTexture, ResourceState,
    TextureFlags, TextureInfo,
};

use super::device::dxgi_format;

use super::device::pixel_format;

/// An owning committed texture.
pub struct D3d12Texture {
    pub(crate) resource: ID3D12Resource,
    pub(crate) info: TextureInfo,
    /// The state the texture is tracked in. Stage 1 relies on this being exact so a barrier is only
    /// emitted when the state actually changes.
    pub(crate) state: Cell<ResourceState>,
}

impl std::fmt::Debug for D3d12Texture {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("D3d12Texture").field("info", &self.info).finish()
    }
}

impl D3d12Texture {
    pub fn raw(&self) -> &ID3D12Resource {
        &self.resource
    }

    pub fn tracked_state(&self) -> ResourceState {
        self.state.get()
    }

    /// Borrows a game-owned resource as a texture, taking a COM reference so the value can be
    /// held beyond the call. `tracked_state` starts at `state`, which the FFX desc reported.
    ///
    /// The resource is not created by Sedici: Stage 1 transitions it to its own states and back to
    /// `state` before returning (the same contract the C++ used for the game's depth and motion).
    pub fn borrow(resource: &ID3D12Resource, state: ResourceState) -> Self {
        Self {
            info: texture_info(resource),
            resource: resource.clone(),
            state: Cell::new(state),
        }
    }

    pub(crate) fn set_state(&self, state: ResourceState) {
        self.state.set(state);
    }
}

impl Clone for D3d12Texture {
    fn clone(&self) -> Self {
        Self { resource: self.resource.clone(), info: self.info, state: Cell::new(self.state.get()) }
    }
}

// SAFETY: the tracked state is a plain cell read and written only while the list that transitions
// the texture holds it; the wrapper's textures are reached from one Present path at a time, and a
// shared reference here only reads `info`/`identity`. The COM resource itself is free-threaded.
unsafe impl Sync for D3d12Texture {}

impl GpuTexture for D3d12Texture {
    fn info(&self) -> TextureInfo {
        self.info
    }

    fn identity(&self) -> usize {
        // `as_raw` is a safe accessor for the pointer value of a live COM object.
        windows::core::Interface::as_raw(&self.resource) as usize
    }
}

/// Reads a texture's creation description.
pub(crate) fn texture_info(resource: &ID3D12Resource) -> TextureInfo {
    // SAFETY: a pure query on a live resource; the desc is a POD.
    let desc: D3D12_RESOURCE_DESC = unsafe { resource.GetDesc() };
    TextureInfo {
        width: desc.Width as u32,
        height: desc.Height,
        format: pixel_format(desc.Format),
        mip_levels: desc.MipLevels,
        array_size: desc.DepthOrArraySize,
        sample_count: desc.SampleDesc.Count,
        flags: TextureFlags(desc.Flags.0 as u32),
    }
}

/// An owning buffer (upload or readback).
pub struct D3d12Buffer {
    pub(crate) resource: ID3D12Resource,
    pub(crate) size: u64,
}

impl std::fmt::Debug for D3d12Buffer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("D3d12Buffer").field("size", &self.size).finish()
    }
}

impl D3d12Buffer {
    pub fn raw(&self) -> &ID3D12Resource {
        &self.resource
    }

    /// Maps, copies `data` and unmaps. Only valid for an upload heap.
    pub fn write(&self, data: &[u8]) -> Result<(), GpuError> {
        if data.len() as u64 > self.size {
            return Err(GpuError::failed("buffer write", crate::domain::gpu_policy::E_INVALIDARG));
        }
        let mut mapped: *mut core::ffi::c_void = std::ptr::null_mut();
        // SAFETY: an upload-heap buffer is mappable; the range says the CPU will not read it.
        unsafe { self.resource.Map(0, None, Some(&mut mapped)) }
            .map_err(|error| GpuError::failed("buffer map", error.code().0 as u32))?;
        // SAFETY: `mapped` points at `self.size` writable bytes and `data` is no larger.
        unsafe { std::ptr::copy_nonoverlapping(data.as_ptr(), mapped.cast::<u8>(), data.len()) };
        // SAFETY: unmapping a buffer this object mapped; no range is reported as written.
        unsafe { self.resource.Unmap(0, None) };
        Ok(())
    }

    /// Maps for reading (readback heap). Returns a copy of `size` bytes.
    pub fn read(&self) -> Result<Vec<u8>, GpuError> {
        let mut mapped: *mut core::ffi::c_void = std::ptr::null_mut();
        // SAFETY: a readback-heap buffer is mappable; `None` says the CPU may read all of it.
        unsafe { self.resource.Map(0, None, Some(&mut mapped)) }
            .map_err(|error| GpuError::failed("buffer map", error.code().0 as u32))?;
        // SAFETY: `mapped` points at `self.size` readable bytes.
        let bytes = unsafe {
            std::slice::from_raw_parts(mapped.cast::<u8>(), self.size as usize).to_vec()
        };
        // SAFETY: unmapping a buffer this object mapped.
        unsafe { self.resource.Unmap(0, None) };
        Ok(bytes)
    }
}

impl GpuBuffer for D3d12Buffer {
    fn size(&self) -> u64 {
        self.size
    }
}

/// An owning command allocator.
pub struct D3d12CommandAllocator {
    pub(crate) allocator: ID3D12CommandAllocator,
}

impl std::fmt::Debug for D3d12CommandAllocator {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("D3d12CommandAllocator")
    }
}

impl GpuCommandAllocator for D3d12CommandAllocator {
    fn reset(&self) -> Result<(), GpuError> {
        // SAFETY: the caller guarantees every list recorded from this allocator has retired.
        unsafe { self.allocator.Reset() }
            .map_err(|error| GpuError::failed("command allocator reset", error.code().0 as u32))
    }
}

/// An owning graphics command list.
pub struct D3d12CommandList {
    pub(crate) list: ID3D12GraphicsCommandList,
}

impl std::fmt::Debug for D3d12CommandList {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("D3d12CommandList")
    }
}

impl Clone for D3d12CommandList {
    fn clone(&self) -> Self {
        Self { list: self.list.clone() }
    }
}

impl crate::application::ports::gpu::CommandListHandle for D3d12CommandList {
    fn object_handle(&self) -> usize {
        windows::core::Interface::as_raw(&self.list) as usize
    }
}

impl D3d12CommandList {
    pub fn raw(&self) -> &ID3D12GraphicsCommandList {
        &self.list
    }

    /// Borrows a game-owned command list as a value, taking a COM reference so it can be held
    /// beyond the call. The caller owns the recording; Sedici only appends to it.
    pub fn borrow(list: &ID3D12GraphicsCommandList) -> Self {
        Self { list: list.clone() }
    }

    /// Resets the list onto `allocator` for a fresh recording.
    pub fn reset(&self, allocator: &D3d12CommandAllocator) -> Result<(), GpuError> {
        // SAFETY: the allocator has been reset and its previous lists retired by the caller.
        unsafe { self.list.Reset(&allocator.allocator, None) }
            .map_err(|error| GpuError::failed("command list reset", error.code().0 as u32))
    }

    /// Closes the recording so the list can be executed.
    pub fn close(&self) -> Result<(), GpuError> {
        // SAFETY: the list is open and recording.
        unsafe { self.list.Close() }
            .map_err(|error| GpuError::failed("command list close", error.code().0 as u32))
    }

    /// The row pitch a texture upload/copy needs: `width * bytes_per_pixel` aligned to 256.
    pub fn row_pitch(width: u32, bytes_per_pixel: u32) -> u32 {
        let pitch = width.saturating_mul(bytes_per_pixel);
        let alignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        pitch.div_ceil(alignment) * alignment
    }

    /// Copies `data` from a mapped upload buffer into `texture`, leaving the texture in
    /// `final_state`. The staging buffer must be `row_pitch * height` bytes.
    #[allow(clippy::too_many_arguments)]
    pub fn upload_texture(
        &self,
        texture: &D3d12Texture,
        staging: &D3d12Buffer,
        data: &[u8],
        width: u32,
        height: u32,
        bytes_per_pixel: u32,
        initial_state: ResourceState,
        final_state: ResourceState,
    ) -> Result<(), GpuError> {
        let row_pitch = Self::row_pitch(width, bytes_per_pixel);
        let row_bytes = (width * bytes_per_pixel) as usize;
        if data.len() < row_bytes * height as usize || staging.size < u64::from(row_pitch) * u64::from(height) {
            return Err(GpuError::failed("texture upload", crate::domain::gpu_policy::E_INVALIDARG));
        }
        let mut mapped: *mut core::ffi::c_void = std::ptr::null_mut();
        // SAFETY: an upload-heap buffer is mappable for the whole size.
        unsafe { staging.resource.Map(0, None, Some(&mut mapped)) }
            .map_err(|error| GpuError::failed("texture upload map", error.code().0 as u32))?;
        for y in 0..height as usize {
            // SAFETY: the mapped buffer holds `row_pitch * height` bytes and the source row fits
            // inside `data`, so each destination row is in range.
            unsafe {
                std::ptr::copy_nonoverlapping(
                    data.as_ptr().add(y * row_bytes),
                    mapped.cast::<u8>().add(y * row_pitch as usize),
                    row_bytes,
                );
            }
        }
        // SAFETY: unmapping a buffer this call mapped.
        unsafe { staging.resource.Unmap(0, None) };

        self.transition(texture, initial_state, ResourceState::COPY_DEST);
        let from = D3D12_TEXTURE_COPY_LOCATION {
            pResource: std::mem::ManuallyDrop::new(Some(staging.resource.clone())),
            Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                PlacedFootprint: D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                    Offset: 0,
                    Footprint: D3D12_SUBRESOURCE_FOOTPRINT {
                        Format: dxgi_format(texture.info.format),
                        Width: width,
                        Height: height,
                        Depth: 1,
                        RowPitch: row_pitch,
                    },
                },
            },
        };
        let to = D3D12_TEXTURE_COPY_LOCATION {
            pResource: std::mem::ManuallyDrop::new(Some(texture.resource.clone())),
            Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 },
        };
        // SAFETY: both locations describe live resources in COPY_DEST / generic-read states, and
        // the footprint matches the upload buffer's size.
        unsafe { self.list.CopyTextureRegion(&to, 0, 0, 0, &from, None) };
        self.transition(texture, ResourceState::COPY_DEST, final_state);
        Ok(())
    }

    /// Copies `texture` into a readback buffer, leaving the texture in `restore_state`.
    pub fn readback_texture(
        &self,
        texture: &D3d12Texture,
        staging: &D3d12Buffer,
        from_state: ResourceState,
        restore_state: ResourceState,
    ) -> Result<u32, GpuError> {
        let info = texture.info;
        let row_pitch = Self::row_pitch(info.width, 4);
        if staging.size < u64::from(row_pitch) * u64::from(info.height) {
            return Err(GpuError::failed("texture readback", crate::domain::gpu_policy::E_INVALIDARG));
        }
        self.transition(texture, from_state, ResourceState::COPY_SOURCE);
        let from = D3D12_TEXTURE_COPY_LOCATION {
            pResource: std::mem::ManuallyDrop::new(Some(texture.resource.clone())),
            Type: D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 { SubresourceIndex: 0 },
        };
        let to = D3D12_TEXTURE_COPY_LOCATION {
            pResource: std::mem::ManuallyDrop::new(Some(staging.resource.clone())),
            Type: D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            Anonymous: D3D12_TEXTURE_COPY_LOCATION_0 {
                PlacedFootprint: D3D12_PLACED_SUBRESOURCE_FOOTPRINT {
                    Offset: 0,
                    Footprint: D3D12_SUBRESOURCE_FOOTPRINT {
                        Format: dxgi_format(info.format),
                        Width: info.width,
                        Height: info.height,
                        Depth: 1,
                        RowPitch: row_pitch,
                    },
                },
            },
        };
        // SAFETY: the source is a live texture in COPY_SOURCE and the destination buffer is large
        // enough for the footprint.
        unsafe { self.list.CopyTextureRegion(&to, 0, 0, 0, &from, None) };
        self.transition(texture, ResourceState::COPY_SOURCE, restore_state);
        Ok(row_pitch)
    }
}

fn transition_barrier(
    resource: &ID3D12Resource,
    before: ResourceState,
    after: ResourceState,
) -> D3D12_RESOURCE_BARRIER {
    D3D12_RESOURCE_BARRIER {
        Type: D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
        Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
        Anonymous: D3D12_RESOURCE_BARRIER_0 {
            Transition: std::mem::ManuallyDrop::new(D3D12_RESOURCE_TRANSITION_BARRIER {
                pResource: std::mem::ManuallyDrop::new(Some(resource.clone())),
                Subresource: D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                StateBefore: D3D12_RESOURCE_STATES(before.0 as i32),
                StateAfter: D3D12_RESOURCE_STATES(after.0 as i32),
            }),
        },
    }
}

fn uav_barrier(resource: &ID3D12Resource) -> D3D12_RESOURCE_BARRIER {
    D3D12_RESOURCE_BARRIER {
        Type: D3D12_RESOURCE_BARRIER_TYPE_UAV,
        Flags: D3D12_RESOURCE_BARRIER_FLAG_NONE,
        Anonymous: D3D12_RESOURCE_BARRIER_0 {
            UAV: std::mem::ManuallyDrop::new(D3D12_RESOURCE_UAV_BARRIER {
                pResource: std::mem::ManuallyDrop::new(Some(resource.clone())),
            }),
        },
    }
}

impl GpuCommandList<D3d12Texture> for D3d12CommandList {
    fn transition(&self, texture: &D3d12Texture, before: ResourceState, after: ResourceState) {
        if before == after {
            return;
        }
        let barrier = transition_barrier(&texture.resource, before, after);
        // SAFETY: the barrier references a live resource and is copied by the call.
        unsafe { self.list.ResourceBarrier(&[barrier]) };
        texture.set_state(after);
    }

    fn uav_barrier(&self, texture: &D3d12Texture) {
        let barrier = uav_barrier(&texture.resource);
        // SAFETY: the barrier references a live resource and is copied by the call.
        unsafe { self.list.ResourceBarrier(&[barrier]) };
    }

    fn copy_resource(&self, destination: &D3d12Texture, source: &D3d12Texture) {
        // SAFETY: both resources are live, distinct and in the states the caller transitioned them
        // to (COPY_DEST / COPY_SOURCE).
        unsafe { self.list.CopyResource(&destination.resource, &source.resource) };
    }

    fn dispatch(&self, groups_x: u32, groups_y: u32, groups_z: u32) {
        // SAFETY: a pipeline state, root signature and descriptor heaps were bound by the caller.
        unsafe { self.list.Dispatch(groups_x, groups_y, groups_z) };
    }
}

// SAFETY: an allocator is reset only after every list recorded from it has retired, a discipline
// the wrapper enforces by serialising admission; the COM object itself is free-threaded.
unsafe impl Send for D3d12CommandAllocator {}
// SAFETY: as above. Sharing the handle is legal; the reset rule is the caller's.
unsafe impl Sync for D3d12CommandAllocator {}

// SAFETY: a command list is recorded from one Present path at a time and the COM interface is
// free-threaded; the wrapper never resets or closes the same list from two threads.
unsafe impl Send for D3d12CommandList {}
// SAFETY: as above.
unsafe impl Sync for D3d12CommandList {}
