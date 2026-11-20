project(
  'SkimMaster',
  'cpp',
  version : '0.1.0',
  license : 'AGPL-3.0-or-later',
  license_files : 'COPYING',
  meson_version : '>= 1.3.0',
  default_options : [
    'warning_level=3',
    'cpp_std=c++17',
    'b_ndebug=if-release',
  ],
)

csdr_dep = dependency('csdr')
ftx = subproject('ft8_lib')
decoder_deps = [
  subproject('readerwriterqueue').get_variable('readerwriterqueue_dep'),
  dependency('threads'),
  dependency('nng'),
  dependency('nlohmann_json', modules : ['nlohmann_json::nlohmann_json']),
]
boost = dependency('boost', modules : ['filesystem', 'system', 'process'])
inja = dependency('inja', method : 'pkg-config', required : false)
if not inja.found()
  inja = dependency('inja', method : 'cmake', modules : ['pantor::inja'])
endif
socket_deps = []
if host_machine.system() == 'windows'
  cpp = meson.get_compiler('cpp')
  socket_deps = [cpp.find_library('ws2_32'), cpp.find_library('mswsock'),
    cpp.find_library('crypt32')]
endif
deps = [
  csdr_dep,
  boost,
  subproject('tomlplusplus').get_variable('toml_dep'),
  dependency('morsedec', version : '>=0.3.0',
    default_options : ['tools=disabled', 'tui=disabled', 'default_library=static']),
  dependency('sndfile'),
  dependency('libpng'),
  subproject('cpp-httplib').get_variable('http_dep'),
  inja,
  dependency('libzip'),
  dependency('openssl'),
  subproject('sqlite_orm').get_variable('sqlite_orm_dep'),
] + socket_deps + decoder_deps

wspr = subproject('wspr')
src_includes = include_directories('src')
module_suffix = host_machine.system() == 'windows' ? 'dll' : 'so'
build_flags = ['-DCPPHTTPLIB_OPENSSL_SUPPORT', '-DSKIM_VERSION="' + meson.project_version() + '"']
build_flags += '-DSKIM_MODULE_SUFFIX=".' + module_suffix + '"'
ftx_plugin = shared_module('decoderftx', 'src/decoder/ftx.cpp',
  include_directories : src_includes,
  dependencies : decoder_deps + [ftx.get_variable('ftx_dep')],
  cpp_args : build_flags, name_prefix : 'lib', name_suffix : module_suffix,
  install : true, install_dir : get_option('bindir'))
wspr_plugin = shared_module('decoderwspr', 'src/decoder/wspr.cpp',
  include_directories : src_includes,
  dependencies : decoder_deps + [csdr_dep, wspr.get_variable('wspr_dep')],
  cpp_args : build_flags,
  name_prefix : 'lib', name_suffix : module_suffix, install : true, install_dir : get_option('bindir'))

radio_modules = []
foreach radio : ['soapy', 'rtltcp', 'alsa', 'wav']
  if radio == 'alsa' and host_machine.system() != 'linux'
    continue
  endif
  driver_source = 'src/receiver/adapters.cpp'
  if radio == 'soapy'
    driver_source = 'src/receiver/soapy.cpp'
    driver_deps = [dependency('SoapySDR')]
  elif radio == 'wav'
    driver_source = 'src/receiver/wav.cpp'
    driver_deps = [dependency('sndfile')]
  elif radio == 'alsa'
    driver_deps = [dependency('alsa')]
  else
    driver_deps = [boost] + socket_deps
  endif
  radio_modules += shared_module('rcvr'+radio, 'src/receiver/plugin.cpp', driver_source,
    include_directories : src_includes,
    dependencies : driver_deps,
    cpp_args : ['-DSKIM_'+radio.to_upper(), '-DSKIM_RECEIVER_ID="' + radio + '"'],
    name_prefix : 'lib', name_suffix : module_suffix, install : true, install_dir : get_option('bindir'))
endforeach

source_url = get_option('source_url')
if source_url == ''
  source_offer = vcs_tag(input : 'src/core/source_offer.h.in', output : 'source_offer.h',
    command : ['git', 'rev-parse', 'HEAD'], fallback : 'master')
