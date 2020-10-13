set(SRCS
  android_host_interface.cpp
  android_host_interface.h
  android_settings_interface.cpp
  android_settings_interface.h
)

add_library(duckstation-native SHARED ${SRCS})
target_link_libraries(duckstation-native PRIVATE android frontend-common core common glad imgui)

find_package(OpenSLES)
if(OPENSLES_FOUND)
  message("Enabling OpenSL ES audio stream")
  target_sources(duckstation-native PRIVATE
    opensles_audio_stream.cpp
    opensles_audio_stream.h)
  target_link_libraries(duckstation-native PRIVATE OpenSLES::OpenSLES)
  target_compile_definitions(duckstation-native PRIVATE "-DUSE_OPENSLES=1")
endif()
