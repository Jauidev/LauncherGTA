# gta4port: the native renderer (LibertyRecompAndroid's gta4_native, GPL-3) and the parts of the
# ReXGlue SDK (BSD, a renamed Xenia: namespace rex::) it needs, from third_party/rexglue.
#
# rex_core: logging, cvars, threading, Xenos format tables. rex_ui: the Vulkan provider, device and
# presenter. rex_gta4native: the renderer itself plus its SPIR-V shader cache. Our runtime talks to
# it through rex::system::IGraphicsSystem (title commands from the D3D hooks), with shims for the
# few runtime objects it asks for (rexglue_glue/).
#
# fmt, xxHash, VMA, utfcpp and SPIRV-Tools come from Xenia's third_party (same commits ReXGlue
# pins); spdlog, volk, simde, Vulkan/SPIR-V headers, toml++, CLI11, zstd, smol-v, picosha2 and the
# RenderDoc header from third_party/rexglue/thirdparty (fetched at ReXGlue's pinned commits).

set(REX_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rexglue)
set(REX_SRC ${REX_ROOT}/src)
set(REX_TP ${REX_ROOT}/thirdparty)
set(REX_GLUE ${CMAKE_CURRENT_SOURCE_DIR}/rexglue_glue)
if (GTA4_IV)
    set(REX_GLUE ${GTA4_IV_SOURCES}/rexglue_glue)
endif()
set(REX_XENIA_TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xenia-gpu/third_party)

set(REX_INCLUDES
    ${REX_GLUE}/shim
    ${REX_ROOT}/include
    ${REX_SRC}
    ${REX_ROOT}
    ${REX_TP}/cli11/include
    ${REX_TP}/vulkan-headers/include
    ${REX_TP}/volk
    ${REX_TP}/spdlog/include
    ${REX_TP}/simde
    ${REX_TP}/tomlplusplus/include
    ${REX_TP}/spirv-headers/include
    ${REX_TP}/smol-v/source
    ${REX_TP}/renderdoc
    ${REX_XENIA_TP}/SPIRV-Tools/include
    ${REX_XENIA_TP}/fmt/include
    ${REX_XENIA_TP}/xxhash
    ${REX_XENIA_TP}/utfcpp/source
    ${REX_XENIA_TP}/VulkanMemoryAllocator/include
    ${REX_TP}/zstd/lib
    ${REX_XENIA_TP}/disruptorplus/include
    ${REX_XENIA_TP}/stb
    ${REX_XENIA_TP}/imgui
    ${REX_ROOT}/LibertyRecompLib
    ${CMAKE_CURRENT_BINARY_DIR}/rexglue-generated)

set(REX_DEFINES
    SPDLOG_FMT_EXTERNAL
    SPDLOG_COMPILED_LIB
    VK_NO_PROTOTYPES
    REX_HAS_VULKAN=1
    REXGLUE_BUILD_CONFIG="Release"
    GTA4_NATIVE_FONT_ASSET_ROOT="${REX_ROOT}/LibertyRecompLib/font_atlases")
if (WIN32)
    list(APPEND REX_DEFINES REX_PLATFORM_WINDOWS=1 NOMINMAX WIN32_LEAN_AND_MEAN
        VK_USE_PLATFORM_WIN32_KHR)
else()
    list(APPEND REX_DEFINES REX_PLATFORM_LINUX=1 VK_USE_PLATFORM_ANDROID_KHR)
endif()

