flex_deps = [dependency('boost', modules : ['system']), dependency('threads')]
if host_machine.system() == 'windows'
  flex_cpp = meson.get_compiler('cpp')
  flex_deps += [flex_cpp.find_library('ws2_32'), flex_cpp.find_library('mswsock')]
endif

plugin_modules += shared_module('rcvrflex', 'flex.cpp',
  include_directories : plugin_sdk_includes,
  dependencies : flex_deps,
  gnu_symbol_visibility : 'hidden',
  name_prefix : 'lib', name_suffix : module_suffix,
  install : true, install_dir : get_option('bindir'))
