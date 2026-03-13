add_subdirectory(common)
add_subdirectory(util)
add_subdirectory(core)
add_subdirectory(scmversion)
add_subdirectory(duckstation-qt)

if(WIN32 OR APPLE)
  add_subdirectory(updater)
endif()

if(WIN32)
  add_subdirectory(installer)
  add_subdirectory(uninstaller)
endif()

if(BUILD_REGTEST)
  add_subdirectory(duckstation-regtest)
endif()

if(BUILD_TESTS)
  add_subdirectory(common-tests EXCLUDE_FROM_ALL)
  add_subdirectory(util-tests EXCLUDE_FROM_ALL)
endif()
