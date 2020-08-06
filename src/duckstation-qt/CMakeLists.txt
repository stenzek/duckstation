set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(SRCS
  resources/resources.qrc
  aboutdialog.cpp
  aboutdialog.h
  advancedsettingswidget.cpp
  advancedsettingswidget.h
  advancedsettingswidget.ui
  audiosettingswidget.cpp
  audiosettingswidget.h
  audiosettingswidget.ui
  autoupdaterdialog.cpp
  autoupdaterdialog.h
  autoupdaterdialog.ui
  consolesettingswidget.cpp
  consolesettingswidget.h
  consolesettingswidget.ui
  controllersettingswidget.cpp
  controllersettingswidget.h
  gamelistmodel.cpp
  gamelistmodel.h
  gamelistsearchdirectoriesmodel.cpp
  gamelistsearchdirectoriesmodel.h
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
  inputbindingdialog.cpp
  inputbindingdialog.h
  inputbindingdialog.ui
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

set(TS_FILES
  translations/duckstation-qt_de.ts
  translations/duckstation-qt_pt-br.ts
  translations/duckstation-qt_pt-pt.ts
)

set_source_files_properties(${TS_FILES} PROPERTIES OUTPUT_LOCATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/translations")
qt5_add_translation(QM_FILES ${TS_FILES})

add_executable(duckstation-qt ${SRCS} ${QM_FILES})
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
