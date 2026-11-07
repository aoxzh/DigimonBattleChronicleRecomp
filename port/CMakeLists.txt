cmake_minimum_required(VERSION 3.20)
project(digimon_gc_desktop C CXX)
option(BUILD_UPSTREAM_GX "Build the graphics-only gcn-recomp desktop adapter" ON)
option(BUILD_MAIN_GX "Link the existing generated guest and HAL into main_gx" ON)
option(BUILD_UPSTREAM_AUDIO "Enable the graphics runner's optional gcn-recomp AX audio adapter" OFF)
if(NOT BUILD_UPSTREAM_GX)
  return()
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
if(WIN32)
  # SDL2's Windows pkg-config adds -mwindows/SDL2main; the runners keep a plain
  # console main(). Static SDL + runtime gives a self-contained .exe.
  find_package(SDL2 CONFIG REQUIRED)
  set(SDL2_TARGET SDL2::SDL2-static)
  add_link_options(-static)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  set(SDL2_TARGET PkgConfig::SDL2)
endif()
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
set(GX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gcn-recomp/gx)
set(ADAPTER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform/desktop/upstream_gx)
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})
target_link_libraries(imgui PUBLIC ${SDL2_TARGET} OpenGL::GL ${CMAKE_DL_LIBS})
# The in-game overlay (touch controls, pause/settings menu) lives with the adapter.
# Android builds the same sources from android/app/src/main/cpp/CMakeLists.txt.
add_library(upstream_gx STATIC
  ${GX_DIR}/cmd.cpp ${GX_DIR}/xf.cpp ${GX_DIR}/texture.cpp
  ${GX_DIR}/shadergen.cpp ${GX_DIR}/render_gl.cpp ${GX_DIR}/gl_load.cpp
  ${GX_DIR}/png.cpp ${ADAPTER_DIR}/adapter.cpp ${ADAPTER_DIR}/overlay.cpp ${ADAPTER_DIR}/ui_style.cpp)
if(NOT APPLE)
  target_sources(upstream_gx PRIVATE ${GX_DIR}/glad/src/gl.c)
  target_include_directories(upstream_gx PRIVATE ${GX_DIR}/glad/include)
endif()
target_compile_definitions(upstream_gx PRIVATE DIGIMON_GX_ADAPTER=1 DIGIMON_GX_ASYNC=1)
target_include_directories(upstream_gx PUBLIC ${ADAPTER_DIR} PRIVATE ${GX_DIR})
target_link_libraries(upstream_gx PUBLIC imgui ${SDL2_TARGET} OpenGL::GL Threads::Threads ${CMAKE_DL_LIBS})
add_executable(gx_fixture ${ADAPTER_DIR}/fixture.cpp)
target_link_libraries(gx_fixture PRIVATE upstream_gx)
enable_testing()
add_test(NAME gx_fifo_pixels COMMAND gx_fixture ${CMAKE_CURRENT_BINARY_DIR}/gx_fixture.png)
set_tests_properties(gx_fifo_pixels PROPERTIES TIMEOUT 30)
if(BUILD_MAIN_GX)
  file(GLOB GENERATED_BLOCKS CONFIGURE_DEPENDS recomp/generated/b_*.c)
  add_executable(main_gx ${ADAPTER_DIR}/main_gx.c ${ADAPTER_DIR}/settings.c ${ADAPTER_DIR}/launcher.cpp
    hal/mem.c hal/mmio.c hal/irq.c hal/di.c hal/vi.c hal/gx.c hal/si.c
    hal/exi.c hal/dsp.c hal/ar.c hal/ai.c hal/hal_boot.c
    third_party/gcn-recomp/cp_fifo.c recomp/runtime_glue.c
    recomp/blocks.c recomp/recomp_entry.c ${GENERATED_BLOCKS})
  target_compile_definitions(main_gx PRIVATE GC_UPSTREAM_GX=1)
  target_link_libraries(main_gx PRIVATE upstream_gx imgui)
  # Launcher background: the JPEG is embedded as a byte array, decoded by stb_image.
  set(LAUNCHER_BG ${ADAPTER_DIR}/launcher_bg.jpg)
  set(LAUNCHER_BG_C ${CMAKE_CURRENT_BINARY_DIR}/launcher_bg.c)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LAUNCHER_BG})
  file(READ ${LAUNCHER_BG} LAUNCHER_BG_HEX HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," LAUNCHER_BG_BYTES "${LAUNCHER_BG_HEX}")
  file(WRITE ${LAUNCHER_BG_C} "const unsigned char launcher_bg_jpg[] = {${LAUNCHER_BG_BYTES}};\nconst unsigned long launcher_bg_jpg_size = sizeof(launcher_bg_jpg);\n")
  target_sources(main_gx PRIVATE ${LAUNCHER_BG_C})
  target_include_directories(main_gx PRIVATE third_party/stb)
  if(WIN32)
    # GUI subsystem (no console window; logs go to %APPDATA%\DigimonBattleGC\logs
    # unless stderr is redirected or --console is given). Windows defaults to a
    # 1 MB main stack; match the 8 MB the port runs with elsewhere.
    enable_language(RC)
    target_sources(main_gx PRIVATE platform/desktop/windows/main_gx.rc)
    set_target_properties(main_gx PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(main_gx PRIVATE -Wl,--stack,8388608)
    target_link_libraries(main_gx PRIVATE comdlg32)
  endif()
  if(BUILD_UPSTREAM_AUDIO)
    set(AUDIO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform/desktop/upstream_audio)
    if(NOT EXISTS ${AUDIO_DIR}/adapter.cpp)
      message(FATAL_ERROR "BUILD_UPSTREAM_AUDIO requires the completed audio adapter.cpp")
    endif()
    add_library(upstream_audio STATIC ${AUDIO_DIR}/adapter.cpp third_party/gcn-recomp/audio/ax.cpp)
    target_include_directories(upstream_audio PRIVATE ${AUDIO_DIR} third_party/gcn-recomp/audio)
    target_sources(main_gx PRIVATE ${ADAPTER_DIR}/host_audio.c)
    set_source_files_properties(hal/dsp.c hal/ai.c ${ADAPTER_DIR}/main_gx.c ${ADAPTER_DIR}/host_audio.c
      PROPERTIES COMPILE_DEFINITIONS GC_UPSTREAM_AUDIO=1)
    target_link_libraries(main_gx PRIVATE upstream_audio)
  endif()
endif()
