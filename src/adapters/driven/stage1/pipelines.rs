//! Stage 1 pipelines: root signature, descriptor heap, shader compilation and the per-pass
//! descriptor writes (ports `generator/stage1.cpp`'s `initialize`/`compile_pipeline`/`write_*`).
//!
//! The HLSL lives in `shaders/*.hlsl` and is embedded with `include_str!`, so the deployed DLL has
//! no external shader files. Each pass source is prefixed with `common.hlsl` and compiled with the
//! system `D3DCompile` (d3dcompiler_47.dll) at first use, exactly as the C++ did.
use windows::core::PCSTR;
use windows::Win32::Graphics::Direct3D::Fxc::{D3DCompile, D3DCOMPILE_OPTIMIZATION_LEVEL3};
use windows::Win32::Graphics::Direct3D::ID3DBlob;
use windows::Win32::Graphics::Direct3D12::{
    ID3D12DescriptorHeap, ID3D12Device, ID3D12PipelineState, ID3D12Resource, ID3D12RootSignature,
    D3D12_BUFFER_SRV, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
    D3D12_DESCRIPTOR_RANGE, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
    D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_ROOT_PARAMETER, D3D12_ROOT_PARAMETER_0,
    D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
    D3D12_ROOT_SIGNATURE_DESC, D3D12_ROOT_SIGNATURE_FLAG_NONE, D3D12_SHADER_RESOURCE_VIEW_DESC,
    D3D12_SHADER_RESOURCE_VIEW_DESC_0, D3D12_SHADER_VISIBILITY_ALL, D3D12_SRV_DIMENSION_BUFFER,
    D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_TEX2D_SRV, D3D12_TEX2D_UAV, D3D12_UAV_DIMENSION_TEXTURE2D,
    D3D12_UNORDERED_ACCESS_VIEW_DESC, D3D12_UNORDERED_ACCESS_VIEW_DESC_0,
    D3D_ROOT_SIGNATURE_VERSION_1,
};
use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_R32_FLOAT;

use crate::application::ports::gpu::GpuError;
use crate::domain::frame::Constants;

use super::super::d3d12::device::{dxgi_format, D3d12Device};
use crate::application::ports::gpu::PixelFormat;

/// Root constants: `sizeof(Constants) / sizeof(u32)`.
pub const ROOT_CONSTANTS: u32 = 24;
pub const SRV_SLOTS: u32 = 8;
pub const UAV_SLOTS: u32 = 4;
/// Blocks 0-1: prepare. Then five blocks (splat clear, splat, fill, warp, composite) per generated
/// phase, so several phases can be recorded into one command list without overwriting each other's
/// descriptors.
pub const PREPARE_BLOCKS: u32 = 2;
pub const PHASE_BLOCKS: u32 = 5;
pub const MAX_PHASES: u32 = 2;
pub const BLOCKS: u32 = PREPARE_BLOCKS + PHASE_BLOCKS * MAX_PHASES;
pub const BLOCK_SLOTS: u32 = SRV_SLOTS + UAV_SLOTS;
pub const HEAP_SLOTS: u32 = BLOCKS * BLOCK_SLOTS;
pub const GROUP_SIZE: u32 = 8;

/// The LUT is a 128x128 single-channel FLOAT atlas (64 KiB), embedded from `lut.bin`.
pub const LUT_TEXELS: u32 = 128 * 128;
pub const LUT_BYTES: &[u8] = include_bytes!("lut.bin");

/// The shared HLSL prefix prepended to every pass.
pub const COMMON: &str = include_str!("shaders/common.hlsl");

/// Dispatch group count for a texture size.
#[allow(clippy::manual_div_ceil)] // `u32::div_ceil` is not const-stable.
pub const fn groups(size: u32) -> u32 {
    (size + GROUP_SIZE - 1) / GROUP_SIZE
}

/// One compiled pipeline plus its name for the pass-mark hook.
pub struct Pipeline {
    pub state: ID3D12PipelineState,
    pub name: &'static str,
}

