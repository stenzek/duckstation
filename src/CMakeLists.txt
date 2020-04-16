add_subdirectory(common)
add_subdirectory(common-tests)
add_subdirectory(core)
add_subdirectory(frontend-common)
add_subdirectory(scmversion)

if(BUILD_SDL_FRONTEND)
  add_subdirectory(duckstation-sdl)
endif()

if(BUILD_QT_FRONTEND)
  add_subdirectory(duckstation-qt)
endif()

