//! Safe `pNext`-chain walker, descriptor conversion and the per-call log lines of
//! `abi/describe.cpp`.
//!
//! Every raw pointer the game hands Sedici is read only here. Each read carries a `// SAFETY:`
//! comment and is guarded by [`readable`] where a length is known, so a truncated or dead pointer
//! becomes a missing descriptor instead of a crash. The walk is bounded to [`MAX_CHAIN_ITEMS`]
//! items and stops at the first address it has already seen, exactly like the C++ `walk_chain`.
//!
//! The converters flatten the chain into [`crate::application::ffx_model`] values; the raw types
//! stay in this module and [`super::types`].

// -------------------------------------------------------------------------------------------------
// describe_call: the call log of describe.cpp
// -------------------------------------------------------------------------------------------------

/// The five exports, in the C++ counter order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExportKind {
    Create = 0,
    Destroy = 1,
    Configure = 2,
    Query = 3,
    Dispatch = 4,
}

impl ExportKind {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Create => "ffxCreateContext",
            Self::Destroy => "ffxDestroyContext",
            Self::Configure => "ffxConfigure",
            Self::Query => "ffxQuery",
            Self::Dispatch => "ffxDispatch",
        }
    }

    const fn index(self) -> usize {
        self as usize
    }
}

/// `describe.cpp`'s policy: the first 50 calls of each export always log in full.
const FULL_LOG_CALL_LIMIT: u64 = 50;

struct CallState {
    calls: [u64; 5],
    seen_types: [BTreeSet<u64>; 5],
}

impl CallState {
    const fn new() -> Self {
        Self {
            calls: [0; 5],
            seen_types: [
                BTreeSet::new(),
                BTreeSet::new(),
                BTreeSet::new(),
                BTreeSet::new(),
                BTreeSet::new(),
            ],
        }
    }
}

static CALL_STATE: Mutex<CallState> = Mutex::new(CallState::new());

/// Logs one export call under the level/novelty policy, re-reading `desc` only when it will log.
/// `context` is the handle value the call saw (zero for null).
pub fn describe_call(kind: ExportKind, desc: *const ffxApiHeader, result: ReturnCode, context: usize) {
    // The counters advance even when nothing is printed, so `#N` matches the C++ numbering.
    let types = walk_chain(desc, |_| {});
    let (count, novel) = {
        // A poisoned lock only means another thread panicked while counting; the counters are
        // advisory, so recover the map rather than fail a game call.
        let mut state = CALL_STATE.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let index = kind.index();
        state.calls[index] += 1;
        let count = state.calls[index];
        let novel = types
            .iter()
            .map(|ty| state.seen_types[index].insert(*ty))
            .fold(false, |seen, inserted| seen | inserted);
        (count, novel)
    };

    let verbose = tracing::enabled!(tracing::Level::DEBUG);
    let interesting = verbose || count <= FULL_LOG_CALL_LIMIT || novel || result != ReturnCode::Ok;
    if !interesting || !tracing::enabled!(tracing::Level::INFO) {
        return;
    }

    let mut out = format!(
        "{} #{} ctx={} result={} ({}) chain=",
        kind.name(),
        count,
        ptr(context),
        result.as_u32(),
        result.name()
    );
    if types.is_empty() {
        out.push_str("none");
    } else {
        for (i, ty) in types.iter().enumerate() {
            if i > 0 {
                out.push_str(" -> ");
            }
            out.push_str(&format!("{} ({})", hx(*ty), type_name(*ty)));
        }
    }

    match kind {
        ExportKind::Create => describe_create(desc, &mut out),
        ExportKind::Configure => describe_configure(desc, &mut out),
        ExportKind::Query => describe_query(desc, &mut out, result),
        ExportKind::Dispatch => describe_dispatch(desc, &mut out),
        ExportKind::Destroy => {}
    }

    // One event per call: the C++ writes the chain header and the indented per-desc lines as a
    // single message with embedded newlines, and the log shape is part of the contract.
    tracing::info!("{out}");
}

fn item_header(out: &mut String, item: &ffxApiHeader) {
    out.push_str(&format!("\n  {} ({})", hx(item.type_), type_name(item.type_)));
}

fn append_prepare(out: &mut String, d: &model::Prepare, reset: bool) {
    out.push_str(&format!(
        " frameID={} flags={} cmd={} render={}x{} jitter=({},{}) mvScale=({},{}) frameTimeDelta={} reset={} near={} far={} fov={} viewToMeters={} depth={} mv={}",
        d.frame_id,
        decode_dispatch_flags(d.flags),
        ptr(d.command_list),
        d.render_size.width,
        d.render_size.height,
        fl(d.jitter.x),
        fl(d.jitter.y),
        fl(d.motion_vector_scale.x),
        fl(d.motion_vector_scale.y),
        fl(d.frame_time_delta),
        tf(reset),
        fl(d.camera_near),
        fl(d.camera_far),
        fl(d.camera_fov_vertical),
        fl(d.view_space_to_meters),
        resource_model_set(&d.depth),
        resource_model_set(&d.motion_vectors),
    ));
}

fn resource_model(value: &model::Resource) -> String {
    let d = &value.description;
    let mut out = format!("res={} format={}", ptr(value.handle), surface_format(d.format.0));
    if matches!(d.kind, model::ResourceKind::Buffer) {
        out.push_str(&format!(" bytes={} stride={}", d.width, d.height));
    } else {
        out.push_str(&format!(" size={}x{}", d.width, d.height));
        if d.depth > 1 {
            out.push_str(&format!(" depth={}", d.depth));
        }
    }
    out.push_str(&format!(" state={} usage={}", hx(value.state as u64), hx(d.usage as u64)));
    out
}

fn resource_model_set(value: &model::Resource) -> String {
    format!("{{{}}}", resource_model(value))
}

fn rect_model_text(value: &model::Rect) -> String {
    format!("{},{},{}x{}", value.left, value.top, value.width, value.height)
}

fn append_swapchain_desc(out: &mut String, desc: Option<&model::SwapchainDesc>) {
    let Some(d) = desc else {
        out.push_str(" desc=null");
        return;
    };
    out.push_str(&format!(
        " desc{{buffers={} size={}x{} format={} refresh={}/{} usage={} swapEffect={} flags={} windowed={} outputWindow={}}}",
        d.buffer_count,
        d.width,
        d.height,
        dxgi_format(d.format),
        d.refresh_numerator,
        d.refresh_denominator,
        hx(d.usage as u64),
        d.swap_effect,
        hx(d.flags as u64),
        tf(d.windowed),
        ptr(d.output_window),
    ));
}

fn append_swapchain_desc1(out: &mut String, desc: Option<&model::SwapchainDesc1>) {
    let Some(d) = desc else {
        out.push_str(" desc=null");
        return;
    };
    out.push_str(&format!(
        " desc{{buffers={} size={}x{} format={} stereo={} sample={}/{} usage={} scaling={} swapEffect={} alpha={} flags={}}}",
        d.buffer_count,
        d.width,
        d.height,
        dxgi_format(d.format),
        tf(d.stereo),
        d.sample_count,
        d.sample_quality,
        hx(d.usage as u64),
        d.scaling,
        d.swap_effect,
        d.alpha_mode,
        hx(d.flags as u64),
    ));
}

fn append_swapchain_ptr(out: &mut String, slot: usize, swapchain: usize) {
    out.push_str(&format!(" swapchainPtr={}", ptr(slot)));
    if slot != 0 {
        out.push_str(&format!(" swapchain={}", ptr(swapchain)));
    }
}

fn version_text(version: u32) -> String {
    format!(
        "{} ({}.{}.{})",
        hx(version as u64),
        version >> 22,
        (version >> 12) & 0x3ff,
        version & 0xfff
    )
}

