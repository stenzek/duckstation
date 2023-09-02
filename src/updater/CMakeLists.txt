add_executable(updater
  updater.cpp
  updater.h
)

target_link_libraries(updater PRIVATE common minizip zlib)

if(WIN32)
  target_sources(updater PRIVATE
    win32_main.cpp
    win32_progress_callback.cpp
    win32_progress_callback.h
  )
  target_link_libraries(updater PRIVATE "Comctl32.lib")
  set_target_properties(updater PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
