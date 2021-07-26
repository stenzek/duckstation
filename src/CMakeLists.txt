if(BUILD_LIBRETRO_CORE)
  add_definitions("-DLIBRETRO=1")
endif()

add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(scmversion)

if(NOT BUILD_LIBRETRO_CORE)
  add_subdirectory(frontend-common)
endif()

if(NOT BUILD_LIBRETRO_CORE AND NOT ANDROID)
  add_subdirectory(common-tests)
  if(WIN32)
    add_subdirectory(updater)
  endif()
endif()

if(BUILD_NOGUI_FRONTEND)
  add_subdirectory(duckstation-nogui)
endif()

if(BUILD_QT_FRONTEND)
  add_subdirectory(duckstation-qt)
endif()

if(BUILD_LIBRETRO_CORE)
  add_subdirectory(duckstation-libretro)
endif()

if(BUILD_REGTEST)
  add_subdirectory(duckstation-regtest)
endif()
