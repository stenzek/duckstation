# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction
#
# NOTE: In addition to the terms of CC-BY-NC-ND-4.0, you may not use this file to create
# packages or build recipes without explicit permission from the copyright holder.

include(CopyBaseTranslations)

set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(SRCS
  aboutdialog.cpp
  aboutdialog.h
  aboutdialog.ui
  achievementlogindialog.cpp
  achievementlogindialog.h
  achievementlogindialog.ui
  achievementsettingswidget.cpp
  achievementsettingswidget.h
  achievementsettingswidget.ui
  advancedsettingswidget.cpp
  advancedsettingswidget.h
  advancedsettingswidget.ui
  animatedstackedwidget.cpp
  animatedstackedwidget.h
  asynchttprequest.cpp
  asynchttprequest.h
  asyncpixmaploader.cpp
  asyncpixmaploader.h
  audiosettingswidget.cpp
  audiosettingswidget.h
  audiosettingswidget.ui
  autoupdaterdialog.cpp
  autoupdaterdialog.h
  autoupdaterdialog.ui
  biossettingswidget.cpp
  biossettingswidget.h
  biossettingswidget.ui
  cardradiobutton.cpp
  cardradiobutton.h
  colorpickerbutton.cpp
  colorpickerbutton.h
  capturesettingswidget.cpp
  capturesettingswidget.h
  capturesettingswidget.ui
  consolesettingswidget.cpp
  consolesettingswidget.h
  consolesettingswidget.ui
  controllerbindingwidget_analog_controller.ui
  controllerbindingwidget_analog_joystick.ui
  controllerbindingwidget_digital_controller.ui
  controllerbindingwidget_guncon.ui
  controllerbindingwidget_justifier.ui
  controllerbindingwidget_mouse.ui
  controllerbindingwidget_negcon.ui
  controllerbindingwidget_negconrumble.ui
  controllerbindingwidgets.cpp
  controllerbindingwidgets.h
  controllerbindingwidget.ui
  controllerglobalsettingswidget.cpp
  controllerglobalsettingswidget.h
  controllerglobalsettingswidget.ui
  controllermacroeditwidget.ui
  controllersettingswindow.cpp
  controllersettingswindow.h
  controllersettingswindow.ui
  controllersettingwidgetbinder.h
  coverdownloadwindow.cpp
  coverdownloadwindow.h
  coverdownloadwindow.ui
  debuggeraddbreakpointdialog.ui
  debuggercodeview.cpp
  debuggercodeview.h
  debuggermodels.cpp
  debuggermodels.h
  debuggerwindow.cpp
  debuggerwindow.h
  debuggerwindow.ui
  debuggingsettingswidget.cpp
  debuggingsettingswidget.h
  debuggingsettingswidget.ui
  displaywidget.cpp
  displaywidget.h
  editgameserialdialog.ui
  emptygamelistwidget.ui
  emulationsettingswidget.cpp
  emulationsettingswidget.h
  emulationsettingswidget.ui
  gamecheatcodechoiceeditordialog.ui
  gamecheatcodeeditordialog.ui
  gamecheatsettingswidget.cpp
  gamecheatsettingswidget.h
  gamecheatsettingswidget.ui
  gamepatchdetailswidget.ui
  gamepatchsettingswidget.cpp
  gamepatchsettingswidget.h
  gamepatchsettingswidget.ui
  gamelistsettingswidget.cpp
  gamelistsettingswidget.h
  gamelistsettingswidget.ui
  gamelistwidget.cpp
  gamelistwidget.h
  gamelistwidget.ui
  gamesummarywidget.cpp
  gamesummarywidget.h
  gamesummarywidget.ui
  graphicssettingswidget.cpp
  graphicssettingswidget.h
  graphicssettingswidget.ui
  hotkeysettingswidget.cpp
  hotkeysettingswidget.h
  inputbindingdialog.cpp
  inputbindingdialog.h
  inputbindingdialog.ui
  inputbindingwidgets.cpp
  inputbindingwidgets.h
  interfacesettingswidget.cpp
  interfacesettingswidget.h
  interfacesettingswidget.ui
  isobrowserwindow.cpp
  isobrowserwindow.h
  isobrowserwindow.ui
  logwindow.cpp
  logwindow.h
  mainwindow.cpp
  mainwindow.h
  mainwindow.ui
  memorycardeditorwindow.cpp
  memorycardeditorwindow.h
  memorycardeditorwindow.ui
  memorycardrenamefiledialog.ui
  memorycardsettingswidget.cpp
  memorycardsettingswidget.h
  memoryeditorwindow.cpp
  memoryeditorwindow.h
  memoryeditorwindow.ui
  memoryscannerwindow.cpp
  memoryscannerwindow.h
  memoryscannerwindow.ui
  memoryviewwidget.cpp
  memoryviewwidget.h
  osdsettingswidget.cpp
  osdsettingswidget.h
  osdsettingswidget.ui
  postprocessingsettingswidget.cpp
  postprocessingsettingswidget.h
  postprocessingchainconfigwidget.ui
  postprocessingoverlayconfigwidget.ui
  postprocessingselectshaderdialog.ui
  qthost.cpp
  qthost.h
  qtprogresscallback.cpp
  qtprogresscallback.h
  qtthemes.cpp
  qttranslations.inl
  qtutils.cpp
  qtutils.h
  qtwindowinfo.cpp
  qtwindowinfo.h
  resource.h
  searchbox.cpp
  searchbox.h
  selectdiscdialog.cpp
  selectdiscdialog.h
  selectdiscdialog.ui
  settingswindow.cpp
  settingswindow.h
  settingswindow.ui
  settingwidgetbinder.h
  setupwizarddialog.cpp
  setupwizarddialog.h
  setupwizarddialog.ui
  svgiconengine.cpp
  svgiconengine.h
  svgwidget.cpp
  svgwidget.h
  texturereplacementsettingsdialog.ui
  togglebutton.cpp
  togglebutton.h
)

