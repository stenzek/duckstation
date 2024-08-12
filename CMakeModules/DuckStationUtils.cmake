function(disable_compiler_warnings_for_target target)
	if(MSVC)
		target_compile_options(${target} PRIVATE "/W0")
	else()
		target_compile_options(${target} PRIVATE "-w")
	endif()
endfunction()

function(detect_operating_system)
	message(STATUS "CMake Version: ${CMAKE_VERSION}")
	message(STATUS "CMake System Name: ${CMAKE_SYSTEM_NAME}")

	# LINUX wasn't added until CMake 3.25.
	if (CMAKE_VERSION VERSION_LESS 3.25.0 AND CMAKE_SYSTEM_NAME MATCHES "Linux")
		# Have to make it visible in this scope as well for below.
		set(LINUX TRUE PARENT_SCOPE)
		set(LINUX TRUE)
	endif()

	if(WIN32)
		message(STATUS "Building for Windows.")
	elseif(APPLE AND NOT IOS)
		message(STATUS "Building for MacOS.")
	elseif(LINUX)
		message(STATUS "Building for Linux.")
	elseif(BSD)
		message(STATUS "Building for *BSD.")
	else()
		message(FATAL_ERROR "Unsupported platform.")
	endif()
endfunction()

function(detect_compiler)
	if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		set(COMPILER_CLANG_CL TRUE PARENT_SCOPE)
		set(IS_SUPPORTED_COMPILER TRUE PARENT_SCOPE)
		message(STATUS "Building with Clang-CL.")
	elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
		set(COMPILER_CLANG TRUE PARENT_SCOPE)
		set(IS_SUPPORTED_COMPILER TRUE PARENT_SCOPE)
		message(STATUS "Building with Clang/LLVM.")
	elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		set(COMPILER_GCC TRUE PARENT_SCOPE)
		set(IS_SUPPORTED_COMPILER FALSE PARENT_SCOPE)
		message(STATUS "Building with GNU GCC.")
	elseif(MSVC)
		set(IS_SUPPORTED_COMPILER TRUE PARENT_SCOPE)
		message(STATUS "Building with MSVC.")
	else()
		message(FATAL_ERROR "Unknown compiler: ${CMAKE_CXX_COMPILER_ID}")
	endif()
endfunction()

