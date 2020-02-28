add_library(frontend-common
  common_host_interface.cpp
  common_host_interface.h
  icon.cpp
  icon.h
  imgui_styles.cpp
  imgui_styles.h
  sdl_audio_stream.cpp
  sdl_audio_stream.h
  sdl_controller_interface.cpp
  sdl_controller_interface.h
  sdl_initializer.cpp
  sdl_initializer.h
)

target_include_directories(frontend-common PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(frontend-common PRIVATE core common imgui ${SDL2_LIBRARIES})

