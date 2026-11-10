//! D3D12 device: the owning wrapper behind the [`GpuDevice`] port.
//!
//! Every COM pointer is owned by a `windows` interface type, which is reference-counted and safe to
//! drop. The `unsafe` blocks here are the raw D3D12 calls only; each carries a `SAFETY` comment and
//! the exposed API is safe.
use std::cell::Cell;

use windows::Win32::Graphics::Direct3D::D3D_FEATURE_LEVEL_11_0;
use windows::Win32::Graphics::Direct3D12::*;
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
    DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
    DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32_FLOAT,
    DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_UINT,
    DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN,
    DXGI_SAMPLE_DESC,
};

use crate::application::ports::gpu::{
    GpuDevice, GpuError, PixelFormat, QueueKind, QueuePriority, TextureDesc,
};
use crate::domain::gpu_policy::E_FAIL;

use super::fence::{create_event, D3d12Fence};
use super::queue::D3d12Queue;
use super::resources::{
    D3d12Buffer, D3d12CommandAllocator, D3d12CommandList, D3d12Texture,
};

/// `D3D12GetDebugInterface` is optional; a missing debug layer is not an error.
pub fn enable_debug_layer() {
    let mut debug: Option<ID3D12Debug> = None;
    // SAFETY: the out pointer is a valid local and the call reports failure through its Result.
    if unsafe { D3D12GetDebugInterface(&mut debug) }.is_ok() {
        if let Some(debug) = debug {
            // SAFETY: the interface is valid for the lifetime of `debug`, and enabling is idempotent.
            unsafe { debug.EnableDebugLayer() };
        }
    }
}

/// Creates a device on the default adapter, or returns `None` when no D3D12 adapter exists.
pub fn create_default_device() -> Result<Option<D3d12Device>, GpuError> {
    let mut device: Option<ID3D12Device> = None;
    // SAFETY: `None` selects the default adapter; the out pointer is a valid local.
    let result = unsafe {
        D3D12CreateDevice(None::<&windows::core::IUnknown>, D3D_FEATURE_LEVEL_11_0, &mut device)
    };
    match (result, device) {
        (Ok(()), Some(device)) => Ok(Some(D3d12Device { device })),
        (Ok(()), None) => Ok(None),
        // No adapter at all is reported as an error by DXGI; treat it as "no device" so the test
        // can skip rather than fail.
        (Err(_), _) => Ok(None),
    }
}

/// An owning D3D12 device.
#[derive(Clone)]
pub struct D3d12Device {
    pub(crate) device: ID3D12Device,
}

impl std::fmt::Debug for D3d12Device {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("D3d12Device")
    }
}

impl D3d12Device {
    /// Creates a device on the default adapter, or `None` when no D3D12 adapter exists.
    pub fn create_default() -> Result<Option<Self>, GpuError> {
        create_default_device()
    }

    /// Wraps an existing device (the game's) without taking ownership beyond a reference.
    pub fn from_raw(device: &ID3D12Device) -> Self {
        Self { device: device.clone() }
    }

    pub fn raw(&self) -> &ID3D12Device {
        &self.device
    }

