set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(SRCS
  resources/resources.qrc
  aboutdialog.cpp
  aboutdialog.h
  aboutdialog.ui
  advancedsettingswidget.cpp
  advancedsettingswidget.h
  advancedsettingswidget.ui
  audiosettingswidget.cpp
  audiosettingswidget.h
  audiosettingswidget.ui
  autoupdaterdialog.cpp
  autoupdaterdialog.h
  autoupdaterdialog.ui
  biossettingswidget.cpp
  biossettingswidget.h
  biossettingswidget.ui
  cheatcodeeditordialog.cpp
  cheatcodeeditordialog.h
  cheatcodeeditordialog.ui
  cheatmanagerdialog.cpp
  cheatmanagerdialog.h
  cheatmanagerdialog.ui
  collapsiblewidget.cpp
  collapsiblewidget.h
  consolesettingswidget.cpp
  consolesettingswidget.h
  consolesettingswidget.ui
  controllerbindingwidget_analog_controller.ui
  controllerbindingwidget_analog_joystick.ui
  controllerbindingwidget_digital_controller.ui
  controllerbindingwidget_guncon.ui
  controllerbindingwidget_mouse.ui
  controllerbindingwidget_negcon.ui
  controllerbindingwidgets.cpp
  controllerbindingwidgets.h
  controllerbindingwidget.ui
  controllerglobalsettingswidget.cpp
  controllerglobalsettingswidget.h
  controllerglobalsettingswidget.ui
  controllermacroeditwidget.ui
  controllermacrowidget.ui
  controllersettingsdialog.cpp
  controllersettingsdialog.h
  controllersettingsdialog.ui
  controllersettingwidgetbinder.h
  coverdownloaddialog.cpp
  coverdownloaddialog.h
  coverdownloaddialog.ui
  debuggermodels.cpp
  debuggermodels.h
  debuggerwindow.cpp
  debuggerwindow.h
  debuggerwindow.ui
  displaysettingswidget.cpp
  displaysettingswidget.h
  displaysettingswidget.ui
  displaywidget.cpp
  displaywidget.h
  emptygamelistwidget.ui
  emulationsettingswidget.cpp
  emulationsettingswidget.h
  emulationsettingswidget.ui
  enhancementsettingswidget.cpp
  enhancementsettingswidget.h
  enhancementsettingswidget.ui
  foldersettingswidget.cpp
  foldersettingswidget.h
  foldersettingswidget.ui
  gamelistmodel.cpp
  gamelistmodel.h
  gamelistrefreshthread.cpp
  gamelistrefreshthread.h
  gamelistsearchdirectoriesmodel.cpp
  gamelistsearchdirectoriesmodel.h
  gamelistsettingswidget.cpp
  gamelistsettingswidget.h
  gamelistsettingswidget.ui
  gamelistwidget.cpp
  gamelistwidget.h
  gamelistwidget.ui
  gamesummarywidget.cpp
  gamesummarywidget.h
  gamesummarywidget.ui
  gdbconnection.cpp
  gdbconnection.h
  gdbserver.cpp
  gdbserver.h
  generalsettingswidget.cpp
  generalsettingswidget.h
  generalsettingswidget.ui
  hotkeysettingswidget.cpp
  hotkeysettingswidget.h
  inputbindingdialog.cpp
  inputbindingdialog.h
  inputbindingdialog.ui
  inputbindingwidgets.cpp
  inputbindingwidgets.h
  mainwindow.cpp
  mainwindow.h
  mainwindow.ui
  memorycardeditordialog.cpp
  memorycardeditordialog.h
  memorycardeditordialog.ui
  memorycardsettingswidget.cpp
  memorycardsettingswidget.h
  memoryviewwidget.cpp
  memoryviewwidget.h
  postprocessingchainconfigwidget.cpp
  postprocessingchainconfigwidget.h
  postprocessingchainconfigwidget.ui
  postprocessingsettingswidget.cpp
  postprocessingsettingswidget.h
  postprocessingsettingswidget.ui
  postprocessingshaderconfigwidget.cpp
  postprocessingshaderconfigwidget.h
  qthost.cpp
  qthost.h
  qtkeycodes.cpp
  qtprogresscallback.cpp
  qtprogresscallback.h
  qttranslations.cpp
  qtutils.cpp
  qtutils.h
  resource.h
  settingsdialog.cpp
  settingsdialog.h
  settingsdialog.ui
  settingwidgetbinder.h
)

