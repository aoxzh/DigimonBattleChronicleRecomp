# libmain.so for the SDL2 Android activity: the recompiled game (recomp/generated, made
# by the lifter from your disc), the HAL, the gcn-recomp renderer on OpenGL ES 3.2, the
# AX mixer, and the shared launcher/settings/overlay. SDL2 is built from third_party/SDL2.
cmake_minimum_required(VERSION 3.22)
project(digimon_android C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(PORT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${PORT}/third_party/SDL2" SDL2)

set(GX "${PORT}/third_party/gcn-recomp/gx")
set(UGX "${PORT}/platform/desktop/upstream_gx")
set(UAUDIO "${PORT}/platform/desktop/upstream_audio")
set(IMGUI "${PORT}/third_party/imgui")
file(GLOB GENERATED "${PORT}/recomp/generated/b_*.c")
if(NOT GENERATED)
  message(FATAL_ERROR "recomp/generated is empty: run the lifter on your main.dol first (see packaging/build.py)")
endif()

add_library(main SHARED
  ${UGX}/main_gx.c ${UGX}/settings.c ${UGX}/adapter.cpp ${UGX}/overlay.cpp
  ${UGX}/host_audio.c ${UGX}/ui_style.cpp
  ${GX}/cmd.cpp ${GX}/xf.cpp ${GX}/texture.cpp ${GX}/shadergen.cpp ${GX}/render_gl.cpp
  ${GX}/gl_load.cpp ${GX}/png.cpp ${GX}/glad_es/src/gles2.c
  ${UAUDIO}/adapter.cpp ${PORT}/third_party/gcn-recomp/audio/ax.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_sdl2.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp
  ${PORT}/hal/mem.c ${PORT}/hal/mmio.c ${PORT}/hal/irq.c ${PORT}/hal/di.c ${PORT}/hal/vi.c
  ${PORT}/hal/gx.c ${PORT}/hal/si.c ${PORT}/hal/exi.c ${PORT}/hal/dsp.c ${PORT}/hal/ar.c
  ${PORT}/hal/ai.c ${PORT}/hal/hal_boot.c
  ${PORT}/third_party/gcn-recomp/cp_fifo.c ${PORT}/recomp/runtime_glue.c
  ${PORT}/recomp/blocks.c ${PORT}/recomp/recomp_entry.c ${GENERATED})
target_compile_definitions(main PRIVATE
  GCN_GL_ES=1 IMGUI_IMPL_OPENGL_ES3=1 DIGIMON_GX_ADAPTER=1 DIGIMON_GX_ASYNC=1
  GC_UPSTREAM_GX=1 GC_UPSTREAM_AUDIO=1)
target_include_directories(main PRIVATE ${UGX} ${GX} ${GX}/glad_es/include ${UAUDIO}
  ${PORT}/third_party/gcn-recomp/audio ${IMGUI})
# Generated blocks are huge straight-line functions; -O2 everywhere (the debug variant too).
# -O3 bloated the hot code enough to make the Snapdragon 8 Gen 2 test phone feel worse, so stay on -O2.
target_compile_options(main PRIVATE -O2 -Wno-unused-parameter)
# The guest dispatch/HAL functions belong to this library and are never interposed.
# Bind internal calls directly: Android battle profiles spent ~5% in PLT stubs.
# Exported entry points (including SDL_main) remain available to SDL's loader.
target_link_options(main PRIVATE -Wl,-Bsymbolic-functions)
target_link_libraries(main PRIVATE SDL2::SDL2 GLESv3 EGL log android)
