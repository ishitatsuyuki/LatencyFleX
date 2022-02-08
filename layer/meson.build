project('latencyflex_layer', 'c', 'cpp', default_options : ['c_std=c11', 'cpp_std=c++17'])

cmake = import('cmake')

cc = meson.get_compiler('c')
funchook_opt = cmake.subproject_options()
funchook_opt.add_cmake_defines({'CMAKE_POSITION_INDEPENDENT_CODE': true})
funchook_opt.set_install(false)
funchook = cmake.subproject('funchook', options: funchook_opt)
funchook_dep = funchook.dependency('funchook-static')
distorm_dep = funchook.dependency('distorm')
libdl_dep = cc.find_library('dl')

vulkan_dep = dependency('vulkan')
thread_dep = dependency('threads')

deps = [vulkan_dep, thread_dep, funchook_dep, distorm_dep, libdl_dep]

with_perfetto = get_option('perfetto')
if with_perfetto
  dep_perfetto = dependency('perfetto', fallback : ['perfetto', 'dep_perfetto'])
  deps += dep_perfetto
  add_project_arguments('-DLATENCYFLEX_HAVE_PERFETTO', language : ['c', 'cpp'])
endif

incdir = include_directories('..')
layer = library('latencyflex_layer', 'latencyflex_layer.cpp', 'latencyflex_ue4_hook.cpp', 'latencyflex_perfetto.cpp',
        gnu_symbol_visibility : 'hidden',
        link_args : '-Wl,--exclude-libs,ALL',
        dependencies : deps,
        include_directories : incdir,
        install: true)

configure_file(input : 'layer.json.in',
  output : 'latencyflex.json',
  configuration : {'lib_path' : join_paths(get_option('prefix'), get_option('libdir'), 'liblatencyflex_layer.so')},
  install : true,
  install_dir : join_paths(get_option('datadir'), 'vulkan', 'implicit_layer.d'),
)