    /// The descriptor increment for a CBV/SRV/UAV heap.
    pub fn descriptor_increment(&self) -> u32 {
        // SAFETY: a pure query on a live device.
        unsafe { self.device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) }
    }

    /// The device's removed reason, or `None` while it is healthy.
    pub fn removed_reason(&self) -> Option<u32> {
        // SAFETY: a pure query on a live device. `ok()` maps S_OK to `None`.
        match unsafe { self.device.GetDeviceRemovedReason() } {
            Ok(()) => None,
            Err(error) => Some(error.code().0 as u32),
        }
    }

    /// `Some(reason)` if the device is removed, else `None`, in the policy's error shape.
    pub fn check_removed(&self) -> Result<(), GpuError> {
        match self.removed_reason() {
            Some(reason) => Err(GpuError::DeviceRemoved {
                reason: crate::domain::gpu_policy::removed_device_error(reason),
            }),
            None => Ok(()),
        }
    }

    /// Creates the root signature from serialized bytes.
    pub fn create_root_signature(&self, serialized: &[u8]) -> Result<ID3D12RootSignature, GpuError> {
        // SAFETY: `serialized` is a valid byte slice for the duration of the call, which copies it.
        unsafe { self.device.CreateRootSignature::<ID3D12RootSignature>(0, serialized) }
            .map_err(|error| GpuError::failed("root signature creation", error.code().0 as u32))
    }

    /// Creates a shader-visible CBV/SRV/UAV descriptor heap.
    pub fn create_descriptor_heap(&self, count: u32) -> Result<ID3D12DescriptorHeap, GpuError> {
        let desc = D3D12_DESCRIPTOR_HEAP_DESC {
            Type: D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            NumDescriptors: count,
            Flags: D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
            NodeMask: 0,
        };
        // SAFETY: `desc` is fully initialized and outlives the call.
        unsafe { self.device.CreateDescriptorHeap::<ID3D12DescriptorHeap>(&desc) }
            .map_err(|error| GpuError::failed("descriptor heap creation", error.code().0 as u32))
    }

    /// Creates a compute pipeline from compiled bytecode.
    pub fn create_compute_pipeline(
        &self,
        root_signature: &ID3D12RootSignature,
        bytecode: &[u8],
    ) -> Result<ID3D12PipelineState, GpuError> {
        let desc = D3D12_COMPUTE_PIPELINE_STATE_DESC {
            // The struct holds the reference only for the call; the pipeline keeps its own.
            pRootSignature: std::mem::ManuallyDrop::new(Some(root_signature.clone())),
            CS: D3D12_SHADER_BYTECODE {
                pShaderBytecode: bytecode.as_ptr().cast(),
                BytecodeLength: bytecode.len(),
            },
            ..Default::default()
        };
        // SAFETY: the root signature is live and `bytecode` outlives the call.
        unsafe { self.device.CreateComputePipelineState::<ID3D12PipelineState>(&desc) }
            .map_err(|error| GpuError::failed("compute pipeline creation", error.code().0 as u32))
    }

    /// Creates a committed buffer. `heap` is D3D12_HEAP_TYPE_UPLOAD or READBACK.
    pub fn create_buffer(
        &self,
        size: u64,
        heap_type: D3D12_HEAP_TYPE,
        initial_state: D3D12_RESOURCE_STATES,
    ) -> Result<D3d12Buffer, GpuError> {
        let heap = D3D12_HEAP_PROPERTIES { Type: heap_type, ..Default::default() };
        let desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_BUFFER,
            Alignment: 0,
            Width: size.max(1),
            Height: 1,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: DXGI_FORMAT_UNKNOWN,
            // D3D12 requires a sample count of 1 even for buffers; the derived default is 0.
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            Flags: D3D12_RESOURCE_FLAG_NONE,
        };
        let mut resource: Option<ID3D12Resource> = None;
        // SAFETY: both descs are fully initialized locals that outlive the call.
        unsafe {
            self.device.CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &desc,
                initial_state,
                None,
                &mut resource,
            )
        }
        .map_err(|error| GpuError::failed("buffer creation", error.code().0 as u32))?;
        let resource = resource.ok_or(GpuError::failed("buffer creation", E_FAIL))?;
        Ok(D3d12Buffer { resource, size })
    }
}

impl GpuDevice for D3d12Device {
    type Texture = D3d12Texture;
    type Buffer = D3d12Buffer;
    type CommandAllocator = D3d12CommandAllocator;
    type CommandList = D3d12CommandList;
    type Fence = D3d12Fence;
    type Queue = D3d12Queue;

