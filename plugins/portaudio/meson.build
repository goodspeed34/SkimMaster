portaudio_dep = dependency('portaudio-2.0')
plugin_modules += shared_module('rcvrportaudio', 'portaudio.cpp',
  include_directories : plugin_sdk_includes,
  dependencies : [portaudio_dep, dependency('threads')],
  gnu_symbol_visibility : 'hidden',
  name_prefix : 'lib', name_suffix : module_suffix,
  install : true, install_dir : get_option('bindir'))

# Exercise the actual plugin against a deterministic PortAudio input.
portaudio_test = executable('portaudio-test', 'test_portaudio.cpp', 'portaudio.cpp',
  include_directories : plugin_sdk_includes,
  dependencies : [portaudio_dep.partial_dependency(compile_args : true, includes : true),
    dependency('threads')])
test('PortAudio IQ capture', portaudio_test)
