cmake_minimum_required(VERSION 3.20)
project(framegen_provider CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The FidelityFX SDK tree prepared by tools/build_fg_provider.py. The tree is AMD's MIT-licensed SDK 2.4.0 source with
# the Linux compatibility edits applied; nothing here contains AMD binaries.
set(SDK "$ENV{FG_SDK}" CACHE PATH "Prepared FidelityFX SDK tree")
if(NOT EXISTS "${SDK}/Kits/FidelityFX/api/include/ffx_api.h")
  message(FATAL_ERROR "FG_SDK must point at the prepared FidelityFX SDK tree (got '${SDK}')")
endif()

find_package(Vulkan REQUIRED)
find_package(ZLIB REQUIRED)

set(FG_DIR ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB FG_BLOB_SOURCES ${FG_DIR}/fg_blob_*.cpp)
set(SRC
  ${FG_DIR}/fg_blobs.cpp
  ${FG_BLOB_SOURCES}
  ${FG_DIR}/fg_provider_list.cpp
  ${SDK}/Kits/FidelityFX/api/internal/ffx_assert.cpp
  ${SDK}/Kits/FidelityFX/api/internal/ffx_message.cpp
  ${SDK}/Kits/FidelityFX/api/internal/ffx_object_management.cpp
  ${SDK}/Kits/FidelityFX/api/internal/ffx_query_fallback.cpp
  ${SDK}/Kits/FidelityFX/backend/vk/ffx_backends_vk.cpp
  ${SDK}/Kits/FidelityFX/backend/vk/ffx_vk.cpp
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/internal/ffx_frameinterpolation.cpp
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/internal/ffx_opticalflow.cpp
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/vk/ffx_provider_framegenerationswapchain_vk.cpp
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/vk/FrameInterpolationSwapchainVK.cpp
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/vk/FrameInterpolationSwapchainVK_Helpers.cpp
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/vk/FrameInterpolationSwapchainVK_UiComposition.cpp
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/vk/FrameInterpolationSwapchainVK_DebugPacing.cpp
)

add_library(amd_fidelityfx_framegeneration_vk SHARED ${SRC})

target_include_directories(amd_fidelityfx_framegeneration_vk PRIVATE
  ${FG_DIR}/win32compat
  ${FG_DIR}/miniz
  ${FG_DIR}
  ${SDK}/Kits/FidelityFX/api/include
  ${SDK}/Kits/FidelityFX/api/internal
  ${SDK}/Kits/FidelityFX/backend/vk
  ${SDK}/Kits/FidelityFX/framegeneration/include
  ${SDK}/Kits/FidelityFX/framegeneration/include/vk
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/include
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/include/gpu
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/internal
  ${SDK}/Kits/FidelityFX/framegeneration/fsr3/vk
)

target_compile_definitions(amd_fidelityfx_framegeneration_vk PRIVATE FFX_VK=1 FFX_BACKEND_VK=1)
target_compile_options(amd_fidelityfx_framegeneration_vk PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-include${FG_DIR}/fg_compat.h>)

target_link_libraries(amd_fidelityfx_framegeneration_vk PRIVATE Vulkan::Vulkan ZLIB::ZLIB ${CMAKE_DL_LIBS})

set_target_properties(amd_fidelityfx_framegeneration_vk PROPERTIES
  OUTPUT_NAME amd_fidelityfx_framegeneration_vk
  PREFIX "lib"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
