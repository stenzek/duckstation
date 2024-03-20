add_executable(duckstation-nogui
  nogui_host.cpp
  nogui_host.h
  nogui_platform.h
)

target_precompile_headers(duckstation-nogui PRIVATE "pch.h")
target_link_libraries(duckstation-nogui PRIVATE core util common imgui scmversion)

add_core_resources(duckstation-nogui)

if(WIN32)
  message(STATUS "Building Win32 NoGUI Platform.")
  target_sources(duckstation-nogui PRIVATE
    duckstation-nogui.manifest
    resource.h
    win32_nogui_platform.cpp
    win32_nogui_platform.h
  )

  # We want a Windows subsystem application not console.
  set_target_properties(duckstation-nogui PROPERTIES
    WIN32_EXECUTABLE TRUE
    DEBUG_POSTFIX "-debug")
endif()


if(APPLE)
  message(STATUS "Building Cocoa NoGUI Platform.")
  set(COCOA_SOURCES PRIVATE
    cocoa_key_names.h
    cocoa_nogui_platform.mm
    cocoa_nogui_platform.h
  )
  target_sources(duckstation-nogui PRIVATE ${COCOA_SOURCES})
  find_library(QUARTZCORE_LIBRARY QuartzCore)
  find_library(COCOA_LIBRARY Cocoa)
  target_link_libraries(duckstation-nogui PRIVATE ${QUARTZCORE_LIBRARY} ${COCOA_LIBRARY})
  set_source_files_properties(${COCOA_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
endif()

if(ENABLE_X11)
  message(STATUS "Building X11 NoGUI Platform.")
  target_compile_definitions(duckstation-nogui PRIVATE "NOGUI_PLATFORM_X11=1")
  target_sources(duckstation-nogui PRIVATE
    x11_nogui_platform.cpp
    x11_nogui_platform.h
  )
  target_include_directories(duckstation-nogui PRIVATE "${X11_INCLUDE_DIR}" "${X11_Xrandr_INCLUDE_PATH}")
  target_link_libraries(duckstation-nogui PRIVATE "${X11_LIBRARIES}" "${X11_Xrandr_LIB}")
endif()

if(ENABLE_WAYLAND)
  message(STATUS "Building Wayland NoGUI Platform.")
  find_package(ECM REQUIRED NO_MODULE)
  list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")
  find_package(Wayland REQUIRED Client)
  find_package(WaylandScanner REQUIRED)
  find_package(WaylandProtocols 1.15 REQUIRED)
  find_package(X11 REQUIRED)
  if (NOT X11_xkbcommon_FOUND)
    message(FATAL_ERROR "XKBCommon is required.")
  endif()

  target_compile_definitions(duckstation-nogui PRIVATE "NOGUI_PLATFORM_WAYLAND=1")
  target_sources(duckstation-nogui PRIVATE
    wayland_nogui_platform.cpp
    wayland_nogui_platform.h
  )

  # Required protocols.
  ecm_add_wayland_client_protocol(WAYLAND_PROTOCOL_SRCS
    PROTOCOL "${WaylandProtocols_DATADIR}/stable/xdg-shell/xdg-shell.xml"
    BASENAME xdg-shell)
  ecm_add_wayland_client_protocol(WAYLAND_PROTOCOL_SRCS
    PROTOCOL "${WaylandProtocols_DATADIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
    BASENAME xdg-decoration)
  target_sources(duckstation-nogui PRIVATE ${WAYLAND_PROTOCOL_SRCS})
  target_include_directories(duckstation-nogui PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

  # Disable PCH for protocol files, because they're C, and our PCH above is C++.
  set_source_files_properties(${WAYLAND_PROTOCOL_SRCS} PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)

  target_link_libraries(duckstation-nogui PRIVATE
    Wayland::Client
    X11::xkbcommon
  )
endif()

if(ENABLE_SDL2)
  message(STATUS "Building SDL NoGUI Platform.")
  target_sources(duckstation-nogui PRIVATE
    sdl_nogui_platform.cpp
    sdl_nogui_platform.h
  )
  target_link_libraries(duckstation-nogui PUBLIC SDL2::SDL2)
endif()