set(TS_FILES
  translations/duckstation-qt_az.ts
  translations/duckstation-qt_de.ts
  translations/duckstation-qt_en.ts
  translations/duckstation-qt_es.ts
  translations/duckstation-qt_es-ES.ts
  translations/duckstation-qt_fr.ts
  translations/duckstation-qt_he.ts
  translations/duckstation-qt_id.ts
  translations/duckstation-qt_it.ts
  translations/duckstation-qt_ja.ts
  translations/duckstation-qt_ko.ts
  translations/duckstation-qt_nl.ts
  translations/duckstation-qt_pl.ts
  translations/duckstation-qt_pt-BR.ts
  translations/duckstation-qt_pt-PT.ts
  translations/duckstation-qt_ru.ts
  translations/duckstation-qt_sv.ts
  translations/duckstation-qt_tr.ts
  translations/duckstation-qt_zh-CN.ts
)

add_executable(duckstation-qt ${SRCS} ${QM_FILES})
target_precompile_headers(duckstation-qt PRIVATE "pch.h")
target_include_directories(duckstation-qt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(duckstation-qt PRIVATE core common imgui minizip scmversion Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Widgets)

if(LINUX)
  target_link_libraries(duckstation-qt PRIVATE Qt6::DBus)
endif()

# Our Qt builds may have exceptions on, so force them off.
target_compile_definitions(duckstation-qt PRIVATE QT_NO_EXCEPTIONS QT_NO_SIGNALS_SLOTS_KEYWORDS)

# We need to override QT_STATICPLUGIN to force our SVG plugin to be statically linked to the exectuable.
# Prevent the force include of the pch from interfering.
set_source_files_properties("svgiconengine.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)

add_core_resources(duckstation-qt)

# Automatically generate a list of .ui calls and call qt_wrap_ui() to generate targets.
# This is needed because otherwise CMake rebuilds all source files if any unrelated files change.
# See https://gitlab.kitware.com/cmake/cmake/-/issues/22531 and https://gitlab.kitware.com/cmake/cmake/-/issues/16776
# Documentation says to use qt_add_ui(), but this generates a separate directory for every file.
# I'm concerned about arg length limits, so I manually invoke qt_wrap_ui().
# Also see https://github.com/thorntonryan/autouic_example.
set(UI_FILES "")
foreach(src ${SRCS})
  if(src MATCHES "\\.ui$")
    list(APPEND UI_FILES ${src})
  endif()
endforeach()
qt_wrap_ui(UI_SRCS SOURCES ${UI_FILES})
target_sources(duckstation-qt PRIVATE ${UI_SRCS})
target_include_directories(duckstation-qt PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>")

if(APPLE)
  # Don't generate a bundle for XCode, it makes code signing fail...
  get_scm_version()
  set(BUNDLE_PATH ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DuckStation.app)
  set_target_properties(duckstation-qt PROPERTIES
    MACOSX_BUNDLE true
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    OUTPUT_NAME DuckStation
  )

  if(CMAKE_GENERATOR MATCHES "Xcode")
    set_target_properties(duckstation-qt PROPERTIES
      XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
      XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO
      XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
    )
  elseif(NOT SKIP_POSTPROCESS_BUNDLE)
    # Inject Qt Libraries into bundle.
    find_program(MACDEPLOYQT_EXE macdeployqt HINTS "${QT_BINARY_DIRECTORY}")
    add_custom_target(duckstation-postprocess-bundle ALL
      COMMAND "${MACDEPLOYQT_EXE}" "${BUNDLE_PATH}" -no-strip
    )
    add_dependencies(duckstation-postprocess-bundle duckstation-qt)
  endif()

  # Copy icon into the bundle
  target_sources(duckstation-qt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/DuckStation.icns")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/DuckStation.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

# Compile qrc to a binary file.
if(NOT APPLE)
  set(RCC_FILE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/resources/duckstation-qt.rcc")
  qt_add_binary_resources(duckstation-qt-rcc resources/duckstation-qt.qrc DESTINATION ${RCC_FILE} OPTIONS -no-compress)
  add_dependencies(duckstation-qt duckstation-qt-rcc)

  # Need to ensure the resources directory exists, it might not. Happens when low CPU count and parallel builds.
  add_custom_target(duckstation-qt-rcc-mkdir COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/resources")
  add_dependencies(duckstation-qt-rcc duckstation-qt-rcc-mkdir)
else()
  set(RCC_FILE "${CMAKE_CURRENT_BINARY_DIR}/duckstation-qt.rcc")
  qt_add_binary_resources(duckstation-qt-rcc resources/duckstation-qt.qrc DESTINATION ${RCC_FILE} OPTIONS -no-compress)
  add_dependencies(duckstation-qt duckstation-qt-rcc)
  target_sources(duckstation-qt PRIVATE ${RCC_FILE})
  set_source_files_properties(${RCC_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()


# Translation setup.
qt_add_lrelease(duckstation-qt TS_FILES ${TS_FILES} QM_FILES_OUTPUT_VARIABLE QM_FILES)
if(NOT APPLE)
  set(QM_OUTPUT_DIR "$<TARGET_FILE_DIR:duckstation-qt>/translations")
  add_custom_command(TARGET duckstation-qt POST_BUILD COMMAND "${CMAKE_COMMAND}" -E make_directory "${QM_OUTPUT_DIR}")
  foreach (QM_FILE IN LISTS QM_FILES)
    get_filename_component(QM_FILE_NAME ${QM_FILE} NAME)
    add_custom_command(TARGET duckstation-qt POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${QM_FILE}" "${QM_OUTPUT_DIR}/${QM_FILE_NAME}")
  endforeach()
else()
  foreach (QM_FILE IN LISTS QM_FILES)
    target_sources(duckstation-qt PRIVATE ${QM_FILE})
    set_source_files_properties(${QM_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/translations)
  endforeach()
endif()
copy_base_translations(duckstation-qt)
