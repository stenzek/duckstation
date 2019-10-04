set(SRCS
  main.cpp
  sdl_audio_mixer.cpp
  sdl_audio_mixer.h
  sdl_interface.cpp
  sdl_interface.h
)

add_executable(duckstation ${SRCS})
target_include_directories(duckstation PRIVATE "${SDL2_INCLUDE_DIRS}")
target_link_libraries(duckstation core imgui "${SDL2_LIBRARIES}")
