message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")

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

  if(INSTALL_SELF_CONTAINED)
    message(STATUS "Creating self-contained install at ${CMAKE_INSTALL_PREFIX}")
  else()
    message(STATUS "Creating relative install at ${CMAKE_INSTALL_PREFIX}")
    message(STATUS "  CMAKE_INSTALL_BINDIR: ${CMAKE_INSTALL_BINDIR}")
  endif()
endif()

if(DEFINED HOST_MIN_PAGE_SIZE AND DEFINED HOST_MAX_PAGE_SIZE)
  message(STATUS "Building with a dynamic page size of ${HOST_MIN_PAGE_SIZE} - ${HOST_MAX_PAGE_SIZE} bytes.")
elseif(DEFINED HOST_PAGE_SIZE)
  message(STATUS "Building with detected page size of ${HOST_PAGE_SIZE}")
endif()
if(DEFINED HOST_CACHE_LINE_SIZE)
  message(STATUS "Building with detected cache line size of ${HOST_CACHE_LINE_SIZE}")
endif()

if(NOT IS_SUPPORTED_COMPILER)
  message(WARNING "*************** UNSUPPORTED CONFIGURATION ***************
You are not compiling DuckStation with a supported compiler.
It may not even build successfully.
DuckStation only supports the Clang and MSVC compilers.
No support will be provided, continue at your own risk.
*********************************************************")
endif()

if(WIN32)
  message(WARNING "*************** UNSUPPORTED CONFIGURATION ***************
You are compiling DuckStation with CMake on Windows.
It may not even build successfully.
DuckStation only supports MSBuild on Windows.
No support will be provided, continue at your own risk.
*********************************************************")
endif()

if(CPU_ARCH_X64 AND DISABLE_SSE4)
  message(WARNING "*********************** WARNING ***********************
SSE4 instructions are disabled. This will result in
reduced performance. You should not enable this option
unless you have a pre-2008 CPU.
*******************************************************")
endif()