function(rex_library name)
    add_library(${name} STATIC ${ARGN})
    set_target_properties(${name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(${name} PRIVATE ${REX_INCLUDES})
    target_compile_definitions(${name} PRIVATE ${REX_DEFINES})
    target_compile_options(${name} PRIVATE -Wno-everything -fno-strict-aliasing
        $<$<COMPILE_LANGUAGE:CXX>:-fno-char8_t>)
    if (WIN32)
        target_compile_options(${name} PRIVATE -march=haswell /EHsc /FI${REX_GLUE}/win_fixups.h)
    else()
        # NDK 27's libc++ 18 keeps std::jthread / std::stop_token behind the experimental library.
        target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)
    endif()
endfunction()

# --- third party ----------------------------------------------------------------------------
rex_library(rex_spdlog
    ${REX_TP}/spdlog/src/spdlog.cpp
    ${REX_TP}/spdlog/src/stdout_sinks.cpp
    ${REX_TP}/spdlog/src/color_sinks.cpp
    ${REX_TP}/spdlog/src/file_sinks.cpp
    ${REX_TP}/spdlog/src/async.cpp
    ${REX_TP}/spdlog/src/cfg.cpp)
rex_library(rex_volk ${REX_TP}/volk/volk.c)
rex_library(rex_smolv ${REX_TP}/smol-v/source/smolv.cpp)
file(GLOB REX_ZSTD_SOURCES ${REX_TP}/zstd/lib/common/*.c ${REX_TP}/zstd/lib/compress/*.c
    ${REX_TP}/zstd/lib/decompress/*.c)
rex_library(rex_zstd ${REX_ZSTD_SOURCES})
target_compile_definitions(rex_zstd PRIVATE ZSTD_DISABLE_ASM)
rex_library(rex_xxhash ${REX_XENIA_TP}/xxhash/xxhash.c)

# --- core -----------------------------------------------------------------------------------
set(REX_CORE_SOURCES
    arena.cpp bit_map.cpp bit_stream.cpp byte_stream.cpp clock.cpp cvar.cpp
    diagnostics_policy.cpp filesystem.cpp filesystem_wildcard.cpp gta4_transition.cpp input_trace.cpp
    gpu_flight_recorder.cpp logging.cpp memory.cpp mutex.cpp ring_buffer.cpp sha256.cpp
    string_buffer.cpp threading.cpp timer_queue.cpp utf8.cpp vec128.cpp)
if (WIN32)
    list(APPEND REX_CORE_SOURCES atomic_win.cpp clock_win.cpp dbg_win.cpp dynlib_win.cpp
        filesystem_win.cpp mapped_memory_win.cpp math_msvc.cpp memory_win.cpp
        platform/env_win.cpp system_win.cpp threading_win.cpp)
else()
    list(APPEND REX_CORE_SOURCES atomic_posix.cpp clock_posix.cpp dbg_posix.cpp dynlib_posix.cpp
        filesystem_posix.cpp mapped_memory_posix.cpp math_gcc.cpp
        memory_posix.cpp platform/env_posix.cpp
        system_posix.cpp threading_posix.cpp)
endif()
list(TRANSFORM REX_CORE_SOURCES PREPEND ${REX_SRC}/core/)
list(APPEND REX_CORE_SOURCES
    ${REX_SRC}/graphics/gta4_native/core/options.cpp
    ${REX_SRC}/graphics/gta4_native/modern_shader_options.cpp
    ${REX_SRC}/graphics/xenos.cpp
    ${REX_SRC}/graphics/pipeline/texture/info.cpp
    ${REX_SRC}/graphics/pipeline/texture/info_formats.cpp
    ${REX_SRC}/graphics/pipeline/texture/extent.cpp
    ${REX_SRC}/graphics/pipeline/texture/util.cpp
    ${REX_SRC}/graphics/pipeline/texture/conversion.cpp
    # Native LZX (XMemDecompress) decoder for rexglue_glue/eflc/gta4_lzx_hooks.cpp.
    ${REX_SRC}/system/lzx.cpp)
rex_library(rex_core ${REX_CORE_SOURCES})
# libmspack's LZX decompressor (third_party/libmspack, LGPL-2.1, from libmspack's cabextract tree as
# LibertyRecomp builds it).
add_library(rex_mspack STATIC ${CMAKE_SOURCE_DIR}/third_party/libmspack/mspack/lzxd.c)
target_include_directories(rex_mspack PUBLIC ${CMAKE_SOURCE_DIR}/third_party/libmspack/mspack)
target_compile_options(rex_mspack PRIVATE -w)
target_include_directories(rex_core PRIVATE ${CMAKE_SOURCE_DIR}/third_party/libmspack/mspack)
# gta4deps links XenonRecomp's unpatched lzxd.c (XEX loading) and the linker picked its lzxd_* for
# lzx.cpp too: odd XMem raw blocks then decoded with CAB padding (native LZX failed at ~1 MiB) and
# lzx.cpp wrote uncompressed_block_padding past the end of the smaller struct. Rename this copy.
set(REX_MSPACK_RENAMES lzxd_init=rex_lzxd_init lzxd_free=rex_lzxd_free
    lzxd_decompress=rex_lzxd_decompress lzxd_set_output_length=rex_lzxd_set_output_length
    lzxd_set_reference_data=rex_lzxd_set_reference_data)
target_compile_definitions(rex_mspack PRIVATE ${REX_MSPACK_RENAMES})
target_compile_definitions(rex_core PRIVATE ${REX_MSPACK_RENAMES})

# --- ui (Vulkan) ----------------------------------------------------------------------------
set(REX_UI_SOURCES
    graphics_upload_buffer_pool.cpp graphics_util.cpp image_decode.cpp immediate_drawer.cpp
    presenter.cpp renderdoc_api.cpp window.cpp windowed_app_context.cpp
    vulkan/linked_type_descriptor_set_allocator.cpp vulkan/single_layout_descriptor_set_pool.cpp
    vulkan/spirv_int64_lowering.cpp vulkan/spirv_tools_context.cpp vulkan/ui_samplers.cpp
    vulkan/vulkan_device.cpp
    vulkan/vulkan_immediate_drawer.cpp vulkan/vulkan_instance.cpp vulkan/vulkan_mem_alloc.cpp
    vulkan/vulkan_presenter.cpp vulkan/vulkan_provider.cpp vulkan/vulkan_submission_tracker.cpp
    vulkan/vulkan_upload_buffer_pool.cpp vulkan/vulkan_util.cpp)
if (WIN32)
    list(APPEND REX_UI_SOURCES surface_win.cpp)
else()
    list(APPEND REX_UI_SOURCES surface_android.cpp)
endif()
list(TRANSFORM REX_UI_SOURCES PREPEND ${REX_SRC}/ui/)
rex_library(rex_ui ${REX_UI_SOURCES})

# --- the renderer ---------------------------------------------------------------------------
set(REX_NATIVE ${REX_SRC}/graphics/gta4_native)
rex_library(rex_gta4native
    ${REX_NATIVE}/graphics_system.cpp
    ${REX_NATIVE}/core/font_atlas.cpp
    ${REX_NATIVE}/native_buffer_arena.cpp
    ${REX_NATIVE}/native_descriptor_backend.cpp
    ${REX_NATIVE}/native_frame_context.cpp
    ${REX_NATIVE}/native_gpu_attribution.cpp
    ${REX_NATIVE}/native_memory_samples.cpp
    ${REX_NATIVE}/native_performance_samples.cpp
    ${REX_NATIVE}/native_submission_lifetime.cpp
    ${REX_NATIVE}/native_texture_disk_cache.cpp
    ${REX_NATIVE}/postfx_resource_pool.cpp
    ${REX_NATIVE}/smaa_pipeline.cpp
    ${REX_NATIVE}/split_postfx_pass.cpp
    ${REX_NATIVE}/sun_shafts_pass.cpp
    ${REX_NATIVE}/temporal/dlss_bootstrap.cpp
    ${REX_NATIVE}/temporal/dlss_frame_generation.cpp
    ${REX_NATIVE}/temporal/dlss_upscaler.cpp
    ${REX_NATIVE}/temporal/fsr_vulkan.cpp
    ${REX_NATIVE}/temporal/shader_archive.cpp
    ${REX_NATIVE}/temporal/vulkan_scene.cpp
    ${REX_NATIVE}/temporal/vulkan_frame_generation.cpp
    ${REX_ROOT}/LibertyRecompLib/shader/shader_cache.cpp
    ${REX_GLUE}/shader_override_cache.cpp
    ${REX_GLUE}/memory.cpp)

set(REX_LIBRARIES rex_gta4native rex_ui rex_core rex_mspack rex_spdlog rex_volk rex_smolv rex_zstd
    rex_xxhash xe_fmt)
if (WIN32)
    list(APPEND REX_LIBRARIES dxgi)
endif()

add_executable(rex_link_probe EXCLUDE_FROM_ALL ${REX_GLUE}/link_probe.cpp)
set_target_properties(rex_link_probe PROPERTIES CXX_STANDARD 23)
target_include_directories(rex_link_probe PRIVATE ${REX_INCLUDES})
target_compile_definitions(rex_link_probe PRIVATE ${REX_DEFINES})
target_compile_options(rex_link_probe PRIVATE -Wno-everything -fno-char8_t)
if (WIN32)
    target_compile_options(rex_link_probe PRIVATE -march=haswell /EHsc /FI${REX_GLUE}/win_fixups.h)
endif()
target_link_libraries(rex_link_probe PRIVATE ${REX_LIBRARIES})

# --- LibertyRecomp's GTA IV hooks, ported to EFLC ----------------------------------------------
# rexglue_glue/eflc is generated by tools/port_native_hooks.py; rexglue_glue/hooks_shim puts this
# runtime's PPCContext and recompiled functions under the names the hooks expect.
set(REX_EFLC ${REX_GLUE}/eflc)
rex_library(rex_gta4hooks
    ${REX_EFLC}/gta4_native_hooks.cpp
    ${REX_EFLC}/gta4_aspect_hooks.cpp
    ${REX_EFLC}/gta4_presentation_hooks.cpp
    ${REX_EFLC}/gta4_presentation_options.cpp
    # Resource decompression in native code instead of the recompiled XMemDecompress (the six
    # hooked functions are instruction-identical between TU8 and EFLC).
    ${REX_EFLC}/gta4_lzx_hooks.cpp
    # Diagnostic: who holds the RAGE task pool (it ran out and crashed the task workers).
    ${REX_EFLC}/gta4_task_pool_trace.cpp
    ${REX_GLUE}/graphics.cpp
    ${REX_GLUE}/native_options.cpp
    # Fewer ambient cars and pedestrians (gta4_population_scale).
    ${REX_GLUE}/population.cpp
    # No sun-shadow casters gathered while the shadows are off (gta4_native_shadow_interval 0).
    ${REX_GLUE}/shadows_off.cpp
    # Far clip capped with the fog brought in to hide the cut (gta4_fog_distance).
    ${REX_GLUE}/fog.cpp
    # The sound thread's gain-ramped block mix in native code (gta4_native_audio_mix).
    ${REX_GLUE}/audio_mix.cpp
    # The minimap laid out on its own viewport, its layers kept local (gta4_radar_aspect).
    ${REX_GLUE}/radar.cpp
    # A physics pair whose object table entry is broken is skipped instead of crashing.
    ${REX_GLUE}/physics_guard.cpp
    # Sun shadow map held still between re-centres, its resolution spread further (gta4_shadow_hold_distance).
    ${REX_GLUE}/shadow_hold.cpp
    # GTA4_PC_WINDOW=1: the game in a window with keyboard and pad on the PC (nothing on Android).
    ${REX_GLUE}/pc_window.cpp
    ${REX_GLUE}/native_stubs.cpp)
# Sleep instead of spinning while the title waits for a frame slot (gta4_frame_wait_sleep).
if (ANDROID)
    target_sources(rex_gta4hooks PRIVATE ${REX_GLUE}/frame_wait.cpp)
endif()
if (GTA4_IV)
    target_sources(rex_gta4hooks PRIVATE ${REX_GLUE}/iv_unmapped.cpp)
endif()
target_include_directories(rex_gta4hooks BEFORE PRIVATE ${REX_GLUE}/hooks_shim ${REX_EFLC}
    ${GTA4_PPC_ROOT} ${XENON_THIRDPARTY}/simde ${REX_GLUE})
if (WIN32)
    target_compile_options(rex_gta4hooks PRIVATE /FI${REX_GLUE}/rexglue_glue.h)
else()
    target_compile_options(rex_gta4hooks PRIVATE -include ${REX_GLUE}/rexglue_glue.h)
endif()
set(REX_HOOK_LIBRARIES rex_gta4hooks ${REX_LIBRARIES})
