plugin_modules += shared_module('rcvrsoapy', 'soapy.cpp',
  include_directories : plugin_sdk_includes,
  dependencies : [dependency('SoapySDR')],
  gnu_symbol_visibility : 'hidden',
  name_prefix : 'lib', name_suffix : module_suffix,
  install : true, install_dir : get_option('bindir'))
