# Set prefix path to look for our bundled dependencies first on Windows.
if(WIN32 AND CPU_ARCH_X64)
  list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/dep/msvc/deps-x64")
elseif(WIN32 AND CPU_ARCH_ARM64)
  list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/dep/msvc/deps-arm64")
endif()

# Enable threads everywhere.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# pkg-config gets pulled transitively on some platforms.
if(NOT WIN32 AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
endif()

# libpng relies on zlib, which we need the system version for on Mac.
find_package(ZLIB REQUIRED)

# Enforce use of bundled dependencies to avoid conflicts with system libraries.
set(FIND_ROOT_PATH_BACKUP ${CMAKE_FIND_ROOT_PATH})
set(FIND_ROOT_PATH_MODE_INCLUDE_BACKUP ${CMAKE_FIND_ROOT_PATH_MODE_INCLUDE})
set(FIND_ROOT_PATH_MODE_LIBRARY_BACKUP ${CMAKE_FIND_ROOT_PATH_MODE_LIBRARY})
set(FIND_ROOT_PATH_MODE_PACKAGE_BACKUP ${CMAKE_FIND_ROOT_PATH_MODE_PACKAGE})
set(FIND_ROOT_PATH_MODE_PROGRAM_BACKUP ${CMAKE_FIND_ROOT_PATH_MODE_PROGRAM})
set(CMAKE_FIND_ROOT_PATH ${CMAKE_PREFIX_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM ONLY)

# Bundled dependencies.
find_package(SDL3 3.2.24 REQUIRED)
find_package(zstd 1.5.7 REQUIRED)
find_package(WebP REQUIRED) # v1.4.0, spews an error on Linux because no pkg-config.
find_package(PNG 1.6.50 REQUIRED)
find_package(JPEG REQUIRED)
find_package(Freetype 2.13.3 REQUIRED)
find_package(harfbuzz REQUIRED)
find_package(plutosvg 0.0.6 REQUIRED)
find_package(cpuinfo REQUIRED)
find_package(DiscordRPC 3.4.0 REQUIRED)
find_package(SoundTouch 2.3.3 REQUIRED)
find_package(libzip 1.11.4 REQUIRED)
find_package(Shaderc REQUIRED)
find_package(spirv_cross_c_shared REQUIRED)

if(NOT WIN32 AND NOT APPLE)
  find_package(Libbacktrace REQUIRED)

  # We need to add the rpath for shaderc to the executable.
  get_target_property(SHADERC_LIBRARY Shaderc::shaderc_shared IMPORTED_LOCATION)
  get_filename_component(SHADERC_LIBRARY_DIRECTORY ${SHADERC_LIBRARY} DIRECTORY)
  list(APPEND CMAKE_BUILD_RPATH ${SHADERC_LIBRARY_DIRECTORY})
  get_target_property(SPIRV_CROSS_LIBRARY spirv-cross-c-shared IMPORTED_LOCATION)
  get_filename_component(SPIRV_CROSS_LIBRARY_DIRECTORY ${SPIRV_CROSS_LIBRARY} DIRECTORY)
  list(APPEND CMAKE_BUILD_RPATH ${SPIRV_CROSS_LIBRARY_DIRECTORY})
endif()

# Restore system package search path.
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ${FIND_ROOT_PATH_MODE_INCLUDE_BACKUP})
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ${FIND_ROOT_PATH_MODE_LIBRARY_BACKUP})
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ${FIND_ROOT_PATH_MODE_PACKAGE_BACKUP})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM ${FIND_ROOT_PATH_MODE_PROGRAM_BACKUP})
set(CMAKE_FIND_ROOT_PATH ${FIND_ROOT_PATH_BACKUP})

# Qt has transitive dependencies on system libs, so do it afterwards.
if(BUILD_QT_FRONTEND)
  find_package(Qt6 6.9.3 COMPONENTS Core Gui Widgets LinguistTools REQUIRED)

  # Have to verify it down here, don't want users using unpatched Qt.
  if(NOT Qt6_DIR MATCHES "^${CMAKE_PREFIX_PATH}")
    message(FATAL_ERROR "Using incorrect Qt library. Check your dependencies.")
  endif()
endif()

# Libraries that are pulled in from host.
if(NOT WIN32)
  find_package(CURL REQUIRED)
  if(LINUX)
    find_package(UDEV REQUIRED)
  endif()

  if(NOT APPLE)
    if(ENABLE_X11)
      find_package(X11 REQUIRED)
      if (NOT X11_xcb_FOUND OR NOT X11_xcb_randr_FOUND OR NOT X11_X11_xcb_FOUND)
        message(FATAL_ERROR "XCB, XCB-randr and X11-xcb are required")
      endif()
    endif()

    if(ENABLE_WAYLAND)
      find_package(ECM REQUIRED NO_MODULE)
      list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")
      find_package(Wayland REQUIRED Egl)
    endif()
  endif()
endif()

if(NOT WIN32)
  find_package(FFMPEG 7.0.0 COMPONENTS avcodec avformat avutil swresample swscale)
  if(NOT FFMPEG_FOUND)
    message(WARNING "FFmpeg not found, using bundled headers.")
  endif()
endif()
if(NOT FFMPEG_FOUND)
  set(FFMPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/dep/ffmpeg/include")
endif()
