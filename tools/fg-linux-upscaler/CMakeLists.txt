cmake_minimum_required(VERSION 3.20)
project(upscaler_provider CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The FidelityFX SDK tree prepared by tools/build_upscaler_provider.py. The tree is AMD's MIT-licensed SDK 2.4.0
# source with the Linux compatibility edits applied; nothing here contains AMD binaries.
set(SDK "$ENV{FFX_SDK}" CACHE PATH "Prepared FidelityFX SDK tree")
if(NOT EXISTS "${SDK}/Kits/FidelityFX/api/include/ffx_api.h")
  message(FATAL_ERROR "FFX_SDK must point at the prepared FidelityFX SDK tree (got '${SDK}')")
endif()

find_package(Vulkan REQUIRED)
find_package(ZLIB REQUIRED)

set(UPSCALER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB UPSCALER_BLOB_SOURCES ${UPSCALER_DIR}/upscaler_blob_*.cpp)
set(SRC
  ${UPSCALER_DIR}/upscaler_blobs.cpp
  ${UPSCALER_BLOB_SOURCES}
  ${UPSCALER_DIR}/upscaler_provider_list.cpp
  ${SDK}/Kits/FidelityFX/api/internal/ffx_api.cpp
  ${SDK}/Kits/FidelityFX/api/internal/ffx_assert.cpp
  ${SDK}/Kits/FidelityFX/api/internal/ffx_message.cpp
  ${SDK}/Kits/FidelityFX/api/internal/ffx_object_management.cpp
  ${SDK}/Kits/FidelityFX/api/internal/ffx_query_fallback.cpp
  ${SDK}/Kits/FidelityFX/backend/vk/ffx_backends_vk.cpp
  ${SDK}/Kits/FidelityFX/backend/vk/ffx_vk.cpp
  ${SDK}/Kits/FidelityFX/upscalers/fsr3/internal/ffx_fsr3upscaler.cpp
  ${SDK}/Kits/FidelityFX/upscalers/fsr3/internal/ffx_provider_fsr3upscale.cpp
)

add_library(amd_fidelityfx_upscaler_vk SHARED ${SRC})

target_include_directories(amd_fidelityfx_upscaler_vk PRIVATE
  ${UPSCALER_DIR}/win32compat
  ${UPSCALER_DIR}/miniz
  ${UPSCALER_DIR}
  ${SDK}/Kits/FidelityFX/api/include
  ${SDK}/Kits/FidelityFX/api/internal
  ${SDK}/Kits/FidelityFX/backend/vk
  ${SDK}/Kits/FidelityFX/upscalers/include
  ${SDK}/Kits/FidelityFX/upscalers/fsr3/include
  ${SDK}/Kits/FidelityFX/upscalers/fsr3/include/gpu
  ${SDK}/Kits/FidelityFX/upscalers/fsr3/internal
)

target_compile_definitions(amd_fidelityfx_upscaler_vk PRIVATE FFX_VK=1 FFX_BACKEND_VK=1 FFX_UPSCALER=1)
target_compile_options(amd_fidelityfx_upscaler_vk PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-include${UPSCALER_DIR}/upscaler_compat.h>)

target_link_libraries(amd_fidelityfx_upscaler_vk PRIVATE Vulkan::Vulkan ZLIB::ZLIB ${CMAKE_DL_LIBS})

set_target_properties(amd_fidelityfx_upscaler_vk PROPERTIES
  OUTPUT_NAME amd_fidelityfx_upscaler_vk
  PREFIX "lib"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
