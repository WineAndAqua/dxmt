winemetal_src = [
  'main.c',
  'exports.c',
  'metalcpp-impl.cpp',
  'error_handling.c',
  'unix_thunks.c',
]

if cc.has_link_argument('/entry:WineMetalEntry')
  winemetal_ld_args      = ['-Wl,/entry:WineMetalEntry']
else
  winemetal_ld_args      = ['-Wl,-eWineMetalEntry']
endif

winemetal_link_depends = []

wine_build_path = get_option('wine_build_path')
wine_install_path = get_option('wine_install_path')
if wine_build_path != ''
  if not wine_build_path.startswith('/')
    wine_build_path = join_paths(meson.project_source_root(), wine_build_path)
  endif
  lib_winecrt0 = cc.find_library('winecrt0', dirs : [ join_paths(wine_build_path, 'dlls/winecrt0/x86_64-windows') ])
  lib_ntdll = cc.find_library('ntdll', dirs : [ join_paths(wine_build_path, 'dlls/ntdll/x86_64-windows') ])
  lib_dbghelp = cc.find_library('dbghelp', dirs : [ join_paths(wine_build_path, 'dlls/dbghelp/x86_64-windows') ])
  winebuild = join_paths(wine_build_path, 'tools/winebuild/winebuild')
elif wine_install_path != ''
  if not wine_install_path.startswith('/')
    wine_install_path = join_paths(meson.project_source_root(), wine_install_path)
  endif
  lib_winecrt0 = cc.find_library('winecrt0', dirs : [ join_paths(wine_install_path,'lib/wine/x86_64-windows') ])
  lib_ntdll = cc.find_library('ntdll', dirs : [ join_paths(wine_install_path, 'lib/wine/x86_64-windows') ])
  lib_dbghelp = cc.find_library('dbghelp', dirs : [ join_paths(wine_install_path, 'lib/wine/x86_64-windows') ])
  winebuild = join_paths(wine_install_path, 'bin/winebuild')
else
  error('One of the options -Dwine_build_path or -Dwine_install_path is required')
endif

winemetal_dll = shared_library('winemetal', winemetal_src,
  name_prefix         : '',
  dependencies        : [ lib_winecrt0, lib_ntdll, lib_dbghelp, airconv_forward_dep ],
  include_directories : dxmt_include_path,
  install             : true,
  install_dir         : windows_builtin_install_dir,
  link_args           : winemetal_ld_args,
  link_depends        : [ winemetal_link_depends ],
)

winemetal_dep = declare_dependency(
  link_with           : [ winemetal_dll ],
  include_directories : [ dxmt_include_path ],
)

custom_target('postprocess_lib',
  input : winemetal_dll,
  output: 'winemetal.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : winemetal_dll,
  build_by_default : true
)

subdir('unix')