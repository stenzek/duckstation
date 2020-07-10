set(SRCS
  android_host_interface.cpp
  android_host_interface.h
  android_settings_interface.cpp
  android_settings_interface.h
  main.cpp
)

add_library(duckstation-native SHARED ${SRCS})
target_link_libraries(duckstation-native PRIVATE android frontend-common core common glad imgui)
