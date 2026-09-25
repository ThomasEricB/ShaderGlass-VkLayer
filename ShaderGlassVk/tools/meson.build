executable('shaderglass-ctl',
  ['shmctl.cpp', '../gui/launch_command.cpp'],
  include_directories: include_directories('../common'),
  link_args: ['-static-libstdc++', '-static-libgcc'],
  install: false,
)

# Reads a generated catalogue the way the layer will, through dlopen.
executable('catalog_test',
  '../tests/catalog_test.cpp',
  dependencies: [cpp.find_library('dl', required: false)],
  install: false,
)

# The libretro vocabulary, checked without a device: see tests/semantics_test.cpp.
semantics_test = executable('semantics_test',
  ['../tests/semantics_test.cpp', '../layer/src/semantics.cpp'],
  include_directories: include_directories('../layer/src'),
  install: false,
)
test('semantics', semantics_test)

# Builds and records every preset in a catalogue, with no game and no window. Run it under the
# validation layer; see tests/chain_test.cpp.
executable('chain_test',
  ['../tests/chain_test.cpp', '../layer/src/chain.cpp', '../layer/src/catalogue.cpp',
   '../layer/src/semantics.cpp', '../layer/src/texture.cpp',
   '../layer/src/pixel_grid.cpp', '../layer/src/placement.cpp'],
  include_directories: include_directories('../layer/src', '../common', '../presets'),
  cpp_args: layer_args,
  dependencies: [vulkan_dep, cpp.find_library('dl', required: false)],
  install: false,
)

# The SPIR-V decoration edit, on modules built by hand: see tests/spirv_edit_test.cpp.
spirv_edit_test = executable('spirv_edit_test',
  ['../tests/spirv_edit_test.cpp', '../gen/spirv_edit.cpp'],
  install: false,
)
test('spirv_edit', spirv_edit_test)

# The source-raster detector, on frames whose answer is known: see tests/pixel_grid_test.cpp.
pixel_grid_test = executable('pixel_grid_test',
  ['../tests/pixel_grid_test.cpp', '../layer/src/pixel_grid.cpp'],
  include_directories: include_directories('..', '../layer/src'),
  install: false,
)
test('pixel_grid', pixel_grid_test)

# Output placement, checked against numbers worked out by hand: see tests/placement_test.cpp.
placement_test = executable('placement_test',
  ['../tests/placement_test.cpp', '../layer/src/placement.cpp'],
  include_directories: include_directories('..', '../layer/src'),
  install: false,
)
test('placement', placement_test)

# The Steam launch options the interface generates: see tests/launch_command_test.cpp.
launch_command_test = executable('launch_command_test',
  ['../tests/launch_command_test.cpp', '../gui/launch_command.cpp'],
  include_directories: include_directories('..'),
  install: false,
)
test('launch_command', launch_command_test)

# Dragging the crop rectangle: see tests/crop_geometry_test.cpp.
crop_geometry_test = executable('crop_geometry_test',
  ['../tests/crop_geometry_test.cpp', '../gui/crop_geometry.cpp'],
  include_directories: include_directories('..'),
  install: false,
)
test('crop_geometry', crop_geometry_test)

# The gamescope composite-target tracker: see tests/gamescope_output_test.cpp.
gamescope_output_test = executable('gamescope_output_test',
  ['../tests/gamescope_output_test.cpp', '../layer/src/gamescope_output.cpp'],
  include_directories: include_directories('..', '../layer/src'),
  install: false,
)
test('gamescope_output', gamescope_output_test)