/// The compiled pipelines, in the order the passes run.
pub struct Pipelines {
    pub prepare: Pipeline,
    pub clear: Pipeline,
    pub reproject: Pipeline,
    pub fill: Pipeline,
    pub warp: Pipeline,
    pub composite: Pipeline,
}

impl Pipelines {
    /// Compiles every pass. A failure names the pass in the log line, as the C++ did.
    pub fn compile(
        device: &D3d12Device,
        root_signature: &ID3D12RootSignature,
    ) -> Result<Self, GpuError> {
        let compile = |source: &str, name: &'static str| -> Result<Pipeline, GpuError> {
            let bytecode = compile_shader(source, name)?;
            let state = device.create_compute_pipeline(root_signature, &bytecode)?;
            Ok(Pipeline { state, name })
        };
        Ok(Self {
            prepare: compile(&pass_source("shaders/prepare.hlsl"), "prepare")?,
            clear: compile(&pass_source("shaders/clear.hlsl"), "clear")?,
            reproject: compile(&pass_source("shaders/reproject.hlsl"), "reproject")?,
            fill: compile(&pass_source("shaders/fill.hlsl"), "fill")?,
            warp: compile(&pass_source("shaders/warp.hlsl"), "warp")?,
            composite: compile(&pass_source("shaders/composite.hlsl"), "composite")?,
        })
    }
}

fn pass_source(path: &str) -> String {
    let body = match path {
        "shaders/prepare.hlsl" => include_str!("shaders/prepare.hlsl"),
        "shaders/clear.hlsl" => include_str!("shaders/clear.hlsl"),
        "shaders/reproject.hlsl" => include_str!("shaders/reproject.hlsl"),
        "shaders/fill.hlsl" => include_str!("shaders/fill.hlsl"),
        "shaders/warp.hlsl" => include_str!("shaders/warp.hlsl"),
        "shaders/composite.hlsl" => include_str!("shaders/composite.hlsl"),
        _ => unreachable!("every pass source is embedded above"),
    };
    format!("{COMMON}\n{body}")
}

/// Compiles one pass to `CSMain`, `cs_5_1`, optimisation level 3.
pub fn compile_shader(source: &str, name: &'static str) -> Result<Vec<u8>, GpuError> {
    let mut bytecode: Option<ID3DBlob> = None;
    let mut errors: Option<ID3DBlob> = None;
    let source_name = c"stage1.hlsl";
    let entry = c"CSMain";
    let target = c"cs_5_1";
    // SAFETY: the source pointer and length describe a live UTF-8 string; the out pointers are
    // valid locals; D3DCompile copies what it needs and reports failure through its Result.
    let result = unsafe {
        D3DCompile(
            source.as_ptr().cast(),
            source.len(),
            PCSTR::from_raw(source_name.as_ptr().cast()),
            None,
            None,
            PCSTR::from_raw(entry.as_ptr().cast()),
            PCSTR::from_raw(target.as_ptr().cast()),
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &mut bytecode,
            Some(&mut errors),
        )
    };
    let blob = match (result, bytecode) {
        (Ok(()), Some(blob)) => blob,
        (Err(error), _) => {
            let message = errors
                .as_ref()
                .map(|blob| {
                    // SAFETY: a compiler error blob is a null-terminated ASCII message.
                    let bytes = unsafe {
                        std::slice::from_raw_parts(
                            blob.GetBufferPointer().cast::<u8>(),
                            blob.GetBufferSize(),
                        )
                    };
                    String::from_utf8_lossy(bytes).into_owned()
                })
                .unwrap_or_else(|| "no message".to_string());
            tracing::error!("frame generation: shader {name} failed: {message}");
            return Err(GpuError::failed(
                "shader compilation",
                error.code().0 as u32,
            ));
        }
        (Ok(()), None) => {
            return Err(GpuError::failed(
                "shader compilation",
                crate::domain::gpu_policy::E_FAIL,
            ))
        }
    };
    // SAFETY: the blob owns the bytecode; the slice is read only while the blob is alive.
    let bytes = unsafe {
        std::slice::from_raw_parts(blob.GetBufferPointer().cast::<u8>(), blob.GetBufferSize())
    };
    Ok(bytes.to_vec())
}

