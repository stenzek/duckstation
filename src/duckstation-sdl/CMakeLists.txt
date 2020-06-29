add_executable(duckstation-sdl
  imgui_impl_sdl.cpp
  imgui_impl_sdl.h
  main.cpp
  sdl_host_interface.cpp
  sdl_host_interface.h
  sdl_key_names.h
  sdl_util.cpp
  sdl_util.h
)

target_include_directories(duckstation-sdl PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(duckstation-sdl PRIVATE core common imgui nativefiledialog glad frontend-common scmversion vulkan-loader ${SDL2_LIBRARIES})

if(WIN32)
  target_sources(duckstation-sdl PRIVATE
    duckstation-sdl.manifest
  )

  # We want a Windows subsystem application not console.
  set_target_properties(duckstation-sdl PROPERTIES
    WIN32_EXECUTABLE TRUE
    DEBUG_POSTFIX "-debug")
endif()

