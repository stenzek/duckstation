add_executable(duckstation-sdl
  imgui_impl_sdl.cpp
  imgui_impl_sdl.h
  main.cpp
  opengl_host_display.cpp
  opengl_host_display.h
  sdl_host_interface.cpp
  sdl_host_interface.h
)

target_include_directories(duckstation-sdl PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(duckstation-sdl PRIVATE core common imgui nativefiledialog glad frontend-common ${SDL2_LIBRARIES})

if(WIN32)
  target_sources(duckstation-sdl PRIVATE
    d3d11_host_display.cpp
    d3d11_host_display.h
    duckstation-sdl.manifest
  )
  target_link_libraries(duckstation-sdl PRIVATE d3d11.lib dxgi.lib ${SDL2MAIN_LIBRARIES})
endif()

