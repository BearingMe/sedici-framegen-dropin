// fgcheck group: Stage 1 quality scenes and the frame generation context contract.
#include "fgcheck_common.h"

namespace fgcheck {

bool run_case(Device& state, const SceneConfig& config, bool keepImages)
{
    const auto previousDisplay = make_scene(config, 0.0f, kDisplayWidth, kDisplayHeight);
    const auto currentDisplay = make_scene(config, 1.0f, kDisplayWidth, kDisplayHeight);
    const auto truth = make_scene(config, 0.5f, kDisplayWidth, kDisplayHeight);

    const auto previousRender = make_scene(config, 0.0f, kRenderWidth, kRenderHeight);
    const auto currentRender = make_scene(config, 1.0f, kRenderWidth, kRenderHeight);

    const std::vector<uint8_t> previousColor = quantise(previousDisplay.color);
    const std::vector<uint8_t> currentColor = quantise(currentDisplay.color);
    const std::vector<uint16_t> previousMotion = pack_ffx_motion(previousRender.motion);
    const std::vector<uint16_t> currentMotion = pack_ffx_motion(currentRender.motion);

    FrameTextures frames[2];
    for (int index = 0; index < 2; ++index) {
        frames[index].color = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                             kDisplayWidth, kDisplayHeight,
                                             D3D12_RESOURCE_FLAG_NONE);
        // Match the game's depth-target allocation; FFX describes it as R32_FLOAT.
        frames[index].depth = create_texture(state.device.Get(), DXGI_FORMAT_D32_FLOAT,
                                              kRenderWidth, kRenderHeight,
                                              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        frames[index].motion = create_texture(state.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                              kRenderWidth, kRenderHeight,
                                              D3D12_RESOURCE_FLAG_NONE);
        if (!frames[index].color || !frames[index].depth || !frames[index].motion) {
            std::fprintf(stderr, "input textures failed\n");
            return false;
        }
    }

    const auto output = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                       kDisplayWidth, kDisplayHeight,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!output) {
        std::fprintf(stderr, "output texture failed\n");
        return false;
    }

