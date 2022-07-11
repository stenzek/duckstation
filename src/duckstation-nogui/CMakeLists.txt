add_executable(duckstation-nogui
  main.cpp
  nogui_host_interface.cpp
  nogui_host_interface.h
)

target_link_libraries(duckstation-nogui PRIVATE core common imgui glad frontend-common scmversion)

if(USE_SDL2)
  target_sources(duckstation-nogui PRIVATE 
    sdl_host_interface.cpp
    sdl_host_interface.h
    sdl_key_names.h
  )
  target_include_directories(duckstation-nogui PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(duckstation-nogui PRIVATE ${SDL2_LIBRARIES})
endif()

if(USE_EVDEV)
  target_sources(duckstation-nogui PRIVATE 
    vty_host_interface.cpp
    vty_host_interface.h
  )
  target_compile_definitions(duckstation-nogui PRIVATE "-DWITH_VTY=1")
  target_compile_definitions(duckstation-nogui PRIVATE "-DUSE_LIBEVDEV=1")
  target_include_directories(duckstation-nogui PRIVATE ${LIBEVDEV_INCLUDE_DIRS})
  target_link_libraries(duckstation-nogui PRIVATE ${LIBEVDEV_LIBRARIES})
endif()

if(USE_DRMKMS)
  target_compile_definitions(duckstation-nogui PRIVATE "-DWITH_DRMKMS=1")
endif()

if(WIN32)
  target_sources(duckstation-nogui PRIVATE
    duckstation-nogui.manifest
  )

  # We want a Windows subsystem application not console.
  set_target_properties(duckstation-nogui PROPERTIES
    WIN32_EXECUTABLE TRUE
    DEBUG_POSTFIX "-debug")
endif()

