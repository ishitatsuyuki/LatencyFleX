project('latencyflex_layer_wine', 'cpp')

winebuild = find_program('winebuild')

incdir = include_directories('..')
layer = meson.get_compiler('cpp').find_library('latencyflex_layer')

install_dir = get_option('libdir') / 'wine'

arch_dir_prefix = target_machine.cpu_family() == 'x86_64' ? 'x86_64-' : 'i386-'

install_dir_unix    = install_dir / arch_dir_prefix + 'unix'
install_dir_windows = install_dir / arch_dir_prefix + 'windows'

if get_option('old_install_layout')
    install_dir_unix    = install_dir
    install_dir_windows = install_dir / 'fakedlls'
endif

wine_def_target = custom_target('liblatencyflex_wine.def',
    output      : 'liblatencyflex_wine.def',
    input       : 'latencyflex.spec',
    command     : [winebuild, '--def', '-o', '@OUTPUT@', '-E', '@INPUT@'])

wine_dll = shared_library('latencyflex_wine.dll', ['latencyflex.cpp'],
    name_prefix  : '',
    dependencies : [layer],
    include_directories : incdir,
    link_depends : [wine_def_target],
    link_args    : [meson.current_build_dir() / 'liblatencyflex_wine.def'],
    install     : true,
    install_dir : install_dir_unix)

wine_dll_target = custom_target('latencyflex_wine.dll',
    output      : 'latencyflex_wine.dll',
    input       : [wine_def_target],
    command     : [winebuild, '--dll', '--fake-module', '-o', '@OUTPUT@', '-E', '@INPUT@'],
    install     : true,
    install_dir : install_dir_windows)