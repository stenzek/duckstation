add_subdirectory(common)
add_subdirectory(core)

if(NOT ANDROID)
  add_subdirectory(duckstation)
endif()
