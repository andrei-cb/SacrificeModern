include(FetchContent)
enable_language(C)
FetchContent_Declare(dr_libs
  URL https://codeload.github.com/mackron/dr_libs/tar.gz/35b5c392cc15456736c440a68dbe073178bbffe0
  URL_HASH SHA256=d46fa0a831a65b16fcf2b169e6b739c66536437ee78049893788582eb93e39de
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR header-only)
FetchContent_Declare(libsmacker
  URL https://codeload.github.com/greg-kennedy/libsmacker/tar.gz/76094fb9c8e98bd5fac982c504e8d9aeff3ece01
  URL_HASH SHA256=f28ec225f73bdf9804278bc23e7e27a1329042e0b40df405368cb29cce012306
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR header-only)
FetchContent_MakeAvailable(dr_libs libsmacker)
add_library(sacrifice_smacker STATIC ${libsmacker_SOURCE_DIR}/smacker.c)
target_include_directories(sacrifice_smacker PUBLIC ${libsmacker_SOURCE_DIR})
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(json)

add_library(sacrifice_web_zlib INTERFACE)
target_compile_options(sacrifice_web_zlib INTERFACE -sUSE_ZLIB=1)
target_link_options(sacrifice_web_zlib INTERFACE -sUSE_ZLIB=1)
add_library(ZLIB::ZLIB ALIAS sacrifice_web_zlib)

add_compile_options(-fwasm-exceptions -Wall -Wextra -Wno-unused-parameter)
add_link_options(-fwasm-exceptions -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB
  -sINITIAL_MEMORY=64MB -sSTACK_SIZE=8MB -sGLOBAL_BASE=16777216
  -sCASE_INSENSITIVE_FS=1 -sFORCE_FILESYSTEM=1)
set(CMAKE_CROSSCOMPILING_EMULATOR "${EMSDK_NODE};${CMAKE_CURRENT_SOURCE_DIR}/web/run_node.cjs"
  CACHE STRING "Node for wasm checks" FORCE)

add_library(sacrifice_web_gpu INTERFACE)
target_compile_options(sacrifice_web_gpu INTERFACE -sUSE_SDL=2 --use-port=emdawnwebgpu)
target_link_options(sacrifice_web_gpu INTERFACE -sUSE_SDL=2 --use-port=emdawnwebgpu)
FetchContent_Declare(imgui_web_source
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.tar.gz
  URL_HASH SHA256=fecb33d33930e12ff53a34064e9d3a06c8f7c3e04408f14cd36c80e3faac863b
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui_web_source)
# Keep the pinned upstream backend, adding the additive blend used by the game's HUD.
file(READ "${imgui_web_source_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp" WGPU_BACKEND)
foreach(anchor "WGPURenderPipeline      pipelineState = nullptr;" "    bd->pipelineState = wgpuDeviceCreateRenderPipeline(bd->wgpuDevice, &graphics_pipeline_desc);" "    SafeRelease(bd->pipelineState);")
  string(FIND "${WGPU_BACKEND}" "${anchor}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "ImGui WebGPU HUD patch needs updating: ${anchor}")
  endif()
endforeach()
string(REPLACE "WGPURenderPipeline      pipelineState = nullptr;"
  "WGPURenderPipeline      pipelineState = nullptr;\n    WGPURenderPipeline additivePipeline = nullptr;" WGPU_BACKEND "${WGPU_BACKEND}")
string(REPLACE "    bd->pipelineState = wgpuDeviceCreateRenderPipeline(bd->wgpuDevice, &graphics_pipeline_desc);"
  "    bd->pipelineState = wgpuDeviceCreateRenderPipeline(bd->wgpuDevice, &graphics_pipeline_desc);\n    blend_state.color.dstFactor = WGPUBlendFactor_One;\n    blend_state.alpha.dstFactor = WGPUBlendFactor_One;\n    bd->additivePipeline = wgpuDeviceCreateRenderPipeline(bd->wgpuDevice, &graphics_pipeline_desc);" WGPU_BACKEND "${WGPU_BACKEND}")
string(REPLACE "    SafeRelease(bd->pipelineState);"
  "    SafeRelease(bd->pipelineState);\n    SafeRelease(bd->additivePipeline);" WGPU_BACKEND "${WGPU_BACKEND}")
string(APPEND WGPU_BACKEND "\nvoid ImGui_ImplWGPU_SetAdditive() {\n    auto* bd = ImGui_ImplWGPU_GetBackendData();\n    if (bd && bd->RenderState) wgpuRenderPassEncoderSetPipeline(bd->RenderState->RenderPassEncoder, bd->additivePipeline);\n}\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/imgui_impl_wgpu_hud.cpp" "${WGPU_BACKEND}")
add_library(imgui_web STATIC
  ${imgui_web_source_SOURCE_DIR}/imgui.cpp
  ${imgui_web_source_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_web_source_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_web_source_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_web_source_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/imgui_impl_wgpu_hud.cpp)
target_include_directories(imgui_web PUBLIC ${imgui_web_source_SOURCE_DIR} ${imgui_web_source_SOURCE_DIR}/backends)
target_link_libraries(imgui_web PUBLIC sacrifice_web_gpu)

FetchContent_Declare(directxmath_web
  URL https://github.com/microsoft/DirectXMath/archive/refs/tags/oct2024.tar.gz
  URL_HASH SHA256=69af50e165e8458b0422068cff24dbdaf7d649c3174f140acbb71c39f3c93716
  SOURCE_SUBDIR header-only
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(directxmath_web)
add_library(sacrifice_web_math INTERFACE)
target_include_directories(sacrifice_web_math SYSTEM INTERFACE ${directxmath_web_SOURCE_DIR}/Inc)
target_include_directories(sacrifice_web_math INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/engine/web_compat)
target_compile_definitions(sacrifice_web_math INTERFACE _XM_NO_INTRINSICS_)
