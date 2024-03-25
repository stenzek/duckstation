if(ENABLE_SDL2)
  find_package(SDL2 2.30.1 REQUIRED)
endif()
if(NOT WIN32 AND NOT ANDROID)
  # From PCSX2: On macOS, Mono.framework contains an ancient version of libpng. We don't want that.
  # Avoid it by telling cmake to avoid finding frameworks while we search for libpng.
  if(APPLE)
    set(FIND_FRAMEWORK_BACKUP ${CMAKE_FIND_FRAMEWORK})
    set(CMAKE_FIND_FRAMEWORK NEVER)
  endif()

  find_package(Zstd 1.5.5 REQUIRED)
  find_package(WebP REQUIRED) # v1.3.2, spews an error on Linux because no pkg-config.
  find_package(ZLIB REQUIRED) # 1.3, but Mac currently doesn't use it.
  find_package(PNG 1.6.40 REQUIRED)
  find_package(JPEG REQUIRED) # No version because flatpak uses libjpeg-turbo.
  find_package(CURL REQUIRED)
  if(APPLE)
    set(CMAKE_FIND_FRAMEWORK ${FIND_FRAMEWORK_BACKUP})
  endif()
endif()
if(LINUX AND NOT ANDROID)
  find_package(UDEV REQUIRED)
endif()
if(UNIX AND NOT APPLE)
  find_package(Libbacktrace)
  if(NOT LIBBACKTRACE_FOUND)
    message(WARNING "libbacktrace not found, crashes will not produce backtraces.")
  endif()
endif()
