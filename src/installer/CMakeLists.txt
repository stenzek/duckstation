add_executable(installer
  installer.cpp
  installer.h
  installer.manifest
  installer.rc
  installer_params.h
  installer_ui.cpp
  installer_ui.h
  resource.h
  ../updater/updater_progress_callback.h
  ../updater/win32_progress_callback.cpp
  ../updater/win32_progress_callback.h
  ../updater/win32_window_util.cpp
  ../updater/win32_window_util.h
  ../duckstation-qt/vcruntimecheck.cpp
)

target_include_directories(installer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(installer PRIVATE common fmt lzma)
target_link_libraries(installer PRIVATE "Comctl32.lib")
set_target_properties(installer PROPERTIES WIN32_EXECUTABLE TRUE)