function(detect_architecture)
  if(APPLE AND NOT "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
    # Universal binaries.
    if("x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
      message(STATUS "Building x86_64 MacOS binaries.")
      set(CPU_ARCH_X64 TRUE PARENT_SCOPE)
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Xarch_x86_64 -msse4.1" PARENT_SCOPE)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Xarch_x86_64 -msse4.1" PARENT_SCOPE)
    endif()
    if("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
      message(STATUS "Building ARM64 MacOS binaries.")
      set(CPU_ARCH_ARM64 TRUE PARENT_SCOPE)
    endif()
  elseif(("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "amd64" OR
          "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "AMD64" OR "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "x86_64") AND
         CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(STATUS "Building x86_64 binaries.")
    set(CPU_ARCH_X64 TRUE PARENT_SCOPE)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msse4.1" PARENT_SCOPE)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse4.1" PARENT_SCOPE)
    endif()
  elseif(("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm64") AND
         CMAKE_SIZEOF_VOID_P EQUAL 8) # Might have an A64 kernel, e.g. Raspbian.
    message(STATUS "Building ARM64 binaries.")
    set(CPU_ARCH_ARM64 TRUE PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7-a" OR
         "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7l" OR
         (("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64" OR "${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm64")
          AND CMAKE_SIZEOF_VOID_P EQUAL 4))
    message(STATUS "Building ARM32 binaries.")
    set(CPU_ARCH_ARM32 TRUE PARENT_SCOPE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -marm -march=armv7-a -mfpu=neon-vfpv4" PARENT_SCOPE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -marm -march=armv7-a -mfpu=neon-vfpv4" PARENT_SCOPE)
  elseif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "riscv64")
    message(STATUS "Building RISC-V 64 binaries.")
    set(CPU_ARCH_RISCV64 TRUE PARENT_SCOPE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -finline-atomics" PARENT_SCOPE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -finline-atomics" PARENT_SCOPE)

    # Still need this, apparently.
    link_libraries("-latomic")

    if(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
      # Frame pointers generate an annoying amount of code on leaf functions.
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fomit-frame-pointer" PARENT_SCOPE)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fomit-frame-pointer" PARENT_SCOPE)
    endif()
  else()
    message(FATAL_ERROR "Unknown system processor: ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
endfunction()

function(detect_page_size)
  # This is only needed for ARM64, or if the user hasn't overridden it explicitly.
  if(NOT CPU_ARCH_ARM64 OR HOST_PAGE_SIZE)
    return()
  endif()

  if(NOT LINUX)
    # For universal Apple builds, we use preprocessor macros to determine page size.
    # Similar for Windows, except it's always 4KB.
    return()
  endif()

  if(CMAKE_CROSSCOMPILING)
    message(WARNING "Cross-compiling and can't determine page size, assuming default.")
    return()
  endif()

  message(STATUS "Determining host page size")
  set(detect_page_size_file ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/src.c)
  file(WRITE ${detect_page_size_file} "
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main() {
  int res = sysconf(_SC_PAGESIZE);
  printf(\"%d\", res);
  return (res > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}")
  try_run(
    detect_page_size_run_result
    detect_page_size_compile_result
    ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}
    ${detect_page_size_file}
    RUN_OUTPUT_VARIABLE detect_page_size_output)
  if(NOT detect_page_size_compile_result OR NOT detect_page_size_run_result EQUAL 0)
    message(FATAL_ERROR "Could not determine host page size.")
  else()
    message(STATUS "Host page size: ${detect_page_size_output}")
    set(HOST_PAGE_SIZE ${detect_page_size_output} CACHE STRING "Reported host page size")
  endif()
endfunction()

function(detect_cache_line_size)
  # This is only needed for ARM64, or if the user hasn't overridden it explicitly.
  if(NOT CPU_ARCH_ARM64 OR HOST_CACHE_LINE_SIZE)
    return()
  endif()

  if(NOT LINUX)
    # For universal Apple builds, we use preprocessor macros to determine page size.
    # Similar for Windows, except it's always 64 bytes.
    return()
  endif()

  if(CMAKE_CROSSCOMPILING)
    message(WARNING "Cross-compiling and can't determine page size, assuming default.")
    return()
  endif()

  message(STATUS "Determining host cache line size")
  set(detect_cache_line_size_file ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp/src.c)
  file(WRITE ${detect_cache_line_size_file} "
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main() {
  int l1i = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
  int l1d = sysconf(_SC_LEVEL1_ICACHE_LINESIZE);
  int res = (l1i > l1d) ? l1i : l1d;
  for (int index = 0; index < 16; index++) {
    char buf[128];
    snprintf(buf, sizeof(buf), \"/sys/devices/system/cpu/cpu0/cache/index%d/coherency_line_size\", index);
    FILE* fp = fopen(buf, \"rb\");
    if (!fp)
      break;
    fread(buf, sizeof(buf), 1, fp);
    fclose(fp);
    int val = atoi(buf);
    res = (val > res) ? val : res;
  }
  printf(\"%d\", res);
  return (res > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}")
  try_run(
    detect_cache_line_size_run_result
    detect_cache_line_size_compile_result
    ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}
    ${detect_cache_line_size_file}
    RUN_OUTPUT_VARIABLE detect_cache_line_size_output)
  if(NOT detect_cache_line_size_compile_result OR NOT detect_cache_line_size_run_result EQUAL 0)
    message(FATAL_ERROR "Could not determine host cache line size.")
  else()
    message(STATUS "Host cache line size: ${detect_cache_line_size_output}")
    set(HOST_CACHE_LINE_SIZE ${detect_cache_line_size_output} CACHE STRING "Reported host cache line size")
  endif()
endfunction()

function(get_scm_version)
  if(SCM_VERSION)
    return()
  endif()

  find_package(Git)
  if(EXISTS "${PROJECT_SOURCE_DIR}/.git" AND GIT_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} describe --dirty
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      OUTPUT_VARIABLE LOCAL_SCM_VERSION
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  endif()
  if(NOT LOCAL_SCM_VERSION)
    set(SCM_VERSION "unknown" PARENT_SCOPE)
  else()
    set(SCM_VERSION ${LOCAL_SCM_VERSION} PARENT_SCOPE)
  endif()
endfunction()

function(install_imported_dep_library name)
  get_target_property(SONAME "${name}" IMPORTED_SONAME_RELEASE)
  get_target_property(LOCATION "${name}" IMPORTED_LOCATION_RELEASE)
  install(FILES "${LOCATION}" RENAME "${SONAME}" DESTINATION "${CMAKE_INSTALL_PREFIX}")
endfunction()
