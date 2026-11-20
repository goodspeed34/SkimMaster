ftx = subproject('ft8_lib')
plugin_modules += shared_module('decoderftx', 'ftx.cpp',
  include_directories : plugin_sdk_includes,
  dependencies : decoder_deps + [ftx.get_variable('ftx_dep')],
  cpp_args : plugin_args, gnu_symbol_visibility : 'hidden',
  name_prefix : 'lib', name_suffix : module_suffix,
  install : true, install_dir : get_option('bindir'))
