find_package(Qt6 6.6.0 COMPONENTS Core Gui Widgets Network LinguistTools REQUIRED)

include(CopyBaseTranslations)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(SRCS
  resources/resources.qrc
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
  audioexpansionsettingsdialog.ui
  audiosettingswidget.cpp
  audiosettingswidget.h
  audiosettingswidget.ui
  audiostretchsettingsdialog.ui
  autoupdaterdialog.cpp
  autoupdaterdialog.h
  autoupdaterdialog.ui
  biossettingswidget.cpp
  biossettingswidget.h
  biossettingswidget.ui
  cheatcodeeditordialog.cpp
  cheatcodeeditordialog.h
  cheatcodeeditordialog.ui
  cheatmanagerwindow.cpp
  cheatmanagerwindow.h
  cheatmanagerwindow.ui
  collapsiblewidget.cpp
  collapsiblewidget.h
  colorpickerbutton.cpp
  colorpickerbutton.h
  consolesettingswidget.cpp
  consolesettingswidget.h
  consolesettingswidget.ui
  controllerbindingwidget_analog_controller.ui
  controllerbindingwidget_analog_joystick.ui
  controllerbindingwidget_digital_controller.ui
  controllerbindingwidget_guncon.ui
  controllerbindingwidget_mouse.ui
  controllerbindingwidget_negcon.ui
  controllerbindingwidget_negconrumble.ui
  controllerbindingwidgets.cpp
  controllerbindingwidgets.h
  controllerbindingwidget.ui
  controllerglobalsettingswidget.cpp
  controllerglobalsettingswidget.h
  controllerglobalsettingswidget.ui
  controllerledsettingsdialog.ui
  controllermacroeditwidget.ui
  controllermacrowidget.ui
  controllersettingswindow.cpp
  controllersettingswindow.h
  controllersettingswindow.ui
  controllersettingwidgetbinder.h
  coverdownloaddialog.cpp
  coverdownloaddialog.h
  coverdownloaddialog.ui
  debuggeraddbreakpointdialog.ui
  debuggermodels.cpp
  debuggermodels.h
  debuggerwindow.cpp
  debuggerwindow.h
  debuggerwindow.ui
  displaywidget.cpp
  displaywidget.h
  emptygamelistwidget.ui
  emulationsettingswidget.cpp
  emulationsettingswidget.h
  emulationsettingswidget.ui
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
  logwindow.cpp
  logwindow.h
  mainwindow.cpp
  mainwindow.h
  mainwindow.ui
  memorycardeditorwindow.cpp
  memorycardeditorwindow.h
  memorycardeditorwindow.ui
  memorycardsettingswidget.cpp
  memorycardsettingswidget.h
  memoryscannerwindow.cpp
  memoryscannerwindow.h
  memoryscannerwindow.ui
  memoryviewwidget.cpp
  memoryviewwidget.h
  postprocessingsettingswidget.cpp
  postprocessingsettingswidget.h
  postprocessingsettingswidget.ui
  qthost.cpp
  qthost.h
  qtkeycodes.cpp
  qtprogresscallback.cpp
  qtprogresscallback.h
  qttranslations.cpp
  qtutils.cpp
  qtutils.h
  resource.h
  settingswindow.cpp
  settingswindow.h
  settingswindow.ui
  settingwidgetbinder.h
  setupwizarddialog.cpp
  setupwizarddialog.h
  setupwizarddialog.ui
)

set(TS_FILES
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
  translations/duckstation-qt_tr.ts
  translations/duckstation-qt_zh-CN.ts
)

add_executable(duckstation-qt ${SRCS} ${QM_FILES})
target_precompile_headers(duckstation-qt PRIVATE "pch.h")
target_include_directories(duckstation-qt PRIVATE "${Qt6Gui_PRIVATE_INCLUDE_DIRS}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(duckstation-qt PRIVATE core common imgui minizip scmversion Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# Our Qt builds may have exceptions on, so force them off.
target_compile_definitions(duckstation-qt PRIVATE QT_NO_EXCEPTIONS)

add_core_resources(duckstation-qt)

if(WIN32)
  target_sources(duckstation-qt PRIVATE duckstation-qt.rc)

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
                                 --no-compiler-runtime
                                 --no-system-d3d-compiler
                                 --no-system-dxc-compiler
                                 --no-translations
                                 "$<TARGET_FILE:duckstation-qt>"
  )
  add_custom_command(TARGET duckstation-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/qt.conf.win" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/qt.conf"
  )
  #set_source_files_properties(${TS_FILES} PROPERTIES OUTPUT_LOCATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/translations")

  set(DEPS_TO_COPY freetype.dll harfbuzz.dll libjpeg.dll libpng16.dll libsharpyuv.dll libwebp.dll SDL2.dll shaderc_shared.dll zlib1.dll zstd.dll)
  foreach(DEP ${DEPS_TO_COPY})
    list(APPEND DEP_BINS "${CMAKE_PREFIX_PATH}/bin/${DEP}")
  endforeach()
  add_custom_command(TARGET duckstation-qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DEP_BINS} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  )
  if(CMAKE_BUILD_TYPE MATCHES "Debug")
    get_property(WINPIXEVENTRUNTIME_DLL TARGET WinPixEventRuntime::WinPixEventRuntime PROPERTY IMPORTED_LOCATION)
    message(STATUS WP "${WINPIXEVENTRUNTIME_DLL}")
    add_custom_command(TARGET duckstation-qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WINPIXEVENTRUNTIME_DLL}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    )
  endif()
elseif(APPLE)
  # Don't generate a bundle for XCode, it makes code signing fail...
  if(NOT CMAKE_GENERATOR MATCHES "Xcode")
    set(BUNDLE_PATH ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DuckStation.app)
      set_target_properties(duckstation-qt PROPERTIES
      MACOSX_BUNDLE true
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
      OUTPUT_NAME DuckStation
    )

    # Inject Qt Libraries into bundle.
    if(NOT SKIP_POSTPROCESS_BUNDLE)
      find_program(MACDEPLOYQT_EXE macdeployqt HINTS "${QT_BINARY_DIRECTORY}")
      add_custom_target(duckstation-postprocess-bundle ALL
        COMMAND "${MACDEPLOYQT_EXE}" "${BUNDLE_PATH}" -no-strip
      )
      add_dependencies(duckstation-postprocess-bundle duckstation-qt)
    endif()
  endif()

  # Copy icon into the bundle
  target_sources(duckstation-qt PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/DuckStation.icns")
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/DuckStation.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
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
