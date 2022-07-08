add_subdirectory(common)
add_subdirectory(util)
add_subdirectory(core)
add_subdirectory(scmversion)
add_subdirectory(frontend-common)

if(NOT ANDROID)
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

if(BUILD_REGTEST)
  add_subdirectory(duckstation-regtest)
endif()
