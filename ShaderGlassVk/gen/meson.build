# shaderglass-gen: the .slangp -> SPIR-V catalogue compiler.
#
# glslang is required. SPIRV-Cross is optional: when it is present the generator uses it for
# reflection and can diff it against the built-in reader with --reflect-check; when it is absent the
# built-in reader is used and the generator still builds with nothing but glslang.

glslang_dep = cpp.find_library('glslang', required: true)
glslang_res_dep = cpp.find_library('glslang-default-resource-limits', required: true)

gen_sources = files(
  'main.cpp',
  'shadergc.cpp',
  'glsl.cpp',
  'reflect_builtin.cpp',
)

gen_args = ['-DSG_PRESET_API_H_PATH="' + meson.current_source_dir() / '..' / 'presets' / 'preset_api.h' + '"']
gen_deps = [glslang_dep, glslang_res_dep]

spirv_cross_dep = dependency('spirv-cross-c-shared', required: false)
if not spirv_cross_dep.found()
  spirv_cross_dep = cpp.find_library('spirv-cross-core', required: false)
endif

if spirv_cross_dep.found() and cpp.has_header('spirv_cross/spirv_reflect.hpp')
  message('SPIRV-Cross found: reflection cross-check available')
  gen_sources += files('reflect_cross.cpp')
  gen_args += ['-DSG_HAVE_SPIRV_CROSS']
  gen_deps += [
    spirv_cross_dep,
    cpp.find_library('spirv-cross-glsl', required: false),
    cpp.find_library('spirv-cross-reflect', required: false),
  ]
else
  message('SPIRV-Cross not found: using the built-in SPIR-V reflection')
endif

executable('shaderglass-gen', gen_sources,
  include_directories: include_directories('.', '../presets'),
  cpp_args: gen_args,
  dependencies: gen_deps,
  install: false,
)
