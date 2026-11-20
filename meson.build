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
    cpp.find_library('crypt32'), cpp.find_library('psapi')]
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

subdir('sdk')
src_includes = [include_directories('src'), plugin_sdk_includes]
module_suffix = host_machine.system() == 'windows' ? 'dll' : 'so'
build_flags = ['-DCPPHTTPLIB_OPENSSL_SUPPORT', '-DSKIM_VERSION="' + meson.project_version() + '"']
build_flags += '-DSKIM_MODULE_SUFFIX=".' + module_suffix + '"'
subdir('plugins')

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
  'src/app/archive.cpp',
  'src/app/backup.cpp',
  'src/app/diagnostics.cpp',
  'src/app/instance.cpp',
  'src/app/live.cpp',
  'src/app/replay.cpp',
  'src/app/config.cpp',
  'src/app/station.cpp',
  'src/app/wideband.cpp',
  'src/core/clock_health.cpp',
  'src/core/process_usage.cpp',
  'src/core/schema.cpp',
  'src/core/plugin_modules.cpp',
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
  'src/web/admin.cpp',
  'src/web/common.cpp',
  'src/web/gallery.cpp',
  'src/web/pages.cpp',
  'src/web/receivers.cpp',
  'src/web/schedules.cpp',
  'src/web/services.cpp',
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
exe = executable('SkimMasterTools', 'src/app/tools.cpp',
  link_depends : plugin_modules, dependencies : station_dep, install : true)
server = executable('SkimMaster', 'src/app/skimmaster.cpp',
  link_depends : [exe], dependencies : station_dep, install : true)

test('basic', exe)
instance_test = executable('instance-test', 'tests/instance.cpp',
  dependencies : station_dep)
test('typed instance graph', instance_test)
unit_test = executable('unit-test', 'tests/unit.cpp', dependencies : station_dep)
test('unit', unit_test)

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
test('full station restart', find_program('python3'), args : [files('tests/restart.py'), server])
test('panel restore, diagnostics and reset', find_program('python3'),
  args : [files('tests/maintenance.py'), server])
test('station lifecycle', find_program('python3'), args : [files('tests/station.py'), server, exe])
test('snapshot restore', find_program('python3'), args : [files('tests/backup.py'), exe,
  meson.project_source_root() / 'subprojects/ft8_lib/test/wav/191111_110130.wav'])
test('multichannel CW frontend', find_program('python3'), args : [files('tests/cw.py'), exe])
test('WAV IQ receiver', find_program('python3'), args : [files('tests/wav_receiver.py'), exe, server,
  meson.project_source_root() / 'subprojects/ft8_lib/test/wav/191111_110130.wav'])

install_data('README.md', install_dir : get_option('datadir') / 'doc' / 'SkimMaster')
install_data('COPYING', 'COPYING.GPL-3.0', 'THIRD-PARTY-NOTICES.txt',
  install_dir : get_option('datadir') / 'licenses' / 'SkimMaster')
