//! Device, queue, fence, resource and command-list capabilities.
//!
//! The application talks to the GPU only through these traits. Every type here is plain data: no
//! `windows` types, no COM pointers. The D3D12 adapter in `adapters/driven/d3d12` implements them
//! with safe owning wrappers, and the bounded-wait and removed-device semantics come from
//! [`crate::domain::gpu_policy`] so the policy is testable without a device.
use std::time::Duration;

use thiserror::Error;

use crate::domain::frame::ColorFormat;
use crate::domain::gpu_policy::GpuFailure;

/// A failure from the GPU port. Bookkeeping failures stay non-fatal to the game (AGENTS.md); only
/// the create-allocation and Present1 metadata exceptions may surface an error, and they are
/// decided by the caller, not here.
#[derive(Debug, Clone, PartialEq, Eq, Error)]
pub enum GpuError {
    #[error("{what} failed ({code:#010x})")]
    Failed { what: &'static str, code: u32 },
    /// A bounded wait expired. The work may still be in flight; the caller must not reuse it.
    #[error("GPU wait timed out")]
    Timeout,
    /// The device was removed. Work recorded or submitted on it can never complete, and nothing
    /// recorded on it can be "in flight" (K38).
    #[error("GPU device removed ({reason:#010x})")]
    DeviceRemoved { reason: u32 },
    #[error(transparent)]
    Policy(#[from] GpuFailure),
}

impl GpuError {
    pub fn failed(what: &'static str, code: u32) -> Self {
        Self::Failed { what, code }
    }

    /// The HRESULT-shaped code the adapter's callers log.
    pub fn code(&self) -> u32 {
        match self {
            Self::Failed { code, .. } => *code,
            Self::Timeout => crate::domain::gpu_policy::E_FAIL,
            Self::DeviceRemoved { reason } => *reason,
            Self::Policy(failure) => failure.0,
        }
    }
}

/// A resource state, mirroring `D3D12_RESOURCE_STATES`. Values are the D3D12 bit values so the
/// adapter converts without a table; the application only ever names the states below.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ResourceState(pub u32);

impl ResourceState {
    pub const COMMON: Self = Self(0);
    pub const UNORDERED_ACCESS: Self = Self(8);
    pub const NON_PIXEL_SHADER_RESOURCE: Self = Self(64);
    pub const PIXEL_SHADER_RESOURCE: Self = Self(128);
    pub const COPY_SOURCE: Self = Self(2048);
    pub const COPY_DEST: Self = Self(1024);
    pub const PRESENT: Self = Self(0);
    pub const GENERIC_READ: Self = Self(2755);
    pub const RENDER_TARGET: Self = Self(4);
    pub const DEPTH_WRITE: Self = Self(16);
    pub const INDIRECT_ARGUMENT: Self = Self(512);

    /// The union of two states, for a resource read by both pixel and compute shaders.
    pub const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }

    /// The value as the FFX-facing `FfxApiResourceState` enum, for the ABI edge.
    pub const fn from_ffx(value: u32) -> Self {
        // FFX_API_RESOURCE_STATE_* are 1 << n; D3D12 states are a different encoding, so this
        // mirrors `dx12_state_from_ffx` rather than translating bits.
        match value {
            1 => Self::COMMON,
            2 => Self::UNORDERED_ACCESS,
            4 => Self::NON_PIXEL_SHADER_RESOURCE,
            8 => Self::PIXEL_SHADER_RESOURCE,
            12 => Self::NON_PIXEL_SHADER_RESOURCE.union(Self::PIXEL_SHADER_RESOURCE),
            16 => Self::COPY_SOURCE,
            32 => Self::COPY_DEST,
            20 => Self::GENERIC_READ,
            64 => Self::INDIRECT_ARGUMENT,
            128 => Self::PRESENT,
            256 => Self::RENDER_TARGET,
            512 => Self::DEPTH_WRITE,
            _ => Self::COMMON,
        }
    }
}

/// The pixel formats the generator and its inputs use. Anything else is refused with a defined
/// error rather than guessed at.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PixelFormat {
    R8G8B8A8Unorm,
    R10G10B10A2Unorm,
    R32Float,
    R16G16Float,
    R16G16B16A16Float,
    R16Float,
    R32G32Float,
    R32Uint,
    R32Typeless,
    R32G8X24Typeless,
    R32FloatX8X24Typeless,
    D32Float,
    D32FloatS8X24Uint,
    Unknown,
}

