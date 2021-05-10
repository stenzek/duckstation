add_library(duckstation_libretro SHARED
  libretro_audio_stream.cpp
  libretro_audio_stream.h
  libretro_game_settings.cpp
  libretro_game_settings.h
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

target_link_libraries(duckstation_libretro PRIVATE core common glad scmversion frontend-common vulkan-loader libretro-common)

# no lib prefix
set_target_properties(duckstation_libretro PROPERTIES PREFIX "")

# drop in the build directory
set_target_properties(duckstation_libretro PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set_target_properties(duckstation_libretro PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# work around .sln issue of hardcoded config directories
if(WIN32)
  set_target_properties(duckstation_libretro PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}")
  set_target_properties(duckstation_libretro PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
endif()

# for android, suffix _android
if(ANDROID)
  set_target_properties(duckstation_libretro PROPERTIES OUTPUT_NAME "duckstation_libretro_android")
endif()
