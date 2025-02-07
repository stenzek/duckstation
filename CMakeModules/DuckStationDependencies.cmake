# From PCSX2: On macOS, Mono.framework contains an ancient version of libpng. We don't want that.
# Avoid it by telling cmake to avoid finding frameworks while we search for libpng.
if(APPLE)
  set(FIND_FRAMEWORK_BACKUP ${CMAKE_FIND_FRAMEWORK})
  set(CMAKE_FIND_FRAMEWORK NEVER)
endif()

# Enable threads everywhere.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_package(SDL3 3.2.0 REQUIRED)
find_package(Zstd 1.5.6 REQUIRED)
find_package(WebP REQUIRED) # v1.4.0, spews an error on Linux because no pkg-config.
find_package(ZLIB REQUIRED) # 1.3, but Mac currently doesn't use it.
find_package(PNG 1.6.40 REQUIRED)
find_package(JPEG REQUIRED)
find_package(Freetype 2.13.2 REQUIRED) # 2.13.3, but flatpak is still on 2.13.2.
find_package(lunasvg 2.4.1 REQUIRED)
find_package(cpuinfo REQUIRED)
find_package(DiscordRPC 3.4.0 REQUIRED)
find_package(SoundTouch 2.3.3 REQUIRED)
find_package(libzip 1.11.1 REQUIRED)

if(NOT WIN32)
  find_package(CURL REQUIRED)
endif()

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

if(BUILD_QT_FRONTEND)
  find_package(Qt6 6.8.0 COMPONENTS Core Gui Widgets LinguistTools REQUIRED)
endif()

find_package(Shaderc REQUIRED)
find_package(spirv_cross_c_shared REQUIRED)

if(LINUX AND NOT (ALLOW_INSTALL AND INSTALL_SELF_CONTAINED))
  # We need to add the rpath for shaderc to the executable.
  get_target_property(SHADERC_LIBRARY Shaderc::shaderc_shared IMPORTED_LOCATION)
  get_filename_component(SHADERC_LIBRARY_DIRECTORY ${SHADERC_LIBRARY} DIRECTORY)
  list(APPEND CMAKE_BUILD_RPATH ${SHADERC_LIBRARY_DIRECTORY})
  get_target_property(SPIRV_CROSS_LIBRARY spirv-cross-c-shared IMPORTED_LOCATION)
  get_filename_component(SPIRV_CROSS_LIBRARY_DIRECTORY ${SPIRV_CROSS_LIBRARY} DIRECTORY)
  list(APPEND CMAKE_BUILD_RPATH ${SPIRV_CROSS_LIBRARY_DIRECTORY})
endif()

if(LINUX)
  find_package(UDEV REQUIRED)
endif()

if(NOT WIN32 AND NOT APPLE)
  find_package(Libbacktrace REQUIRED)
endif()

if(NOT ANDROID AND NOT WIN32)
  find_package(FFMPEG COMPONENTS avcodec avformat avutil swresample swscale)
  if(NOT FFMPEG_FOUND)
    message(WARNING "FFmpeg not found, using bundled headers.")
  endif()
endif()
if(NOT ANDROID AND NOT FFMPEG_FOUND)
  set(FFMPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/dep/ffmpeg/include")
endif()

if(APPLE)
  set(CMAKE_FIND_FRAMEWORK ${FIND_FRAMEWORK_BACKUP})
endif()
