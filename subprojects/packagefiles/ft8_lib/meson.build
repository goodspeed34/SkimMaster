project('ft8_lib', 'c', default_options : ['c_std=gnu99'])
includes = include_directories('.')
lib = static_library('ftx', files(
  'ft8/constants.c', 'ft8/crc.c', 'ft8/decode.c', 'ft8/ldpc.c',
  'ft8/message.c', 'ft8/text.c', 'ft8/encode.c', 'common/monitor.c',
  'fft/kiss_fft.c', 'fft/kiss_fftr.c',
), include_directories : includes)
ftx_dep = declare_dependency(link_with : lib, include_directories : includes)
fixture_link_args = host_machine.system() == 'windows' ? ['-Wl,--stack,4194304'] : []
fixture_gen = executable('gen_ftx_fixture', 'demo/gen_ft8.c', 'common/wave.c',
  dependencies : [ftx_dep, meson.get_compiler('c').find_library('m', required : false)],
  link_args : fixture_link_args,
  build_by_default : false)