    const uint32_t colorRowPitch = align_to(kDisplayWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint32_t renderRowPitch = align_to(kRenderWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t colorBytes = static_cast<uint64_t>(colorRowPitch) * kDisplayHeight;
    const uint64_t renderBytes = static_cast<uint64_t>(renderRowPitch) * kRenderHeight;
    const uint64_t motionBytes = static_cast<uint64_t>(align_to(kRenderWidth * 8,
        D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)) * kRenderHeight;

    for (int index = 0; index < 2; ++index) {
        const auto& color = index == 0 ? previousColor : currentColor;
        const auto& motion = index == 0 ? previousMotion : currentMotion;
        const auto& render = index == 0 ? previousRender : currentRender;

        auto colorStaging = create_buffer(state.device.Get(), colorBytes, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_STATE_GENERIC_READ);
        auto depthStaging = create_buffer(state.device.Get(), renderBytes, D3D12_HEAP_TYPE_UPLOAD,
                                          D3D12_RESOURCE_STATE_GENERIC_READ);
        auto motionStaging = create_buffer(state.device.Get(), motionBytes, D3D12_HEAP_TYPE_UPLOAD,
                                           D3D12_RESOURCE_STATE_GENERIC_READ);

        state.allocator->Reset();
        state.list->Reset(state.allocator.Get(), nullptr);
        upload(state.list.Get(), frames[index].color.Get(), colorStaging.Get(), color.data(),
               kDisplayWidth, kDisplayHeight, 4);
        upload(state.list.Get(), frames[index].depth.Get(), depthStaging.Get(), render.depth.data(),
               kRenderWidth, kRenderHeight, 4);
        upload(state.list.Get(), frames[index].motion.Get(), motionStaging.Get(), motion.data(),
               kRenderWidth, kRenderHeight, 8);
        if (!state.submit()) {
            std::fprintf(stderr, "upload failed\n");
            return false;
        }

    }

    sedici::fg::Context context;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = state.device.Get();
    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &backend.header;
    create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    create.displaySize = {kDisplayWidth, kDisplayHeight};
    create.maxRenderSize = {kRenderWidth, kRenderHeight};
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    if (!context.create(&create.header)) {
        std::fprintf(stderr, "context initialization failed\n");
        return false;
    }

    D3D12_QUERY_HEAP_DESC queryDescription{};
    queryDescription.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDescription.Count = 2 + (g_profile ? PassProfile::kMaxMarks : 0);
    ComPtr<ID3D12QueryHeap> queryHeap;
    if (FAILED(state.device->CreateQueryHeap(&queryDescription, IID_PPV_ARGS(&queryHeap)))) {
        std::fprintf(stderr, "query heap failed\n");
        return false;
    }
    auto timings = create_buffer(state.device.Get(), queryDescription.Count * sizeof(uint64_t),
                                 D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    PassProfile profile;
    std::vector<double> passTicks; // summed per mark index over the measured iterations
    std::vector<const char*> passNames;
    if (g_profile) {
        profile.heap = queryHeap.Get();
        sedici::fg::stage1_set_pass_mark(&pass_mark, &profile);
    }

    const uint32_t outputRowPitch = align_to(kDisplayWidth * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    auto readback = create_buffer(state.device.Get(),
                                  static_cast<uint64_t>(outputRowPitch) * kDisplayHeight,
                                  D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON;
    uint64_t ticks = 0;
    uint32_t measured = 0;

    for (uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        const uint32_t source = iteration % 2;
        if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
        state.list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        profile.next = 2;
        profile.names.clear();
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
        // Repeating pairs, not a physically incorrect backwards step with
        // forward-pointing motion. Each pair begins with a history reset.
        prepare.unused_reset = source == 0;
        if (!context.configure(&configure.header) || !context.prepare(&prepare.header)) return false;

        if (!context.generate(state.list.Get(), output.Get(), outputState)) {
            std::fprintf(stderr, "stage 1 rejected the frame\n");
            return false;
        }
        state.list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        // Marks are in recording order; the end timestamp is last in time, so resolve
        // the start, the end and the marks as one range.
        state.list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, profile.next,
                                     timings.Get(), 0);

        transition(state.list.Get(), output.Get(), outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        outputState = D3D12_RESOURCE_STATE_COPY_SOURCE;

        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = output.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        to.PlacedFootprint.Footprint.Width = kDisplayWidth;
        to.PlacedFootprint.Footprint.Height = kDisplayHeight;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = outputRowPitch;

        state.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        if (!state.submit()) {
            std::fprintf(stderr, "dispatch failed\n");
            return false;
        }
        if (!context.track_submission(state.fence.Get(), state.fenceValue)) return false;

        void* mapped = nullptr;
        if (SUCCEEDED(timings->Map(0, nullptr, &mapped))) {
            const auto* values = static_cast<const uint64_t*>(mapped);
            if (source == 1 && values[1] > values[0]) {
                ticks += values[1] - values[0];
                ++measured;
                if (g_profile) {
                    if (passTicks.size() < profile.names.size()) {
                        passTicks.resize(profile.names.size(), 0.0);
                        passNames = profile.names;
                    }
                    uint64_t previous = values[0];
                    for (size_t k = 0; k < profile.names.size(); ++k) {
                        const uint64_t at = values[2 + k];
                        if (at > previous) passTicks[k] += static_cast<double>(at - previous);
                        previous = std::max(previous, at);
                    }
                }
            }
            timings->Unmap(0, nullptr);
        }
    }

    uint64_t frequency = 0;
    state.queue->GetTimestampFrequency(&frequency);

    std::vector<uint8_t> generated(static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 4);
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped))) {
        std::fprintf(stderr, "readback failed\n");
        return false;
    }
    for (uint32_t y = 0; y < kDisplayHeight; ++y) {
        std::memcpy(generated.data() + static_cast<size_t>(y) * kDisplayWidth * 4,
                    static_cast<const uint8_t*>(mapped) + static_cast<size_t>(y) * outputRowPitch,
                    static_cast<size_t>(kDisplayWidth) * 4);
    }
    readback->Unmap(0, nullptr);

    const auto reference = quantise(truth.color);
    const double psnrValue = psnr(reference, generated);
    const double ssimValue = ssim(reference, generated, kDisplayWidth, kDisplayHeight);
    const double milliseconds = measured && frequency
        ? static_cast<double>(ticks) / measured / static_cast<double>(frequency) * 1000.0
        : 0.0;

    std::printf("%-18s psnr %6.2f dB  ssim %.4f  stage1 %.3f ms\n",
                config.name, psnrValue, ssimValue, milliseconds);
    if (g_profile) {
        sedici::fg::stage1_set_pass_mark(nullptr, nullptr);
        std::printf("%-18s passes (ms, time to each pass end):", config.name);
        for (size_t k = 0; k < passTicks.size(); ++k) {
            const double ms = measured && frequency
                ? passTicks[k] / measured / static_cast<double>(frequency) * 1000.0 : 0.0;
            std::printf(" %s %.3f", passNames[k], ms);
        }
        std::printf("\n");
    }

    if (keepImages) {
        char name[128];
        std::snprintf(name, sizeof(name), "fgcheck-%s-reference.ppm", config.name);
        save_ppm(name, reference, kDisplayWidth, kDisplayHeight);
        std::snprintf(name, sizeof(name), "fgcheck-%s-generated.ppm", config.name);
        save_ppm(name, generated, kDisplayWidth, kDisplayHeight);
    }

    // Phases: two generated frames from one prepare, at 1/3 and 2/3, into two
    // outputs recorded in one command list (the "vrr" 1.5x/1.75x/adaptive path).
    // Each must be closer to the true scene at its time than simply showing the
    // nearer real frame, which a wrong or shared alpha would not be.
    {
        const auto second = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                           kDisplayWidth, kDisplayHeight,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!second) return false;
        D3D12_RESOURCE_STATES secondState = D3D12_RESOURCE_STATE_COMMON;
        for (uint32_t step = 0; step < 2; ++step) {
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
            ffxConfigureDescFrameGeneration configure{};
            configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            configure.frameGenerationEnabled = true;
            configure.HUDLessColor = ffxApiGetResourceDX12(frames[step].color.Get(), FFX_API_RESOURCE_STATE_COMMON);
            configure.frameID = 1000 + step;
            ffxDispatchDescFrameGenerationPrepare prepare{};
            prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
            prepare.frameID = 1000 + step;
            prepare.commandList = state.list.Get();
            prepare.renderSize = {kRenderWidth, kRenderHeight};
            prepare.motionVectorScale = {static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight)};
            prepare.depth = ffxApiGetResourceDX12(frames[step].depth.Get(), FFX_API_RESOURCE_STATE_COMMON);
            prepare.motionVectors = ffxApiGetResourceDX12(frames[step].motion.Get(), FFX_API_RESOURCE_STATE_COMMON);
            prepare.unused_reset = step == 0;
            if (!context.configure(&configure.header) || !context.prepare(&prepare.header)) return false;
            const sedici::fg::GeneratePhase phases[2]{{output.Get(), outputState, 1.0f / 3.0f},
                                                     {second.Get(), secondState, 2.0f / 3.0f}};
            if (!context.generate(state.list.Get(), phases, 2)) {
                std::fprintf(stderr, "phases: stage 1 rejected the frame\n");
                return false;
            }
            if (!state.submit() || !context.track_submission(state.fence.Get(), state.fenceValue)) return false;
        }
        const auto readTexture = [&](ID3D12Resource* texture, D3D12_RESOURCE_STATES& textureState,
                                     std::vector<uint8_t>& pixels) {
            if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
            transition(state.list.Get(), texture, textureState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            textureState = D3D12_RESOURCE_STATE_COPY_SOURCE;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = texture;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = readback.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, kDisplayWidth, kDisplayHeight, 1, outputRowPitch};
            state.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            if (!state.submit()) return false;
            void* data = nullptr;
            if (FAILED(readback->Map(0, nullptr, &data))) return false;
            pixels.resize(static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 4);
            for (uint32_t y = 0; y < kDisplayHeight; ++y) {
                std::memcpy(pixels.data() + static_cast<size_t>(y) * kDisplayWidth * 4,
                            static_cast<const uint8_t*>(data) + static_cast<size_t>(y) * outputRowPitch,
                            static_cast<size_t>(kDisplayWidth) * 4);
            }
            readback->Unmap(0, nullptr);
            return true;
        };
        std::vector<uint8_t> third, twoThirds;
        if (!readTexture(output.Get(), outputState, third) || !readTexture(second.Get(), secondState, twoThirds)) return false;
        const auto truthThird = quantise(make_scene(config, 1.0f / 3.0f, kDisplayWidth, kDisplayHeight).color);
        const auto truthTwoThirds = quantise(make_scene(config, 2.0f / 3.0f, kDisplayWidth, kDisplayHeight).color);
        const double generatedThird = psnr(truthThird, third);
        const double generatedTwoThirds = psnr(truthTwoThirds, twoThirds);
        const double nearestThird = psnr(truthThird, previousColor);
        const double nearestTwoThirds = psnr(truthTwoThirds, currentColor);
        const bool phasesOk = generatedThird > nearestThird && generatedTwoThirds > nearestTwoThirds;
        std::printf("%s %-18s phases in one list: 1/3 psnr %6.2f dB (nearest real %6.2f), "
                    "2/3 psnr %6.2f dB (nearest real %6.2f)\n",
                    phasesOk ? "PASS" : "FAIL", config.name, generatedThird, nearestThird,
                    generatedTwoThirds, nearestTwoThirds);
        if (!phasesOk) return false;
    }

