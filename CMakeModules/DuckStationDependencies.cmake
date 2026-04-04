# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction

# Get prebuilt dependencies for the current platform and architecture.
if(WIN32)
  if (CPU_ARCH_X64)
    set(DEPS_PATH "${CMAKE_SOURCE_DIR}/dep/prebuilt/windows-x64")
  elseif(CPU_ARCH_ARM64)
    set(DEPS_PATH "${CMAKE_SOURCE_DIR}/dep/prebuilt/windows-arm64")
  else()
    message(FATAL_ERROR "Unsupported architecture")
  endif()
elseif(APPLE)
  set(DEPS_PATH "${CMAKE_SOURCE_DIR}/dep/prebuilt/macos-universal")
elseif(LINUX)
  if(CMAKE_CROSSCOMPILING)
    set(DEPS_CROSS_PREFIX "-cross")
  else()
    set(DEPS_CROSS_PREFIX "")
  endif()

  if (CPU_ARCH_X64)
    set(DEPS_PATH "${CMAKE_SOURCE_DIR}/dep/prebuilt/linux-x64")
  elseif(CPU_ARCH_ARM32)
    set(DEPS_PATH "${CMAKE_SOURCE_DIR}/dep/prebuilt/linux${DEPS_CROSS_PREFIX}-armhf")
  elseif(CPU_ARCH_ARM64)
    set(DEPS_PATH "${CMAKE_SOURCE_DIR}/dep/prebuilt/linux${DEPS_CROSS_PREFIX}-arm64")
  elseif(CPU_ARCH_LOONGARCH64)
    set(DEPS_PATH "${CMAKE_SOURCE_DIR}/dep/prebuilt/linux${DEPS_CROSS_PREFIX}-loongarch64")
  else()
    message(FATAL_ERROR "Unsupported architecture")
  endif()
else()
  message(FATAL_ERROR "Unsupported platform")
endif()
if(NOT EXISTS "${DEPS_PATH}")
  message(FATAL_ERROR "Prebuilt dependencies not found for the current platform and architecture.")
endif()
set(CMAKE_PREFIX_PATH "${DEPS_PATH}")

# Enable threads everywhere.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# pkg-config gets pulled transitively on some platforms.
if(NOT WIN32 AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  find_package(Libbacktrace REQUIRED)
endif()

# libpng relies on zlib, which we need the system version for on Mac.
if(APPLE OR CPU_ARCH_ARM32 OR CPU_ARCH_ARM64)
  find_package(ZLIB REQUIRED)
else()
  find_package(ZLIB 1.3.1 REQUIRED
               NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/ZLIB")
endif()

# Enforce use of bundled dependencies to avoid conflicts with system libraries.
find_package(zstd 1.5.7 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/zstd")
find_package(WebP 1.6.0 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/share/WebP/cmake")
find_package(PNG 1.6.56 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/PNG")
find_package(libjpeg-turbo 3.1.4.1 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/libjpeg-turbo")
find_package(freetype 2.14.3 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/freetype")
find_package(harfbuzz REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/harfbuzz")
find_package(plutosvg 0.0.7 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/plutosvg")
find_package(cpuinfo REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/cpuinfo")
find_package(DiscordRPC 3.4.0 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/DiscordRPC")
find_package(SoundTouch 2.3.3 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/SoundTouch")
find_package(libzip 1.11.4 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/libzip")
find_package(Shaderc 2026.1 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/Shaderc")
find_package(spirv_cross_c_shared REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/share/spirv_cross_c_shared/cmake")
find_package(SDL3 3.4.4 REQUIRED
             NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/SDL3")

# Verify dependency paths.
foreach(dep zstd WebP PNG libjpeg-turbo freetype harfbuzz plutosvg cpuinfo
            DiscordRPC SoundTouch libzip Shaderc spirv_cross_c_shared SDL3)
  if((${dep}_LIBRARY AND NOT "${${dep}_LIBRARY}" MATCHES "^${DEPS_PATH}") OR
     (${dep}_DIR AND NOT "${${dep}_DIR}" MATCHES "^${DEPS_PATH}"))
    message(FATAL_ERROR "Using incorrect ${dep} library. Check your dependencies.")
  endif()
endforeach()

# All our builds include Qt, so this is not a problem.
set(QT_NO_PRIVATE_MODULE_WARNING ON)

# Should be prebuilt.
if(LINUX)
  find_package(Qt6 6.11.0 REQUIRED
                NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/Qt6"
                COMPONENTS Core Gui GuiPrivate Widgets LinguistTools DBus)
else()
  find_package(Qt6 6.11.0 REQUIRED
                NO_DEFAULT_PATH PATHS "${DEPS_PATH}/lib/cmake/Qt6"
                COMPONENTS Core Gui GuiPrivate Widgets LinguistTools)
endif()

# Have to verify it down here, don't want users using unpatched Qt.
if(NOT Qt6_DIR MATCHES "^${DEPS_PATH}")
message(FATAL_ERROR "Using incorrect Qt library. Check your dependencies.")
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
      if (NOT X11_xcb_FOUND)
        message(FATAL_ERROR "XCB is required")
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
  find_package(FFMPEG 8.1.0 COMPONENTS avcodec avformat avutil swresample swscale)
  if(NOT FFMPEG_FOUND)
    message(WARNING "FFmpeg not found, using bundled headers.")
  endif()
endif()
if(NOT FFMPEG_FOUND)
  set(FFMPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/dep/ffmpeg/include")
endif()
