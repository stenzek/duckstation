add_executable(duckstation-sdl
  main.cpp
)

if(WIN32)
  target_sources(duckstation-sdl PRIVATE
    duckstation-sdl.manifest
  )

  # We want a Windows subsystem application not console.
  set_target_properties(duckstation-sdl PROPERTIES
    WIN32_EXECUTABLE TRUE
    DEBUG_POSTFIX "-debug")
endif()

