set(SRCS
  main.cpp
  sdl_audio_mixer.cpp
  sdl_audio_mixer.h
  sdl_interface.cpp
  sdl_interface.h
)

add_executable(pse-sdl ${SRCS})
target_include_directories(pse-sdl PRIVATE "${SDL2_INCLUDE_DIRS}")
target_link_libraries(pse-sdl pse imgui "${SDL2_LIBRARIES}")
