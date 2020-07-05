add_library(duckstation_libretro SHARED
  libretro_audio_stream.cpp
  libretro_audio_stream.h
  libretro_host_display.cpp
  libretro_host_display.h
  libretro_host_interface.cpp
  libretro_host_interface.h
  libretro_opengl_host_display.cpp
  libretro_opengl_host_display.h
  libretro_settings_interface.cpp
  libretro_settings_interface.h
  libretro_vulkan_host_display.cpp
  libretro_vulkan_host_display.h
  main.cpp
)

if(WIN32)
  target_sources(duckstation_libretro PRIVATE
    libretro_d3d11_host_display.cpp
    libretro_d3d11_host_display.h
  )
endif()

target_link_libraries(duckstation_libretro PRIVATE core common imgui glad scmversion frontend-common vulkan-loader libretro-common)

# no lib prefix
set_target_properties(duckstation_libretro PROPERTIES PREFIX "")

