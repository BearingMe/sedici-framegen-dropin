cmake_minimum_required(VERSION 3.21)
project(sedici LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(SEDICI_SANITIZE STREQUAL "address")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL") # the ASan runtime needs the dynamic CRT; the UBSan libraries are static
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

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
# K140: the "build <date> <time>" line in sedici.log comes from config.cpp, so it only moved when that
# file changed and two different DLLs could carry one stamp. The output below is never created, so the
# command is always out of date and config.cpp is compiled again on every build.
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/always_stamp
    COMMAND ${CMAKE_COMMAND} -E echo_append "" VERBATIM)
set_source_files_properties(src/settings/config.cpp PROPERTIES OBJECT_DEPENDS ${CMAKE_BINARY_DIR}/always_stamp)

add_library(sedici_core OBJECT
    src/abi/describe.cpp
    src/diagnostics/stats.cpp
    src/presentation/limiter.cpp
    src/ui/overlay.cpp
    src/presentation/overlay_compose.cpp
    src/diagnostics/timing.cpp
    src/settings/live.cpp
    src/settings/settings_tx.cpp
    src/settings/settings_io.cpp
    src/ui/ui.cpp
    src/ui/ui_input.cpp
    src/settings/config_patch.cpp
    src/settings/config.cpp
    src/diagnostics/log.cpp
    src/status/diag.cpp
    src/generator/params.cpp
    src/diagnostics/bookkeeping_faults.cpp
    src/generator/stage1.cpp
    src/runtime/context.cpp
    src/presentation/swapchain.cpp
    src/presentation/presenter.cpp
    src/presentation/pacing.cpp
    src/runtime/integration.cpp
    src/abi/route.cpp
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
    IMGUI_USER_CONFIG="ui/imgui_config.h"
)

if(MSVC)
    target_compile_options(sedici_core PUBLIC /utf-8 /W4 /permissive-)
endif()

target_link_libraries(sedici_core PUBLIC dxgi d3d12 d3dcompiler bcrypt)

# ---------------------------------------------------------------------------------------------
# Developer tooling (docs/dev/workflow.md, tools/check.ps1). Every option is OFF by default, so the
# default build and the shipped DLL are exactly what they were. Vendored code (third_party/) is
# never analysed, linted or formatted.
# ---------------------------------------------------------------------------------------------
option(SEDICI_WERROR  "Treat compiler warnings as errors (/WX)" OFF)
option(SEDICI_ANALYZE "Run MSVC static analysis (/analyze) on Sedici's own sources" OFF)
option(SEDICI_TIDY    "Run clang-tidy on Sedici's own sources (Ninja generator)" OFF)
option(SEDICI_CRTDBG  "Test hosts report CPU heap leaks at exit through the CRT debug heap" OFF)
set(SEDICI_SANITIZE "" CACHE STRING "Sanitizer for the test hosts: address, undefined (needs clang-cl)")

set(SEDICI_LLVM_HINTS "$ENV{ProgramFiles}/LLVM/bin" "C:/Program Files/LLVM/bin")
set(SEDICI_VENDORED_SOURCES ${SEDICI_IMGUI_SOURCES})

if(SEDICI_TIDY)
    find_program(SEDICI_CLANG_TIDY clang-tidy HINTS ${SEDICI_LLVM_HINTS} REQUIRED)
    # /EHsc is added to the compiler line by CMake, but clang-tidy parses without it and rejects try/throw.
    set(CMAKE_CXX_CLANG_TIDY "${SEDICI_CLANG_TIDY};--extra-arg=/EHsc")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set_source_files_properties(${SEDICI_VENDORED_SOURCES} PROPERTIES SKIP_LINTING ON)
endif()

if(SEDICI_ANALYZE)
    set_source_files_properties(${SEDICI_VENDORED_SOURCES} PROPERTIES COMPILE_OPTIONS "/analyze-")
endif()

if(SEDICI_SANITIZE)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "SEDICI_SANITIZE needs clang-cl (this machine's MSVC ships only an x86 ASan runtime). "
                            "Use tools/check.ps1, or configure with -DCMAKE_CXX_COMPILER=clang-cl -G Ninja.")
    endif()
endif()

