cmake_minimum_required(VERSION 3.24)
project(fsr4_linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

# The FFX API headers come from the user's FidelityFX SDK 2.4.0 tree. Inside the
# SDK this directory is <sdk>/linux and the default below is right; when the
# port is vendored outside the SDK (tools/fsr4-linux) pass -DFSR4_SDK_ROOT=<sdk>
# or set the FSR4_SDK_ROOT environment variable.
set(FSR4_SDK_ROOT "$ENV{FSR4_SDK_ROOT}" CACHE PATH
    "FidelityFX SDK 2.4.0 tree (contains Kits/FidelityFX)")
if(NOT FSR4_SDK_ROOT)
    set(FSR4_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
get_filename_component(FSR4_SDK_ROOT "${FSR4_SDK_ROOT}" ABSOLUTE)
if(NOT EXISTS "${FSR4_SDK_ROOT}/Kits/FidelityFX/api/include/ffx_api.h")
    message(FATAL_ERROR
        "FSR4_SDK_ROOT must point at the FidelityFX SDK 2.4.0 tree "
        "(got '${FSR4_SDK_ROOT}'); set -DFSR4_SDK_ROOT or the FSR4_SDK_ROOT environment variable")
endif()
set(FSR4_FFX_API_INCLUDE "${FSR4_SDK_ROOT}/Kits/FidelityFX/api/include")
set(FSR4_FFX_UPSCALER_INCLUDE "${FSR4_SDK_ROOT}/Kits/FidelityFX/upscalers/include")

# Recovered 4.1.2 ABI: tensor sizes, scratch layout, constants blocks.
add_library(fsr4_abi STATIC src/abi/fsr4_412_abi.cpp)
target_include_directories(fsr4_abi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(fsr4_abi PRIVATE -Wall -Wextra)

# Byte-comparison test against captured reference traces.
add_executable(fsr4_abi_test tests/abi/abi_test.cpp)
target_link_libraries(fsr4_abi_test PRIVATE fsr4_abi)
target_compile_options(fsr4_abi_test PRIVATE -Wall -Wextra)

# Phase 0 bring-up: one-case native runner against the 4.1.2 SPIR-V extracted
# from the user's signed DLL. The provider/loader targets are added in M1.1.
add_executable(fsr4_native_runner src/runner/fsr4_native_runner.cpp)
target_link_libraries(fsr4_native_runner PRIVATE Vulkan::Vulkan fsr4_abi)
target_compile_options(fsr4_native_runner PRIVATE -Wall -Wextra)

# Native FFX provider and loader (M1.1b).
add_library(amd_fidelityfx_upscaler_vk SHARED src/provider/ffx_provider.cpp)
target_include_directories(amd_fidelityfx_upscaler_vk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FSR4_FFX_API_INCLUDE}
    ${FSR4_FFX_UPSCALER_INCLUDE})
target_link_libraries(amd_fidelityfx_upscaler_vk PRIVATE Vulkan::Vulkan fsr4_abi)
set_target_properties(amd_fidelityfx_upscaler_vk PROPERTIES
    OUTPUT_NAME amd_fidelityfx_upscaler_vk
    CXX_VISIBILITY_PRESET hidden)

add_library(amd_fidelityfx_loader_vk SHARED src/loader/ffx_loader.cpp)
target_include_directories(amd_fidelityfx_loader_vk PRIVATE ${FSR4_FFX_API_INCLUDE})
target_link_libraries(amd_fidelityfx_loader_vk PRIVATE ${CMAKE_DL_LIBS} Vulkan::Vulkan)
set_target_properties(amd_fidelityfx_loader_vk PROPERTIES
    OUTPUT_NAME amd_fidelityfx_loader_vk
    CXX_VISIBILITY_PRESET hidden)

add_executable(fsr4_provider_test tests/provider/provider_test.cpp)
target_include_directories(fsr4_provider_test PRIVATE
    ${FSR4_FFX_API_INCLUDE}
    ${FSR4_FFX_UPSCALER_INCLUDE})
target_link_libraries(fsr4_provider_test PRIVATE Vulkan::Vulkan ${CMAKE_DL_LIBS})
