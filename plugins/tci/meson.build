tci_deps = [dependency('boost', modules : ['system']), dependency('threads')]
if host_machine.system() == 'windows'
  tci_cpp = meson.get_compiler('cpp')
  tci_deps += [tci_cpp.find_library('ws2_32'), tci_cpp.find_library('mswsock')]
endif

plugin_modules += shared_module('rcvrtci', 'tci.cpp',
  include_directories : plugin_sdk_includes,
  dependencies : tci_deps,
  gnu_symbol_visibility : 'hidden',
  name_prefix : 'lib', name_suffix : module_suffix,
  install : true, install_dir : get_option('bindir'))
