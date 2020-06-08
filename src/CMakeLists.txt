add_subdirectory(common)
add_subdirectory(common-tests)
add_subdirectory(core)
add_subdirectory(scmversion)

if(ANDROID OR BUILD_SDL_FRONTEND OR BUILD_QT_FRONTEND)
  add_subdirectory(frontend-common)
endif()

if(BUILD_SDL_FRONTEND)
  add_subdirectory(duckstation-sdl)
endif()

if(BUILD_QT_FRONTEND)
  add_subdirectory(duckstation-qt)
endif()

if(BUILD_LIBRETRO_CORE)
  add_subdirectory(duckstation-libretro)
endif()

