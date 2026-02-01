add_executable(installer
  installer.cpp
  installer.h
  installer.manifest
  installer.rc
  installer_params.h
  installer_ui.cpp
  installer_ui.h
  resource.h
  updater_progress_callback.h
  win32_progress_callback.cpp
  win32_progress_callback.h
  win32_window_util.h
)

target_include_directories(installer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(installer PRIVATE common fmt lzma)
target_link_libraries(installer PRIVATE "Comctl32.lib")
set_target_properties(installer PROPERTIES WIN32_EXECUTABLE TRUE)
