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

# Refuse to build in hostile package environments. The code and build script licenses do not allow for
# packages, and I'm sick of dealing with people complaining about things broken by packagers, and then
# being attacked by package maintainers who violate their distribution's codes of conduct. Attempts to
# request removal of these packages have been unsuccessful, so we have to resort to this.
# NOTE: You do NOT have permission to distribute build scripts or patches that modify the build system
# without explicit permission from the copyright holder.
# DuckStation's code is public so it can be audited and learned from. Not to repackage.
# This is why we can't have nice things.
if(EXISTS /etc/os-release)
  file(READ /etc/os-release OS_RELEASE_CONTENT)
  if(OS_RELEASE_CONTENT MATCHES "ID=arch" OR OS_RELEASE_CONTENT MATCHES "ID_LIKE=arch" OR OS_RELEASE_CONTENT MATCHES "ID=nixos")
    message(FATAL_ERROR "Unsupported environment.")
  endif()
endif()
if(DEFINED ENV{NIX_BUILD_TOP} OR DEFINED ENV{NIX_STORE} OR DEFINED ENV{IN_NIX_SHELL} OR EXISTS "/etc/NIXOS")
  message(FATAL_ERROR "Unsupported environment.")
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
