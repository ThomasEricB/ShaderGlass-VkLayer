layer_sources = files(
  'src/layer.cpp',
  'src/chain.cpp',
)

layer_args = []
layer_name = 'VkLayer_ShaderGlass'

if host_machine.cpu_family() in ['x86', 'i686']
  layer_args += ['-DSHADERGLASS_LAYER_32']
  layer_name = 'VkLayer_ShaderGlass_32'
endif

shared_library(layer_name, layer_sources,
  name_prefix: 'lib',
  include_directories: include_directories('src', '../common'),
  cpp_args: layer_args + ['-fPIC'],
  link_args: [
    '-static-libstdc++',
    '-static-libgcc',
    '-Wl,--version-script=' + meson.current_source_dir() / 'shaderglass.map',
    '-Wl,-Bsymbolic',
  ],
  dependencies: [vulkan_dep, dependency('threads')],
  install: false,
)
