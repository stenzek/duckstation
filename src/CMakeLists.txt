add_subdirectory(common)
add_subdirectory(core)

if(BUILD_SDL_FRONTEND)
  add_subdirectory(duckstation)
endif()

if(BUILD_QT_FRONTEND)
  add_subdirectory(duckstation-qt)
endif()

