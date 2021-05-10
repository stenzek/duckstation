if(BUILD_LIBRETRO_CORE)
  add_definitions("-DLIBRETRO=1")
endif()

add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(scmversion)

if(NOT BUILD_LIBRETRO_CORE)
  add_subdirectory(common-tests)
  if(WIN32)
    add_subdirectory(updater)
  endif()
endif()

add_subdirectory(frontend-common)

if(BUILD_NOGUI_FRONTEND)
  add_subdirectory(duckstation-nogui)
endif()

if(BUILD_QT_FRONTEND)
  add_subdirectory(duckstation-qt)
endif()

if(BUILD_LIBRETRO_CORE)
  add_subdirectory(duckstation-libretro)
endif()

