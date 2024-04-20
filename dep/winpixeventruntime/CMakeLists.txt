add_library(WinPixEventRuntime::WinPixEventRuntime SHARED IMPORTED GLOBAL)

if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm64")
  set_target_properties(WinPixEventRuntime::WinPixEventRuntime PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/binarm64/WinPixEventRuntime.dll"
    IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/libarm64/WinPixEventRuntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )
else()
  set_target_properties(WinPixEventRuntime::WinPixEventRuntime PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/bin64/WinPixEventRuntime.dll"
    IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/lib64/WinPixEventRuntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )
endif()
