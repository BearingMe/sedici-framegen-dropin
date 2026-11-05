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

add_library(sedici SHARED
    src/dllmain.cpp
    src/forward.cpp
    src/describe.cpp
    src/present_hook.cpp
    src/stats.cpp
    src/limiter.cpp
    src/toggles.cpp
    src/config.cpp
    src/log.cpp
    exports.def
)

target_include_directories(sedici PRIVATE
    src
    third_party/ffx/api/include
    third_party/ffx/api/include/dx12
    third_party/ffx/framegeneration/include
    third_party/ffx/framegeneration/include/dx12
    third_party/tomlplusplus
)

target_compile_definitions(sedici PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(sedici PRIVATE /utf-8 /W4 /permissive-)
endif()

target_link_libraries(sedici PRIVATE dxgi)

set_target_properties(sedici PROPERTIES
    OUTPUT_NAME amd_fidelityfx_framegeneration_dx12
    PREFIX ""
)
