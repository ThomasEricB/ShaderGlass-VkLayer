# The interface. Optional: a build with no Qt still produces the layer, the generator and the
# control tool, which is everything a headless or packaging build needs.
qt6 = import('qt6')
qt6_dep = dependency('qt6', modules: ['Core', 'Gui', 'Widgets'], required: false)

if not qt6_dep.found()
  warning('Qt 6 not found: skipping shaderglass-gui')
  subdir_done()
endif

gui_moc = qt6.preprocess(
  moc_headers: [
    'mainwindow.h',
    'preset_tree.h',
    'param_panel.h',
    'capture_view.h',
    'shm_binder.h',
  ],
  dependencies: qt6_dep,
)

executable('shaderglass-gui',
  [
    'main.cpp',
    'mainwindow.cpp',
    'preset_tree.cpp',
    'param_panel.cpp',
    'capture_view.cpp',
    'shm_binder.cpp',
    # The catalogue loader is the layer's own, so the interface and the layer can never disagree
    # about what a catalogue contains or which ABI they accept.
    '../layer/src/catalogue.cpp',
  ] + gui_moc,
  include_directories: include_directories('.', '../common', '../presets', '../layer/src'),
  dependencies: [qt6_dep, cpp.find_library('dl', required: false)],
  install: false,
)
