# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0 + Packaging Restriction
#
# NOTE: In addition to the terms of CC-BY-NC-ND-4.0, you may not use this file to create
# packages or build recipes without explicit permission from the copyright holder.

add_executable(updater
  updater.cpp
  updater.h
)

target_include_directories(updater PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(updater PRIVATE common minizip ZLIB::ZLIB)

if(APPLE)
  set(MAC_SOURCES
    cocoa_main.mm
    cocoa_progress_callback.mm
    cocoa_progress_callback.h
    updater_progress_callback.h
  )
  target_sources(updater PRIVATE ${MAC_SOURCES})
  set_source_files_properties(${MAC_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
  find_library(COCOA_LIBRARY Cocoa REQUIRED)
  target_link_libraries(updater PRIVATE ${COCOA_LIBRARY})

  if(NOT CMAKE_GENERATOR MATCHES "Xcode" AND NOT SKIP_POSTPROCESS_BUNDLE)
    set_target_properties(updater PROPERTIES OUTPUT_NAME "Updater")
    set_target_properties(updater PROPERTIES
      MACOSX_BUNDLE true
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
      OUTPUT_NAME Updater
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/DuckStation.app/Contents/Resources
    )

    # Copy icon into the bundle
    target_sources(updater PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Updater.icns")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Updater.icns" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  endif()
endif()
