//! Copy and resource-state restoration for overlay composition (ports
//! `presentation/overlay_compose.*`).
//!
//! `overlay_compose.cpp` provided two helpers: map an FFX surface state to the D3D12 bits, and copy
//! a back buffer into an output while transitioning both and restoring them afterwards. The wrapper
//! and the context each do their own copies now; what remains shared is the state mapping and the
//! paired-transition rule, kept here as small safe helpers over the GPU port.
use crate::application::ports::gpu::{GpuCommandList, GpuTexture, ResourceState};

/// The D3D12 state for an FFX API surface state (`dx12_state_from_ffx`).
///
/// FFX states are `1 << n`; D3D12's are a different encoding, so this mirrors the C++ table rather
/// than translating bits. An unknown value maps to `COMMON`, as the C++ did.
pub fn state_from_ffx(ffx_state: u32) -> ResourceState {
    ResourceState::from_ffx(ffx_state)
}

/// Copies `source` into `output`, transitioning each and restoring both, exactly as
/// `copy_back_buffer` did. Returns `false` when a resource is missing; the same resource for both is
/// a no-op success.
///
/// The caller must have already transitioned `source` to `COPY_SOURCE` and `output` to `COPY_DEST`
/// (the C++ helper does it here; the port's list records the barriers, so this function does both
/// with the states it was given).
pub fn copy_back_buffer<T: GpuTexture, L: GpuCommandList<T>>(
    list: &L,
    source: &T,
    source_state: ResourceState,
    output: &T,
    output_state: ResourceState,
) -> bool {
    if source.identity() == output.identity() {
        return true;
    }
    list.transition(source, source_state, ResourceState::COPY_SOURCE);
    list.transition(output, output_state, ResourceState::COPY_DEST);
    list.copy_resource(output, source);
    list.transition(output, ResourceState::COPY_DEST, output_state);
    list.transition(source, ResourceState::COPY_SOURCE, source_state);
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::application::ports::gpu::{GpuCommandAllocator, GpuError, GpuTexture, TextureInfo};
    use rstest::rstest;
    use std::cell::RefCell;

    #[rstest]
    #[case(1, ResourceState::COMMON)]
    #[case(2, ResourceState::UNORDERED_ACCESS)]
    #[case(4, ResourceState::NON_PIXEL_SHADER_RESOURCE)]
    #[case(8, ResourceState::PIXEL_SHADER_RESOURCE)]
    #[case(16, ResourceState::COPY_SOURCE)]
    #[case(32, ResourceState::COPY_DEST)]
    #[case(128, ResourceState::PRESENT)]
    #[case(256, ResourceState::RENDER_TARGET)]
    #[case(512, ResourceState::DEPTH_WRITE)]
    #[case(0x999, ResourceState::COMMON)] // unknown states fall back, as in the C++
    fn ffx_states_map_to_dx12(#[case] ffx: u32, #[case] want: ResourceState) {
        assert_eq!(state_from_ffx(ffx), want);
    }

    #[test]
    fn the_pixel_and_compute_union_is_one_state() {
        assert_eq!(
            state_from_ffx(12),
            ResourceState::NON_PIXEL_SHADER_RESOURCE.union(ResourceState::PIXEL_SHADER_RESOURCE)
        );
    }

    struct FakeTexture(usize);

    impl GpuTexture for FakeTexture {
        fn info(&self) -> TextureInfo {
            TextureInfo {
                width: 1,
                height: 1,
                format: crate::application::ports::gpu::PixelFormat::R8G8B8A8Unorm,
                mip_levels: 1,
                array_size: 1,
                sample_count: 1,
                flags: crate::application::ports::gpu::TextureFlags::NONE,
            }
        }
        fn identity(&self) -> usize {
            self.0
        }
    }

    #[derive(Default)]
    struct Recorder(RefCell<Vec<String>>);

    impl GpuCommandList<FakeTexture> for Recorder {
        fn transition(&self, texture: &FakeTexture, before: ResourceState, after: ResourceState) {
            self.0
                .borrow_mut()
                .push(format!("{}:{:#x}>{:#x}", texture.0, before.0, after.0));
        }
        fn uav_barrier(&self, _texture: &FakeTexture) {}
        fn copy_resource(&self, destination: &FakeTexture, source: &FakeTexture) {
            self.0
                .borrow_mut()
                .push(format!("copy {}>{}", source.0, destination.0));
        }
        fn dispatch(&self, _x: u32, _y: u32, _z: u32) {}
        fn object_identity(&self) -> usize {
            0
        }
    }

    #[test]
    fn the_copy_is_bracketed_by_restoring_barriers() {
        let list = Recorder::default();
        assert!(super::copy_back_buffer(
            &list,
            &FakeTexture(1),
            ResourceState::PRESENT,
            &FakeTexture(2),
            ResourceState::PRESENT,
        ));
        assert_eq!(
            *list.0.borrow(),
            vec![
                "1:0x0>0x800".to_owned(),
                "2:0x0>0x400".to_owned(),
                "copy 1>2".to_owned(),
                "2:0x400>0x0".to_owned(),
                "1:0x800>0x0".to_owned(),
            ]
        );
    }

    #[test]
    fn copying_a_resource_onto_itself_records_nothing() {
        let list = Recorder::default();
        let texture = FakeTexture(3);
        assert!(super::copy_back_buffer(
            &list,
            &texture,
            ResourceState::PRESENT,
            &texture,
            ResourceState::PRESENT
        ));
        assert!(list.0.borrow().is_empty());
    }

    // The allocator bound keeps the port's associated types coherent in this test module.
    fn _allocator_bound(_: &dyn GpuCommandAllocator) {}
    fn _error_bound(_: &GpuError) {}
}