/// Creates the Stage 1 root signature: root constants at b0, an 8-SRV table at t0 and a 4-UAV table
/// at u0.
pub fn create_root_signature(device: &D3d12Device) -> Result<ID3D12RootSignature, GpuError> {
    let srv_range = D3D12_DESCRIPTOR_RANGE {
        RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        NumDescriptors: SRV_SLOTS,
        BaseShaderRegister: 0,
        RegisterSpace: 0,
        OffsetInDescriptorsFromTableStart: 0,
    };
    let uav_range = D3D12_DESCRIPTOR_RANGE {
        RangeType: D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        NumDescriptors: UAV_SLOTS,
        BaseShaderRegister: 0,
        RegisterSpace: 0,
        OffsetInDescriptorsFromTableStart: 0,
    };

    let mut parameters = [D3D12_ROOT_PARAMETER::default(); 3];
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Anonymous = D3D12_ROOT_PARAMETER_0 {
        Constants: windows::Win32::Graphics::Direct3D12::D3D12_ROOT_CONSTANTS {
            ShaderRegister: 0,
            RegisterSpace: 0,
            Num32BitValues: ROOT_CONSTANTS,
        },
    };
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].Anonymous = D3D12_ROOT_PARAMETER_0 {
        DescriptorTable: windows::Win32::Graphics::Direct3D12::D3D12_ROOT_DESCRIPTOR_TABLE {
            NumDescriptorRanges: 1,
            pDescriptorRanges: &srv_range,
        },
    };
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].Anonymous = D3D12_ROOT_PARAMETER_0 {
        DescriptorTable: windows::Win32::Graphics::Direct3D12::D3D12_ROOT_DESCRIPTOR_TABLE {
            NumDescriptorRanges: 1,
            pDescriptorRanges: &uav_range,
        },
    };
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    let description = D3D12_ROOT_SIGNATURE_DESC {
        NumParameters: parameters.len() as u32,
        pParameters: parameters.as_ptr(),
        NumStaticSamplers: 0,
        pStaticSamplers: std::ptr::null(),
        Flags: D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };

    let mut serialized: Option<ID3DBlob> = None;
    let mut errors: Option<ID3DBlob> = None;
    // SAFETY: the description and its parameter/range arrays outlive the call, which copies them.
    let result = unsafe {
        windows::Win32::Graphics::Direct3D12::D3D12SerializeRootSignature(
            &description,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &mut serialized,
            Some(&mut errors),
        )
    };
    let blob = match (result, serialized) {
        (Ok(()), Some(blob)) => blob,
        (Err(error), _) => {
            tracing::error!(
                "frame generation: root signature serialization failed ({:#010x})",
                error.code().0
            );
            return Err(GpuError::failed(
                "root signature serialization",
                error.code().0 as u32,
            ));
        }
        (Ok(()), None) => {
            return Err(GpuError::failed(
                "root signature serialization",
                crate::domain::gpu_policy::E_FAIL,
            ))
        }
    };
    // SAFETY: the blob owns the serialized bytes; the slice is read only while it is alive.
    let bytes = unsafe {
        std::slice::from_raw_parts(blob.GetBufferPointer().cast::<u8>(), blob.GetBufferSize())
    };
    device.create_root_signature(bytes)
}

/// Writes SRV/UAV descriptors into one block of the heap.
pub struct DescriptorBlocks<'a> {
    device: &'a ID3D12Device,
    heap: &'a ID3D12DescriptorHeap,
    increment: u32,
    block: u32,
}

impl<'a> DescriptorBlocks<'a> {
    pub fn new(device: &'a ID3D12Device, heap: &'a ID3D12DescriptorHeap, increment: u32) -> Self {
        Self {
            device,
            heap,
            increment,
            block: 0,
        }
    }

    pub fn begin(&mut self, index: u32) {
        self.block = index;
    }

