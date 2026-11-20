plugin_modules += shared_module('rcvrwav', 'wav.cpp',
  include_directories : plugin_sdk_includes,
  dependencies : [dependency('sndfile')],
  gnu_symbol_visibility : 'hidden',
  name_prefix : 'lib', name_suffix : module_suffix,
  install : true, install_dir : get_option('bindir'))
