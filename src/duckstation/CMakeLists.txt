add_executable(duckstation
  icon.cpp
  main.cpp
  opengl_host_display.cpp
  opengl_host_display.h
  sdl_audio_stream.cpp
  sdl_audio_stream.h
  sdl_host_interface.cpp
  sdl_host_interface.h
)

target_include_directories(duckstation PRIVATE "${SDL2_INCLUDE_DIRS}")
target_link_libraries(duckstation PRIVATE YBaseLib core common imgui nativefiledialog glad "${SDL2_LIBRARIES}")

if(WIN32)
  target_sources(duckstation PRIVATE
    d3d11_host_display.cpp
    d3d11_host_display.h
  )
  target_link_libraries(duckstation PRIVATE d3d11.lib)
endif()
