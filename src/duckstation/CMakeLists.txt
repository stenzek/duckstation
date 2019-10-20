set(SRCS
  icon.cpp
  main.cpp
  sdl_audio_stream.cpp
  sdl_audio_stream.h
  sdl_interface.cpp
  sdl_interface.h
)

add_executable(duckstation ${SRCS})
target_include_directories(duckstation PRIVATE "${SDL2_INCLUDE_DIRS}")
target_link_libraries(duckstation core imgui nativefiledialog "${SDL2_LIBRARIES}")