/// Creates and configures share the per-type walk; the caller supplies the body printer. The
/// C++ prints every chain item in order, so the flattening is done once and the walk repeats
/// only to keep that order.
fn describe_create(root: *const ffxApiHeader, out: &mut String) {
    let desc = convert_create(root);
    walk_chain(root, |item| {
        item_header(out, item);
        match item.type_ {
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION => {
                if let Some(d) = desc.frame_generation {
                    out.push_str(&format!(
                        " flags={} display={}x{} maxRender={}x{} backbufferFormat={}",
                        decode_create_flags(d.flags),
                        d.display_size.width,
                        d.display_size.height,
                        d.max_render_size.width,
                        d.max_render_size.height,
                        surface_format(d.back_buffer_format.0),
                    ));
                }
            }
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS => {
                if let Some(d) = desc.frame_generation_hudless {
                    out.push_str(&format!(
                        " hudlessFormat={}",
                        surface_format(d.hudless_back_buffer_format.0)
                    ));
                }
            }
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION => {
                if let Some(version) = desc.frame_generation_version {
                    out.push_str(&format!(" version={}", version_text(version)));
                }
            }
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12 => {
                if let Some(d) = desc.swapchain_wrap {
                    out.push_str(" variant=wrap");
                    append_swapchain_ptr(out, d.swapchain_slot, d.swapchain);
                    out.push_str(&format!(" gameQueue={}", ptr(d.game_queue)));
                }
            }
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12 => {
                if let Some(d) = desc.swapchain_new {
                    out.push_str(" variant=new");
                    append_swapchain_ptr(out, d.swapchain_slot, 0);
                    append_swapchain_desc(out, d.desc.as_ref());
                    out.push_str(&format!(
                        " factory={} gameQueue={}",
                        ptr(d.dxgi_factory),
                        ptr(d.game_queue)
                    ));
                }
            }
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12 => {
                if let Some(d) = desc.swapchain_for_hwnd {
                    out.push_str(" variant=forHwnd");
                    append_swapchain_ptr(out, d.swapchain_slot, 0);
                    out.push_str(&format!(" hwnd={}", ptr(d.hwnd)));
                    append_swapchain_desc1(out, d.desc.as_ref());
                    out.push_str(&format!(
                        " fullscreenDesc={} factory={} gameQueue={}",
                        ptr(d.fullscreen_desc),
                        ptr(d.dxgi_factory),
                        ptr(d.game_queue)
                    ));
                }
            }
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12 => {
                if let Some(version) = desc.swapchain_version {
                    out.push_str(&format!(" version={}", version_text(version)));
                }
            }
            FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 => {
                if let Some(d) = desc.backend_dx12 {
                    out.push_str(&format!(" device={}", ptr(d.device)));
                }
            }
            FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12_ALLOCATION_CALLBACKS => {
                if let Some(d) = desc.backend_dx12_allocation_callbacks {
                    out.push_str(&format!(
                        " alloc={} dealloc={} heapAlloc={} heapDealloc={} cbAlloc={}",
                        ptr(d.resource_allocator),
                        ptr(d.resource_deallocator),
                        ptr(d.heap_allocator),
                        ptr(d.heap_deallocator),
                        ptr(d.constant_buffer_allocator)
                    ));
                }
            }
            FFX_API_DESC_TYPE_OVERRIDE_VERSION => {
                if let Some(id) = desc.override_version {
                    out.push_str(&format!(" versionId={}", hx(id)));
                }
            }
            _ => {}
        }
    });
}

fn describe_configure(root: *const ffxApiHeader, out: &mut String) {
    let desc = convert_configure(root);
    walk_chain(root, |item| {
        item_header(out, item);
        match item.type_ {
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION => {
                if let Some(d) = desc.frame_generation {
                    out.push_str(&format!(
                        " swapChain={} presentCallback={} presentCtx={} generationCallback={} generationCtx={} frameGenerationEnabled={} allowAsyncWorkloads={} HUDLessColor={} HUDLess={} flags={} onlyPresentGenerated={} generationRect={} frameID={}",
                        ptr(d.swapchain),
                        set(d.present_callback.is_some()),
                        ptr(d.present_callback_user_context),
                        set(d.frame_generation_callback.is_some()),
                        ptr(d.frame_generation_callback_user_context),
                        tf(d.frame_generation_enabled),
                        tf(d.allow_async_workloads),
                        yes_no(!d.hud_less_color.is_empty()),
                        resource_model_set(&d.hud_less_color),
                        decode_dispatch_flags(d.flags),
                        tf(d.only_present_generated),
                        rect_model_text(&d.generation_rect),
                        d.frame_id,
                    ));
                }
            }
            FFX_API_FRAME_GENERATION_CONFIG => {
                if let Some(d) = desc.legacy_frame_generation {
                    out.push_str(&format!(
                        " swapChain={} presentCallback={} presentCtx={} generationCallback={} generationCtx={} frameGenerationEnabled={} allowAsyncWorkloads={} allowAsyncPresent={} HUDLessColor={} HUDLess={} flags={} onlyPresentInterpolated={} interpolationRect={} frameID={} drawDebugPacingLines={}",
                        ptr(d.swapchain),
                        set(d.present_callback.is_some()),
                        ptr(d.present_callback_context),
                        set(d.frame_generation_callback.is_some()),
                        ptr(d.frame_generation_callback_context),
                        tf(d.frame_generation_enabled),
                        tf(d.allow_async_workloads),
                        tf(d.allow_async_present),
                        yes_no(!d.hud_less_color.is_empty()),
                        resource_model_set(&d.hud_less_color),
                        decode_dispatch_flags(d.flags),
                        tf(d.only_present_interpolated),
                        rect_model_text(&d.interpolation_rect),
                        d.frame_id,
                        tf(d.draw_debug_pacing_lines),
                    ));
                }
            }
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE => {
                if let Some(d) = desc.frame_generation_key_value {
                    out.push_str(&format!(" key={}", d.key));
                    if d.key == FFX_API_CONFIGURE_FRAMEGENERATION_KEY_DEBUG_VIEW_MODE {
                        out.push_str(" (DebugViewMode)");
                    } else if d.key == FFX_API_CONFIGURE_FRAMEGENERATION_KEY_DEBUG_VIEW_FLOW_SCALE {
                        out.push_str(" (DebugViewFlowScale)");
                    }
                    out.push_str(&format!(" u64={} ptr={}", d.u64_value, ptr(d.ptr)));
                }
            }
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE => {
                if let Some(d) = desc.distortion_field {
                    out.push_str(&format!(" distortionField={}", resource_model_set(&d)));
                }
            }
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12 => {
                if let Some(d) = desc.ui_resource {
                    out.push_str(&format!(
                        " uiResource={} ui={} flags={}",
                        yes_no(!d.ui_resource.is_empty()),
                        resource_model_set(&d.ui_resource),
                        decode_ui_flags(d.flags)
                    ));
                }
            }
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12 => {
                if let Some(d) = desc.swapchain_key_value {
                    out.push_str(&format!(" key={}", d.key));
                    if d.key == FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_WAITCALLBACK {
                        out.push_str(" (WaitCallback)");
                    } else if d.key == FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING {
                        out.push_str(" (FramePacingTuning)");
                    }
                    out.push_str(&format!(" u64={} ptr={}", d.u64_value, ptr(d.ptr)));
                    if d.key == FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING {
                        match d.tuning {
                            Some(t) => out.push_str(&format!(
                                " tuning{{safetyMarginInMs={} varianceFactor={} allowHybridSpin={} hybridSpinTime={} allowWaitForSingleObjectOnFence={}}}",
                                fl(t.safety_margin_in_ms),
                                fl(t.variance_factor),
                                tf(t.allow_hybrid_spin),
                                t.hybrid_spin_time,
                                tf(t.allow_wait_for_single_object_on_fence)
                            )),
                            None => out.push_str(" tuning=null"),
                        }
                    }
                }
            }
            FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG => {
                if let Some(d) = desc.global_debug {
                    out.push_str(&format!(
                        " effectId={} messageCallback={} debugLevel={}",
                        hx(d.effect_id),
                        set(d.message_callback.is_some()),
                        d.debug_level
                    ));
                }
            }
            FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1 => {
                if let Some(d) = desc.global_debug1 {
                    out.push_str(&format!(
                        " messageCallback={} debugLevel={}",
                        set(d.message_callback.is_some()),
                        d.debug_level
                    ));
                }
            }
            FFX_API_DESC_TYPE_OVERRIDE_VERSION => {
                if let Some(id) = desc.override_version {
                    out.push_str(&format!(" versionId={}", hx(id)));
                }
            }
            _ => {}
        }
    });
}

fn append_camera(out: &mut String, label: &str, value: &model::Float3) {
    out.push_str(&format!(" {} =({},{},{})", label, fl(value.x), fl(value.y), fl(value.z)));
}

fn describe_dispatch(root: *const ffxApiHeader, out: &mut String) {
    let desc = convert_dispatch(root);
    walk_chain(root, |item| {
        item_header(out, item);
        match item.type_ {
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE => {
                if let Some(d) = desc.prepare_v1 {
                    append_prepare(out, &d.prepare, d.unused_reset);
                }
            }
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 => {
                if let Some(d) = desc.prepare_v2 {
                    append_prepare(out, &d.prepare, d.reset);
                    append_camera(out, "camPos", &d.camera_position);
                    append_camera(out, "camUp", &d.camera_up);
                    append_camera(out, "camRight", &d.camera_right);
                    append_camera(out, "camForward", &d.camera_forward);
                }
            }
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO => {
                if let Some(d) = desc.camera_info {
                    append_camera(out, "camPos", &d.camera_position);
                    append_camera(out, "camUp", &d.camera_up);
                    append_camera(out, "camRight", &d.camera_right);
                    append_camera(out, "camForward", &d.camera_forward);
                }
            }
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION => {
                if let Some(d) = desc.frame_generation {
                    out.push_str(&format!(
                        " cmd={} presentColor={} numGeneratedFrames={} reset={} transferFunction={} minLuminance={} maxLuminance={} generationRect={} frameID={}",
                        ptr(d.command_list),
                        resource_model_set(&d.present_color),
                        d.num_generated_frames,
                        tf(d.reset),
                        d.backbuffer_transfer_function,
                        fl(d.min_max_luminance[0]),
                        fl(d.min_max_luminance[1]),
                        rect_model_text(&d.generation_rect),
                        d.frame_id,
                    ));
                    for (i, output) in d.outputs.iter().enumerate() {
                        if (i as u32) < d.num_generated_frames || !output.is_empty() {
                            out.push_str(&format!(" output[{}]={}", i, resource_model_set(output)));
                        }
                    }
                }
            }
            FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT => {
                if let Some(d) = desc.present_callback {
                    out.push_str(&format!(
                        " device={} cmd={} currentBackBuffer={} currentUI={} outputSwapChainBuffer={} isGeneratedFrame={} frameID={}",
                        ptr(d.device),
                        ptr(d.command_list),
                        resource_model_set(&d.current_back_buffer),
                        resource_model_set(&d.current_ui),
                        resource_model_set(&d.output_swap_chain_buffer),
                        tf(d.is_generated_frame),
                        d.frame_id,
                    ));
                }
            }
            FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA => {
                if let Some(value) = desc.present_premul_alpha {
                    out.push_str(&format!(" usePremulAlpha={}", tf(value)));
                }
            }
            _ => {}
        }
    });
}

