# Adapted from: https://github.com/wjakob/layerlab/blob/master/cmake/FindFFTW.cmake

# Copyright (c) 2015, Wenzel Jakob
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
#   list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# - Find FFTW3
# Find the native FFTW3 includes and library
#
#  Cache variables:
#
#  FFTW3_INCLUDE_DIR  - where to find fftw3.h
#  FFTW3_LIBRARY      - Path to FFTW3 libray.
#  FFTW3_ROOT         - Root of FFTW3 installation.
#
#  User variables:
#
#  FFTW3_INCLUDE_DIRS - where to find fftw3.h
#  FFTW3_LIBRARIES    - List of libraries when using FFTW3.
#  FFTW3_FOUND        - True if FFTW3 found.


if(FFTW3_INCLUDE_DIR)
  # Already in cache, be silent
  set(FFTW3_FIND_QUIETLY TRUE)
endif(FFTW3_INCLUDE_DIR)

find_package(PkgConfig QUIET)
pkg_check_modules(PC_FFTW3 QUIET fftw3)

set(FFTW3_VERSION ${PC_FFTW3_VERSION})

find_path(FFTW3_INCLUDE_DIR fftw3.h
  HINTS
    ${PC_FFTW3_INCLUDEDIR}
    ${PC_FFTW3_INCLUDE_DIRS}
    ${FFTW3_ROOT})

find_library(FFTW3_LIBRARY NAMES fftw3
  HINTS
    ${PC_FFTW3_LIBDIR}
    ${PC_FFTW3_LIBRARY_DIRS}
    ${FFTW3_ROOT})

# handle the QUIETLY and REQUIRED arguments and set FFTW3_FOUND to TRUE if
# all listed variables are TRUE
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFTW3
  REQUIRED_VARS
    FFTW3_LIBRARY
    FFTW3_INCLUDE_DIR
  VERSION_VAR
    FFTW3_VERSION)

if(FFTW3_FOUND)
	set(FFTW3_LIBRARIES ${FFTW3_LIBRARY})
	set(FFTW3_INCLUDE_DIRS ${FFTW3_INCLUDE_DIR})
	
	if(NOT TARGET FFTW3::fftw3)
	  add_library(FFTW3::fftw3 UNKNOWN IMPORTED)
		set_target_properties(FFTW3::fftw3 PROPERTIES
			INTERFACE_INCLUDE_DIRECTORIES "${FFTW3_INCLUDE_DIR}"
			IMPORTED_LOCATION "${FFTW3_LIBRARY}"
		)
  endif()
endif()

mark_as_advanced(FFTW3_LIBRARY FFTW3_INCLUDE_DIR)
