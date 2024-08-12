if(ENABLE_OPENGL)
  message(STATUS "Building with OpenGL support.")
endif()
if(ENABLE_VULKAN)
  message(STATUS "Building with Vulkan support.")
endif()
if(ENABLE_X11)
  message(STATUS "Building with X11 support.")
endif()
if(ENABLE_WAYLAND)
  message(STATUS "Building with Wayland support.")
endif()

if(BUILD_QT_FRONTEND)
  message(STATUS "Building Qt frontend.")
endif()
if(BUILD_NOGUI_FRONTEND)
  message(STATUS "Building NoGUI frontend.")
endif()
if(BUILD_REGTEST)
  message(STATUS "Building RegTest frontend.")
endif()
if(BUILD_TESTS)
  message(STATUS "Building unit tests.")
endif()

if(ALLOW_INSTALL)
  message(WARNING "Install target is enabled. This will install all DuckStation files into:
  ${CMAKE_INSTALL_PREFIX}
It does **not** use the LSB subdirectories of bin, share, etc, so you should disable this option if it is set to /usr or /usr/local.")
endif()

if(NOT IS_SUPPORTED_COMPILER)
  message(WARNING "
*************** UNSUPPORTED CONFIGURATION ***************
You are not compiling DuckStation with a supported compiler.
It may not even build successfully.
DuckStation only supports the Clang and MSVC compilers.
No support will be provided, continue at your own risk.
*********************************************************")
endif()

if(WIN32)
  message(WARNING "
*************** UNSUPPORTED CONFIGURATION ***************
You are compiling DuckStation with CMake on Windows.
It may not even build successfully.
DuckStation only supports MSBuild on Windows.
No support will be provided, continue at your own risk.
*********************************************************")
endif()