impl PixelFormat {
    /// The typed SRV format a native depth allocation is viewed with (ports `depth_srv_format`).
    /// FFX reports `D32_FLOAT` as `R32_FLOAT`; depth and typeless allocations are not SRV formats
    /// themselves.
    pub const fn depth_srv_format(self) -> Option<Self> {
        match self {
            Self::D32Float | Self::R32Typeless | Self::R32Float => Some(Self::R32Float),
            // Cyberpunk's native depth/stencil allocation has two planes. Sample only plane 0 as
            // float depth; the stencil plane is not an input to generation.
            Self::R32G8X24Typeless | Self::D32FloatS8X24Uint => Some(Self::R32FloatX8X24Typeless),
            Self::R16Float => Some(Self::R16Float),
            _ => None,
        }
    }

    /// Motion vectors must be float; the decode pass reads `.xy` and converts to UV.
    pub const fn is_supported_motion(self) -> bool {
        matches!(
            self,
            Self::R16G16B16A16Float | Self::R16G16Float | Self::R32G32Float
        )
    }
}

impl From<ColorFormat> for PixelFormat {
    fn from(format: ColorFormat) -> Self {
        match format {
            ColorFormat::R8G8B8A8Unorm => Self::R8G8B8A8Unorm,
            ColorFormat::R10G10B10A2Unorm => Self::R10G10B10A2Unorm,
        }
    }
}

impl From<PixelFormat> for ColorFormat {
    fn from(format: PixelFormat) -> Self {
        match format {
            PixelFormat::R10G10B10A2Unorm => Self::R10G10B10A2Unorm,
            _ => Self::R8G8B8A8Unorm,
        }
    }
}

/// Resource flags, mirroring `D3D12_RESOURCE_FLAGS`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TextureFlags(pub u32);

impl TextureFlags {
    pub const NONE: Self = Self(0);
    /// `D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET`. Required before `CreateRenderTargetView` accepts a
    /// resource; the overlay draws onto the game's back buffer, which the game created with it.
    pub const ALLOW_RENDER_TARGET: Self = Self(1);
    pub const ALLOW_DEPTH_STENCIL: Self = Self(2);
    pub const ALLOW_UNORDERED_ACCESS: Self = Self(4);
    pub const DENY_SHADER_RESOURCE: Self = Self(8);

    pub const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }

    pub const fn union(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

/// What to create. Committed 2D textures with one mip, one sample and one array slice, which is
/// everything Stage 1 uses.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureDesc {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
    pub flags: TextureFlags,
}

impl TextureDesc {
    pub const fn texture2d(
        width: u32,
        height: u32,
        format: PixelFormat,
        flags: TextureFlags,
    ) -> Self {
        Self {
            width,
            height,
            format,
            flags,
        }
    }
}

/// A texture's creation description, as read back from the GPU.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TextureInfo {
    pub width: u32,
    pub height: u32,
    pub format: PixelFormat,
    pub mip_levels: u16,
    pub array_size: u16,
    pub sample_count: u32,
    pub flags: TextureFlags,
}

