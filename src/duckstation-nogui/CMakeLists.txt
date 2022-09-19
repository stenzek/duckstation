add_executable(duckstation-nogui
  nogui_host.cpp
  nogui_host.h
  nogui_platform.h
)

target_link_libraries(duckstation-nogui PRIVATE core util common imgui glad frontend-common scmversion)

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

if(USE_X11)
  message(STATUS "Building X11 NoGUI Platform.")
  target_compile_definitions(duckstation-nogui PRIVATE "NOGUI_PLATFORM_X11=1")
  target_sources(duckstation-nogui PRIVATE
    x11_nogui_platform.cpp
    x11_nogui_platform.h
  )
  target_include_directories(duckstation-nogui PRIVATE "${X11_INCLUDE_DIR}" "${X11_Xrandr_INCLUDE_PATH}")
  target_link_libraries(duckstation-nogui PRIVATE "${X11_LIBRARIES}" "${X11_Xrandr_LIB}")
endif()

if(USE_WAYLAND)
  message(STATUS "Building Wayland NoGUI Platform.")
  find_package(ECM REQUIRED NO_MODULE)
  list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")

  find_package(Wayland REQUIRED Client)
  find_package(WaylandScanner REQUIRED)
  find_package(WaylandProtocols 1.15 REQUIRED)
  find_package(XKBCommon REQUIRED)

  target_compile_definitions(duckstation-nogui PRIVATE "NOGUI_PLATFORM_WAYLAND=1")
  target_sources(duckstation-nogui PRIVATE
    wayland_nogui_platform.cpp
    wayland_nogui_platform.h
  )

  # Generate the xdg-shell and xdg-decoration protocols at build-time.
  # Because these are C, not C++, we have to put them in their own library, otherwise
  # cmake tries to generate a C PCH as well as the C++ one...
  ecm_add_wayland_client_protocol(WAYLAND_PLATFORM_SRCS
    PROTOCOL "${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/xdg-shell/xdg-shell.xml"
    BASENAME xdg-shell)
  ecm_add_wayland_client_protocol(WAYLAND_PLATFORM_SRCS
    PROTOCOL "${WAYLAND_PROTOCOLS_PKGDATADIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
    BASENAME xdg-decoration)
  add_library(duckstation-nogui-wayland-protocols STATIC ${WAYLAND_PLATFORM_SRCS})
  target_include_directories(duckstation-nogui-wayland-protocols PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

  target_link_libraries(duckstation-nogui PRIVATE
    duckstation-nogui-wayland-protocols
    Wayland::Client
    XKBCommon::XKBCommon
  )
endif()

if(USE_DRMKMS AND USE_EVDEV)
  message(STATUS "Building VTY/DRM/KMS/EVDev NoGUI Platform.")
  target_compile_definitions(duckstation-nogui PRIVATE "NOGUI_PLATFORM_VTY=1" "WITH_DRMKMS=1")
  target_sources(duckstation-nogui PRIVATE 
    vty_key_names.h
    vty_nogui_platform.cpp
    vty_nogui_platform.h
  )
  target_include_directories(duckstation-nogui PRIVATE ${LIBEVDEV_INCLUDE_DIRS})
  target_link_libraries(duckstation-nogui PRIVATE ${LIBEVDEV_LIBRARIES})
endif()