fn describe_query(root: *const ffxApiHeader, out: &mut String, result: ReturnCode) {
    let desc = convert_query(root);
    walk_chain(root, |item| {
        item_header(out, item);
        match item.type_ {
            FFX_API_QUERY_DESC_TYPE_GET_VERSIONS => {
                if let Some(d) = desc.get_versions {
                    out.push_str(&format!(
                        " createDescType={} device={} outputCount={}",
                        hx(d.create_desc_type),
                        ptr(d.device),
                        ptr(d.output_count_slot)
                    ));
                    if d.output_count_slot != 0 && result == ReturnCode::Ok {
                        out.push_str(&format!(" count={}", d.output_count));
                    }
                    out.push_str(&format!(
                        " versionIds={} versionNames={}",
                        ptr(d.version_ids_slot),
                        ptr(d.version_names_slot)
                    ));
                }
            }
            FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION => {
                // The provider query writes its answer into the desc, so the log reads it back.
                let d: ffxQueryGetProviderVersion = read_desc(item);
                out.push_str(&format!(
                    " versionId={} versionName={}",
                    hx(d.versionId),
                    ptr(d.versionName as usize)
                ));
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE => {
                if let Some(d) = desc.gpu_memory_usage {
                    out.push_str(&format!(" out={}", ptr(d.output_slot)));
                    if let Some(usage) = read_usage(d.output_slot, result) {
                        out.push_str(&format!(
                            " total={} aliasable={}",
                            usage.totalUsageInBytes, usage.aliasableUsageInBytes
                        ));
                    }
                }
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2 => {
                if let Some(d) = desc.gpu_memory_usage_v2 {
                    out.push_str(&format!(
                        " device={} maxRender={}x{} display={}x{} createFlags={} dispatchFlags={} backbufferFormat={} hudlessFormat={} out={}",
                        ptr(d.device),
                        d.max_render_size.width,
                        d.max_render_size.height,
                        d.display_size.width,
                        d.display_size.height,
                        decode_create_flags(d.create_flags),
                        decode_dispatch_flags(d.dispatch_flags),
                        surface_format(d.back_buffer_format.0),
                        surface_format(d.hudless_back_buffer_format.0),
                        ptr(d.output_slot),
                    ));
                }
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12 => {
                if let Some(d) = desc.interpolation_command_list {
                    out.push_str(&format!(" outCommandList={}", ptr(d.output_slot)));
                    if d.output_slot != 0 {
                        out.push_str(&format!(" commandList={}", ptr(read_pointer(d.output_slot))));
                    }
                }
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12 => {
                if let Some(d) = desc.interpolation_texture {
                    out.push_str(&format!(" outTexture={}", ptr(d.output_slot)));
                    if let Some(value) = read_texture(d.output_slot) {
                        out.push_str(&format!(" texture={}", resource_set(&value)));
                    }
                }
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12 => {
                if let Some(d) = desc.swapchain_gpu_memory_usage {
                    out.push_str(&format!(" out={}", ptr(d.output_slot)));
                }
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2 => {
                if let Some(d) = desc.swapchain_gpu_memory_usage_v2 {
                    out.push_str(&format!(
                        " device={} display={}x{} backbufferFormat={} backBufferCount={} uiSize={}x{} uiFormat={} flags={} out={}",
                        ptr(d.device),
                        d.display_size.width,
                        d.display_size.height,
                        surface_format(d.back_buffer_format.0),
                        d.back_buffer_count,
                        d.ui_resource_size.width,
                        d.ui_resource_size.height,
                        surface_format(d.ui_resource_format.0),
                        decode_ui_flags(d.flags),
                        ptr(d.output_slot),
                    ));
                }
            }
            FFX_API_DESC_TYPE_OVERRIDE_VERSION => {
                if let Some(id) = desc.override_version {
                    out.push_str(&format!(" versionId={}", hx(id)));
                }
            }
            _ => {}
        }
    });
}

fn read_usage(slot: usize, result: ReturnCode) -> Option<FfxApiEffectMemoryUsage> {
    if result != ReturnCode::Ok || !typed_readable(slot, std::mem::size_of::<FfxApiEffectMemoryUsage>()) {
        return None;
    }
    // SAFETY: the address was just found readable for a whole FfxApiEffectMemoryUsage.
    Some(unsafe { std::ptr::read_unaligned(slot as *const FfxApiEffectMemoryUsage) })
}

fn read_pointer(slot: usize) -> usize {
    if !typed_readable(slot, std::mem::size_of::<usize>()) {
        return 0;
    }
    // SAFETY: the address was just found readable for a pointer.
    unsafe { std::ptr::read_unaligned(slot as *const usize) }
}

fn read_texture(slot: usize) -> Option<FfxApiResource> {
    if !typed_readable(slot, std::mem::size_of::<FfxApiResource>()) {
        return None;
    }
    // SAFETY: the address was just found readable for a whole FfxApiResource.
    Some(unsafe { std::ptr::read_unaligned(slot as *const FfxApiResource) })
}
use std::collections::BTreeSet;
use std::sync::Mutex;

use crate::application::ffx_model::{self as model, ReturnCode};

use super::types::*;

/// The C++ chain guard: 64 items, cycles stopped by first repeat.
pub const MAX_CHAIN_ITEMS: usize = 64;

// -------------------------------------------------------------------------------------------------
// Pointer validation
// -------------------------------------------------------------------------------------------------

/// True when `ptr` names at least `len` readable bytes. Uses `VirtualQuery`, so an unmapped or
/// guard page is refused before the read rather than faulting the game's thread.
#[cfg(windows)]
pub fn readable(ptr: *const u8, len: usize) -> bool {
    use windows::Win32::System::Memory::{
        VirtualQuery, MEMORY_BASIC_INFORMATION, MEM_COMMIT, PAGE_GUARD, PAGE_NOACCESS,
    };

    if ptr.is_null() || len == 0 {
        return false;
    }
    let mut query = MEMORY_BASIC_INFORMATION::default();
    let mut address = ptr as usize;
    let end = match address.checked_add(len) {
        Some(end) => end,
        None => return false,
    };
    while address < end {
        // SAFETY: `address` is a live address from the caller and `query` is an owned, correctly
        // sized output buffer; VirtualQuery neither reads nor writes the queried range.
        let written = unsafe {
            VirtualQuery(Some(address as *const _), &mut query, std::mem::size_of::<MEMORY_BASIC_INFORMATION>())
        };
        if written == 0 {
            return false;
        }
        let base = query.BaseAddress as usize;
        let region_end = base.saturating_add(query.RegionSize);
        if query.State != MEM_COMMIT
            || query.Protect == PAGE_NOACCESS
            || query.Protect.contains(PAGE_GUARD)
            || region_end <= address
        {
            return false;
        }
        address = region_end;
    }
    true
}

/// The probe above is Windows-specific; elsewhere only the null check is available.
#[cfg(not(windows))]
pub fn readable(ptr: *const u8, len: usize) -> bool {
    !ptr.is_null() && len > 0
}

fn header_readable(header: *const ffxApiHeader) -> bool {
    !header.is_null() && readable(header.cast::<u8>(), std::mem::size_of::<ffxApiHeader>())
}

/// True when `value` names at least `size` readable bytes. `0` is never a readable pointer.
fn typed_readable(value: usize, size: usize) -> bool {
    readable(value as *const u8, size)
}

/// Reads one descriptor of type `T` from a chain item. The chain item was itself read from a
/// readable header, so its address is a live allocation; the header says `T` is what follows only
/// because the game set `type` accordingly.
fn desc_at<T>(item: &ffxApiHeader) -> T {
    // SAFETY: `item` points at a live chain item whose header was readable; the C ABI guarantees
    // that a descriptor carrying `item.type` begins with that header and extends at least as far
    // as the matching `T`. Every `T` here is `repr(C)` and starts with `ffxApiHeader`.
    unsafe { std::ptr::read_unaligned((item as *const ffxApiHeader).cast::<T>()) }
}

// -------------------------------------------------------------------------------------------------
// The walker
// -------------------------------------------------------------------------------------------------

/// Walks `root`'s chain, calling `visit` for each readable header in order. Stops on a null link,
/// a repeat or [`MAX_CHAIN_ITEMS`]. Returns the types seen, in order.
pub fn walk_chain(root: *const ffxApiHeader, mut visit: impl FnMut(&ffxApiHeader)) -> Vec<u64> {
    let mut seen: BTreeSet<usize> = BTreeSet::new();
    let mut types = Vec::new();
    let mut current = root;
    while !current.is_null() && seen.len() < MAX_CHAIN_ITEMS {
        let address = current as usize;
        if !seen.insert(address) {
            break; // a cycle
        }
        if !header_readable(current) {
            break;
        }
        // SAFETY: `current` was just found readable for one header and is non-null. The reference
        // borrows the game's own descriptor, not a copy, so a caller that reads the whole struct
        // through `item` sees the bytes that follow the header.
        let header = unsafe { &*current };
        types.push(header.type_);
        visit(header);
        current = header.pNext;
    }
    types
}

// -------------------------------------------------------------------------------------------------
// Formatting helpers, matching describe.cpp's output
// -------------------------------------------------------------------------------------------------

fn hx(value: u64) -> String {
    format!("0x{value:x}")
}

fn ptr(value: usize) -> String {
    if value == 0 {
        "null".to_owned()
    } else {
        format!("0x{value:x}")
    }
}

fn fl(value: f32) -> String {
    format!("{value:.4}")
}

fn tf(value: bool) -> &'static str {
    if value {
        "true"
    } else {
        "false"
    }
}

fn set(value: bool) -> &'static str {
    if value {
        "set"
    } else {
        "none"
    }
}

fn yes_no(value: bool) -> &'static str {
    if value {
        "yes"
    } else {
        "no"
    }
}

fn decode_bits(value: u32, names: &[(u32, &str)]) -> String {
    if value == 0 {
        return "none".to_owned();
    }
    let mut out = String::new();
    let mut remaining = value;
    for &(bit, name) in names {
        if value & bit == 0 {
            continue;
        }
        if !out.is_empty() {
            out.push('|');
        }
        out.push_str(name);
        remaining &= !bit;
    }
    if remaining != 0 {
        if !out.is_empty() {
            out.push('|');
        }
        out.push_str(&hx(remaining as u64));
    }
    out
}

fn decode_create_flags(flags: u32) -> String {
    decode_bits(
        flags,
        &[
            (1 << 0, "AsyncWorkloads"),
            (1 << 1, "DisplayResMotionVectors"),
            (1 << 2, "MVJitterCancellation"),
            (1 << 3, "DepthInverted"),
            (1 << 4, "DepthInfinite"),
            (1 << 5, "HDR"),
            (1 << 6, "DebugChecking"),
        ],
    )
}

fn decode_dispatch_flags(flags: u32) -> String {
    decode_bits(
        flags,
        &[
            (1 << 0, "DebugTearLines"),
            (1 << 1, "DebugResetIndicators"),
            (1 << 2, "DebugView"),
            (1 << 3, "NoSwapchainContextNotify"),
            (1 << 4, "DebugPacingLines"),
            (1 << 5, "Reserved1"),
            (1 << 6, "Reserved2"),
        ],
    )
}

fn decode_ui_flags(flags: u32) -> String {
    decode_bits(flags, &[(1 << 0, "PremulAlpha"), (1 << 1, "InternalUiDoubleBuffer")])
}

fn surface_format_name(format: u32) -> Option<&'static str> {
    Some(match format {
        0 => "UNKNOWN",
        1 => "R32G32B32A32_TYPELESS",
        2 => "R32G32B32A32_UINT",
        3 => "R32G32B32A32_FLOAT",
        4 => "R16G16B16A16_FLOAT",
        5 => "R32G32B32_FLOAT",
        6 => "R32G32_FLOAT",
        7 => "R8_UINT",
        8 => "R32_UINT",
        9 => "R8G8B8A8_TYPELESS",
        10 => "R8G8B8A8_UNORM",
        11 => "R8G8B8A8_SNORM",
        12 => "R8G8B8A8_SRGB",
        13 => "B8G8R8A8_TYPELESS",
        14 => "B8G8R8A8_UNORM",
        15 => "B8G8R8A8_SRGB",
        16 => "R11G11B10_FLOAT",
        17 => "R10G10B10A2_UNORM",
        18 => "R16G16_FLOAT",
        19 => "R16G16_UINT",
        20 => "R16G16_SINT",
        21 => "R16_FLOAT",
        22 => "R16_UINT",
        23 => "R16_UNORM",
        24 => "R16_SNORM",
        25 => "R8_UNORM",
        26 => "R8G8_UNORM",
        27 => "R8G8_UINT",
        28 => "R32_FLOAT",
        29 => "R9G9B9E5_SHAREDEXP",
        30 => "R16G16B16A16_TYPELESS",
        31 => "R32G32_TYPELESS",
        32 => "R10G10B10A2_TYPELESS",
        33 => "R16G16_TYPELESS",
        34 => "R16_TYPELESS",
        35 => "R8_TYPELESS",
        36 => "R8G8_TYPELESS",
        37 => "R32_TYPELESS",
        38 => "R32G32_UINT",
        _ => return None,
    })
}

fn surface_format(format: u32) -> String {
    match surface_format_name(format) {
        Some(name) => format!("{name}({})", hx(format as u64)),
        None => hx(format as u64),
    }
}

fn dxgi_format_name(format: u32) -> Option<&'static str> {
    Some(match format {
        0 => "UNKNOWN",
        2 => "R32G32B32A32_FLOAT",
        10 => "R16G16B16A16_FLOAT",
        16 => "R32G32_FLOAT",
        24 => "R10G10B10A2_UNORM",
        26 => "R11G11B10_FLOAT",
        28 => "R8G8B8A8_UNORM",
        29 => "R8G8B8A8_UNORM_SRGB",
        87 => "B8G8R8A8_UNORM",
        91 => "B8G8R8A8_UNORM_SRGB",
        _ => return None,
    })
}

fn dxgi_format(format: u32) -> String {
    match dxgi_format_name(format) {
        Some(name) => format!("{name}({})", hx(format as u64)),
        None => hx(format as u64),
    }
}

fn resource(value: &FfxApiResource) -> String {
    if value.resource.is_null() {
        return "none".to_owned();
    }
    let d = &value.description;
    let mut out = format!(
        "res={} format={}",
        ptr(value.resource as usize),
        surface_format(d.format)
    );
    if d.type_ == FFX_API_RESOURCE_TYPE_BUFFER {
        out.push_str(&format!(" bytes={} stride={}", d.width_or_size, d.height_or_stride));
    } else {
        out.push_str(&format!(" size={}x{}", d.width_or_size, d.height_or_stride));
        if d.depth_or_alignment > 1 {
            out.push_str(&format!(" depth={}", d.depth_or_alignment));
        }
    }
    out.push_str(&format!(" state={} usage={}", hx(value.state as u64), hx(d.usage as u64)));
    out
}

fn resource_set(value: &FfxApiResource) -> String {
    format!("{{{}}}", resource(value))
}

fn type_name(type_: u64) -> &'static str {
    match type_ {
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION => "FrameGenerationCreate",
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS => "FrameGenerationCreateHudless",
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION => "FrameGenerationCreateVersion",
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12 => "FrameGenerationSwapChainWrapDX12",
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12 => "FrameGenerationSwapChainNewDX12",
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12 => "FrameGenerationSwapChainForHwndDX12",
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12 => "FrameGenerationSwapChainVersionDX12",
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION => "FrameGenerationConfigure",
        FFX_API_FRAME_GENERATION_CONFIG => "FrameGenerationConfig",
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE => "FrameGenerationConfigureKeyValue",
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE => "FrameGenerationDistortionResource",
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12 => "SwapChainRegisterUiResourceDX12",
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12 => "SwapChainConfigureKeyValueDX12",
        FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE => "FrameGenerationPrepare",
        FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 => "FrameGenerationPrepareV2",
        FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO => "FrameGenerationPrepareCameraInfo",
        FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION => "FrameGenerationDispatch",
        FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12 => "SwapChainWaitForPresentsDX12",
        FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT => "FrameGenerationPresentCallback",
        FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA => "FrameGenerationPresentPremulAlpha",
        FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE => "FrameGenerationGpuMemoryUsage",
        FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2 => "FrameGenerationGpuMemoryUsageV2",
        FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12 => "SwapChainInterpolationCommandListDX12",
        FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12 => "SwapChainInterpolationTextureDX12",
        FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12 => "SwapChainGpuMemoryUsageDX12",
        FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2 => "SwapChainGpuMemoryUsageDX12V2",
        FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 => "BackendDX12",
        FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12_ALLOCATION_CALLBACKS => "BackendDX12AllocationCallbacks",
        FFX_API_DESC_TYPE_OVERRIDE_VERSION => "OverrideVersion",
        FFX_API_QUERY_DESC_TYPE_GET_VERSIONS => "GetVersions",
        FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION => "GetProviderVersion",
        FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG => "GlobalDebug",
        FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1 => "GlobalDebug1",
        _ => "Unknown",
    }
}

