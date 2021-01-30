add_definitions("-DWITH_IMGUI=1")

add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(scmversion)

add_subdirectory(common-tests)
if(WIN32)
  add_subdirectory(updater)
endif()

if(ANDROID OR BUILD_SDL_FRONTEND OR BUILD_QT_FRONTEND)
  add_subdirectory(frontend-common)
endif()

if(BUILD_SDL_FRONTEND)
  add_subdirectory(duckstation-sdl)
endif()

if(BUILD_QT_FRONTEND)
  add_subdirectory(duckstation-qt)
endif()
