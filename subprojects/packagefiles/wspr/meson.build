project('wspr', 'c', default_options : ['c_std=gnu17'])
inc = include_directories('.')
fft = dependency('fftw3f')
lib = static_library('wspr_decode', files('wsprd/wsprd.c', 'wsprd/wsprsim_utils.c',
  'wsprd/wsprd_utils.c', 'wsprd/tab.c', 'wsprd/fano.c', 'wsprd/nhash.c'),
  dependencies : fft)
wspr_dep = declare_dependency(link_with : lib, include_directories : inc, dependencies : fft)
