cmake_minimum_required(VERSION 3.16)
project(duckstation C CXX)

# Policy settings.
cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

message("CMake Version: ${CMAKE_VERSION}")
message("CMake System Name: ${CMAKE_SYSTEM_NAME}")
message("Build Type: ${CMAKE_BUILD_TYPE}")

# Pull in modules.
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMakeModules/")
include(DuckStationUtils)

# Platform detection.
if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  set(LINUX TRUE)
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD")
  set(FREEBSD TRUE)
endif()

# Renderer options.
option(ENABLE_OPENGL "Build with OpenGL renderer" ON)
option(ENABLE_VULKAN "Build with Vulkan renderer" ON)

# Global options.
if(NOT ANDROID)
  option(BUILD_NOGUI_FRONTEND "Build the NoGUI frontend" OFF)
  option(BUILD_QT_FRONTEND "Build the Qt frontend" ON)
  option(BUILD_REGTEST "Build regression test runner" OFF)
  option(BUILD_TESTS "Build unit tests" OFF)

  set(ENABLE_CUBEB ON)
  set(ENABLE_DISCORD_PRESENCE ON)
  set(ENABLE_SDL2 ON)

  if(LINUX OR FREEBSD)
    option(ENABLE_X11 "Support X11 window system" ON)
    option(ENABLE_WAYLAND "Support Wayland window system" ON)
  endif()
  if(APPLE)
    option(SKIP_POSTPROCESS_BUNDLE "Disable bundle post-processing, including Qt additions" OFF)
  endif()
endif()


# Required libraries.
if(ENABLE_SDL2)
  find_package(SDL2 2.28.5 REQUIRED)
endif()
if(NOT WIN32 AND NOT ANDROID)
  find_package(CURL REQUIRED)
  if(NOT APPLE)
    find_package(Libbacktrace)
    if(NOT LIBBACKTRACE_FOUND)
      message(WARNING "libbacktrace not found, crashes will not produce backtraces.")
    endif()
  endif()
endif()
if(BUILD_QT_FRONTEND)
  find_package(Qt6 6.5.3 COMPONENTS Core Gui Widgets Network LinguistTools REQUIRED)
endif()


# Everything except Windows/Mac use EGL.
if(ENABLE_OPENGL AND (LINUX OR FREEBSD OR ANDROID))
  find_package(EGL REQUIRED)
  set(ENABLE_EGL TRUE)
endif()


if(ENABLE_X11)
  find_package(X11 REQUIRED)
  if (NOT X11_Xrandr_FOUND)
    message(FATAL_ERROR "XRandR extension is required")
  endif()
endif()
if(ENABLE_WAYLAND)
  message(STATUS "Wayland support enabled")
endif()


# Set _DEBUG macro for Debug builds.
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -D_DEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -D_DEBUG")


# Release build optimizations for MSVC.
if(MSVC)
  add_definitions("/D_CRT_SECURE_NO_WARNINGS")
  foreach(config CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
    # Set warning level 3 instead of 4.
    string(REPLACE "/W3" "/W4" ${config} "${${config}}")

    # Enable intrinsic functions, disable minimal rebuild, UTF-8 source, set __cplusplus version.
    set(${config} "${${config}} /Oi /Gm- /utf-8 /Zc:__cplusplus")
  endforeach()

  # RelWithDebInfo is set to Ob1 instead of Ob2.   
  string(REPLACE "/Ob1" "/Ob2" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
  string(REPLACE "/Ob1" "/Ob2" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")

  # Disable incremental linking in RelWithDebInfo.
  string(REPLACE "/INCREMENTAL" "/INCREMENTAL:NO" CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}")

  # COMDAT folding/remove unused functions.
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /OPT:REF /OPT:ICF")
  set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO} /OPT:REF /OPT:ICF")
endif()


# Default symbol visibility to hidden, that way we don't go through the PLT for intra-library calls.
if(ANDROID)
  cmake_policy(SET CMP0063 NEW)
  set(CMAKE_POLICY_DEFAULT_CMP0063 NEW)
  set(CMAKE_C_VISIBILITY_PRESET hidden)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-semantic-interposition")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-semantic-interposition")
endif()


# Warning disables.
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    include(CheckCXXFlag)
    check_cxx_flag(-Wall COMPILER_SUPPORTS_WALL)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-switch")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-switch")
    if(NOT ANDROID)
      check_cxx_flag(-Wno-class-memaccess COMPILER_SUPPORTS_MEMACCESS)
      check_cxx_flag(-Wno-invalid-offsetof COMPILER_SUPPORTS_OFFSETOF)
    endif()
