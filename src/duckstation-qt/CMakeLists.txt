set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(duckstation-qt
  resources/icons.qrc
  audiosettingswidget.cpp
  audiosettingswidget.h
  audiosettingswidget.ui
  consolesettingswidget.cpp
  consolesettingswidget.h
  consolesettingswidget.ui
  gamelistsettingswidget.cpp
  gamelistsettingswidget.h
  gamelistsettingswidget.ui
  gamelistwidget.cpp
  gamelistwidget.h
  generalsettingswidget.cpp
  generalsettingswidget.h
  generalsettingswidget.ui
  gpusettingswidget.cpp
  gpusettingswidget.h
  gpusettingswidget.ui
  hotkeysettingswidget.cpp
  hotkeysettingswidget.h
  inputbindingwidgets.cpp
  inputbindingwidgets.h
  main.cpp
  mainwindow.cpp
  mainwindow.h
  mainwindow.ui
  openglhostdisplay.cpp
  openglhostdisplay.h
  portsettingswidget.cpp
  portsettingswidget.h
  qthostdisplay.cpp
  qthostdisplay.h
  qtdisplaywidget.cpp
  qtdisplaywidget.h
  qthostinterface.cpp
  qthostinterface.h
  qtprogresscallback.cpp
  qtprogresscallback.h
  qtsettingsinterface.cpp
  qtsettingsinterface.h
  qtutils.cpp
  qtutils.h
  settingsdialog.cpp
  settingsdialog.h
  settingsdialog.ui
)

target_link_libraries(duckstation-qt PRIVATE frontend-common core common imgui glad minizip scmversion Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network)

if(WIN32)
  target_sources(duckstation-qt PRIVATE
    d3d11hostdisplay.cpp
    d3d11hostdisplay.h
  )
  target_link_libraries(duckstation-qt PRIVATE d3d11.lib dxgi.lib)
else()
  if(OpenGL_GLX_FOUND)
    target_compile_definitions(duckstation-qt PRIVATE "HAS_GLX")
    target_link_libraries(duckstation-qt PRIVATE OpenGL::GLX)
  endif()
endif()
