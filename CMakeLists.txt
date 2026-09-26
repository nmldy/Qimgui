# AImGui: minimal Dear ImGui Android ARM64 ELF.
#
# Build via build.sh (which configures the NDK toolchain), or manually:
#
#   cmake -S . -B build/arm64-v8a \
#       -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
#       -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
#       -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/arm64-v8a -j
#
# ── Configuration guide ─────────────────────────────────────────────────
#   Release        (default) – size-optimised, stripped, no unwind tables.
#                    Same profile as the legacy Application.mk build.
#   RelWithDebInfo          – -O2 -g, keeps .symtab/.debug_*/unwind so a
#                    tombstone can be decoded with
#                      llvm-addr2line -e AImGui <pc>...
#                      ndk-stack -sym build/arm64-v8a -dump tombstone_*.txt
#                    Build with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` when
#                    you need to investigate a native crash.
#   Debug                   – -O0 -g3, no size hacks at all.
# ────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.18)
project(AImGui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default to Release when the caller didn't pick one, so a bare
# `cmake -S . -B build` keeps the old behaviour.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(JNI ${CMAKE_SOURCE_DIR}/jni)

# ── SURFACE_LOG_ENABLE ────────────────────────────────────────────────────
# Previously hard-coded to 0 (fully silent). Keep that as the default so
# shipped binaries match the old behaviour byte-for-byte in this respect,
# but allow -DAIMGUI_SURFACE_LOG=ON to flip it back on for investigations
# (init-order crashes, SurfaceComposerClient symbol resolution, ...).
option(AIMGUI_SURFACE_LOG "Enable SURFACE_LOG_* macros in ANativeWindowCreator" OFF)
if(AIMGUI_SURFACE_LOG)
    add_compile_definitions(SURFACE_LOG_ENABLE=1)
else()
    add_compile_definitions(SURFACE_LOG_ENABLE=0)
endif()

# ── Common compile options (all configurations) ───────────────────────────
# Section splitting helps --gc-sections; hidden visibility shrinks dynamic
# symbol tables and lets the linker ICF aggressively.
# Note: -flto is intentionally omitted. The old Application.mk listed it
# under APP_LDFLAGS only, so no objects ever carried LTO IR and link-time
# LTO was a no-op. Turning real LTO on broke
# ANativeWindowCreator::ProcessMirrorDisplay() (the heavy-static-state
# mirror path crashed when a screen-recording virtual display appeared).
add_compile_options(
    -ffunction-sections
    -fdata-sections
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-rtti)

# ── Per-configuration compile options ─────────────────────────────────────
# The old CMakeLists applied -Os, -fno-unwind-tables and -Wl,-s
# unconditionally. That produced the smallest possible ELF (matching the
# legacy Application.mk build) — but it also stripped .symtab/.debug_* and
# dropped DWARF unwind info, which is why a recent tombstone only had raw
# PCs for the AImGui frames instead of function names / source lines.
#
#   Release        : same size profile as before (stripped, no unwind).
#   RelWithDebInfo : -O2 -g, keep symbols + unwind for crash analysis.
#   Debug          : -O0 -g3.
add_compile_options(
    $<$<CONFIG:Release>:-Os>
    $<$<CONFIG:Release>:-fno-unwind-tables>
    $<$<CONFIG:Release>:-fno-asynchronous-unwind-tables>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-g>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>)

# --gc-sections and --icf=all are size wins in every configuration.
# -s (--strip-all) is only applied to the shipped Release binary; the
# RelWithDebInfo / Debug builds must keep .symtab so a later tombstone
# can be resolved with llvm-addr2line.
add_link_options(-Wl,--gc-sections -Wl,--icf=all)
add_link_options($<$<CONFIG:Release>:-Wl,-s>)

# ── Prebuilt Dear ImGui ──────────────────────────────────────────────────
# Shipped under prebuilt/$ABI/libimgui.a; only public headers live in-tree.
add_library(imgui STATIC IMPORTED GLOBAL)
set_target_properties(imgui PROPERTIES
    IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/prebuilt/${ANDROID_ABI}/libimgui.a)
target_include_directories(imgui INTERFACE
    ${JNI}/include/imgui
    ${JNI}/include/imgui/backends)

# ── aimgui_platform: ANativeWindow + Touch helper ────────────────────────
add_library(aimgui_platform STATIC
    ${JNI}/src/platform/TouchHelperA.cpp)
target_include_directories(aimgui_platform PUBLIC ${JNI}/src)
target_link_libraries(aimgui_platform PUBLIC imgui)

# ── aimgui_core: renderers, bloom, font, keyboard, window-session ────────
add_library(aimgui_core STATIC
    ${JNI}/src/core/renderer_factory.cpp
    ${JNI}/src/core/renderer_gl.cpp
    ${JNI}/src/core/renderer_vk.cpp
    ${JNI}/src/core/vulkan_wrapper.cpp
    ${JNI}/src/core/bloom_gl.cpp
    ${JNI}/src/core/bloom_vk.cpp
    ${JNI}/src/core/font.cpp
    ${JNI}/src/core/keyboard_input.cpp
    ${JNI}/src/core/window_session.cpp)
target_compile_definitions(aimgui_core PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_include_directories(aimgui_core
    PUBLIC  ${JNI}/src
    PRIVATE ${JNI}/include)
target_link_libraries(aimgui_core PUBLIC imgui aimgui_platform)

# ── Executable ───────────────────────────────────────────────────────────
add_executable(AImGui
    ${JNI}/src/main.cpp
    ${JNI}/src/ui/ui.cpp
    ${JNI}/src/ui/main_ui.cpp)
target_include_directories(AImGui PRIVATE ${JNI}/include ${JNI}/src)
target_link_libraries(AImGui PRIVATE
    aimgui_core aimgui_platform imgui
    log android EGL GLESv3)
