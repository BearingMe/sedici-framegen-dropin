// fgcheck group: Stage 1 on a 10-bit (R10G10B10A2_UNORM) back buffer, with the HDR create flag (P4.8).
//
// Alan Wake 2 creates its frame generation context with R10G10B10A2_UNORM colour and the HDR
// flag (K14, K129). Stage 1 samples and writes float4, so the format should change nothing but
// the storage. The check proves that on this GPU: the same scene is run through an 8-bit and a
// 10-bit context, the 8-bit pictures are lifted to 10 bits exactly (v << 2 | v >> 6), and the
// generated 10-bit frame is reduced back with rounding. The two results must agree within the
// quantisation of the two storage formats, and the 10-bit result must score like the 8-bit one.
#include "fgcheck_common.h"

namespace fgcheck {

namespace {

uint32_t lift(uint8_t v)
{
    return (static_cast<uint32_t>(v) << 2) | (v >> 6);
}

std::vector<uint8_t> to_ten_bit(const std::vector<uint8_t>& rgba8)
{
    std::vector<uint8_t> out(rgba8.size());
    for (size_t i = 0; i + 3 < rgba8.size(); i += 4) {
        const uint32_t packed = lift(rgba8[i]) | (lift(rgba8[i + 1]) << 10) | (lift(rgba8[i + 2]) << 20) |
                                (static_cast<uint32_t>(rgba8[i + 3] >> 6) << 30);
        std::memcpy(&out[i], &packed, 4);
    }
    return out;
}

uint8_t reduce(uint32_t v10)
{
    return static_cast<uint8_t>((v10 * 255 + 511) / 1023);
}

std::vector<uint8_t> from_ten_bit(const std::vector<uint8_t>& packed)
{
    std::vector<uint8_t> out(packed.size());
    for (size_t i = 0; i + 3 < packed.size(); i += 4) {
        uint32_t value = 0;
        std::memcpy(&value, &packed[i], 4);
        out[i] = reduce(value & 1023);
        out[i + 1] = reduce((value >> 10) & 1023);
        out[i + 2] = reduce((value >> 20) & 1023);
        out[i + 3] = 255;
    }
    return out;
}

// One scene through one context. Returns the generated midpoint as RGBA8 (the 10-bit result is
// reduced), or an empty vector when something failed.
std::vector<uint8_t> run_format(Device& state, const SceneConfig& config, bool tenBit)
{
    const DXGI_FORMAT format = tenBit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    const auto previousDisplay = make_scene(config, 0.0f, kDisplayWidth, kDisplayHeight);
    const auto currentDisplay = make_scene(config, 1.0f, kDisplayWidth, kDisplayHeight);
    const auto previousRender = make_scene(config, 0.0f, kRenderWidth, kRenderHeight);
    const auto currentRender = make_scene(config, 1.0f, kRenderWidth, kRenderHeight);

    std::vector<uint8_t> colors[2] = {quantise(previousDisplay.color), quantise(currentDisplay.color)};
    if (tenBit)
        for (auto& color : colors) color = to_ten_bit(color);
    const std::vector<uint16_t> motions[2] = {pack_ffx_motion(previousRender.motion),
                                              pack_ffx_motion(currentRender.motion)};
    const Image* renders[2] = {&previousRender, &currentRender};

    FrameTextures frames[2];
    for (int index = 0; index < 2; ++index) {
        frames[index].color =
            create_texture(state.device.Get(), format, kDisplayWidth, kDisplayHeight, D3D12_RESOURCE_FLAG_NONE);
        frames[index].depth = create_texture(state.device.Get(), DXGI_FORMAT_D32_FLOAT, kRenderWidth, kRenderHeight,
                                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        frames[index].motion = create_texture(state.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kRenderWidth,
                                              kRenderHeight, D3D12_RESOURCE_FLAG_NONE);
        if (!frames[index].color || !frames[index].depth || !frames[index].motion) return {};
    }
    const auto output = create_texture(state.device.Get(), format, kDisplayWidth, kDisplayHeight,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!output) return {};

    const uint32_t rowPitch = align_to(kDisplayWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint32_t renderPitch = align_to(kRenderWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint32_t motionPitch = align_to(kRenderWidth * 8, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    for (int index = 0; index < 2; ++index) {
        auto colorStaging = create_buffer(state.device.Get(), static_cast<uint64_t>(rowPitch) * kDisplayHeight,
                                          D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto depthStaging = create_buffer(state.device.Get(), static_cast<uint64_t>(renderPitch) * kRenderHeight,
                                          D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto motionStaging = create_buffer(state.device.Get(), static_cast<uint64_t>(motionPitch) * kRenderHeight,
                                           D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        state.allocator->Reset();
        state.list->Reset(state.allocator.Get(), nullptr);
        upload(state.list.Get(), frames[index].color.Get(), colorStaging.Get(), colors[index].data(), kDisplayWidth,
               kDisplayHeight, 4);
        upload(state.list.Get(), frames[index].depth.Get(), depthStaging.Get(), renders[index]->depth.data(),
               kRenderWidth, kRenderHeight, 4);
        upload(state.list.Get(), frames[index].motion.Get(), motionStaging.Get(), motions[index].data(),
               kRenderWidth, kRenderHeight, 8);
        if (!state.submit()) return {};
    }

    sedici::fg::Context context;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = state.device.Get();
    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &backend.header;
    // The HDR flag is what the game sends; it must not change the passes.
    create.flags =
        FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | (tenBit ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0u);
    create.displaySize = {kDisplayWidth, kDisplayHeight};
    create.maxRenderSize = {kRenderWidth, kRenderHeight};
    create.backBufferFormat =
        tenBit ? FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM : FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    if (!context.create(&create.header)) return {};

    auto readback = create_buffer(state.device.Get(), static_cast<uint64_t>(rowPitch) * kDisplayHeight,
                                  D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON;
    for (uint32_t iteration = 0; iteration < 4; ++iteration) {
        const uint32_t source = iteration % 2;
        if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr)))
            return {};
        ffxConfigureDescFrameGeneration configure{};
        configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        configure.frameGenerationEnabled = true;
        configure.HUDLessColor = ffxApiGetResourceDX12(frames[source].color.Get(), FFX_API_RESOURCE_STATE_COMMON);
        configure.frameID = iteration;
        ffxDispatchDescFrameGenerationPrepare prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prepare.frameID = iteration;
        prepare.commandList = state.list.Get();
        prepare.renderSize = {kRenderWidth, kRenderHeight};
        prepare.motionVectorScale = {static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight)};
        prepare.depth = ffxApiGetResourceDX12(frames[source].depth.Get(), FFX_API_RESOURCE_STATE_COMMON);
        prepare.motionVectors = ffxApiGetResourceDX12(frames[source].motion.Get(), FFX_API_RESOURCE_STATE_COMMON);
        prepare.unused_reset = source == 0;
        if (!context.configure(&configure.header) || !context.prepare(&prepare.header)) return {};
        if (!context.generate(state.list.Get(), output.Get(), outputState)) return {};

        transition(state.list.Get(), output.Get(), outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        outputState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = output.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint = {format, kDisplayWidth, kDisplayHeight, 1, rowPitch};
        state.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        if (!state.submit()) return {};
        if (!context.track_submission(state.fence.Get(), state.fenceValue)) return {};
    }

    std::vector<uint8_t> raw(static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 4);
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped))) return {};
    for (uint32_t y = 0; y < kDisplayHeight; ++y)
        std::memcpy(raw.data() + static_cast<size_t>(y) * kDisplayWidth * 4,
                    static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * rowPitch,
                    static_cast<size_t>(kDisplayWidth) * 4);
    readback->Unmap(0, nullptr);
    return tenBit ? from_ten_bit(raw) : raw;
}

} // namespace

bool check_ten_bit(Device& state)
{
    bool ok = true;
    for (const auto& scene : {kScenes[0], kScenes[2]}) {
        const auto eight = run_format(state, scene, false);
        const auto ten = run_format(state, scene, true);
        if (eight.empty() || ten.empty() || eight.size() != ten.size()) {
            std::printf("FAIL 10-bit %s: a run failed\n", scene.name);
            ok = false;
            continue;
        }
        const auto truth = quantise(make_scene(scene, 0.5f, kDisplayWidth, kDisplayHeight).color);
        const double eightScore = psnr(truth, eight);
        const double tenScore = psnr(truth, ten);
        const double between = psnr(eight, ten);
        // Different storage rounds differently inside the warp's float filter, so the two results
        // are not identical; they must agree closely and the 10-bit one must not score worse.
        const bool pass = between > 42.0 && tenScore > eightScore - 0.5;
        std::printf("%s 10-bit %-18s psnr 8-bit %.2f dB, 10-bit %.2f dB, between them %.2f dB\n",
                    pass ? "PASS" : "FAIL", scene.name, eightScore, tenScore, between);
        ok = pass && ok;
    }
    return ok;
}

} // namespace fgcheck
