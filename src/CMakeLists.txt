add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(scmversion)

if(NOT BUILD_LIBRETRO_CORE)
  add_subdirectory(common-tests)
endif()

if(ANDROID OR BUILD_SDL_FRONTEND OR BUILD_QT_FRONTEND OR BUILD_LIBRETRO_CORE)
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

