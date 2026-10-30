cmake_minimum_required(VERSION 3.20)
project(gta4port C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(XENON_RECOMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/XenonRecomp)
set(XENON_UTILS_ROOT ${XENON_RECOMP_ROOT}/XenonUtils)
set(XENON_THIRDPARTY ${XENON_RECOMP_ROOT}/thirdparty)
set(GTA4_PPC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/ppc)
set(GTA4_RUNTIME_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
# gta4port: GTA IV (config/gta4iv.toml) instead of Episodes from Liberty City. Its recompiled code is
# ppc_iv/ and its runtime and hooks are the EFLC sources with the addresses translated into the build
# tree (tools/gen_iv_sources.py, which the build scripts run before every build): fixes are made once.
option(GTA4_IV "GTA IV instead of Episodes from Liberty City" OFF)
if (GTA4_IV)
    add_compile_definitions(GTA4_IV=1)
    set(GTA4_PPC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/ppc_iv)
    set(GTA4_IV_SOURCES ${CMAKE_BINARY_DIR}/iv_src)
    set(GTA4_RUNTIME_ROOT ${GTA4_IV_SOURCES}/runtime)
    execute_process(COMMAND py -3 -I tools/gen_iv_sources.py ${GTA4_IV_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND_ERROR_IS_FATAL ANY)
endif()

if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl on the PC (debug host). The MSVC STL in Build Tools 17.14 wants clang 19; the NDK has 18.
    add_compile_definitions(_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH _CRT_SECURE_NO_WARNINGS NOMINMAX)
    add_compile_options(/EHsc -march=x86-64-v2)
    set(GTA4_FP_STRICT /fp:strict)
else()
    set(GTA4_FP_STRICT -ffp-model=strict)
endif()
# gta4port: since 0.7.0D one APK for every phone, gta4port.apk (android/AndroidManifest.xml,
# build/arm64): its launcher (runtime/os/launcher.h) tells Mali from Adreno, and on Adreno
# libadrenotools can load Mesa's Turnip or an imported driver (runtime/os/vk_driver.cpp).
# GTA4_SNAPDRAGON only changes the CPU tuning (the old gta4snap.apk line, build/arm64-snap,
# tools/build-android.sh snap, tools/build-apk.sh snap), for comparisons on the Snapdragon 680.
option(GTA4_SNAPDRAGON "Android: CPU tuning for the Snapdragon 680 (Cortex-A73, inline atomics)" OFF)
if (ANDROID AND GTA4_SNAPDRAGON)
    add_compile_definitions(GTA4_SNAPDRAGON=1)
    # Snapdragon 680 (Redmi Note 11): Cortex-A73 + A53, ARMv8.0 without LSE, so atomics are inline
    # LL/SC loops instead of outline helpers that test for LSE on every call.
    add_compile_options(-march=armv8-a -mno-outline-atomics -mtune=cortex-a73)
elseif (ANDROID)
    # gta4port: the NDK's ARMv8.0 baseline, so the build runs on both test phones. ARMv8.2 (LSE,
    # fp16, dotprod, 2026-10-26 to 0.6.1D) died with SIGILL on an ldadd while loading on the Redmi
    # Note 11 (Snapdragon 680: Cortex-A73 + A53, ARMv8.0). Outline atomics keep LSE on the
    # Dimensity 8300 (Cortex-A715 + A510): each atomic read-modify-write calls a __aarch64_ldadd*/
    # cas* helper that tests for LSE at run time. Tuned for the A715. Floating-point semantics are
    # the same either way.
    add_compile_options(-march=armv8-a -moutline-atomics -mtune=cortex-a715)
endif()

# Recompiled game code (XenonRecomp output). Regenerate with tools/recompile.bat.
file(GLOB GTA4_PPC_SOURCES CONFIGURE_DEPENDS ${GTA4_PPC_ROOT}/ppc_recomp.*.cpp)

add_library(gta4ppc STATIC ${GTA4_PPC_SOURCES} ${GTA4_PPC_ROOT}/ppc_func_mapping.cpp)
# Same flags UnleashedRecomp uses for its recompiled library: the generated code type-puns
# through PPCRegister and relies on strict IEEE behaviour.
target_compile_options(gta4ppc PRIVATE -fno-strict-aliasing ${GTA4_FP_STRICT})
target_include_directories(gta4ppc PUBLIC ${GTA4_PPC_ROOT} ${XENON_THIRDPARTY}/simde)
# guest_mmio.h first: it redefines PPC_MM_STORE_U32 to forward the APU register writes.
target_precompile_headers(gta4ppc PRIVATE ${GTA4_RUNTIME_ROOT}/apu/guest_mmio.h ${GTA4_PPC_ROOT}/ppc_recomp_shared.h)

# Link probe: a shared library with --no-undefined lists every symbol the runtime must provide.
# Built on demand (Android/Linux linker only): cmake --build <dir> --target gta4ppc_linkprobe
if (NOT WIN32)
    add_library(gta4ppc_linkprobe SHARED EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/tools/linkprobe.cpp)
    target_link_libraries(gta4ppc_linkprobe PRIVATE -Wl,--whole-archive gta4ppc -Wl,--no-whole-archive)
    target_link_options(gta4ppc_linkprobe PRIVATE -Wl,--no-undefined -Wl,--error-limit=0)
endif()

# Third-party pieces of the runtime: XEX loading (AES + LZX), fmt, xxHash, o1heap.
add_library(gta4deps STATIC
    ${XENON_UTILS_ROOT}/xex.cpp
    ${XENON_UTILS_ROOT}/image.cpp
    ${XENON_UTILS_ROOT}/xex_patcher.cpp
    ${XENON_UTILS_ROOT}/memory_mapped_file.cpp
    ${XENON_THIRDPARTY}/tiny-AES-c/aes.c
    ${XENON_THIRDPARTY}/libmspack/libmspack/mspack/lzxd.c
    ${XENON_THIRDPARTY}/fmt/src/format.cc
    ${XENON_THIRDPARTY}/xxHash/xxhash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/o1heap/o1heap/o1heap.c)
target_include_directories(gta4deps
    PUBLIC
        ${XENON_UTILS_ROOT}
        ${XENON_THIRDPARTY}/simde
        ${XENON_THIRDPARTY}/fmt/include
        ${XENON_THIRDPARTY}/xxHash
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/o1heap/o1heap
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unordered_dense/include
    PRIVATE
        ${XENON_THIRDPARTY}/libmspack/libmspack/mspack
        ${XENON_THIRDPARTY}/tiny-AES-c
        ${XENON_THIRDPARTY}/TinySHA1)

# o1heap counts leading zeros of size_t with __builtin_clzl, which is 32-bit on Windows (LLP64)
# and corrupts its bins; force the 64-bit builtin everywhere.
target_compile_definitions(gta4deps PRIVATE O1HEAP_CLZ=__builtin_clzll)

# The runtime: kernel/XAM HLE, memory, threads, file system. Headless until phase 4.
file(GLOB_RECURSE GTA4_RUNTIME_SOURCES CONFIGURE_DEPENDS ${GTA4_RUNTIME_ROOT}/*.cpp)
add_executable(gta4 ${GTA4_RUNTIME_SOURCES})
target_include_directories(gta4 PRIVATE ${GTA4_RUNTIME_ROOT})
target_link_libraries(gta4 PRIVATE gta4ppc gta4deps)
target_precompile_headers(gta4 PRIVATE ${GTA4_RUNTIME_ROOT}/stdafx.h)
# Xenia's GPU (phase 4): cmake/xenia.cmake. On the PC it runs offscreen to compare with the phone.
include(cmake/xenia.cmake)
# Native renderer (LibertyRecomp's gta4_native on ReXGlue) and its D3D hooks, in progress.
include(cmake/rexglue.cmake)
# The hooks replace weak recompiled functions, so nothing references them: link the whole archive.
if (WIN32)
    target_link_libraries(gta4 PRIVATE ${REX_HOOK_LIBRARIES})
    # xenia_ui_vulkan and rex_ui each compile VulkanMemoryAllocator; the static libraries only clash
    # on the PC's linker (the APK's lld takes the first).
    target_link_options(gta4 PRIVATE /WHOLEARCHIVE:rex_gta4hooks.lib /FORCE:MULTIPLE)
endif()
target_include_directories(gta4 PRIVATE ${XENIA_GLUE} ${REX_GLUE})
if (WIN32)
    target_link_libraries(gta4 PRIVATE ${XENIA_LIBRARIES})
endif()
if (ANDROID)
    target_link_libraries(gta4 PRIVATE log aaudio)
    target_link_libraries(gta4 PRIVATE -Wl,--start-group ${XENIA_LIBRARIES} -Wl,--end-group)
    target_link_libraries(gta4 PRIVATE -Wl,--whole-archive rex_gta4hooks -Wl,--no-whole-archive
        -Wl,--start-group ${REX_LIBRARIES} -Wl,--end-group)

    # The Android app (android/, tools/build-apk.sh) loads the same runtime as libgta4.so and
    # starts it through JNI (runtime/os/android_app.cpp) instead of main().
    add_library(gta4app SHARED ${GTA4_RUNTIME_SOURCES})
    set_target_properties(gta4app PROPERTIES OUTPUT_NAME gta4)
    target_compile_definitions(gta4app PRIVATE GTA4_SHARED)
    target_include_directories(gta4app PRIVATE ${GTA4_RUNTIME_ROOT})
    target_link_libraries(gta4app PRIVATE gta4ppc gta4deps log aaudio)
    # libadrenotools (runtime/os/vk_driver.cpp) loads Mesa's Turnip on Adreno phones. Its hook
    # libraries (libhook_impl, libmain_hook, libfile_redirect_hook, libgsl_alloc_hook) are built
    # with the app and packaged by tools/build-apk.sh. Its CMake checks the ABI in
    # CMAKE_ANDROID_ARCH_ABI, which the NDK's legacy toolchain leaves unset.
    if (NOT CMAKE_ANDROID_ARCH_ABI)
        set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
    endif()
    add_subdirectory(third_party/libadrenotools EXCLUDE_FROM_ALL)
    target_link_libraries(gta4 PRIVATE adrenotools)
    target_link_libraries(gta4app PRIVATE adrenotools)
    add_dependencies(gta4app hook_impl main_hook file_redirect_hook gsl_alloc_hook)
    # The launcher (runtime/os/launcher.cpp, launcher_ui.cpp): Xenia's Dear ImGui (xe_imgui) drawn
    # with its OpenGL ES 3 backend on an EGL context, stb_image for the backgrounds.
    set(GTA4_IMGUI_GL ${XENIA_TP}/imgui/backends/imgui_impl_opengl3.cpp)
    target_sources(gta4app PRIVATE ${GTA4_IMGUI_GL})
    set_source_files_properties(${GTA4_IMGUI_GL} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(gta4app PRIVATE ${XENIA_TP}/imgui ${XENIA_TP}/imgui/backends ${XENIA_TP}/stb)
    target_link_libraries(gta4app PRIVATE EGL GLESv3 android)
    target_include_directories(gta4app PRIVATE ${XENIA_GLUE} ${REX_GLUE})
    target_link_libraries(gta4app PRIVATE -Wl,--start-group ${XENIA_LIBRARIES} -Wl,--end-group)
    target_link_libraries(gta4app PRIVATE -Wl,--whole-archive rex_gta4hooks -Wl,--no-whole-archive
        -Wl,--start-group ${REX_LIBRARIES} -Wl,--end-group)
    target_precompile_headers(gta4app PRIVATE ${GTA4_RUNTIME_ROOT}/stdafx.h)
    target_compile_options(gta4app PRIVATE -g)
    # The recompiled functions are weak (PPC_WEAK_FUNC) and every function of a shared library can
    # be interposed by default, so ~28,700 of them, 22,840 guest functions among them, were called
    # through PLT stubs (GOT load + indirect branch on every guest call). Nothing interposes this
    # library: bind its function references to its own definitions so they become direct calls.
    target_link_options(gta4app PRIVATE -Wl,-Bsymbolic-functions)
endif()
# Debug info for the runtime only (the recompiled library would be enormous): crash offsets
# printed by runtime/os/crash.cpp are symbolized with llvm-symbolizer.
if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(gta4 PRIVATE /Z7)
    target_compile_options(gta4deps PRIVATE /Z7)
    target_link_options(gta4 PRIVATE /DEBUG)
    target_link_libraries(gta4 PRIVATE dbghelp)
else()
    target_compile_options(gta4 PRIVATE -g)
endif()
