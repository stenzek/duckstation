add_executable(duckstation-mini
  mini_host.cpp
)

target_include_directories(duckstation-mini PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(duckstation-mini PRIVATE core util common imgui scmversion SDL3::SDL3)

add_core_resources(duckstation-mini)

if(WIN32)
  target_sources(duckstation-mini PRIVATE
    duckstation-mini.manifest
    resource.h
  )

  # We want a Windows subsystem application not console.
  set_target_properties(duckstation-mini PROPERTIES
    WIN32_EXECUTABLE TRUE
    DEBUG_POSTFIX "-debug")
endif()
