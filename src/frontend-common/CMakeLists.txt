add_library(frontend-common
  game_settings.cpp
  game_settings.h
  opengl_host_display.cpp
  opengl_host_display.h
  postprocessing_chain.cpp
  postprocessing_chain.h
  postprocessing_shader.cpp
  postprocessing_shader.h
  postprocessing_shadergen.cpp
  postprocessing_shadergen.h
  vulkan_host_display.cpp
  vulkan_host_display.h
)

target_link_libraries(frontend-common PUBLIC core common glad vulkan-loader)

if(WIN32)
  target_sources(frontend-common PRIVATE
    d3d11_host_display.cpp
    d3d11_host_display.h
  )
  target_link_libraries(frontend-common PRIVATE d3d11.lib dxgi.lib)
endif()

if(NOT BUILD_LIBRETRO_CORE)
  target_sources(frontend-common PRIVATE
    imgui_impl_opengl3.h
    imgui_impl_opengl3.cpp
  )

  target_sources(frontend-common PRIVATE
    imgui_impl_vulkan.h
    imgui_impl_vulkan.cpp
  )

  if(WIN32)
    target_sources(frontend-common PRIVATE
      imgui_impl_dx11.h
      imgui_impl_dx11.cpp
    )
  endif()

  target_sources(frontend-common PRIVATE
    common_host_interface.cpp
    common_host_interface.h
    controller_interface.cpp
    controller_interface.h
    cubeb_audio_stream.cpp
    cubeb_audio_stream.h
    game_list.cpp
    game_list.h
    icon.cpp
    icon.h
    imgui_styles.cpp
    imgui_styles.h
    ini_settings_interface.cpp
    ini_settings_interface.h
    save_state_selector_ui.cpp
    save_state_selector_ui.h
  )

  if(WIN32)
    target_sources(frontend-common PRIVATE
      dinput_controller_interface.cpp
      dinput_controller_interface.h
      xinput_controller_interface.cpp
      xinput_controller_interface.h
    )
  endif()

  target_compile_definitions(frontend-common PRIVATE "WITH_IMGUI=1")
  target_link_libraries(frontend-common PUBLIC cubeb imgui simpleini tinyxml2 scmversion)

  if(SDL2_FOUND)
    target_sources(frontend-common PRIVATE 
      sdl_audio_stream.cpp
      sdl_audio_stream.h
      sdl_controller_interface.cpp
      sdl_controller_interface.h
      sdl_initializer.cpp
      sdl_initializer.h
    )
    target_compile_definitions(frontend-common PUBLIC "WITH_SDL2=1")
    target_include_directories(frontend-common PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(frontend-common PRIVATE ${SDL2_LIBRARIES})

    # Copy bundled SDL2 to output on Windows.
    if(WIN32)
      add_custom_command(TARGET frontend-common POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDL2_DLL_PATH}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/SDL2.dll")
    endif()
  endif()

  if(ENABLE_DISCORD_PRESENCE AND NOT BUILD_LIBRETRO_CORE)
    target_compile_definitions(frontend-common PUBLIC -DWITH_DISCORD_PRESENCE=1)
    target_link_libraries(frontend-common PRIVATE discord-rpc)
  endif()

  # Copy the provided data directory to the output directory.
  add_custom_command(TARGET frontend-common POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
  )
endif()

