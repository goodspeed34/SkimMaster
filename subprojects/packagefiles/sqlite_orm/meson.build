project('sqlite_orm', 'cpp', version : '1.9.1', license : 'AGPL-3.0-only')
sqlite_orm_dep = declare_dependency(
  include_directories : include_directories('include', is_system : true),
  dependencies : dependency('sqlite3'),
)
install_data('LICENSE', rename : 'sqlite_orm-LICENSE',
  install_dir : get_option('datadir') / 'licenses' / 'SkimMaster')
