add_executable(updater
  updater.cpp
  updater.h
)

target_link_libraries(updater PRIVATE common minizip zlib)

if(WIN32)
  target_sources(updater PRIVATE
    win32_main.cpp
  )
endif()
