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
  opengldisplaywindow.cpp
  opengldisplaywindow.h
  portsettingswidget.cpp
  portsettingswidget.h
  qtdisplaywindow.cpp
  qtdisplaywindow.h
  qthostinterface.cpp
  qthostinterface.h
  qtsettingsinterface.cpp
  qtsettingsinterface.h
  qtutils.cpp
  qtutils.h
  settingsdialog.cpp
  settingsdialog.h
  settingsdialog.ui
)

target_link_libraries(duckstation-qt PRIVATE frontend-common core common imgui glad minizip Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network)

if(WIN32)
  target_sources(duckstation-qt PRIVATE
    d3d11displaywindow.cpp
    d3d11displaywindow.h
  )
  target_link_libraries(duckstation PRIVATE d3d11.lib dxgi.lib)
endif()
