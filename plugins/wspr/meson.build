wspr = subproject('wspr')
plugin_modules += shared_module('decoderwspr', 'wspr.cpp',
  include_directories : plugin_sdk_includes,
  dependencies : decoder_deps + [csdr_dep, wspr.get_variable('wspr_dep')],
  cpp_args : plugin_args, gnu_symbol_visibility : 'hidden',
  name_prefix : 'lib', name_suffix : module_suffix,
  install : true, install_dir : get_option('bindir'))
