add_executable(uninstaller
  main.cpp
  resource.h
  uninstaller.manifest
  uninstaller.rc
)

target_include_directories(uninstaller PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(uninstaller PRIVATE common fmt)
target_link_libraries(uninstaller PRIVATE "Comctl32.lib")
set_target_properties(uninstaller PROPERTIES WIN32_EXECUTABLE TRUE)
