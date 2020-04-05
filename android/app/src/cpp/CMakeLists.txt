set(SRCS
  android_host_interface.cpp
  android_host_interface.h
  android_gles_host_display.cpp
  android_gles_host_display.h
  main.cpp
)

add_library(duckstation-native SHARED ${SRCS})
target_link_libraries(duckstation-native PRIVATE android frontend-common core common glad imgui EGL::EGL)