    pub fn write_srv(&self, slot: u32, resource: &ID3D12Resource, format: PixelFormat) {
        let description = D3D12_SHADER_RESOURCE_VIEW_DESC {
            Format: dxgi_format(format),
            ViewDimension: D3D12_SRV_DIMENSION_TEXTURE2D,
            Shader4ComponentMapping: D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 {
                Texture2D: D3D12_TEX2D_SRV {
                    MipLevels: 1,
                    ..Default::default()
                },
            },
        };
        let index = u64::from(self.block) * u64::from(BLOCK_SLOTS) + u64::from(slot);
        let handle = self.cpu_handle(index);
        // SAFETY: the descriptor heap has `HEAP_SLOTS` entries, so `index` is in range, and the
        // resource is live for the duration of the call.
        unsafe {
            self.device
                .CreateShaderResourceView(resource, Some(&description), handle)
        };
    }

    /// The LUT buffer view at `t7` of the current block (the warp pass's binding).
    pub fn write_lut_srv(&self, resource: &ID3D12Resource) {
        let description = D3D12_SHADER_RESOURCE_VIEW_DESC {
            Format: DXGI_FORMAT_R32_FLOAT,
            ViewDimension: D3D12_SRV_DIMENSION_BUFFER,
            Shader4ComponentMapping: D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            Anonymous: D3D12_SHADER_RESOURCE_VIEW_DESC_0 {
                Buffer: D3D12_BUFFER_SRV {
                    NumElements: LUT_TEXELS,
                    ..Default::default()
                },
            },
        };
        // The LUT lives at slot 7 of the current block.
        let index = u64::from(self.block) * u64::from(BLOCK_SLOTS) + 7;
        let handle = self.cpu_handle(index);
        // SAFETY: the heap has room for the slot and the LUT buffer is live.
        unsafe {
            self.device
                .CreateShaderResourceView(resource, Some(&description), handle)
        };
    }

    pub fn write_uav(&self, slot: u32, resource: &ID3D12Resource, format: PixelFormat) {
        let description = D3D12_UNORDERED_ACCESS_VIEW_DESC {
            Format: dxgi_format(format),
            ViewDimension: D3D12_UAV_DIMENSION_TEXTURE2D,
            Anonymous: D3D12_UNORDERED_ACCESS_VIEW_DESC_0 {
                Texture2D: D3D12_TEX2D_UAV::default(),
            },
        };
        let index =
            u64::from(self.block) * u64::from(BLOCK_SLOTS) + u64::from(SRV_SLOTS) + u64::from(slot);
        let handle = self.cpu_handle(index);
        // SAFETY: the heap has room for the slot and the resource is live for the call.
        unsafe {
            self.device
                .CreateUnorderedAccessView(resource, None, Some(&description), handle)
        };
    }

    fn cpu_handle(&self, index: u64) -> D3D12_CPU_DESCRIPTOR_HANDLE {
        // SAFETY: a pure query on a live heap.
        let base = unsafe { self.heap.GetCPUDescriptorHandleForHeapStart() };
        D3D12_CPU_DESCRIPTOR_HANDLE {
            ptr: base.ptr + index as usize * self.increment as usize,
        }
    }

    /// The GPU handle of a block's SRV table.
    pub fn gpu_srv_table(&self, block: u32) -> D3D12_GPU_DESCRIPTOR_HANDLE {
        // SAFETY: a pure query on a live heap.
        let base = unsafe { self.heap.GetGPUDescriptorHandleForHeapStart() };
        D3D12_GPU_DESCRIPTOR_HANDLE {
            ptr: base.ptr + u64::from(block) * u64::from(BLOCK_SLOTS) * u64::from(self.increment),
        }
    }

    /// The GPU handle of a block's UAV table (the SRV table plus `SRV_SLOTS`).
    pub fn gpu_uav_table(&self, block: u32) -> D3D12_GPU_DESCRIPTOR_HANDLE {
        let mut handle = self.gpu_srv_table(block);
        handle.ptr += u64::from(SRV_SLOTS) * u64::from(self.increment);
        handle
    }
}

/// The root-constant words with the HUD composite weight written into `padding[0]`.
pub fn constant_words(constants: &Constants, composite_weight: f32) -> [u32; 24] {
    let mut with_weight = *constants;
    with_weight.padding[0] = composite_weight;
    with_weight.as_u32_words()
}