# clang-format: format rewrites src/ and tests/, format-check only reports. Nothing else is passed to it.
find_program(SEDICI_CLANG_FORMAT clang-format HINTS ${SEDICI_LLVM_HINTS})
if(SEDICI_CLANG_FORMAT)
    file(GLOB_RECURSE SEDICI_FORMAT_FILES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/src/*.cpp ${CMAKE_SOURCE_DIR}/src/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.cpp ${CMAKE_SOURCE_DIR}/tests/*.h)
    add_custom_target(format
        COMMAND ${SEDICI_CLANG_FORMAT} --style=file -i ${SEDICI_FORMAT_FILES}
        COMMENT "clang-format: rewriting src/ and tests/" VERBATIM)
    add_custom_target(format-check
        COMMAND ${SEDICI_CLANG_FORMAT} --style=file --dry-run --Werror ${SEDICI_FORMAT_FILES}
        COMMENT "clang-format: checking src/ and tests/" VERBATIM)
endif()

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
    add_executable(sedici_regression tests/regression/regression.cpp $<TARGET_OBJECTS:sedici_core>)
    target_link_libraries(sedici_regression PRIVATE sedici_core)
    set_target_properties(sedici_regression PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_library(sedici_cursor_probe SHARED tests/regression/cursor_probe.cpp)
    set_target_properties(sedici_cursor_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_dependencies(sedici_regression sedici_cursor_probe)
    add_test(NAME sedici_regression COMMAND sedici_regression)
    add_test(NAME sedici_gpu_mini COMMAND sedici_regression gpu-mini)
    add_test(NAME sedici_gpu_full COMMAND sedici_regression gpu-full)
    add_test(NAME sedici_settings_sedici_capability COMMAND sedici_regression settings-sedici)
    add_test(NAME sedici_settings_env_priority COMMAND sedici_regression settings-env)

    # Deploy/restore safety rules against a throwaway fake repo and game folder.
    add_test(NAME sedici_deploy_tools COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_SOURCE_DIR}/tests/tools/deploy_check.ps1 -Dll $<TARGET_FILE:sedici> -Repo ${CMAKE_SOURCE_DIR})
    set_tests_properties(sedici_deploy_tools PROPERTIES TIMEOUT 120 LABELS "packaging")

    # P4.5: every module include goes to a strictly lower layer, so no cycle can form.
    add_test(NAME sedici_layers COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_SOURCE_DIR}/tools/check_layers.ps1 -Root ${CMAKE_SOURCE_DIR})
    set_tests_properties(sedici_layers PROPERTIES TIMEOUT 60 LABELS "structure")

    # P4.6/P4.7: the log and present-interval analyzers the owner's QA runs are judged with.
    add_test(NAME sedici_analyzers COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_SOURCE_DIR}/tests/tools/analyze_check.ps1 -Repo ${CMAKE_SOURCE_DIR})
    set_tests_properties(sedici_analyzers PROPERTIES TIMEOUT 120 LABELS "structure")

    # Test-only module and isolated host: unlike the proxy, this helper exports
    # one control entry so the host can verify the worker's DLL reference.
    add_library(sedici_settings_worker_stub SHARED
        tests/settings/settings_worker_stub.cpp $<TARGET_OBJECTS:sedici_core>)
    target_link_libraries(sedici_settings_worker_stub PRIVATE sedici_core)
    set_target_properties(sedici_settings_worker_stub PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_executable(sedici_settings_worker_host tests/settings/settings_worker_host.cpp)
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
    # stub "original" that records what is forwarded (tests/abi/abi_stub.h). Each
    # scenario runs in its own process and scratch directory.
    add_library(sedici_abi_stub SHARED tests/abi/abi_stub.cpp)
    target_include_directories(sedici_abi_stub PRIVATE
        third_party/ffx/api/include
        third_party/ffx/framegeneration/include
        third_party/ffx/framegeneration/include/dx12)
    target_compile_options(sedici_abi_stub PRIVATE /utf-8 /W4 /permissive-)
    set_target_properties(sedici_abi_stub PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_executable(sedici_abi tests/abi/abi.cpp)
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
    foreach(scenario IN ITEMS exports contract legacy-config target-auto readonly-sedici bad-config session
            window-replace window-replace-vsync destroy-inflight log-failure invalid-calls overlay-hotkey stale-forward unload integrated)
        add_test(NAME sedici_abi_${scenario}
            COMMAND sedici_abi ${scenario} $<TARGET_FILE:sedici> $<TARGET_FILE:sedici_abi_stub>)
        set_tests_properties(sedici_abi_${scenario} PROPERTIES TIMEOUT 60 LABELS "abi")
    endforeach()
    # K137: these tests take the foreground window, so two of them (or the regression host's window
    # checks) running together steal it from each other. One lock serialises them.
    set_tests_properties(sedici_abi_overlay-hotkey sedici_abi_integrated sedici_regression
        PROPERTIES RESOURCE_LOCK foreground_window)
    # The session scenario opens a window and presents; keep it alone on the GPU.
    foreach(scenario IN ITEMS session window-replace window-replace-vsync destroy-inflight log-failure invalid-calls overlay-hotkey stale-forward readonly-sedici unload integrated)
        set_tests_properties(sedici_abi_${scenario} PROPERTIES RUN_SERIAL TRUE LABELS "abi;gpu")
    endforeach()
endif()

# Offline acceptance host for the frame generator (see MILESTONES.md, P1.4).
# It runs the production stage 1 on a synthetic scene and prints PSNR, SSIM and
# GPU time; --images also writes the two frames as PPM.
option(SEDICI_BUILD_FGCHECK "Build the offline frame generation acceptance host" OFF)
if(SEDICI_BUILD_FGCHECK)
    add_executable(sedici_fgcheck
        tests/integration/fgcheck.cpp
        tests/integration/fgcheck_quality.cpp
        tests/integration/fgcheck_tenbit.cpp
        tests/integration/fgcheck_pacing.cpp
        tests/integration/fgcheck_present.cpp
        tests/integration/fgcheck_routing.cpp
        tests/integration/fgcheck_retention.cpp
        $<TARGET_OBJECTS:sedici_core>)
    target_include_directories(sedici_fgcheck PRIVATE tests/integration)
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

# Developer tooling flags, applied to every Sedici target that exists in this configuration.
# Called last so test targets are covered. Does nothing unless a SEDICI_* tooling option is set.
set(SEDICI_TEST_HOSTS sedici_regression sedici_abi sedici_fgcheck)
function(sedici_apply_tooling)
    foreach(tgt IN ITEMS sedici_core sedici sedici_regression sedici_cursor_probe sedici_settings_worker_stub
            sedici_settings_worker_host sedici_abi sedici_abi_stub sedici_fgcheck)
        if(NOT TARGET ${tgt})
            continue()
        endif()
        if(tgt IN_LIST SEDICI_TEST_HOSTS)
            target_include_directories(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
        endif()
        if(SEDICI_WERROR AND MSVC)
            target_compile_options(${tgt} PRIVATE /WX)
        endif()
        if(SEDICI_ANALYZE AND MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # Own sources only: vendored headers are external so /analyze skips them and their warnings.
            target_compile_options(${tgt} PRIVATE /analyze /analyze:external-
                /external:W0
                /external:I${CMAKE_SOURCE_DIR}/third_party/ffx
                /external:I${CMAKE_SOURCE_DIR}/third_party/tomlplusplus
                /external:I${CMAKE_SOURCE_DIR}/third_party/imgui)
        endif()
        if(SEDICI_CRTDBG)
            target_compile_definitions(${tgt} PRIVATE SEDICI_CRTDBG=1)
            if(tgt IN_LIST SEDICI_TEST_HOSTS)
                target_link_libraries(${tgt} PRIVATE dxguid) # DXGI_DEBUG_ALL for tests/common/leak_check.h
            endif()
        endif()
        if(SEDICI_SANITIZE)
            # /Z7 + /DEBUG: without a PDB the sanitizer prints no source lines (K145 could not be located).
            target_compile_options(${tgt} PRIVATE -fsanitize=${SEDICI_SANITIZE} -fno-omit-frame-pointer /Z7)
            target_link_options(${tgt} PRIVATE /DEBUG)
            if(SEDICI_SANITIZE STREQUAL "address")
                # Global-variable instrumentation reports false positives with merged string literals under
                # clang-cl (odr-violation "-" vs L"-", an overflow read of a NUL-terminated literal); heap and stack
                # checks, the point of this run, are unaffected.
                target_compile_options(${tgt} PRIVATE -mllvm -asan-globals=0)
            endif()
            # stl_asan.lib (container annotations) is part of an optional VS component that is not installed here.
            target_compile_definitions(${tgt} PRIVATE _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
            get_target_property(kind ${tgt} TYPE)
            if(SEDICI_SANITIZE STREQUAL "address")
                if(kind STREQUAL "EXECUTABLE" OR kind STREQUAL "SHARED_LIBRARY")
                    target_link_directories(${tgt} PRIVATE "${SEDICI_CLANG_RT_DIR}")
                    target_link_libraries(${tgt} PRIVATE clang_rt.asan_dynamic-x86_64
                        clang_rt.asan_dynamic_runtime_thunk-x86_64)
                    target_link_options(${tgt} PRIVATE /wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib)
                endif()
            else()
                if(kind STREQUAL "EXECUTABLE" OR kind STREQUAL "SHARED_LIBRARY")
                    target_link_directories(${tgt} PRIVATE "${SEDICI_CLANG_RT_DIR}")
                    target_link_libraries(${tgt} PRIVATE clang_rt.ubsan_standalone-x86_64 clang_rt.ubsan_standalone_cxx-x86_64)
                endif()
            endif()
        endif()
    endforeach()
endfunction()

if(SEDICI_SANITIZE)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} /clang:-print-resource-dir
        OUTPUT_VARIABLE SEDICI_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(SEDICI_CLANG_RT_DIR "${SEDICI_CLANG_RESOURCE_DIR}/lib/windows")
endif()
sedici_apply_tooling()
