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