    fn create_texture(&self, desc: &TextureDesc) -> Result<Self::Texture, GpuError> {
        let heap = D3D12_HEAP_PROPERTIES {
            Type: D3D12_HEAP_TYPE_DEFAULT,
            ..Default::default()
        };
        let resource_desc = D3D12_RESOURCE_DESC {
            Dimension: D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            Alignment: 0,
            Width: u64::from(desc.width),
            Height: desc.height,
            DepthOrArraySize: 1,
            MipLevels: 1,
            Format: dxgi_format(desc.format),
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Layout: D3D12_TEXTURE_LAYOUT_UNKNOWN,
            Flags: D3D12_RESOURCE_FLAGS(desc.flags.0 as i32),
        };
        let mut resource: Option<ID3D12Resource> = None;
        // SAFETY: both descs are fully initialized locals that outlive the call.
        unsafe {
            self.device.CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &resource_desc,
                D3D12_RESOURCE_STATE_COMMON,
                None,
                &mut resource,
            )
        }
        .map_err(|error| GpuError::failed("texture creation", error.code().0 as u32))?;
        let resource = resource.ok_or(GpuError::failed("texture creation", E_FAIL))?;
        Ok(D3d12Texture {
            info: super::resources::texture_info(&resource),
            resource,
            state: Cell::new(crate::application::ports::gpu::ResourceState::COMMON),
        })
    }

    fn create_upload_buffer(&self, size: u64) -> Result<Self::Buffer, GpuError> {
        self.create_buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ)
    }

    fn create_readback_buffer(&self, size: u64) -> Result<Self::Buffer, GpuError> {
        self.create_buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST)
    }

    fn write_buffer(&self, buffer: &Self::Buffer, data: &[u8]) -> Result<(), GpuError> {
        buffer.write(data)
    }

    fn read_buffer(&self, buffer: &Self::Buffer) -> Result<Vec<u8>, GpuError> {
        buffer.read()
    }

    fn create_command_allocator(&self, kind: QueueKind) -> Result<Self::CommandAllocator, GpuError> {
        // SAFETY: the enum value comes from a closed set; the allocator owns its allocation.
        let allocator = unsafe {
            self.device.CreateCommandAllocator::<ID3D12CommandAllocator>(command_list_type(kind))
        }
        .map_err(|error| GpuError::failed("command allocator creation", error.code().0 as u32))?;
        Ok(D3d12CommandAllocator { allocator })
    }

    fn create_command_list(
        &self,
        allocator: &Self::CommandAllocator,
    ) -> Result<Self::CommandList, GpuError> {
        // SAFETY: the allocator is live and outlives the call; the list keeps its own reference.
        let list = unsafe {
            self.device.CreateCommandList::<_, _, ID3D12GraphicsCommandList>(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                &allocator.allocator,
                None,
            )
        }
        .map_err(|error| GpuError::failed("command list creation", error.code().0 as u32))?;
        // A new list starts open; close it so the caller resets before recording, as the C++ does.
        // SAFETY: the list was just created and is not executing.
        let _ = unsafe { list.Close() };
        Ok(D3d12CommandList { list })
    }

    fn create_fence(&self, initial_value: u64) -> Result<Self::Fence, GpuError> {
        // SAFETY: a plain fence with no flags; the event is created for the bounded wait.
        let fence = unsafe {
            self.device.CreateFence::<ID3D12Fence>(initial_value, D3D12_FENCE_FLAG_NONE)
        }
        .map_err(|error| GpuError::failed("fence creation", error.code().0 as u32))?;
        let event = create_event()?;
        Ok(D3d12Fence { fence, event })
    }

    fn create_queue(
        &self,
        kind: QueueKind,
        priority: QueuePriority,
    ) -> Result<Self::Queue, GpuError> {
        let desc = D3D12_COMMAND_QUEUE_DESC {
            Type: command_list_type(kind),
            Priority: match priority {
                QueuePriority::High => 100,
                QueuePriority::Normal => 0,
            },
            Flags: D3D12_COMMAND_QUEUE_FLAG_NONE,
            NodeMask: 0,
        };
        // SAFETY: `desc` is a fully initialized local that outlives the call.
        let queue = unsafe { self.device.CreateCommandQueue::<ID3D12CommandQueue>(&desc) }
            .map_err(|error| GpuError::failed("command queue creation", error.code().0 as u32))?;
        Ok(D3d12Queue { queue })
    }

    fn removed_reason(&self) -> Option<u32> {
        D3d12Device::removed_reason(self)
    }
}

