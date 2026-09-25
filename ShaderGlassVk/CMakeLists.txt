# ShaderGlassVk -- ShaderGlass on a Vulkan layer.
#
# Meson is the working build (see meson.build and tools/build.sh); this exists so the tree can be
# configured and reviewed with the tool most C++ contributors expect. The two produce the same
# binaries and must be kept in step.
#
# 32-bit builds come from a 32-bit toolchain, not a flag here:
#   cmake -B build/linux32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 \
#         -DCMAKE_SHARED_LINKER_FLAGS=-m32

cmake_minimum_required(VERSION 3.16)
project(shaderglassvk VERSION 0.1.0 LANGUAGES CXX)

# Without this every add_test() below registers nothing, and ctest reports "No tests were found"
# while the test binaries sit built and unrun -- which is how it was until this line.
enable_testing()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "ShaderGlassVk targets Linux; the Windows app is the tree beside this one")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)

# Vulkan headers only -- the layer resolves every entry point through the loader's next-chain
# GetProcAddr and must never link the loader itself.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
  set(SG_VULKAN_INCLUDE ${Vulkan_INCLUDE_DIRS})
else()
  set(SG_VULKAN_INCLUDE "")
endif()

# --- the layer ---------------------------------------------------------------
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(SG_LAYER_NAME VkLayer_ShaderGlass_32)
  set(SG_LAYER_DEFINE SHADERGLASS_LAYER_32)
else()
  set(SG_LAYER_NAME VkLayer_ShaderGlass)
  set(SG_LAYER_DEFINE)
endif()

add_library(${SG_LAYER_NAME} SHARED
  layer/src/layer.cpp
  layer/src/chain.cpp
  layer/src/catalogue.cpp
  layer/src/semantics.cpp
  layer/src/texture.cpp
  layer/src/crash_trace.cpp
  layer/src/pixel_grid.cpp
  layer/src/chain_order.cpp
  layer/src/placement.cpp
  layer/src/gamescope_output.cpp)

