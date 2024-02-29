# - Try to find UDEV
# Once done, this will define
#
#  UDEV_FOUND - system has UDEV
#  UDEV_INCLUDE_DIRS - the UDEV include directories
#  UDEV_LIBRARIES - the UDEV library
find_package(PkgConfig)

pkg_check_modules(UDEV_PKGCONF libudev)

find_path(UDEV_INCLUDE_DIRS
  NAMES libudev.h
  PATHS ${UDEV_PKGCONF_INCLUDE_DIRS}
)

find_library(UDEV_LIBRARIES
  NAMES udev
  PATHS ${UDEV_PKGCONF_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(UDEV DEFAULT_MSG UDEV_INCLUDE_DIRS UDEV_LIBRARIES)

mark_as_advanced(UDEV_INCLUDE_DIRS UDEV_LIBRARIES)

if(UDEV_FOUND AND NOT (TARGET UDEV::UDEV))
  add_library (UDEV::UDEV UNKNOWN IMPORTED)
  set_target_properties(UDEV::UDEV
    PROPERTIES
    IMPORTED_LOCATION ${UDEV_LIBRARIES}
    INTERFACE_INCLUDE_DIRECTORIES ${UDEV_INCLUDE_DIRS})
endif()
