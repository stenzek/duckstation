# - Find ogg
# Find the native ogg includes and libraries
#
#  Ogg_INCLUDE_DIRS - where to find ogg.h, etc.
#  Ogg_LIBRARIES    - List of libraries when using ogg.
#  Ogg_FOUND        - True if ogg found.

if(Ogg_INCLUDE_DIR)
  # Already in cache, be silent
  set(Ogg_FIND_QUIETLY TRUE)
endif()

find_package(PkgConfig QUIET)
pkg_check_modules(PC_Ogg QUIET ogg)

set(Ogg_VERSION ${PC_Ogg_VERSION})

find_path(Ogg_INCLUDE_DIR ogg/ogg.h
  HINTS
    ${PC_Ogg_INCLUDEDIR}
    ${PC_Ogg_INCLUDE_DIRS}
    ${Ogg_ROOT})
# MSVC built ogg may be named ogg_static.
# The provided project files name the library with the lib prefix.
find_library(Ogg_LIBRARY
  NAMES
    ogg
    ogg_static
    libogg
    libogg_static
  HINTS
    ${PC_Ogg_LIBDIR}
    ${PC_Ogg_LIBRARY_DIRS}
    ${Ogg_ROOT})
# Handle the QUIETLY and REQUIRED arguments and set Ogg_FOUND
# to TRUE if all listed variables are TRUE.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Ogg
  REQUIRED_VARS
    Ogg_LIBRARY
    Ogg_INCLUDE_DIR
  VERSION_VAR
    Ogg_VERSION)

if(Ogg_FOUND)
  set(Ogg_LIBRARIES ${Ogg_LIBRARY})
  set(Ogg_INCLUDE_DIRS ${Ogg_INCLUDE_DIR})
  
  if(NOT TARGET Ogg::ogg)
  add_library(Ogg::ogg UNKNOWN IMPORTED)
    set_target_properties(Ogg::ogg PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${Ogg_INCLUDE_DIR}"
      IMPORTED_LOCATION "${Ogg_LIBRARY}")
  endif()
endif()

mark_as_advanced(Ogg_INCLUDE_DIR Ogg_LIBRARY)
