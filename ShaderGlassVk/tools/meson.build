executable('shaderglass-ctl',
  'shmctl.cpp',
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
   '../layer/src/semantics.cpp', '../layer/src/texture.cpp'],
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
