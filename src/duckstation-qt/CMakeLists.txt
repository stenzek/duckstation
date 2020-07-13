set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(duckstation-qt
  resources/resources.qrc
  aboutdialog.cpp
  aboutdialog.h
  advancedsettingswidget.cpp
  advancedsettingswidget.h
  advancedsettingswidget.ui
  audiosettingswidget.cpp
  audiosettingswidget.h
  audiosettingswidget.ui
  consolesettingswidget.cpp
  consolesettingswidget.h
  consolesettingswidget.ui
  controllersettingswidget.cpp
  controllersettingswidget.h
  gamelistsettingswidget.cpp
  gamelistsettingswidget.h
  gamelistsettingswidget.ui
  gamelistwidget.cpp
  gamelistwidget.h
  gamepropertiesdialog.cpp
  gamepropertiesdialog.h
  gamepropertiesdialog.ui
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
  memorycardsettingswidget.cpp
  memorycardsettingswidget.h
  qtdisplaywidget.cpp
  qtdisplaywidget.h
  qthostinterface.cpp
  qthostinterface.h
  qtprogresscallback.cpp
  qtprogresscallback.h
  qtutils.cpp
  qtutils.h
  settingsdialog.cpp
  settingsdialog.h
  settingsdialog.ui
)

target_include_directories(duckstation-qt PRIVATE "${Qt5Gui_PRIVATE_INCLUDE_DIRS}")
target_link_libraries(duckstation-qt PRIVATE frontend-common core common imgui glad minizip scmversion vulkan-loader Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network)

if(WIN32)
  # We want a Windows subsystem application not console.
  set_target_properties(duckstation-qt PROPERTIES
    WIN32_EXECUTABLE TRUE
    DEBUG_POSTFIX "-debug")

  # Copy in Qt DLLs. Borrowed from Dolphin.
  get_target_property(MOC_EXECUTABLE_LOCATION Qt5::moc IMPORTED_LOCATION)
  get_filename_component(QT_BINARY_DIRECTORY "${MOC_EXECUTABLE_LOCATION}" DIRECTORY)
  find_program(WINDEPLOYQT_EXE windeployqt HINTS "${QT_BINARY_DIRECTORY}")
  add_custom_command(TARGET duckstation-qt POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E env PATH="${QT_BINARY_DIRECTORY}"
            "${WINDEPLOYQT_EXE}" --libdir="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                                 --plugindir="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/QtPlugins"
                                 $<IF:$<CONFIG:Debug>,--debug,--release>
                                 --no-translations
                                 --no-compiler-runtime
                                 --no-angle
                                 "$<TARGET_FILE:duckstation-qt>"
  )
  add_custom_command(TARGET duckstation-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/qt.conf.win" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/qt.conf"
  )
endif()