target_include_directories(${SG_LAYER_NAME} PRIVATE layer/src common presets ${SG_VULKAN_INCLUDE})
target_link_libraries(${SG_LAYER_NAME} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

if(SG_LAYER_DEFINE)
  target_compile_definitions(${SG_LAYER_NAME} PRIVATE ${SG_LAYER_DEFINE})
endif()

# Preset textures are PNG and JPEG, decoded when a preset is selected. stb_image is header-only, so
# this adds nothing to what the layer links against -- which matters for a library mapped into every
# game. Distributions spell the include path differently; without it the layer still builds and
# every other part of a preset still runs, minus its textures.
include(CheckIncludeFileCXX)
check_include_file_cxx("stb/stb_image.h" SG_STB_IN_SUBDIR)
if(SG_STB_IN_SUBDIR)
  target_compile_definitions(${SG_LAYER_NAME} PRIVATE
    SG_HAVE_STB_IMAGE "SG_STB_IMAGE_HEADER=<stb/stb_image.h>")
else()
  check_include_file_cxx("stb_image.h" SG_STB_BARE)
  if(SG_STB_BARE)
    target_compile_definitions(${SG_LAYER_NAME} PRIVATE
      SG_HAVE_STB_IMAGE "SG_STB_IMAGE_HEADER=<stb_image.h>")
  else()
    message(WARNING
      "stb_image.h not found: presets will run without their textures (LUTs, bezels, overlays)")
  endif()
endif()

# Only the loader's entry points leave this object; see layer/shaderglass.map for why that matters.
target_link_options(${SG_LAYER_NAME} PRIVATE
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/layer/shaderglass.map"
  "-Wl,-Bsymbolic"
  "-static-libstdc++"
  "-static-libgcc")

set_target_properties(${SG_LAYER_NAME} PROPERTIES PREFIX "lib")

# --- the generator -----------------------------------------------------------
# glslang is required; SPIRV-Cross is optional and only changes which reflection backend is used.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  find_library(SG_GLSLANG glslang)
  find_library(SG_GLSLANG_RES glslang-default-resource-limits)

  if(SG_GLSLANG AND SG_GLSLANG_RES)
    set(SG_GEN_SOURCES
      gen/main.cpp
      gen/shadergc.cpp
    gen/spirv_edit.cpp
    layer/src/semantics.cpp
      gen/glsl.cpp
      gen/reflect_builtin.cpp)

    find_library(SG_SPIRV_CROSS_CORE spirv-cross-core)
    find_path(SG_SPIRV_CROSS_INC spirv_cross/spirv_cross.hpp)

    if(SG_SPIRV_CROSS_CORE AND SG_SPIRV_CROSS_INC)
      message(STATUS "SPIRV-Cross found: reflection cross-check available")
      list(APPEND SG_GEN_SOURCES gen/reflect_cross.cpp)
    else()
      message(STATUS "SPIRV-Cross not found: using the built-in SPIR-V reflection")
    endif()

    add_executable(shaderglass-gen ${SG_GEN_SOURCES})
    target_include_directories(shaderglass-gen PRIVATE gen presets)
    target_compile_definitions(shaderglass-gen PRIVATE
      "SG_PRESET_API_H_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/presets/preset_api.h\"")
    target_link_libraries(shaderglass-gen PRIVATE ${SG_GLSLANG} ${SG_GLSLANG_RES})

    if(SG_SPIRV_CROSS_CORE AND SG_SPIRV_CROSS_INC)
      target_compile_definitions(shaderglass-gen PRIVATE SG_HAVE_SPIRV_CROSS)
      target_include_directories(shaderglass-gen PRIVATE ${SG_SPIRV_CROSS_INC})
      target_link_libraries(shaderglass-gen PRIVATE ${SG_SPIRV_CROSS_CORE})
    endif()
  else()
    message(STATUS "glslang not found: skipping shaderglass-gen")
  endif()

  # Reads a generated catalogue the way the layer will, through dlopen.
  add_executable(catalog_test tests/catalog_test.cpp)
  target_link_libraries(catalog_test PRIVATE ${CMAKE_DL_LIBS})

  # The libretro vocabulary, checked without a device.
  add_executable(semantics_test tests/semantics_test.cpp layer/src/semantics.cpp)
  target_include_directories(semantics_test PRIVATE layer/src)
  add_test(NAME semantics COMMAND semantics_test)

  # The SPIR-V decoration edit, on modules built by hand.
  add_executable(spirv_edit_test tests/spirv_edit_test.cpp gen/spirv_edit.cpp)
  add_test(NAME spirv_edit COMMAND spirv_edit_test)

  # The source-raster detector, on frames whose answer is known.
  add_executable(pixel_grid_test tests/pixel_grid_test.cpp layer/src/pixel_grid.cpp)
  target_include_directories(pixel_grid_test PRIVATE layer/src)
  add_test(NAME pixel_grid COMMAND pixel_grid_test)

  # Output placement, checked against numbers worked out by hand.
  add_executable(placement_test tests/placement_test.cpp layer/src/placement.cpp)
  target_include_directories(placement_test PRIVATE layer/src)
  add_test(NAME placement COMMAND placement_test)

  # The Steam launch options the interface generates.
  add_executable(launch_command_test tests/launch_command_test.cpp gui/launch_command.cpp)
  add_test(NAME launch_command COMMAND launch_command_test)

  # Dragging the crop rectangle on a capture.
  add_executable(crop_geometry_test tests/crop_geometry_test.cpp gui/crop_geometry.cpp)
  add_test(NAME crop_geometry COMMAND crop_geometry_test)

  # The gamescope composite-target tracker.
  add_executable(gamescope_output_test tests/gamescope_output_test.cpp layer/src/gamescope_output.cpp)
  target_include_directories(gamescope_output_test PRIVATE layer/src)
  add_test(NAME gamescope_output COMMAND gamescope_output_test)

  # --- the interface -----------------------------------------------------------
  # Optional: a build with no Qt still produces the layer, the generator and the control tool,
  # which is everything a headless or packaging build needs.
  find_package(Qt6 COMPONENTS Core Gui Widgets QUIET)
  if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    add_executable(shaderglass-gui
      gui/main.cpp
      gui/mainwindow.cpp
      gui/preset_tree.cpp
      gui/param_panel.cpp
      gui/capture_view.cpp
      gui/crop_picker.cpp
      gui/crop_geometry.cpp
      gui/shm_binder.cpp
      gui/game_probe.cpp
      gui/launch_command.cpp
      # The catalogue loader is the layer's own, so the interface and the layer can never disagree
      # about what a catalogue contains or which ABI they accept.
      layer/src/catalogue.cpp)
    target_include_directories(shaderglass-gui PRIVATE gui common presets layer/src)
    target_link_libraries(shaderglass-gui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets ${CMAKE_DL_LIBS})
  else()
    message(STATUS "Qt 6 not found: skipping shaderglass-gui")
  endif()
endif()

# --- the control tool --------------------------------------------------------
# 64-bit only: a 32-bit game needs a 32-bit layer loaded into it, but nothing else here is per-arch.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  add_executable(shaderglass-ctl tools/shmctl.cpp gui/launch_command.cpp)
  target_include_directories(shaderglass-ctl PRIVATE common)
  target_link_options(shaderglass-ctl PRIVATE "-static-libstdc++" "-static-libgcc")
endif()