endif()


# Detect processor type.
if(APPLE AND NOT "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
  # Universal binaries.
  if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
    set(CPU_ARCH_X64 TRUE)
    message("Building x86_64 MacOS binaries.")
  endif()
  if("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
    set(CPU_ARCH_ARM64 TRUE)
    message("Building ARM64 MacOS binaries.")
  endif()
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "amd64" OR
       "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "AMD64" OR "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "x86_64")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CPU_ARCH_X64 TRUE)
    message("Building x86_64 binaries.")
  else()
    # Cross-compiling 32-bit build. 32-bit hosts are not supported.
    set(CPU_ARCH_X86 TRUE)
    message(FATAL_ERROR "Building x86_32 binaries is not supported.")
  endif()
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i386" OR
       "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i686")
  set(CPU_ARCH_X86 TRUE)
  message(FATAL_ERROR "Building x86_32 binaries is not supported.")
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm64")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8) # Might have an A64 kernel, e.g. Raspbian.
    set(CPU_ARCH_ARM64 TRUE)
    message("Building ARM64 binaries.")
  else()
    set(CPU_ARCH_ARM32 TRUE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -marm -march=armv7-a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -marm -march=armv7-a")
    message("Building ARM32 binaries on ARM64.")
  endif()
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7-a" OR
       "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7l")
  set(CPU_ARCH_ARM32 TRUE)
  message("Building ARM32 binaries.")
  if(ANDROID)
    # Force ARM mode, since apparently ANDROID_ARM_MODE isn't working..
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -marm")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -marm")
  else()
    # Enable NEON.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -marm -march=armv7-a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -marm -march=armv7-a")
  endif()
elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "riscv64")
  set(CPU_ARCH "riscv64")
  message("Building RISC-V 64 binaries.")

  # Not done for us. Or we should inline atomics?
  link_libraries("-latomic")
else()
  message(FATAL_ERROR "Unknown system processor: ${CMAKE_SYSTEM_PROCESSOR}")
endif()


# We don't need exceptions, disable them to save a bit of code size.
if(MSVC)
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /D_HAS_EXCEPTIONS=0 /permissive-")
else()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti")
endif()


# Write binaries to a seperate directory.
if(WIN32)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/${CPU_ARCH}")
else()
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
endif()

# Needed for Linux - put shared libraries in the binary directory.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")


# Enable threads everywhere.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)


# Enable large file support on Linux 32-bit platforms.
# Android is deliberately ommitted here as it didn't support 64-bit ops on files until Android 7/N.
if((LINUX OR FREEBSD) AND (CPU_ARCH_X86 OR CPU_ARCH_ARM32))
  add_definitions("-D_FILE_OFFSET_BITS=64")
endif()

if(BUILD_TESTS)
  enable_testing()
endif()

# Prevent fmt from being built with exceptions, or being thrown at call sites.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DFMT_EXCEPTIONS=0")

# Use C++17 for building dependencies (some aren't C++20-aware, e.g. reshadefx).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Recursively include the source tree.
add_subdirectory(dep)

# Use C++20 for building the main libraries.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_subdirectory(src)

if(ANDROID)
  add_subdirectory(android/app/src/cpp)
endif()