else
  if not source_url.startswith('https://') and not source_url.startswith('http://')
    error('source_url must be an HTTP or HTTPS URL')
  endif
  source_config = configuration_data()
  source_config.set_quoted('SKIM_SOURCE_URL', source_url)
  source_offer = configure_file(output : 'source_offer.h', configuration : source_config)
endif

subdir('src/web')
sources = files(
  'src/app/backup.cpp',
  'src/app/instance.cpp',
  'src/app/live.cpp',
  'src/app/replay.cpp',
  'src/app/config.cpp',
  'src/app/station.cpp',
  'src/app/wideband.cpp',
  'src/core/clock_health.cpp',
  'src/decoder/session.cpp',
  'src/decoder/slot.cpp',
  'src/decoder/sstv.cpp',
  'src/decoder/text_modes.cpp',
  'src/dsp/spectrum.cpp',
  'src/receiver/receiver.cpp',
  'src/report/cluster.cpp',
  'src/report/reporting.cpp',
  'src/store/store.cpp',
  'src/store/workers.cpp',
  'src/web/web.cpp',
)

station_lib = static_library(
  'skimmaster_station', sources, assets, source_offer,
  include_directories : src_includes,
  cpp_args : build_flags,
  dependencies : deps,
)
station_dep = declare_dependency(
  link_with : station_lib, include_directories : src_includes, dependencies : deps,
)
modules = [ftx_plugin, wspr_plugin] + radio_modules
exe = executable('SkimMasterTools', 'src/app/tools.cpp',
  link_depends : modules, dependencies : station_dep, install : true)
server = executable('SkimMaster', 'src/app/skimmaster.cpp',
  link_depends : [exe], dependencies : station_dep, install : true)

test('basic', exe)
instance_test = executable('instance-test', 'tests/instance.cpp',
  dependencies : station_dep)
test('typed instance graph', instance_test)

test('replay', find_program('python3'), args : [files('tests/replay.py'), exe])

test('FT4 and FT8 slots', find_program('python3'), args : [files('tests/digital.py'), exe,
  ftx.get_variable('fixture_gen'), meson.project_source_root() / 'subprojects/ft8_lib/test/wav/191111_110130.wav'])
foreach mode : ['Martin1','Martin2','Scottie1','Scottie2','Robot36','Robot72']
  test(mode + ' SSTV', find_program('python3'), args : [files('tests/sstv.py'), exe, mode])
endforeach
test('WSPR slot', find_program('python3'), args : [files('tests/wspr.py'), exe,
  meson.project_source_root() / 'subprojects/wspr/signals/refSignalSnr0dB.iq'])
test('local control panel', find_program('python3'), args : [files('tests/panel.py'), exe, server,
  meson.project_source_root() / 'subprojects/ft8_lib/test/wav/191111_110130.wav'])
test('RTTY stream', find_program('python3'), args : [files('tests/text_modes.py'), exe])
test('reporting protocols', find_program('python3'), args : [files('tests/reporting.py'), exe,
  meson.project_source_root() / 'subprojects/ft8_lib/test/wav/191111_110130.wav'])
test('station lifecycle', find_program('python3'), args : [files('tests/station.py'), server, exe])
test('snapshot restore', find_program('python3'), args : [files('tests/backup.py'), exe,
  meson.project_source_root() / 'subprojects/ft8_lib/test/wav/191111_110130.wav'])
test('multichannel CW frontend', find_program('python3'), args : [files('tests/cw.py'), exe])
test('WAV IQ receiver', find_program('python3'), args : [files('tests/wav_receiver.py'), exe, server,
  meson.project_source_root() / 'subprojects/ft8_lib/test/wav/191111_110130.wav'])

install_data('README.md', install_dir : get_option('datadir') / 'doc' / 'SkimMaster')
install_data('COPYING', 'COPYING.GPL-3.0', 'THIRD-PARTY-NOTICES.txt',
  install_dir : get_option('datadir') / 'licenses' / 'SkimMaster')
