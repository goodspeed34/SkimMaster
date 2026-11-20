rtltcp_deps = [dependency('boost', modules : ['system']), dependency('threads')]
if host_machine.system() == 'windows'
  rtltcp_cpp = meson.get_compiler('cpp')
  rtltcp_deps += [rtltcp_cpp.find_library('ws2_32'), rtltcp_cpp.find_library('mswsock')]
endif

plugin_modules += shared_module('rcvrrtltcp', 'rtltcp.cpp',
  include_directories : plugin_sdk_includes,
  dependencies : rtltcp_deps,
  gnu_symbol_visibility : 'hidden',
  name_prefix : 'lib', name_suffix : module_suffix,
  install : true, install_dir : get_option('bindir'))
