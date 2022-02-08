project('latencyflex_layer_wine', 'cpp')

is_mingw = target_machine.system() == 'windows'

incdir = include_directories('..')
if not is_mingw
    layer = meson.get_compiler('cpp').find_library('latencyflex_layer')
endif

arch_dir_prefix = target_machine.cpu_family() == 'x86_64' ? 'x86_64-' : 'i386-'
os_suffix = is_mingw ? 'windows' : 'unix'

install_dir = get_option('libdir') / 'wine' / arch_dir_prefix + os_suffix

if get_option('old_install_layout')
    install_dir    = is_mingw ? install_dir / 'fakedlls' : install_dir
endif

if is_mingw
layer_dll = shared_library('latencyflex_layer', ['builtin.cpp'],
    name_prefix  : '',
    link_depends : ['latencyflex_layer.spec'],
    link_args    : ['-Wl,--wine-builtin', meson.current_source_dir() / 'latencyflex_layer.spec'],
    install     : true,
    install_dir : install_dir)

compat_dll = shared_library('latencyflex_wine', [],
    name_prefix  : '',
    link_depends : ['latencyflex_wine.spec'],
    link_args    : ['-Wl,--wine-builtin', meson.current_source_dir() / 'latencyflex_wine.spec'],
    install     : true,
    install_dir : install_dir)
else
binding = shared_library('latencyflex_layer', ['unixlib.cpp'],
    name_prefix  : '',
    dependencies : [layer],
    include_directories : incdir,
    install     : true,
    install_dir : install_dir)
endif