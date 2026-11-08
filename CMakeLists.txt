cmake_minimum_required(VERSION 3.21)
project(sedici LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(SEDICI_FFX_HEADERS
    third_party/ffx/api/include/ffx_api.h
    third_party/ffx/api/include/ffx_api_types.h
    third_party/ffx/api/include/dx12/ffx_api_dx12.h
    third_party/ffx/framegeneration/include/ffx_framegeneration.h
    third_party/ffx/framegeneration/include/ffx_framegeneration_api_types.h
    third_party/ffx/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h
)
foreach(header IN LISTS SEDICI_FFX_HEADERS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${header}")
        message(FATAL_ERROR "Missing required FidelityFX header: ${header}. See third_party/ffx/README.md.")
    endif()
endforeach()

# Vendored, unmodified single-header toml++ v3.4.0 (MIT). See README.md.
set(SEDICI_TOML_HEADER third_party/tomlplusplus/toml.hpp)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SEDICI_TOML_HEADER}")
    message(FATAL_ERROR
        "Missing ${SEDICI_TOML_HEADER}. Download the single-header toml.hpp from "
        "https://github.com/marzer/tomlplusplus/releases/tag/v3.4.0 and place it there unmodified.")
endif()

# Vendored, unmodified Dear ImGui v1.93.0. See third_party/imgui/README.md.
set(SEDICI_IMGUI_DIR third_party/imgui)
set(SEDICI_IMGUI_SOURCES
    ${SEDICI_IMGUI_DIR}/imgui.cpp
    ${SEDICI_IMGUI_DIR}/imgui_draw.cpp
    ${SEDICI_IMGUI_DIR}/imgui_tables.cpp
    ${SEDICI_IMGUI_DIR}/imgui_widgets.cpp
    ${SEDICI_IMGUI_DIR}/backends/imgui_impl_dx12.cpp
    ${SEDICI_IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
foreach(source IN LISTS SEDICI_IMGUI_SOURCES)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "Missing vendored Dear ImGui source: ${source}")
    endif()
endforeach()

# Everything except the five exports lives in one object library, so the DLL and
# every test host compile the same production objects (no private source lists).
add_library(sedici_core OBJECT
    src/describe.cpp
    src/stats.cpp
    src/limiter.cpp
    src/overlay.cpp
    src/overlay_compose.cpp
    src/timing.cpp
    src/live.cpp
    src/settings_tx.cpp
    src/settings_io.cpp
    src/ui.cpp
    src/ui_input.cpp
    src/config_patch.cpp
    src/config.cpp
    src/log.cpp
    src/diag.cpp
    src/fg_params.cpp
    src/fg_bookkeeping_faults.cpp
    src/fg_stage1.cpp
    src/fg_context.cpp
    src/fg_swapchain.cpp
    src/fg_presenter.cpp
    src/fg_pacing.cpp
    src/fg_integration.cpp
    src/fg_route.cpp
    ${SEDICI_IMGUI_SOURCES}
)

target_include_directories(sedici_core PUBLIC
    src
    third_party/ffx/api/include
    third_party/ffx/api/include/dx12
    third_party/ffx/framegeneration/include
    third_party/ffx/framegeneration/include/dx12
    third_party/tomlplusplus
    ${SEDICI_IMGUI_DIR}
    ${SEDICI_IMGUI_DIR}/backends
)

target_compile_definitions(sedici_core PUBLIC
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    IMGUI_USER_CONFIG="imgui_config.h"
)

if(MSVC)
    target_compile_options(sedici_core PUBLIC /utf-8 /W4 /permissive-)
endif()

target_link_libraries(sedici_core PUBLIC dxgi d3d12 d3dcompiler bcrypt)

add_library(sedici SHARED src/dllmain.cpp exports.def)
target_link_libraries(sedici PRIVATE sedici_core)
target_sources(sedici PRIVATE $<TARGET_OBJECTS:sedici_core>)

set_target_properties(sedici PROPERTIES
    OUTPUT_NAME amd_fidelityfx_framegeneration_dx12
    PREFIX ""
)

option(SEDICI_BUILD_TESTS "Build the offline Windows regression checks" OFF)
if(SEDICI_BUILD_TESTS)
    enable_testing()
    add_executable(sedici_regression tests/regression.cpp $<TARGET_OBJECTS:sedici_core>)
    target_link_libraries(sedici_regression PRIVATE sedici_core)
    set_target_properties(sedici_regression PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME sedici_regression COMMAND sedici_regression)
    add_test(NAME sedici_gpu_mini COMMAND sedici_regression gpu-mini)
    add_test(NAME sedici_gpu_full COMMAND sedici_regression gpu-full)
    add_test(NAME sedici_settings_sedici_capability COMMAND sedici_regression settings-sedici)
    add_test(NAME sedici_settings_env_priority COMMAND sedici_regression settings-env)

    # Deploy/restore safety rules against a throwaway fake repo and game folder.
    add_test(NAME sedici_deploy_tools COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_SOURCE_DIR}/tests/deploy_check.ps1 -Dll $<TARGET_FILE:sedici> -Repo ${CMAKE_SOURCE_DIR})
    set_tests_properties(sedici_deploy_tools PROPERTIES TIMEOUT 120 LABELS "packaging")

    # Test-only module and isolated host: unlike the proxy, this helper exports
    # one control entry so the host can verify the worker's DLL reference.
    add_library(sedici_settings_worker_stub SHARED
        tests/settings_worker_stub.cpp $<TARGET_OBJECTS:sedici_core>)
    target_link_libraries(sedici_settings_worker_stub PRIVATE sedici_core)
    set_target_properties(sedici_settings_worker_stub PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_executable(sedici_settings_worker_host tests/settings_worker_host.cpp)
    set_target_properties(sedici_settings_worker_host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_dependencies(sedici_settings_worker_host sedici_settings_worker_stub)
    add_test(NAME sedici_settings_worker_lifetime
        COMMAND sedici_settings_worker_host $<TARGET_FILE:sedici_settings_worker_stub>)
    set_tests_properties(sedici_settings_worker_lifetime PROPERTIES RUN_SERIAL TRUE TIMEOUT 20 LABELS "settings")

    # Each process owns the same isolated test config beside its executable.
    set_tests_properties(sedici_regression sedici_gpu_mini sedici_gpu_full
        sedici_settings_sedici_capability sedici_settings_env_priority
        PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)

    # Export-level harness: the built DLL through its five exports, next to a
    # stub "original" that records what is forwarded (tests/abi_stub.h). Each
    # scenario runs in its own process and scratch directory.
    add_library(sedici_abi_stub SHARED tests/abi_stub.cpp)
    target_include_directories(sedici_abi_stub PRIVATE
        third_party/ffx/api/include
        third_party/ffx/framegeneration/include
        third_party/ffx/framegeneration/include/dx12)
    target_compile_options(sedici_abi_stub PRIVATE /utf-8 /W4 /permissive-)
    set_target_properties(sedici_abi_stub PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_executable(sedici_abi tests/abi.cpp)
    target_include_directories(sedici_abi PRIVATE
        third_party/ffx/api/include
        third_party/ffx/api/include/dx12
        third_party/ffx/framegeneration/include
        third_party/ffx/framegeneration/include/dx12)
    target_compile_definitions(sedici_abi PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_compile_options(sedici_abi PRIVATE /utf-8 /W4 /permissive-)
    target_link_libraries(sedici_abi PRIVATE dxgi d3d12)
    add_dependencies(sedici_abi sedici sedici_abi_stub)
    set_target_properties(sedici_abi PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    foreach(scenario IN ITEMS exports contract legacy-config readonly-sedici bad-config session
            window-replace destroy-inflight log-failure invalid-calls overlay-hotkey stale-forward)
        add_test(NAME sedici_abi_${scenario}
            COMMAND sedici_abi ${scenario} $<TARGET_FILE:sedici> $<TARGET_FILE:sedici_abi_stub>)
        set_tests_properties(sedici_abi_${scenario} PROPERTIES TIMEOUT 60 LABELS "abi")
    endforeach()
    # The session scenario opens a window and presents; keep it alone on the GPU.
    foreach(scenario IN ITEMS session window-replace destroy-inflight log-failure invalid-calls overlay-hotkey stale-forward readonly-sedici)
        set_tests_properties(sedici_abi_${scenario} PROPERTIES RUN_SERIAL TRUE LABELS "abi;gpu")
    endforeach()
endif()

# Offline acceptance host for the frame generator (see MILESTONES.md, P1.4).
# It runs the production stage 1 on a synthetic scene and prints PSNR, SSIM and
# GPU time; --images also writes the two frames as PPM.
option(SEDICI_BUILD_FGCHECK "Build the offline frame generation acceptance host" OFF)
if(SEDICI_BUILD_FGCHECK)
    add_executable(sedici_fgcheck tests/fgcheck.cpp $<TARGET_OBJECTS:sedici_core>)
    target_link_libraries(sedici_fgcheck PRIVATE sedici_core)
    set_target_properties(sedici_fgcheck PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    if(SEDICI_BUILD_TESTS)
        # Stage 1 quality, context contract and schedules; then the windowed
        # presentation checks (known failures are reported as XFAIL, see
        # docs/refactor/reliability.md); then routing with its own sedici.toml.
        add_test(NAME sedici_fgcheck COMMAND sedici_fgcheck)
        add_test(NAME sedici_fgcheck_present COMMAND sedici_fgcheck --present)
        add_test(NAME sedici_fgcheck_routing COMMAND sedici_fgcheck --routing)
        # P2.5: RemoveDevice after generation, in its own process.
        add_test(NAME sedici_fgcheck_device_loss COMMAND sedici_fgcheck --device-loss)
        add_test(NAME sedici_fgcheck_submission_fault COMMAND sedici_fgcheck --submission-fault)
        add_test(NAME sedici_fgcheck_submission_fault_vrr COMMAND sedici_fgcheck --submission-fault-vrr)
        add_test(NAME sedici_fgcheck_recording_fault COMMAND sedici_fgcheck --recording-fault)
        add_test(NAME sedici_fgcheck_recording_fault_vrr COMMAND sedici_fgcheck --recording-fault-vrr)
        add_test(NAME sedici_fgcheck_reset_fault COMMAND sedici_fgcheck --reset-fault)
        add_test(NAME sedici_fgcheck_reset_fault_vrr COMMAND sedici_fgcheck --reset-fault-vrr)
        add_test(NAME sedici_fgcheck_context_race COMMAND sedici_fgcheck --context-race)
        add_test(NAME sedici_fgcheck_session_recovery COMMAND sedici_fgcheck --session-recovery)
        add_test(NAME sedici_fgcheck_registration_recovery COMMAND sedici_fgcheck --registration-recovery)
        add_test(NAME sedici_fgcheck_submission_recovery_vrr COMMAND sedici_fgcheck --submission-recovery-vrr)
        add_test(NAME sedici_fgcheck_params_fault_vrr COMMAND sedici_fgcheck --params-fault-vrr)
        add_test(NAME sedici_fgcheck_retention_timeout COMMAND sedici_fgcheck --retention-timeout)
        set_tests_properties(sedici_fgcheck_device_loss sedici_fgcheck_submission_fault sedici_fgcheck_submission_fault_vrr
            sedici_fgcheck_recording_fault sedici_fgcheck_recording_fault_vrr
            sedici_fgcheck_reset_fault sedici_fgcheck_reset_fault_vrr
            sedici_fgcheck_submission_recovery_vrr sedici_fgcheck_params_fault_vrr PROPERTIES SKIP_RETURN_CODE 77)
        set_tests_properties(sedici_fgcheck sedici_fgcheck_present sedici_fgcheck_routing sedici_fgcheck_device_loss sedici_fgcheck_submission_fault sedici_fgcheck_submission_fault_vrr sedici_fgcheck_recording_fault sedici_fgcheck_recording_fault_vrr sedici_fgcheck_reset_fault sedici_fgcheck_reset_fault_vrr sedici_fgcheck_context_race sedici_fgcheck_session_recovery sedici_fgcheck_registration_recovery sedici_fgcheck_submission_recovery_vrr sedici_fgcheck_params_fault_vrr sedici_fgcheck_retention_timeout
            PROPERTIES RUN_SERIAL TRUE TIMEOUT 300 LABELS "fgcheck;gpu")
    endif()
endif()