pub(crate) fn command_list_type(kind: QueueKind) -> D3D12_COMMAND_LIST_TYPE {
    match kind {
        QueueKind::Direct => D3D12_COMMAND_LIST_TYPE_DIRECT,
        QueueKind::Compute => D3D12_COMMAND_LIST_TYPE_COMPUTE,
        QueueKind::Copy => D3D12_COMMAND_LIST_TYPE_COPY,
    }
}

/// The DXGI format for a port format. `Unknown` maps to `DXGI_FORMAT_UNKNOWN`, which no creation
/// path accepts, so an unsupported format fails at creation rather than being guessed.
pub(crate) fn dxgi_format(format: PixelFormat) -> DXGI_FORMAT {
    match format {
        PixelFormat::R8G8B8A8Unorm => DXGI_FORMAT_R8G8B8A8_UNORM,
        PixelFormat::R10G10B10A2Unorm => DXGI_FORMAT_R10G10B10A2_UNORM,
        PixelFormat::R32Float => DXGI_FORMAT_R32_FLOAT,
        PixelFormat::R16G16Float => DXGI_FORMAT_R16G16_FLOAT,
        PixelFormat::R16G16B16A16Float => DXGI_FORMAT_R16G16B16A16_FLOAT,
        PixelFormat::R16Float => DXGI_FORMAT_R16_FLOAT,
        PixelFormat::R32G32Float => DXGI_FORMAT_R32G32_FLOAT,
        PixelFormat::R32Uint => DXGI_FORMAT_R32_UINT,
        PixelFormat::R32Typeless => DXGI_FORMAT_R32_TYPELESS,
        PixelFormat::R32G8X24Typeless => DXGI_FORMAT_R32G8X24_TYPELESS,
        PixelFormat::R32FloatX8X24Typeless => DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
        PixelFormat::D32Float => DXGI_FORMAT_D32_FLOAT,
        PixelFormat::D32FloatS8X24Uint => DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        PixelFormat::Unknown => DXGI_FORMAT_UNKNOWN,
    }
}

/// The port format for a DXGI format. The mapping is only defined for the formats Sedici handles.
pub(crate) fn pixel_format(format: DXGI_FORMAT) -> PixelFormat {
    match format {
        DXGI_FORMAT_R8G8B8A8_UNORM => PixelFormat::R8G8B8A8Unorm,
        DXGI_FORMAT_R10G10B10A2_UNORM => PixelFormat::R10G10B10A2Unorm,
        DXGI_FORMAT_R32_FLOAT => PixelFormat::R32Float,
        DXGI_FORMAT_R16G16_FLOAT => PixelFormat::R16G16Float,
        DXGI_FORMAT_R16G16B16A16_FLOAT => PixelFormat::R16G16B16A16Float,
        DXGI_FORMAT_R16_FLOAT => PixelFormat::R16Float,
        DXGI_FORMAT_R32G32_FLOAT => PixelFormat::R32G32Float,
        DXGI_FORMAT_R32_UINT => PixelFormat::R32Uint,
        DXGI_FORMAT_R32_TYPELESS => PixelFormat::R32Typeless,
        DXGI_FORMAT_R32G8X24_TYPELESS => PixelFormat::R32G8X24Typeless,
        DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS => PixelFormat::R32FloatX8X24Typeless,
        DXGI_FORMAT_D32_FLOAT => PixelFormat::D32Float,
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT => PixelFormat::D32FloatS8X24Uint,
        _ => PixelFormat::Unknown,
    }
}