if(ENABLE_CHEEVOS)
  set(SRCS ${SRCS}
    achievementlogindialog.cpp
    achievementlogindialog.h
    achievementlogindialog.ui
    achievementsettingswidget.cpp
    achievementsettingswidget.h
    achievementsettingswidget.ui
  )
endif()

set(TS_FILES
  translations/duckstation-qt_de.ts
  translations/duckstation-qt_en.ts
  translations/duckstation-qt_es.ts
  translations/duckstation-qt_es-es.ts
  translations/duckstation-qt_fr.ts
  translations/duckstation-qt_he.ts
  translations/duckstation-qt_it.ts
  translations/duckstation-qt_ja.ts
  translations/duckstation-qt_nl.ts
  translations/duckstation-qt_pl.ts
  translations/duckstation-qt_pt-br.ts
  translations/duckstation-qt_pt-pt.ts
  translations/duckstation-qt_ru.ts
  translations/duckstation-qt_tr.ts
  translations/duckstation-qt_zh-cn.ts
)

set_source_files_properties(${TS_FILES} PROPERTIES OUTPUT_LOCATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/translations")
qt6_add_translation(QM_FILES ${TS_FILES})

add_executable(duckstation-qt ${SRCS} ${QM_FILES})
target_include_directories(duckstation-qt PRIVATE "${Qt6Gui_PRIVATE_INCLUDE_DIRS}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckstation-qt PRIVATE frontend-common core common imgui glad minizip scmversion Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

if(WIN32)
  # We want a Windows subsystem application not console.
  set_target_properties(duckstation-qt PROPERTIES
    WIN32_EXECUTABLE TRUE
    DEBUG_POSTFIX "-debug")

  # Copy in Qt DLLs. Borrowed from Dolphin.
  get_target_property(MOC_EXECUTABLE_LOCATION Qt6::moc IMPORTED_LOCATION)
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

if(APPLE)
  set(BUNDLE_PATH ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DuckStation.app)

  # Ask for an application bundle.
  set_target_properties(duckstation-qt PROPERTIES
    MACOSX_BUNDLE true
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    OUTPUT_NAME DuckStation
  )

  # Use macdeployqt to inject Qt into the bundle.
  get_target_property(MOC_EXECUTABLE_LOCATION Qt6::moc IMPORTED_LOCATION)
  get_filename_component(QT_BINARY_DIRECTORY "${MOC_EXECUTABLE_LOCATION}" DIRECTORY)
  find_program(MACDEPLOYQT_EXE macdeployqt HINTS "${QT_BINARY_DIRECTORY}")
  add_custom_target(duckstation-postprocess-bundle ALL
    COMMAND "${MACDEPLOYQT_EXE}" "${BUNDLE_PATH}"
  )
  add_dependencies(duckstation-postprocess-bundle duckstation-qt)

  # Copy icon into the bundle
  target_sources(duckstation-qt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/DuckStation.icns")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/DuckStation.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

  # Copy resources into the bundle
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/data/resources")
  file(GLOB_RECURSE resources RELATIVE "${CMAKE_SOURCE_DIR}/data/resources" "${CMAKE_SOURCE_DIR}/data/resources/*")
  foreach(res ${resources})
    target_sources(duckstation-qt PRIVATE "${CMAKE_SOURCE_DIR}/data/resources/${res}")
    get_filename_component(resdir "${res}" DIRECTORY)
    set_source_files_properties("${CMAKE_SOURCE_DIR}/data/resources/${res}" PROPERTIES
      MACOSX_PACKAGE_LOCATION "Resources/${resdir}")
    source_group("Resources" FILES "${CMAKE_SOURCE_DIR}/data/resources/${res}")
  endforeach()

  # Copy translations into the bundle
  add_custom_command(TARGET duckstation-qt
                     POST_BUILD
                     COMMAND mkdir -p $<TARGET_FILE_DIR:duckstation-qt>/translations
                     COMMAND cp ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/translations/*.qm $<TARGET_FILE_DIR:duckstation-qt>/translations)

  # Copy MoltenVK into the bundle
  unset(MOLTENVK_PATH CACHE)
  find_file(MOLTENVK_PATH NAMES
    libMoltenVK.dylib
    lib/libMoltenVK.dylib
  )
  if (MOLTENVK_PATH)
    target_sources(duckstation-qt PRIVATE "${MOLTENVK_PATH}")
    set_source_files_properties("${MOLTENVK_PATH}" PROPERTIES MACOSX_PACKAGE_LOCATION Frameworks)
    message(STATUS "Using MoltenVK from ${MOLTENVK_PATH}")
  else()
    message(WARNING "MoltenVK not found in path, it will depend on the target system having it.")
  endif()
endif()