impl TextureInfo {
    /// The shape every Stage 1 input and target must have.
    pub const fn is_single_sample_2d(&self) -> bool {
        self.mip_levels == 1 && self.array_size == 1 && self.sample_count == 1
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum QueueKind {
    Direct,
    Compute,
    Copy,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum QueuePriority {
    Normal,
    High,
}

/// A GPU resource the generator can read or write. The adapter's type owns the COM reference.
pub trait GpuTexture {
    fn info(&self) -> TextureInfo;

    /// Stable identity for aliasing checks (`output == frame.color` in the C++). The value is the
    /// underlying resource address and is only ever compared, never dereferenced.
    fn identity(&self) -> usize;
}

pub trait GpuBuffer {
    fn size(&self) -> u64;
}

/// Recording operations. Everything else a pass needs (root signature, pipeline, descriptor heap)
/// is D3D12-specific and lives in the adapter; the application never touches it.
///
/// Parameterized on the texture type so the adapter's concrete list can build barriers without a
/// downcast, and the application still only names the port type.
pub trait GpuCommandList<T: GpuTexture> {
    fn transition(&self, texture: &T, before: ResourceState, after: ResourceState);

    /// Orders UAV writes before the following UAV reads of the same resource.
    fn uav_barrier(&self, texture: &T);

    fn copy_resource(&self, destination: &T, source: &T);

    fn dispatch(&self, groups_x: u32, groups_y: u32, groups_z: u32);

    /// The list's underlying object address, for a driven port that needs to name the list it was
    /// handed (the overlay renderer's [`crate::application::ports::ui_renderer::RenderTarget`]).
    ///
    /// The value is opaque: it is only ever passed back to the adapter that produced it, never
    /// dereferenced by the application. `0` means the adapter cannot name one, and a caller that
    /// needs it treats that as "no list".
    fn object_identity(&self) -> usize;
}

pub trait GpuCommandAllocator {
    /// Resets the allocator. The caller must have retired every list recorded from it.
    fn reset(&self) -> Result<(), GpuError>;
}

/// A timeline fence. Every wait is bounded; there is no unbounded wait in the port.
pub trait GpuFence {
    fn completed(&self) -> u64;

    /// Waits until `value` completes. Returns [`GpuError::Timeout`] on expiry and
    /// [`GpuError::DeviceRemoved`] when the fence reports `u64::MAX`, so a removed device never
    /// reads as success.
    fn wait(&self, value: u64, timeout: Duration) -> Result<(), GpuError>;
}

/// Parameterized on the device's list and fence types, which is what the adapter's queue records.
pub trait GpuQueue<L, F> {
    fn execute(&self, lists: &[&L]) -> Result<(), GpuError>;

    fn signal(&self, fence: &F, value: u64) -> Result<(), GpuError>;

    /// Makes the queue wait for `fence >= value` before the next submitted work.
    fn wait(&self, fence: &F, value: u64) -> Result<(), GpuError>;
}

/// Everything the application may ask of the device.
pub trait GpuDevice {
    type Texture: GpuTexture;
    type Buffer: GpuBuffer;
    type CommandAllocator: GpuCommandAllocator;
    type CommandList: GpuCommandList<Self::Texture>;
    type Fence: GpuFence;
    type Queue: GpuQueue<Self::CommandList, Self::Fence>;

    fn create_texture(&self, desc: &TextureDesc) -> Result<Self::Texture, GpuError>;

    /// A CPU-writable buffer, used for the LUT and for test uploads.
    fn create_upload_buffer(&self, size: u64) -> Result<Self::Buffer, GpuError>;

    /// A CPU-readable buffer, used for readback after a GPU copy.
    fn create_readback_buffer(&self, size: u64) -> Result<Self::Buffer, GpuError>;

    /// Copies `data` into an upload buffer. The buffer must be at least `data.len()` bytes.
    fn write_buffer(&self, buffer: &Self::Buffer, data: &[u8]) -> Result<(), GpuError>;

    /// Reads a readback buffer's contents.
    fn read_buffer(&self, buffer: &Self::Buffer) -> Result<Vec<u8>, GpuError>;

    fn create_command_allocator(&self, kind: QueueKind)
        -> Result<Self::CommandAllocator, GpuError>;

    fn create_command_list(
        &self,
        allocator: &Self::CommandAllocator,
    ) -> Result<Self::CommandList, GpuError>;

    fn create_fence(&self, initial_value: u64) -> Result<Self::Fence, GpuError>;

    fn create_queue(
        &self,
        kind: QueueKind,
        priority: QueuePriority,
    ) -> Result<Self::Queue, GpuError>;

    /// `Some(reason)` when the device has been removed, as `GetDeviceRemovedReason` reports it.
    fn removed_reason(&self) -> Option<u32>;
}

/// A command list that can name its underlying object, so a list held by reference can be handed
/// to the FFX-facing seam as a handle the adapter resolves back.
///
/// The wrapper records generation into its own list; a session-driven context resolves every list
/// through [`crate::application::context::FrameObjects`], which speaks in handles. The value is the
/// object's address: it is only ever compared and passed back to the adapter that produced it,
/// never dereferenced by the application.
pub trait CommandListHandle {
    fn object_handle(&self) -> usize;
}
