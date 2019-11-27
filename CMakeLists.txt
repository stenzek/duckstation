cmake_minimum_required(VERSION 3.10)
project(duckstation C CXX)

# Pull in modules.
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMakeModules/")


# Common include/library directories on Windows.
if(WIN32)
  set(SDL2_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dep/msvc/include/SDL")
  if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(SDL2_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/dep/msvc/lib64-debug/SDL2.lib")
      set(SDL2MAIN_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/dep/msvc/lib64-debug/SDL2main.lib")
    else()
      set(SDL2_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/dep/msvc/lib32-debug/SDL2.lib")
      set(SDL2MAIN_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/dep/msvc/lib32-debug/SDL2main.lib")
    endif()
  else()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(SDL2_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/dep/msvc/lib64/SDL2.lib")
      set(SDL2MAIN_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/dep/msvc/lib64/SDL2main.lib")
    else()
      set(SDL2_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/dep/msvc/lib32/SDL2.lib")
      set(SDL2MAIN_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/dep/msvc/lib32/SDL2main.lib")
    endif()
  endif()
endif()


# Required libraries.
if(NOT ANDROID)
  find_package(SDL2 REQUIRED)
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

    # Enable intrinsic functions, disable minimal rebuild.
    set(${config} "${${config}} /Oi /Gm-")
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


# Detect C++ version support.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    include(CheckCXXCompilerFlag)
    CHECK_CXX_COMPILER_FLAG("-Wall" COMPILER_SUPPORTS_WALL)
    if(COMPILER_SUPPORTS_WALL)
      message("Enabling -Wall")
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
    endif()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-switch")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-switch")
    if(NOT ANDROID)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-class-memaccess -Wno-invalid-offsetof")
    endif()
endif()


# Detect processor type.
if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "amd64")
  set(CPU_ARCH "x64")
elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "AMD64")
  # MSVC x86/x64
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CPU_ARCH "x64")
  else()
    set(CPU_ARCH "x86")
  endif()
elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i386" OR
       ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "i686")
  set(CPU_ARCH "x86")
elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "aarch64")
  set(CPU_ARCH "aarch64")
elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "arm" OR ${CMAKE_SYSTEM_PROCESSOR} STREQUAL "armv7-a")
  set(CPU_ARCH "arm")
else()
  message(FATAL_ERROR "Unknown system processor: " ${CMAKE_SYSTEM_PROCESSOR})
endif()


# Enable threads everywhere.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)


# Recursively include the source tree.
enable_testing()
add_subdirectory(dep)
add_subdirectory(src)

if(ANDROID)
  add_subdirectory(android/app/src/cpp)
endif()