// -------------------------------------------------------------------------------------------------
// Conversion into the safe model
// -------------------------------------------------------------------------------------------------

fn convert_resource(value: &FfxApiResource) -> model::Resource {
    let d = &value.description;
    let kind = match d.type_ {
        FFX_API_RESOURCE_TYPE_BUFFER => model::ResourceKind::Buffer,
        FFX_API_RESOURCE_TYPE_TEXTURE1D => model::ResourceKind::Texture1D,
        FFX_API_RESOURCE_TYPE_TEXTURE2D => model::ResourceKind::Texture2D,
        FFX_API_RESOURCE_TYPE_TEXTURE_CUBE => model::ResourceKind::TextureCube,
        FFX_API_RESOURCE_TYPE_TEXTURE3D => model::ResourceKind::Texture3D,
        other => model::ResourceKind::Unknown(other),
    };
    model::Resource {
        handle: value.resource as usize,
        description: model::ResourceDescription {
            kind,
            format: model::SurfaceFormat(d.format),
            width: d.width_or_size,
            height: d.height_or_stride,
            depth: d.depth_or_alignment,
            mip_count: d.mipCount,
            flags: d.flags,
            usage: d.usage,
        },
        state: value.state,
    }
}

fn dims(value: &FfxApiDimensions2D) -> model::Dimensions {
    model::Dimensions { width: value.width, height: value.height }
}

