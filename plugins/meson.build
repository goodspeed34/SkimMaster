plugin_modules = []
plugin_args = ['-DSKIM_VERSION="' + meson.project_version() + '"']

subdir('ftx')
subdir('wspr')
subdir('soapy')
subdir('rtltcp')
subdir('wav')
if host_machine.system() == 'linux'
  subdir('alsa')
endif