    // HUD preservation: the game's backbuffer carries a HUD the HUD-less input
    // does not. A known white block over a background-only corner must survive
    // the composite; it is a corner the synthetic scenes never reach.
    {
        const uint32_t x0 = 4, y0 = 4, x1 = 60, y1 = 40;
        auto hudColor = currentDisplay.color;
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = x0; x < x1; ++x) {
                const size_t index = (static_cast<size_t>(y) * kDisplayWidth + x) * 4;
                hudColor[index + 0] = 1.0f;
                hudColor[index + 1] = 1.0f;
                hudColor[index + 2] = 1.0f;
            }
        }
        const auto hudBytes = quantise(hudColor);
        auto hudTexture = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                         kDisplayWidth, kDisplayHeight, D3D12_RESOURCE_FLAG_NONE);
        auto hudStaging = create_buffer(state.device.Get(), outputRowPitch * kDisplayHeight,
                                        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!hudTexture || !hudStaging) return false;
        if (FAILED(state.allocator->Reset()) ||
            FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;

        upload(state.list.Get(), hudTexture.Get(), hudStaging.Get(), hudBytes.data(),
               kDisplayWidth, kDisplayHeight, 4);

        ffxConfigureDescFrameGeneration hudConfigure{};
        hudConfigure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        hudConfigure.frameGenerationEnabled = true;
        hudConfigure.HUDLessColor = ffxApiGetResourceDX12(frames[1].color.Get(), FFX_API_RESOURCE_STATE_COMMON);
        hudConfigure.frameID = kIterations;

        ffxDispatchDescFrameGenerationPrepare hudPrepare{};
        hudPrepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        hudPrepare.frameID = kIterations;
        hudPrepare.commandList = state.list.Get();
        hudPrepare.renderSize = {kRenderWidth, kRenderHeight};
        hudPrepare.motionVectorScale = {static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight)};
        hudPrepare.depth = ffxApiGetResourceDX12(frames[1].depth.Get(), FFX_API_RESOURCE_STATE_COMMON);
        hudPrepare.motionVectors = ffxApiGetResourceDX12(frames[1].motion.Get(), FFX_API_RESOURCE_STATE_COMMON);
        if (!context.configure(&hudConfigure.header) || !context.prepare(&hudPrepare.header)) return false;

        sedici::fg::HudSource hud{hudTexture.Get(), D3D12_RESOURCE_STATE_COMMON};
        if (!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, &hud)) {
            std::fprintf(stderr, "HUD composite generation failed\n");
            return false;
        }

        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = output.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        to.PlacedFootprint.Footprint.Width = kDisplayWidth;
        to.PlacedFootprint.Footprint.Height = kDisplayHeight;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = outputRowPitch;
        state.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        if (!state.submit() || !context.track_submission(state.fence.Get(), state.fenceValue)) return false;

        std::vector<uint8_t> composite(static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 4);
        void* hudMapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &hudMapped))) return false;
        for (uint32_t y = 0; y < kDisplayHeight; ++y) {
            std::memcpy(composite.data() + static_cast<size_t>(y) * kDisplayWidth * 4,
                        static_cast<const uint8_t*>(hudMapped) + static_cast<size_t>(y) * outputRowPitch,
                        static_cast<size_t>(kDisplayWidth) * 4);
        }
        readback->Unmap(0, nullptr);

        size_t inside = 0, insideWhite = 0;
        for (uint32_t y = y0 + 1; y < y1 - 1; ++y) {
            for (uint32_t x = x0 + 1; x < x1 - 1; ++x) {
                const size_t index = (static_cast<size_t>(y) * kDisplayWidth + x) * 4;
                ++inside;
                if (composite[index] > 216 && composite[index + 1] > 216 && composite[index + 2] > 216) {
                    ++insideWhite;
                }
            }
        }
        const double preserved = inside ? static_cast<double>(insideWhite) / inside : 0.0;
        const bool ok = preserved > 0.95;
        std::printf("%-18s hud preservation %.1f%%\n", config.name, preserved * 100.0);
        if (!ok) std::fprintf(stderr, "HUD layer did not survive compositing\n");
        if (!ok) return false;

        // K53: a reset seed emits the real frame, not a HUD-less phase. With a
        // HUD source the output is the game's own frame; without one, the
        // HUD-less colour.
        const auto seedOutput = [&](const sedici::fg::HudSource* source, uint64_t frameId,
                                    std::vector<uint8_t>& pixels) {
            if (FAILED(state.allocator->Reset()) ||
                FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return false;
            hudConfigure.frameID = frameId;
            hudPrepare.frameID = frameId;
            hudPrepare.unused_reset = true;
            if (!context.configure(&hudConfigure.header) || !context.prepare(&hudPrepare.header)) return false;
            if (!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, source))
                return false;
            state.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            if (!state.submit() || !context.track_submission(state.fence.Get(), state.fenceValue)) return false;
            void* data = nullptr;
            if (FAILED(readback->Map(0, nullptr, &data))) return false;
            pixels.resize(static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 4);
            for (uint32_t y = 0; y < kDisplayHeight; ++y) {
                std::memcpy(pixels.data() + static_cast<size_t>(y) * kDisplayWidth * 4,
                            static_cast<const uint8_t*>(data) + static_cast<size_t>(y) * outputRowPitch,
                            static_cast<size_t>(kDisplayWidth) * 4);
            }
            readback->Unmap(0, nullptr);
            return true;
        };
        std::vector<uint8_t> seededWithHud, seededWithout;
        const bool seeded = seedOutput(&hud, kIterations + 1, seededWithHud) &&
                            seedOutput(nullptr, kIterations + 2, seededWithout);
        const bool seedOk = seeded && seededWithHud == hudBytes &&
                            seededWithout == quantise(currentDisplay.color);
        std::printf("%s %-18s reset seed: the game frame with a HUD source, the HUD-less colour without\n",
                    seedOk ? "PASS" : "FAIL", config.name);
        return seedOk;
    }

    return true;
}


