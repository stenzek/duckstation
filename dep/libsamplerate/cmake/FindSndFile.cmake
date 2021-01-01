# Variables defined:
#  SNDFILE_FOUND
#  SNDFILE_INCLUDE_DIR
#  SNDFILE_LIBRARY
#
# Environment variables used:
#  SNDFILE_ROOT

if(SndFile_INCLUDE_DIR)
  # Already in cache, be silent
  set(SndFile_FIND_QUIETLY TRUE)
endif(SndFile_INCLUDE_DIR)

find_package(PkgConfig QUIET)
pkg_check_modules(PC_SndFile QUIET sndfile)

set(SndFile_VERSION ${PC_SndFile_VERSION})

find_package(Vorbis COMPONENTS Enc QUIET)
find_package(FLAC QUIET)
find_package(Opus QUIET)

find_path(SndFile_INCLUDE_DIR sndfile.h
  HINTS
    ${PC_SndFile_INCLUDEDIR}
    ${PC_SndFile_INCLUDE_DIRS}
    ${SndFile_ROOT})

find_library(SndFile_LIBRARY NAMES sndfile
  HINTS
    ${PC_SndFile_LIBDIR}
    ${PC_SndFile_LIBRARY_DIRS}
    ${SndFile_ROOT})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SndFile
  REQUIRED_VARS
    SndFile_LIBRARY
    SndFile_INCLUDE_DIR
  VERSION_VAR
    SndFile_VERSION)

if(SndFile_FOUND)

  set(SndFile_LIBRARIES ${SndFile_LIBRARY} ${Vorbis_Enc_LIBRARIES} ${FLAC_LIBRARIES} ${OPUS_LIBRARIES})
  set(SndFile_INCLUDE_DIRS ${SndFile_INCLUDE_DIR} ${Vorbis_Enc_INCLUDE_DIRS} ${FLAC_INCLUDE_DIRS} ${OPUS_INCLUDE_DIRS})
  
  if(NOT TARGET SndFile::sndfile)
    add_library(SndFile::sndfile UNKNOWN IMPORTED)
    set_target_properties(SndFile::sndfile PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${SndFile_INCLUDE_DIR}"
      IMPORTED_LOCATION "${SndFile_LIBRARY}"
      INTERFACE_LINK_LIBRARIES "Vorbis::vorbisenc;Opus::opus;FLAC::FLAC")
  endif()
endif()

mark_as_advanced(SndFile_LIBRARY SndFile_INCLUDE_DIR)
