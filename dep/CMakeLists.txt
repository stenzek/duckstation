add_subdirectory(glad)
add_subdirectory(imgui)
add_subdirectory(libcue)
add_subdirectory(simpleini)
add_subdirectory(stb)
add_subdirectory(tinyxml2)

if(NOT ANDROID)
  add_subdirectory(nativefiledialog)
endif()

if(${CPU_ARCH} STREQUAL "aarch64")
  add_subdirectory(vixl)
endif()

