add_executable(duckstation-nogui
  imgui_impl_sdl.cpp
  imgui_impl_sdl.h
  main.cpp
  sdl_host_interface.cpp
  sdl_host_interface.h
  sdl_key_names.h
  sdl_util.cpp
  sdl_util.h
)

target_include_directories(duckstation-nogui PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(duckstation-nogui PRIVATE core common imgui glad frontend-common scmversion vulkan-loader ${SDL2_LIBRARIES})

if(WIN32)
  target_sources(duckstation-nogui PRIVATE
    duckstation-nogui.manifest
  )

  # We want a Windows subsystem application not console.
  set_target_properties(duckstation-nogui PROPERTIES
    WIN32_EXECUTABLE TRUE
    DEBUG_POSTFIX "-debug")
endif()

