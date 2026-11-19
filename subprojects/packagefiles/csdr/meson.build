project(
  'csdr',
  'c',
  version : '0.15',
  default_options : ['c_std=gnu99', 'warning_level=1'],
)

cc = meson.get_compiler('c')

# libcsdr is a set of simple SDR DSP routines. Upstream compiles it as a single
# translation unit: libcsdr_wrapper.c #includes libcsdr.c, libcsdr_gpl.c,
# ima_adpcm.c and fastddc.c; fft_fftw.c is compiled alongside it.
# -DLIBCSDR_GPL enables the GPL-licensed portions ("GPL on").
csdr_defs = ['-DLIBCSDR_GPL', '-DUSE_FFTW']

csdr_link_deps = [
  dependency('fftw3f'),
  dependency('threads'),
  cc.find_library('m', required : false),
  cc.find_library('rt', required : false),
]

csdr_lib = static_library(
  'csdr',
  files('libcsdr_wrapper.c', 'fft_fftw.c'),
  # Upstream fastddc.c expects GNU inline linkage for is_integer.
  c_args : csdr_defs + ['-fgnu89-inline'],
  include_directories : include_directories('.'),
  dependencies : csdr_link_deps,
)

csdr_dep = declare_dependency(
  include_directories : include_directories('.'),
  compile_args : csdr_defs,
  link_with : csdr_lib,
  dependencies : csdr_link_deps,
)