fn float2(value: &FfxApiFloatCoords2D) -> model::Float2 {
    model::Float2 { x: value.x, y: value.y }
}

fn float3(value: &[f32; 3]) -> model::Float3 {
    model::Float3 { x: value[0], y: value[1], z: value[2] }
}

fn rect_model(value: &FfxApiRect2D) -> model::Rect {
    model::Rect { left: value.left, top: value.top, width: value.width, height: value.height }
}

fn callback(value: *mut std::ffi::c_void) -> model::Callback {
    (!value.is_null()).then_some(value as usize)
}

/// Reads a `T` through a chain-item pointer that is known to hold `T`.
fn read_desc<T: Copy>(item: &ffxApiHeader) -> T {
    desc_at(item)
}

fn read_swapchain_desc(value: *const DXGI_SWAP_CHAIN_DESC) -> Option<model::SwapchainDesc> {
    let address = value as usize;
    if !typed_readable(address, std::mem::size_of::<DXGI_SWAP_CHAIN_DESC>()) {
        return None;
    }
    // SAFETY: the address was just found readable for a whole DXGI_SWAP_CHAIN_DESC.
    let d = unsafe { std::ptr::read_unaligned(value) };
    Some(model::SwapchainDesc {
        buffer_count: d.BufferCount,
        width: d.BufferDesc.Width,
        height: d.BufferDesc.Height,
        format: d.BufferDesc.Format,
        refresh_numerator: d.BufferDesc.RefreshRate.Numerator,
        refresh_denominator: d.BufferDesc.RefreshRate.Denominator,
        usage: d.BufferUsage,
        swap_effect: d.SwapEffect,
        flags: d.Flags,
        windowed: d.Windowed != 0,
        output_window: d.OutputWindow as usize,
    })
}

fn read_swapchain_desc1(value: *const DXGI_SWAP_CHAIN_DESC1) -> Option<model::SwapchainDesc1> {
    let address = value as usize;
    if !typed_readable(address, std::mem::size_of::<DXGI_SWAP_CHAIN_DESC1>()) {
        return None;
    }
    // SAFETY: the address was just found readable for a whole DXGI_SWAP_CHAIN_DESC1.
    let d = unsafe { std::ptr::read_unaligned(value) };
    Some(model::SwapchainDesc1 {
        width: d.Width,
        height: d.Height,
        format: d.Format,
        stereo: d.Stereo != 0,
        sample_count: d.SampleDesc.Count,
        sample_quality: d.SampleDesc.Quality,
        usage: d.BufferUsage,
        buffer_count: d.BufferCount,
        scaling: d.Scaling,
        swap_effect: d.SwapEffect,
        alpha_mode: d.AlphaMode,
        flags: d.Flags,
    })
}

fn read_tuning(value: *const FfxApiSwapchainFramePacingTuning) -> Option<model::FramePacingTuning> {
    let address = value as usize;
    if !typed_readable(address, std::mem::size_of::<FfxApiSwapchainFramePacingTuning>()) {
        return None;
    }
    // SAFETY: the address was just found readable for a whole FfxApiSwapchainFramePacingTuning.
    let t = unsafe { std::ptr::read_unaligned(value) };
    Some(model::FramePacingTuning {
        safety_margin_in_ms: t.safetyMarginInMs,
        variance_factor: t.varianceFactor,
        allow_hybrid_spin: t.allowHybridSpin != 0,
        hybrid_spin_time: t.hybridSpinTime,
        allow_wait_for_single_object_on_fence: t.allowWaitForSingleObjectOnFence != 0,
    })
}

/// Flattens a create chain. A null root yields the default (no context named).
pub fn convert_create(root: *const ffxApiHeader) -> model::CreateContextDesc {
    let mut out = model::CreateContextDesc::default();
    walk_chain(root, |item| match item.type_ {
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION if out.frame_generation.is_none() => {
            let d: ffxCreateContextDescFrameGeneration = read_desc(item);
            out.frame_generation = Some(model::FrameGenerationCreate {
                flags: d.flags,
                display_size: dims(&d.displaySize),
                max_render_size: dims(&d.maxRenderSize),
                back_buffer_format: model::SurfaceFormat(d.backBufferFormat),
            });
        }
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS if out.frame_generation_hudless.is_none() => {
            let d: ffxCreateContextDescFrameGenerationHudless = read_desc(item);
            out.frame_generation_hudless =
                Some(model::FrameGenerationHudless { hudless_back_buffer_format: model::SurfaceFormat(d.hudlessBackBufferFormat) });
        }
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION if out.frame_generation_version.is_none() => {
            let d: ffxCreateContextDescFrameGenerationVersion = read_desc(item);
            out.frame_generation_version = Some(d.version);
        }
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12 if out.swapchain_wrap.is_none() => {
            let d: ffxCreateContextDescFrameGenerationSwapChainWrapDX12 = read_desc(item);
            out.swapchain_wrap = Some(model::SwapchainWrap {
                swapchain_slot: d.swapchain as usize,
                // SAFETY: `swapchain` is the game's `IDXGISwapChain4**` output slot. The C ABI
                // requires it to stay live until `ffxDestroyContext`, so dereferencing it once
                // on entry is the same read the C++ does. A null slot reads as the null chain.
                swapchain: unsafe { d.swapchain.as_ref() }.map_or(0, |slot| *slot as usize),
                game_queue: d.gameQueue as usize,
            });
        }
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12 if out.swapchain_new.is_none() => {
            let d: ffxCreateContextDescFrameGenerationSwapChainNewDX12 = read_desc(item);
            out.swapchain_new = Some(model::SwapchainNew {
                swapchain_slot: d.swapchain as usize,
                desc: read_swapchain_desc(d.desc),
                dxgi_factory: d.dxgiFactory as usize,
                game_queue: d.gameQueue as usize,
            });
        }
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12 if out.swapchain_for_hwnd.is_none() => {
            let d: ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 = read_desc(item);
            out.swapchain_for_hwnd = Some(model::SwapchainForHwnd {
                swapchain_slot: d.swapchain as usize,
                hwnd: d.hwnd as usize,
                desc: read_swapchain_desc1(d.desc),
                fullscreen_desc: d.fullscreenDesc as usize,
                dxgi_factory: d.dxgiFactory as usize,
                game_queue: d.gameQueue as usize,
            });
        }
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12 if out.swapchain_version.is_none() => {
            let d: ffxCreateContextDescFrameGenerationSwapChainVersionDX12 = read_desc(item);
            out.swapchain_version = Some(d.version);
        }
        FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 if out.backend_dx12.is_none() => {
            let d: ffxCreateBackendDX12Desc = read_desc(item);
            out.backend_dx12 = Some(model::BackendDx12 { device: d.device as usize });
        }
        FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12_ALLOCATION_CALLBACKS
            if out.backend_dx12_allocation_callbacks.is_none() =>
        {
            let d: ffxCreateBackendDX12AllocationCallbacksDesc = read_desc(item);
            out.backend_dx12_allocation_callbacks = Some(model::BackendDx12AllocationCallbacks {
                resource_allocator: d.pfnFfxResourceAllocator as usize,
                resource_deallocator: d.pfnFfxResourceDeallocator as usize,
                heap_allocator: d.pfnFfxHeapAllocator as usize,
                heap_deallocator: d.pfnFfxHeapDeallocator as usize,
                constant_buffer_allocator: d.pfnFfxConstantBufferAllocator.map_or(0, |f| f as usize),
            });
        }
        FFX_API_DESC_TYPE_OVERRIDE_VERSION if out.override_version.is_none() => {
            let d: ffxOverrideVersion = read_desc(item);
            out.override_version = Some(d.versionId);
        }
        other => out.unknown_types.push(other),
    });
    out
}

