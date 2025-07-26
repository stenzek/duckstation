# Renderer options.
option(ENABLE_OPENGL "Build with OpenGL renderer" ON)
option(ENABLE_VULKAN "Build with Vulkan renderer" ON)
option(BUILD_QT_FRONTEND "Build the Qt frontend" ON)
option(BUILD_MINI_FRONTEND "Build the Mini frontend" OFF)
option(BUILD_REGTEST "Build regression test runner" OFF)
option(BUILD_TESTS "Build unit tests" OFF)
option(DISABLE_SSE4 "Build with SSE4 instructions disabled, reduces performance" OFF)

if(LINUX OR BSD)
  option(ENABLE_X11 "Support X11 window system" ON)
  option(ENABLE_WAYLAND "Support Wayland window system" ON)
  option(ALLOW_INSTALL "Allow installation to CMAKE_INSTALL_PREFIX" OFF)
endif()
if(APPLE)
  option(SKIP_POSTPROCESS_BUNDLE "Disable bundle post-processing, including Qt additions" OFF)
endif()

# Set _DEBUG macro for Debug builds.
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -D_DEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG")

# Create the Devel build type based on RelWithDebInfo.
set(CMAKE_C_FLAGS_DEVEL "${CMAKE_C_FLAGS_RELWITHDEBINFO} -D_DEVEL" CACHE STRING "Flags used by the C compiler during DEVEL builds." FORCE)
set(CMAKE_CXX_FLAGS_DEVEL "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -D_DEVEL" CACHE STRING "Flags used by the CXX compiler during DEVEL builds." FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DEVEL "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING "Flags used for the linker during DEVEL builds." FORCE)
set(CMAKE_MODULE_LINKER_FLAGS_DEVEL "${CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING "Flags used by the linker during the creation of modules during DEVEL builds." FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DEVEL "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING "Flags used by the linker during the creation of shared libraries during DEVEL builds." FORCE)
set(CMAKE_STATIC_LINKER_FLAGS_DEVEL "${CMAKE_STATIC_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING "Flags used by the linker during the creation of static libraries during DEVEL builds." FORCE)
list(APPEND CMAKE_CONFIGURATION_TYPES "Devel")
mark_as_advanced(CMAKE_C_FLAGS_DEVEL CMAKE_CXX_FLAGS_DEVEL CMAKE_EXE_LINKER_FLAGS_DEVEL CMAKE_MODULE_LINKER_FLAGS_DEVEL CMAKE_SHARED_LINKER_FLAGS_DEVEL CMAKE_STATIC_LINKER_FLAGS_DEVEL)
