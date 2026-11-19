assets = custom_target(
  'panel assets',
  input : [
    'assets/panel.html',
    'assets/wizard_fields.html',
    '../../ReferenceDesign/index.html',
    '../../ReferenceDesign/wizard.html',
    '../../ReferenceDesign/style.css',
    'assets/charts.js',
    'assets/chrome.js',
    'assets/wizard.js',
    'assets/receiver_editor.html',
    'assets/receiver_probe.html',
    'assets/receivers.js',
  ],
  output : 'panel_assets.h',
  command : [find_program('python3'), files('assets/embed.py'), '@INPUT@', '@OUTPUT@'],
)
