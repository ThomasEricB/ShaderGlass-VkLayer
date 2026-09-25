layer_sources = files(
  'src/layer.cpp',
  'src/chain.cpp',
  'src/catalogue.cpp',
  'src/semantics.cpp',
  'src/pixel_grid.cpp',
  'src/chain_order.cpp',
  'src/placement.cpp',
  'src/gamescope_output.cpp',
  'src/texture.cpp',
  'src/crash_trace.cpp',
)

layer_args = []
layer_name = 'VkLayer_ShaderGlass'

if host_machine.cpu_family() in ['x86', 'i686']
  layer_args += ['-DSHADERGLASS_LAYER_32']
  layer_name = 'VkLayer_ShaderGlass_32'
endif

# Preset textures are PNG and JPEG, decoded when a preset is selected. stb_image is header-only, so
# this adds nothing to what the layer links against -- which matters for a library mapped into every
# game. Distributions spell the include path differently; without it the layer still builds and
# every other part of a preset still runs, minus its textures.
cxx = meson.get_compiler('cpp')
foreach candidate : ['stb/stb_image.h', 'stb_image.h']
  if not layer_args.contains('-DSG_HAVE_STB_IMAGE')
    if cxx.has_header(candidate)
      layer_args += ['-DSG_HAVE_STB_IMAGE', '-DSG_STB_IMAGE_HEADER=<' + candidate + '>']
    endif
  endif
endforeach

if not layer_args.contains('-DSG_HAVE_STB_IMAGE')
  warning('stb_image.h not found: presets will run without their textures (LUTs, bezels, overlays)')
endif

shared_library(layer_name, layer_sources,
  name_prefix: 'lib',
  include_directories: include_directories('src', '../common', '../presets'),
  cpp_args: layer_args + ['-fPIC'],
  link_args: [
    '-static-libstdc++',
    '-static-libgcc',
    '-Wl,--version-script=' + meson.current_source_dir() / 'shaderglass.map',
    '-Wl,-Bsymbolic',
  ],
  dependencies: [vulkan_dep, dependency('threads'), cxx.find_library('dl', required: false)],
  install: false,
)
