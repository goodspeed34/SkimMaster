project('skimmaster-external-plugin-check', 'cpp',
  meson_version : '>= 1.3.0', default_options : ['cpp_std=c++17'])
sdk = include_directories(get_option('sdk_include'))
suffix = host_machine.system() == 'windows' ? 'dll' : 'so'

shared_module('rcvr_external', 'receiver.cpp', include_directories : sdk,
  name_prefix : 'lib', name_suffix : suffix, gnu_symbol_visibility : 'hidden')
shared_module('decoder_external', 'decoder.cpp', include_directories : sdk,
  dependencies : [dependency('threads'), dependency('nng'),
    dependency('nlohmann_json', modules : ['nlohmann_json::nlohmann_json'])],
  name_prefix : 'lib', name_suffix : suffix, gnu_symbol_visibility : 'hidden')