/// Flattens a configure chain. A null root marks `null_descriptor`, because the router answers a
/// null configure on a Sedici-owned context with `FFX_API_RETURN_ERROR`.
pub fn convert_configure(root: *const ffxApiHeader) -> model::ConfigureDesc {
    let mut out = model::ConfigureDesc { null_descriptor: root.is_null(), ..Default::default() };
    walk_chain(root, |item| match item.type_ {
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION if out.frame_generation.is_none() => {
            let d: ffxConfigureDescFrameGeneration = read_desc(item);
            out.frame_generation = Some(model::FrameGenerationConfigure {
                swapchain: d.swapChain as usize,
                present_callback: callback(d.presentCallback.map_or(std::ptr::null_mut(), |f| f as *mut _)),
                present_callback_user_context: d.presentCallbackUserContext as usize,
                frame_generation_callback: callback(
                    d.frameGenerationCallback.map_or(std::ptr::null_mut(), |f| f as *mut _),
                ),
                frame_generation_callback_user_context: d.frameGenerationCallbackUserContext as usize,
                frame_generation_enabled: d.frameGenerationEnabled != 0,
                allow_async_workloads: d.allowAsyncWorkloads != 0,
                hud_less_color: convert_resource(&d.HUDLessColor),
                flags: d.flags,
                only_present_generated: d.onlyPresentGenerated != 0,
                generation_rect: rect_model(&d.generationRect),
                frame_id: d.frameID,
            });
        }
        FFX_API_FRAME_GENERATION_CONFIG if out.legacy_frame_generation.is_none() => {
            let d: FfxFrameGenerationConfig = read_desc(item);
            out.legacy_frame_generation = Some(model::FrameGenerationConfigureLegacy {
                swapchain: d.swapChain as usize,
                present_callback: callback(d.presentCallback.map_or(std::ptr::null_mut(), |f| f as *mut _)),
                present_callback_context: d.presentCallbackContext as usize,
                frame_generation_callback: callback(
                    d.frameGenerationCallback.map_or(std::ptr::null_mut(), |f| f as *mut _),
                ),
                frame_generation_callback_context: d.frameGenerationCallbackContext as usize,
                frame_generation_enabled: d.frameGenerationEnabled != 0,
                allow_async_workloads: d.allowAsyncWorkloads != 0,
                allow_async_present: d.allowAsyncPresent != 0,
                hud_less_color: convert_resource(&d.HUDLessColor),
                flags: d.flags,
                only_present_interpolated: d.onlyPresentInterpolated != 0,
                interpolation_rect: rect_model(&d.interpolationRect),
                frame_id: d.frameID,
                draw_debug_pacing_lines: d.drawDebugPacingLines != 0,
            });
        }
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE if out.frame_generation_key_value.is_none() => {
            let d: ffxConfigureDescFrameGenerationKeyValue = read_desc(item);
            out.frame_generation_key_value = Some(model::KeyValue {
                key: d.key,
                u64_value: d.u64_,
                ptr: d.ptr as usize,
                tuning: None,
            });
        }
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE if out.distortion_field.is_none() => {
            let d: ffxConfigureDescFrameGenerationRegisterDistortionFieldResource = read_desc(item);
            out.distortion_field = Some(convert_resource(&d.distortionField));
        }
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12 if out.ui_resource.is_none() => {
            let d: ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 = read_desc(item);
            out.ui_resource = Some(model::UiResourceRegister {
                ui_resource: convert_resource(&d.uiResource),
                flags: d.flags,
            });
        }
        FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12 if out.swapchain_key_value.is_none() => {
            let d: ffxConfigureDescFrameGenerationSwapChainKeyValueDX12 = read_desc(item);
            let tuning = if d.key == FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING {
                read_tuning(d.ptr.cast())
            } else {
                None
            };
            out.swapchain_key_value = Some(model::KeyValue { key: d.key, u64_value: d.u64_, ptr: d.ptr as usize, tuning });
        }
        FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG if out.global_debug.is_none() => {
            let d: ffxConfigureDescGlobalDebug = read_desc(item);
            out.global_debug = Some(model::GlobalDebug {
                effect_id: d.effectId,
                message_callback: callback(d.fpMessage.map_or(std::ptr::null_mut(), |f| f as *mut _)),
                debug_level: d.debugLevel,
            });
        }
        FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1 if out.global_debug1.is_none() => {
            let d: ffxConfigureDescGlobalDebug1 = read_desc(item);
            out.global_debug1 = Some(model::GlobalDebug1 {
                message_callback: callback(d.fpMessage.map_or(std::ptr::null_mut(), |f| f as *mut _)),
                debug_level: d.debugLevel,
            });
        }
        FFX_API_DESC_TYPE_OVERRIDE_VERSION if out.override_version.is_none() => {
            let d: ffxOverrideVersion = read_desc(item);
            out.override_version = Some(d.versionId);
        }
        other => out.unknown_types.push(other),
    });
    out
}

fn prepare_model(d: &PrepareFields) -> model::Prepare {
    model::Prepare {
        frame_id: d.frame_id,
        flags: d.flags,
        command_list: d.command_list,
        render_size: d.render_size,
        jitter: d.jitter,
        motion_vector_scale: d.motion_vector_scale,
        frame_time_delta: d.frame_time_delta,
        camera_near: d.camera_near,
        camera_far: d.camera_far,
        camera_fov_vertical: d.camera_fov_vertical,
        view_space_to_meters: d.view_space_to_meters,
        depth: d.depth,
        motion_vectors: d.motion_vectors,
    }
}

/// The union of the fields both prepare forms carry, so the shared part is converted once.
struct PrepareFields {
    frame_id: u64,
    flags: u32,
    command_list: usize,
    render_size: model::Dimensions,
    jitter: model::Float2,
    motion_vector_scale: model::Float2,
    frame_time_delta: f32,
    camera_near: f32,
    camera_far: f32,
    camera_fov_vertical: f32,
    view_space_to_meters: f32,
    depth: model::Resource,
    motion_vectors: model::Resource,
}

/// Flattens a dispatch chain.
pub fn convert_dispatch(root: *const ffxApiHeader) -> model::DispatchDesc {
    let mut out = model::DispatchDesc { null_descriptor: root.is_null(), ..Default::default() };
    let mut first = true;
    walk_chain(root, |item| {
        if first {
            out.root_type = Some(item.type_);
            first = false;
        }
        match item.type_ {
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE if out.prepare_v1.is_none() => {
                let d: ffxDispatchDescFrameGenerationPrepare = read_desc(item);
                out.prepare_v1 = Some(model::PrepareV1 {
                    prepare: prepare_model(&PrepareFields {
                        frame_id: d.frameID,
                        flags: d.flags,
                        command_list: d.commandList as usize,
                        render_size: dims(&d.renderSize),
                        jitter: float2(&d.jitterOffset),
                        motion_vector_scale: float2(&d.motionVectorScale),
                        frame_time_delta: d.frameTimeDelta,
                        camera_near: d.cameraNear,
                        camera_far: d.cameraFar,
                        camera_fov_vertical: d.cameraFovAngleVertical,
                        view_space_to_meters: d.viewSpaceToMetersFactor,
                        depth: convert_resource(&d.depth),
                        motion_vectors: convert_resource(&d.motionVectors),
                    }),
                    unused_reset: d.unused_reset != 0,
                });
            }
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 if out.prepare_v2.is_none() => {
                let d: ffxDispatchDescFrameGenerationPrepareV2 = read_desc(item);
                out.prepare_v2 = Some(model::PrepareV2 {
                    prepare: prepare_model(&PrepareFields {
                        frame_id: d.frameID,
                        flags: d.flags,
                        command_list: d.commandList as usize,
                        render_size: dims(&d.renderSize),
                        jitter: float2(&d.jitterOffset),
                        motion_vector_scale: float2(&d.motionVectorScale),
                        frame_time_delta: d.frameTimeDelta,
                        camera_near: d.cameraNear,
                        camera_far: d.cameraFar,
                        camera_fov_vertical: d.cameraFovAngleVertical,
                        view_space_to_meters: d.viewSpaceToMetersFactor,
                        depth: convert_resource(&d.depth),
                        motion_vectors: convert_resource(&d.motionVectors),
                    }),
                    reset: d.reset != 0,
                    camera_position: float3(&d.cameraPosition),
                    camera_up: float3(&d.cameraUp),
                    camera_right: float3(&d.cameraRight),
                    camera_forward: float3(&d.cameraForward),
                });
            }
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO if out.camera_info.is_none() => {
                let d: ffxDispatchDescFrameGenerationPrepareCameraInfo = read_desc(item);
                out.camera_info = Some(model::PrepareCameraInfo {
                    camera_position: float3(&d.cameraPosition),
                    camera_up: float3(&d.cameraUp),
                    camera_right: float3(&d.cameraRight),
                    camera_forward: float3(&d.cameraForward),
                });
            }
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION if out.frame_generation.is_none() => {
                let d: ffxDispatchDescFrameGeneration = read_desc(item);
                let mut outputs = [model::Resource::default(); 4];
                for (slot, value) in outputs.iter_mut().zip(d.outputs.iter()) {
                    *slot = convert_resource(value);
                }
                out.frame_generation = Some(model::FrameGenerationDispatch {
                    command_list: d.commandList as usize,
                    present_color: convert_resource(&d.presentColor),
                    outputs,
                    num_generated_frames: d.numGeneratedFrames,
                    reset: d.reset != 0,
                    backbuffer_transfer_function: d.backbufferTransferFunction,
                    min_max_luminance: d.minMaxLuminance,
                    generation_rect: rect_model(&d.generationRect),
                    frame_id: d.frameID,
                });
            }
            FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT if out.present_callback.is_none() => {
                let d: ffxCallbackDescFrameGenerationPresent = read_desc(item);
                out.present_callback = Some(model::PresentCallbackDesc {
                    device: d.device as usize,
                    command_list: d.commandList as usize,
                    current_back_buffer: convert_resource(&d.currentBackBuffer),
                    current_ui: convert_resource(&d.currentUI),
                    output_swap_chain_buffer: convert_resource(&d.outputSwapChainBuffer),
                    is_generated_frame: d.isGeneratedFrame != 0,
                    frame_id: d.frameID,
                });
            }
            FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA if out.present_premul_alpha.is_none() => {
                let d: ffxCallbackDescFrameGenerationPresentPremulAlpha = read_desc(item);
                out.present_premul_alpha = Some(d.usePremulAlpha != 0);
            }
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12 => {
                out.wait_for_presents = true;
            }
            other => out.unknown_types.push(other),
        }
    });
    out
}

