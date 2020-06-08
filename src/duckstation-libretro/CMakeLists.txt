add_library(duckstation-libretro SHARED
  libretro_audio_stream.cpp
  libretro_audio_stream.h
  libretro_host_display.cpp
  libretro_host_display.h
  libretro_host_interface.cpp
  libretro_host_interface.h
  libretro_settings_interface.cpp
  libretro_settings_interface.h
  main.cpp
  opengl_host_display.cpp
  opengl_host_display.h
)

if(WIN32)
  target_sources(duckstation-libretro PRIVATE
    d3d11_host_display.cpp
    d3d11_host_display.h
  )
endif()

target_link_libraries(duckstation-libretro PRIVATE core common imgui glad scmversion libretro-common)

