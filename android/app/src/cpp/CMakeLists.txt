set(SRCS
  android_audio_stream.cpp
  android_audio_stream.h
  android_host_interface.cpp
  android_host_interface.h
  android_gles2_host_display.cpp
  android_gles2_host_display.h
  main.cpp
)

add_library(duckstation-native SHARED ${SRCS})
target_link_libraries(duckstation-native PRIVATE android core common glad imgui EGL::EGL)
