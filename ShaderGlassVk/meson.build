# ShaderGlassVk -- ShaderGlass on a Vulkan layer.
#
# Additive: this build knows nothing about the Windows tree beside it and touches none of it.
project('shaderglassvk', 'cpp',
  version: '0.1.0',
  default_options: [
    'cpp_std=c++17',
    'warning_level=3',
    'optimization=2',
  ],
)

add_project_arguments('-Wno-unused-parameter', language: 'cpp')

cpp = meson.get_compiler('cpp')

host_system = host_machine.system()
host_cpu = host_machine.cpu_family()

# Vulkan headers are found on the system; unlike DLSS5VKLayer this tree vendors none, because it
# targets the host's own loader rather than a Wine one.
vulkan_dep = dependency('vulkan', required: false)
if not vulkan_dep.found()
  vulkan_dep = declare_dependency()
endif

if host_system != 'linux'
  error('ShaderGlassVk targets Linux; the Windows app is the tree beside this one')
endif

subdir('layer')

# 32-bit builds produce the layer and nothing else: the interface and the tools are 64-bit only, and
# a 32-bit game needs only a 32-bit layer to load into it.
if host_cpu == 'x86_64'
  subdir('tools')
  subdir('gen')
endif