/// Reads `*outputCount` when the game's slot is readable; `None` otherwise, so the router can
/// answer `ERROR_PARAMETER` exactly as the C++ does for a null count.
pub fn read_output_count(slot: usize) -> Option<u64> {
    if !typed_readable(slot, std::mem::size_of::<u64>()) {
        return None;
    }
    // SAFETY: the address was just found readable for a u64.
    Some(unsafe { std::ptr::read_unaligned(slot as *const u64) })
}

/// Flattens a query chain. `*outputCount` is read for `GetVersions` before the router sees it.
pub fn convert_query(root: *const ffxApiHeader) -> model::QueryDesc {
    let mut out = model::QueryDesc { null_descriptor: root.is_null(), ..Default::default() };
    let mut first = true;
    walk_chain(root, |item| {
        if first {
            out.root_type = Some(item.type_);
            first = false;
        }
        match item.type_ {
            FFX_API_QUERY_DESC_TYPE_GET_VERSIONS if out.get_versions.is_none() => {
                let d: ffxQueryDescGetVersions = read_desc(item);
                out.get_versions = Some(model::GetVersions {
                    create_desc_type: d.createDescType,
                    device: d.device as usize,
                    output_count_slot: d.outputCount as usize,
                    version_ids_slot: d.versionIds as usize,
                    version_names_slot: d.versionNames as usize,
                    output_count: read_output_count(d.outputCount as usize).unwrap_or(0),
                });
            }
            FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION if out.get_provider_version.is_none() => {
                let _d: ffxQueryGetProviderVersion = read_desc(item);
                out.get_provider_version = Some(());
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE if out.gpu_memory_usage.is_none() => {
                let d: ffxQueryDescFrameGenerationGetGPUMemoryUsage = read_desc(item);
                out.gpu_memory_usage =
                    Some(model::MemoryUsageQuery { output_slot: d.gpuMemoryUsageFrameGeneration as usize });
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2 if out.gpu_memory_usage_v2.is_none() => {
                let d: ffxQueryDescFrameGenerationGetGPUMemoryUsageV2 = read_desc(item);
                out.gpu_memory_usage_v2 = Some(model::MemoryUsageQueryV2 {
                    device: d.device as usize,
                    max_render_size: dims(&d.maxRenderSize),
                    display_size: dims(&d.displaySize),
                    create_flags: d.createFlags,
                    dispatch_flags: d.dispatchFlags,
                    back_buffer_format: model::SurfaceFormat(d.backBufferFormat),
                    hudless_back_buffer_format: model::SurfaceFormat(d.hudlessBackBufferFormat),
                    output_slot: d.gpuMemoryUsageFrameGeneration as usize,
                });
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12
                if out.interpolation_command_list.is_none() =>
            {
                let d: ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12 = read_desc(item);
                out.interpolation_command_list = Some(model::CommandListQuery { output_slot: d.pOutCommandList as usize });
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12
                if out.interpolation_texture.is_none() =>
            {
                let d: ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12 = read_desc(item);
                out.interpolation_texture = Some(model::TextureQuery { output_slot: d.pOutTexture as usize });
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12
                if out.swapchain_gpu_memory_usage.is_none() =>
            {
                let d: ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12 = read_desc(item);
                out.swapchain_gpu_memory_usage =
                    Some(model::MemoryUsageQuery { output_slot: d.gpuMemoryUsageFrameGenerationSwapchain as usize });
            }
            FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2
                if out.swapchain_gpu_memory_usage_v2.is_none() =>
            {
                let d: ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2 = read_desc(item);
                out.swapchain_gpu_memory_usage_v2 = Some(model::SwapchainMemoryUsageQueryV2 {
                    device: d.device as usize,
                    display_size: dims(&d.displaySize),
                    back_buffer_format: model::SurfaceFormat(d.backBufferFormat),
                    back_buffer_count: d.backBufferCount,
                    ui_resource_size: dims(&d.uiResourceSize),
                    ui_resource_format: model::SurfaceFormat(d.uiResourceFormat),
                    flags: d.flags,
                    output_slot: d.gpuMemoryUsageFrameGenerationSwapchain as usize,
                });
            }
            FFX_API_DESC_TYPE_OVERRIDE_VERSION if out.override_version.is_none() => {
                let d: ffxOverrideVersion = read_desc(item);
                out.override_version = Some(d.versionId);
            }
            other => out.unknown_types.push(other),
        }
    });
    out
}

// -------------------------------------------------------------------------------------------------
// Writes back through the game's output slots
// -------------------------------------------------------------------------------------------------

/// True when `ptr` names at least `len` writable bytes (committed, not a guard page, and mapped
/// with a write protection). Checked before every write so a read-only descriptor is skipped
/// instead of faulting the game's thread.
#[cfg(windows)]
pub fn writable(ptr: *mut u8, len: usize) -> bool {
    use windows::Win32::System::Memory::{
        VirtualQuery, MEMORY_BASIC_INFORMATION, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY,
        PAGE_READWRITE, PAGE_WRITECOPY,
    };

    if ptr.is_null() || len == 0 {
        return false;
    }
    let mut query = MEMORY_BASIC_INFORMATION::default();
    let mut address = ptr as usize;
    let end = match address.checked_add(len) {
        Some(end) => end,
        None => return false,
    };
    while address < end {
        // SAFETY: `address` is a live address from the caller and `query` is an owned, correctly
        // sized output buffer; VirtualQuery neither reads nor writes the queried range.
        let written = unsafe {
            VirtualQuery(Some(address as *const _), &mut query, std::mem::size_of::<MEMORY_BASIC_INFORMATION>())
        };
        if written == 0 {
            return false;
        }
        let base = query.BaseAddress as usize;
        let region_end = base.saturating_add(query.RegionSize);
        let writable_protection = query.Protect == PAGE_READWRITE
            || query.Protect == PAGE_WRITECOPY
            || query.Protect == PAGE_EXECUTE_READWRITE
            || query.Protect == PAGE_EXECUTE_WRITECOPY;
        if !writable_protection || region_end <= address {
            return false;
        }
        address = region_end;
    }
    true
}

#[cfg(not(windows))]
pub fn writable(ptr: *mut u8, len: usize) -> bool {
    !ptr.is_null() && len > 0
}

/// Writes a `u64` through a game slot; a null, unreadable or read-only slot is skipped.
pub fn write_u64(slot: usize, value: u64) -> bool {
    if !writable(slot as *mut u8, std::mem::size_of::<u64>()) {
        return false;
    }
    // SAFETY: the slot was just found writable for a u64.
    unsafe { std::ptr::write_unaligned(slot as *mut u64, value) };
    true
}

/// Writes a pointer-sized value (an output handle, a version name) through a game slot.
pub fn write_pointer(slot: usize, value: usize) -> bool {
    if !writable(slot as *mut u8, std::mem::size_of::<usize>()) {
        return false;
    }
    // SAFETY: the slot was just found writable for a pointer.
    unsafe { std::ptr::write_unaligned(slot as *mut usize, value) };
    true
}

fn raw_resource(value: &model::Resource) -> FfxApiResource {
    let d = &value.description;
    let type_ = match d.kind {
        model::ResourceKind::Buffer => FFX_API_RESOURCE_TYPE_BUFFER,
        model::ResourceKind::Texture1D => FFX_API_RESOURCE_TYPE_TEXTURE1D,
        model::ResourceKind::Texture2D => FFX_API_RESOURCE_TYPE_TEXTURE2D,
        model::ResourceKind::TextureCube => FFX_API_RESOURCE_TYPE_TEXTURE_CUBE,
        model::ResourceKind::Texture3D => FFX_API_RESOURCE_TYPE_TEXTURE3D,
        model::ResourceKind::Unknown(other) => other,
    };
    FfxApiResource {
        resource: value.handle as *mut std::ffi::c_void,
        description: FfxApiResourceDescription {
            type_,
            format: d.format.0,
            width_or_size: d.width,
            height_or_stride: d.height,
            depth_or_alignment: d.depth,
            mipCount: d.mip_count,
            flags: d.flags,
            usage: d.usage,
        },
        state: value.state,
    }
}

/// Writes a resource through a game `FfxApiResource*` slot.
pub fn write_resource(slot: usize, value: &model::Resource) -> bool {
    let size = std::mem::size_of::<FfxApiResource>();
    if !writable(slot as *mut u8, size) {
        return false;
    }
    let raw = raw_resource(value);
    // SAFETY: the slot was just found writable for a whole FfxApiResource.
    unsafe { std::ptr::write_unaligned(slot as *mut FfxApiResource, raw) };
    true
}

/// Writes a successful query's answer through the descriptor's output pointers. Writes that the
/// game has no slot for are skipped; the router chose the reply, not the write.
pub fn apply_query_reply(root: *mut ffxApiHeader, reply: &model::QueryReply) {
    let desc = convert_query(root);
    match reply {
        model::QueryReply::Versions { count, id, name } => {
            let Some(query) = desc.get_versions else { return };
            write_u64(query.output_count_slot, *count);
            if query.output_count >= 1 {
                if let Some(id) = id {
                    write_u64(query.version_ids_slot, *id);
                }
                if let Some(name) = name {
                    write_pointer(query.version_names_slot, name.as_ptr() as usize);
                }
            }
        }
        model::QueryReply::ProviderVersion { id, name } => {
            if root.is_null() || !header_readable(root) {
                return;
            }
            // SAFETY: `root` is a readable chain header whose type the router routed as
            // `GetProviderVersion`; the descriptor therefore extends through the version fields.
            let fields = unsafe {
                std::ptr::addr_of_mut!((*root.cast::<ffxQueryGetProviderVersion>()).versionId)
            };
            let name_field = unsafe {
                std::ptr::addr_of_mut!((*root.cast::<ffxQueryGetProviderVersion>()).versionName)
            };
            if writable(fields.cast::<u8>(), std::mem::size_of::<u64>()) {
                // SAFETY: the field was just found writable.
                unsafe { std::ptr::write_unaligned(fields, *id) };
            }
            if writable(name_field.cast::<u8>(), std::mem::size_of::<usize>()) {
                // SAFETY: the field was just found writable.
                unsafe {
                    std::ptr::write_unaligned(
                        name_field,
                        name.map_or(std::ptr::null(), |value| value.as_ptr()),
                    );
                }
            }
        }
        model::QueryReply::CommandList(handle) => {
            if let Some(query) = desc.interpolation_command_list {
                write_pointer(query.output_slot, *handle);
            }
        }
        model::QueryReply::Texture(value) => {
            if let Some(query) = desc.interpolation_texture {
                write_resource(query.output_slot, value);
            }
        }
        model::QueryReply::MemoryUsage(usage) => {
            let slot = desc
                .gpu_memory_usage
                .map(|q| q.output_slot)
                .or_else(|| desc.swapchain_gpu_memory_usage.map(|q| q.output_slot))
                .or_else(|| desc.gpu_memory_usage_v2.map(|q| q.output_slot))
                .or_else(|| desc.swapchain_gpu_memory_usage_v2.map(|q| q.output_slot));
            let Some(slot) = slot else { return };
            let raw = FfxApiEffectMemoryUsage {
                totalUsageInBytes: usage.total_usage_in_bytes,
                aliasableUsageInBytes: usage.aliasable_usage_in_bytes,
            };
            if writable(slot as *mut u8, std::mem::size_of::<FfxApiEffectMemoryUsage>()) {
                // SAFETY: the slot was just found writable for the whole usage struct.
                unsafe { std::ptr::write_unaligned(slot as *mut FfxApiEffectMemoryUsage, raw) };
            }
        }
        model::QueryReply::Acknowledged => {}
    }
}

/// Writes the issued context handle to `*context` and, for a swapchain form, the wrapper chain to
/// the game's output slot. Returns false when either write could not be made; the caller then
/// answers with an error rather than handing back a context the game cannot use.
pub fn apply_created(context: *mut ffxContext, root: *mut ffxApiHeader, created: &model::CreatedContext) -> bool {
    let Some(swapchain) = created.swapchain else {
        return write_pointer(context as usize, created.context);
    };
    let desc = convert_create(root);
    let slot = desc
        .swapchain_wrap
        .map(|d| d.swapchain_slot)
        .or_else(|| desc.swapchain_new.map(|d| d.swapchain_slot))
        .or_else(|| desc.swapchain_for_hwnd.map(|d| d.swapchain_slot));
    let Some(slot) = slot else { return false };
    if !write_pointer(slot, swapchain) {
        return false;
    }
    write_pointer(context as usize, created.context)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn header(type_: u64, p_next: *mut ffxApiHeader) -> ffxApiHeader {
        ffxApiHeader { type_, pNext: p_next }
    }

    #[test]
    fn a_null_chain_yields_nothing() {
        assert!(walk_chain(std::ptr::null(), |_| {}).is_empty());
        assert!(!convert_create(std::ptr::null()).names_a_sedici_context());
        assert!(convert_configure(std::ptr::null()).null_descriptor);
        assert!(convert_dispatch(std::ptr::null()).null_descriptor);
        assert!(convert_query(std::ptr::null()).null_descriptor);
    }

    #[test]
    #[allow(unused_assignments)] // the cycle is built by patching a node the walk will follow
    fn a_cycle_stops_at_the_first_repeat() {
        let mut tail = header(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS, std::ptr::null_mut());
        let mut head = header(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &mut tail);
        tail.pNext = &mut head;
        let types = walk_chain(&head, |_| {});
        assert_eq!(
            types,
            [FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS]
        );
    }

    #[test]
    fn unknown_types_are_collected_but_name_no_context() {
        let mut unknown = header(0x7fff_fffe, std::ptr::null_mut());
        let root = header(0x7fff_fffd, &mut unknown);
        let desc = convert_create(&root);
        assert!(!desc.names_a_sedici_context());
        assert_eq!(desc.unknown_types, [0x7fff_fffd, 0x7fff_fffe]);
    }

    #[test]
    fn a_known_chain_flattens_into_the_safe_model() {
        let mut fg: ffxCreateContextDescFrameGeneration = unsafe { std::mem::zeroed() };
        fg.header.type_ = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        fg.flags = 1 << 5;
        fg.displaySize = FfxApiDimensions2D { width: 1920, height: 1080 };
        fg.maxRenderSize = FfxApiDimensions2D { width: 1280, height: 720 };
        fg.backBufferFormat = 14;
        let desc = convert_create(&fg.header);
        assert!(desc.names_a_sedici_context());
        let fg = desc.frame_generation.expect("frame generation desc");
        assert_eq!(fg.display_size, model::Dimensions { width: 1920, height: 1080 });
        assert_eq!(fg.max_render_size, model::Dimensions { width: 1280, height: 720 });
        assert_eq!(fg.back_buffer_format, model::SurfaceFormat(14));
    }

    #[test]
    fn the_chain_walk_is_bounded() {
        let mut nodes: Vec<ffxApiHeader> = (0..MAX_CHAIN_ITEMS + 8)
            .map(|_| header(FFX_API_DESC_TYPE_OVERRIDE_VERSION, std::ptr::null_mut()))
            .collect();
        for i in 1..nodes.len() {
            // Reborrowing through a raw pointer keeps the vector alive and stable for the walk.
            let previous = &mut nodes[i - 1] as *mut ffxApiHeader;
            nodes[i].pNext = std::ptr::null_mut();
            unsafe { (*previous).pNext = &mut nodes[i] };
        }
        let types = walk_chain(&nodes[0], |_| {});
        assert_eq!(types.len(), MAX_CHAIN_ITEMS);
    }
}