// Contract checks for the FFX-facing context: the descs Cyberpunk sends must be
// accepted, and anything the generator cannot honour must be refused so the
// caller can fall back.
int check_contract(Device& state)
{
    int failures = 0;
    auto expect = [&failures](bool condition, const char* what) {
        std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
        if (!condition) ++failures;
    };

    const auto color = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                      64, 36, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto depth = create_texture(state.device.Get(), DXGI_FORMAT_R32_FLOAT,
                                      32, 18, D3D12_RESOURCE_FLAG_NONE);
    const auto motion = create_texture(state.device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       32, 18, D3D12_RESOURCE_FLAG_NONE);
    const auto output = create_texture(state.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                       64, 36, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!color || !depth || !motion || !output) {
        std::printf("FAIL contract textures\n");
        return 1;
    }

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = state.device.Get();

    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &backend.header;
    create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    create.displaySize = {64, 36};
    create.maxRenderSize = {64, 36};
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;

    sedici::fg::Context context;
    expect(context.create(&create.header), "create accepts Cyberpunk's create desc");

    ffxConfigureDescFrameGeneration configure{};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.frameGenerationEnabled = true;
    configure.HUDLessColor.resource = color.Get();
    configure.HUDLessColor.state = FFX_API_RESOURCE_STATE_COMMON;
    configure.frameID = 7;
    expect(context.configure(&configure.header), "configure accepts the FrameGenerationConfig desc");

    ffxDispatchDescFrameGenerationPrepare prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.frameID = configure.frameID;
    state.allocator->Reset();
    state.list->Reset(state.allocator.Get(), nullptr);
    prepare.commandList = state.list.Get();
    prepare.renderSize = {32, 18};
    prepare.motionVectorScale = {32.0f, 18.0f};
    prepare.depth.resource = depth.Get();
    prepare.depth.state = FFX_API_RESOURCE_STATE_COMMON;
    prepare.motionVectors.resource = motion.Get();
    prepare.motionVectors.state = FFX_API_RESOURCE_STATE_COMMON;
    expect(context.prepare(&prepare.header), "prepare accepts the legacy prepare desc");
    expect(!context.retired() && !context.destroy(),
           "recorded prepare prevents premature teardown");

    // Queue ordering alone is not proof that the borrowed prepare retired.
    // A generation rejection must leave its resources protected (K45).
    context.note_ordered_submission();
    expect(!context.generate(state.list.Get(), static_cast<ID3D12Resource*>(nullptr),
                             D3D12_RESOURCE_STATE_COMMON) && !context.retired() && !context.destroy(),
           "rejected ordered generation keeps the prepare unretired");

    const bool generated = context.generate(state.list.Get(), output.Get(),
                                            D3D12_RESOURCE_STATE_COMMON);
    expect(generated, "generate produces a frame");
    expect(state.submit() && context.track_submission(state.fence.Get(), state.fenceValue),
           "the frame command list executes and registers retirement");
    expect(!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "a second generate without a new prepare is refused");

    // The FFX description hides the native depth format. Exercise the actual
    // D32 and two-plane depth/stencil allocations through Context, not only
    // Stage1 with a hand-selected view. Each must use a typed depth-plane SRV.
    for (const auto format : {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_TYPELESS,
                              DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT}) {
        const bool stencil = format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                             format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        const DXGI_FORMAT dsvFormat = stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = dsvFormat;
        clear.DepthStencil.Depth = 0.75f;
        clear.DepthStencil.Stencil = 37;
        const auto nativeDepth = create_texture(state.device.Get(), format, 32, 18,
                                                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, &clear);
        if (!nativeDepth) { expect(false, "native depth texture creation"); continue; }
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> heap;
        if (FAILED(state.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)))) return failures + 1;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = dsvFormat;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        const auto target = heap->GetCPUDescriptorHandleForHeapStart();
        state.device->CreateDepthStencilView(nativeDepth.Get(), &dsv, target);
        if (FAILED(state.allocator->Reset()) ||
            FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return failures + 1;
        transition(state.list.Get(), nativeDepth.Get(), D3D12_RESOURCE_STATE_COMMON,
                   D3D12_RESOURCE_STATE_DEPTH_WRITE);
        state.list->ClearDepthStencilView(target, stencil
            ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH,
            0.75f, 37, 0, nullptr);
        transition(state.list.Get(), nativeDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        prepare.depth = ffxApiGetResourceDX12(nativeDepth.Get(),
            FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        const bool accepted = context.prepare(&prepare.header);
        const char* label = format == DXGI_FORMAT_D32_FLOAT
            ? "prepare samples native D32_FLOAT using an R32_FLOAT view"
            : format == DXGI_FORMAT_R32_TYPELESS
            ? "prepare samples native R32_TYPELESS using an R32_FLOAT view"
            : format == DXGI_FORMAT_R32G8X24_TYPELESS
            ? "prepare samples Cyberpunk's R32G8X24_TYPELESS depth plane"
            : "prepare samples native D32_FLOAT_S8X24_UINT depth plane";
        expect(accepted, label);
        const bool generatedDepth = accepted && context.generate(state.list.Get(), output.Get(),
                                                                  D3D12_RESOURCE_STATE_COMMON);
        expect(generatedDepth, "native depth snapshot generates a frame");
        const bool submitted = state.submit();
        expect(submitted && (!accepted || context.track_submission(state.fence.Get(), state.fenceValue)),
               "native depth work retires with input states restored");
    }
    prepare.depth = ffxApiGetResourceDX12(depth.Get(), FFX_API_RESOURCE_STATE_COMMON);

    prepare.depth.resource = color.Get();
    expect(!context.prepare(&prepare.header) && !context.pending(),
           "incompatible native depth format is still refused");
    prepare.depth.resource = depth.Get();

    prepare.motionVectors.resource = color.Get();
    expect(!context.prepare(&prepare.header) && !context.pending(),
           "a non-float motion resource is refused and clears pending preparation");
    prepare.motionVectors.resource = motion.Get();

    configure.frameGenerationEnabled = false;
    expect(context.configure(&configure.header), "configure accepts frame generation off");
    expect(!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "generate is refused while frame generation is off");

    // One deterministic lifetime check, not a timing race: hold a prepare
    // submission behind a gate, invalidate it, then recover after retirement.
    ComPtr<ID3D12CommandQueue> prepareQueue;
    ComPtr<ID3D12CommandAllocator> prepareAllocator;
    ComPtr<ID3D12GraphicsCommandList> prepareList;
    ComPtr<ID3D12Fence> prepareFence, gate;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (FAILED(state.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&prepareQueue))) ||
        FAILED(state.device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&prepareAllocator))) ||
        FAILED(state.device->CreateCommandList(0, queueDesc.Type, prepareAllocator.Get(), nullptr,
                                               IID_PPV_ARGS(&prepareList))) ||
        FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&prepareFence))) ||
        FAILED(state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))) return failures + 1;

    configure.frameGenerationEnabled = true;
    configure.frameID = 8;
    prepare.frameID = 8;
    prepare.commandList = prepareList.Get();
    expect(context.configure(&configure.header) && context.prepare(&prepare.header),
           "next prepare records on a separate compute queue");
    configure.frameGenerationEnabled = false;
    expect(context.configure(&configure.header) && !context.pending(),
           "FG off invalidates the recorded snapshot without freeing it");
    if (FAILED(prepareList->Close())) return failures + 1;
    // Only this test gate is CPU-signalled; retirement is queue-signalled.
    if (FAILED(prepareQueue->Wait(gate.Get(), 1))) return failures + 1;
    ID3D12CommandList* prepareLists[]{prepareList.Get()};
    prepareQueue->ExecuteCommandLists(1, prepareLists);
    const HRESULT signaled = prepareQueue->Signal(prepareFence.Get(), 1);
    const bool tracked = SUCCEEDED(signaled) && context.track_submission(prepareFence.Get(), 1);
    expect(tracked && !context.retired(), "submitted prepare remains occupied behind its GPU fence");
    expect(!context.wait_for_retirement(2) && !context.retired(),
           "a bounded retirement timeout leaves gated GPU work unretired");
    expect(!context.prepare(&prepare.header) && !context.destroy(),
           "in-flight snapshot refuses reuse and teardown");
    // Always open the gate before any return or blocking wait.
    if (FAILED(gate->Signal(1))) return failures + 1;
    if (FAILED(state.queue->Wait(prepareFence.Get(), 1)) || !state.wait()) return failures + 1;
    expect(context.retired(), "prepare becomes reusable after GPU retirement");

    if (FAILED(prepareAllocator->Reset()) ||
        FAILED(prepareList->Reset(prepareAllocator.Get(), nullptr))) return failures + 1;
    configure.frameGenerationEnabled = true;
    configure.frameID = 9;
    prepare.frameID = 9;
    expect(context.configure(&configure.header) && context.prepare(&prepare.header),
           "FG re-enable recovers an abandoned retired snapshot");
    if (FAILED(prepareList->Close())) return failures + 1;
    prepareQueue->ExecuteCommandLists(1, prepareLists);
    if (FAILED(prepareQueue->Signal(prepareFence.Get(), 2))) return failures + 1;
    if (FAILED(state.allocator->Reset()) ||
        FAILED(state.list->Reset(state.allocator.Get(), nullptr))) return failures + 1;
    expect(context.track_submission(prepareFence.Get(), 2), "split prepare registers its completion fence");
    expect(!context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "split generation without an explicit queue dependency is refused");
    expect(context.wait_prepare(state.queue.Get()),
           "generation queue waits for the registered compute prepare");
    expect(context.generate(state.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COMMON),
           "split-queue generation consumes the recovered snapshot");
    expect(state.submit() && context.track_submission(state.fence.Get(), state.fenceValue),
           "generation retirement covers the cross-queue dependency");

    expect(context.destroy(), "retired context destroys safely");
    return failures;
}


} // namespace fgcheck
