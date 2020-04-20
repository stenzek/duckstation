add_library(frontend-common
  common_host_interface.cpp
  common_host_interface.h
  controller_interface.cpp
  controller_interface.h
  icon.cpp
  icon.h
  imgui_styles.cpp
  imgui_styles.h
  ini_settings_interface.cpp
  ini_settings_interface.h
  save_state_selector_ui.cpp
  save_state_selector_ui.h
)

if(SDL2_FOUND)
  target_sources(frontend-common PRIVATE 
    sdl_audio_stream.cpp
    sdl_audio_stream.h
    sdl_controller_interface.cpp
    sdl_controller_interface.h
    sdl_initializer.cpp
    sdl_initializer.h
  )
  target_compile_definitions(frontend-common PRIVATE "WITH_SDL2=1")
  target_include_directories(frontend-common PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(frontend-common PRIVATE ${SDL2_LIBRARIES})
endif()

target_link_libraries(frontend-common PUBLIC core common imgui simpleini scmversion)

