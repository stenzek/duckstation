# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.
# Sourced from https://raw.githubusercontent.com/aminosbh/sdl2-cmake-modules/master/FindSDL2.cmake

#  Copyright 2019 Amine Ben Hassouna <amine.benhassouna@gmail.com>
#  Copyright 2000-2019 Kitware, Inc. and Contributors
#  All rights reserved.

#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:

#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.

#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.

#  * Neither the name of Kitware, Inc. nor the names of Contributors
#    may be used to endorse or promote products derived from this
#    software without specific prior written permission.

#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#[=======================================================================[.rst:
FindSDL2
--------

Locate SDL2 library

This module defines the following 'IMPORTED' targets:

::

  SDL2::Core
    The SDL2 library, if found.
    Libraries should link to SDL2::Core

  SDL2::Main
    The SDL2main library, if found.
    Applications should link to SDL2::Main instead of SDL2::Core



This module will set the following variables in your project:

::

  SDL2_LIBRARIES, the name of the library to link against
  SDL2_INCLUDE_DIRS, where to find SDL.h
  SDL2_FOUND, if false, do not try to link to SDL2
  SDL2MAIN_FOUND, if false, do not try to link to SDL2main
  SDL2_VERSION_STRING, human-readable string containing the version of SDL2



This module responds to the following cache variables:

::

  SDL2_PATH
    Set a custom SDL2 Library path (default: empty)

  SDL2_NO_DEFAULT_PATH
    Disable search SDL2 Library in default path.
      If SDL2_PATH (default: ON)
      Else (default: OFF)

  SDL2_INCLUDE_DIR
    SDL2 headers path.

  SDL2_LIBRARY
    SDL2 Library (.dll, .so, .a, etc) path.

  SDL2MAIN_LIBRAY
    SDL2main Library (.a) path.

  SDL2_BUILDING_LIBRARY
    This flag is useful only when linking to SDL2_LIBRARIES insead of
    SDL2::Main. It is required only when building a library that links to
    SDL2_LIBRARIES, because only applications need main() (No need to also
    link to SDL2main).
    If this flag is defined, then no SDL2main will be added to SDL2_LIBRARIES
    and no SDL2::Main target will be created.


Don't forget to include SDLmain.h and SDLmain.m in your project for the
OS X framework based version. (Other versions link to -lSDL2main which
this module will try to find on your behalf.) Also for OS X, this
module will automatically add the -framework Cocoa on your behalf.


Additional Note: If you see an empty SDL2_LIBRARY in your project
configuration, it means CMake did not find your SDL2 library
(SDL2.dll, libsdl2.so, SDL2.framework, etc). Set SDL2_LIBRARY to point
to your SDL2 library, and  configure again. Similarly, if you see an
empty SDL2MAIN_LIBRARY, you should set this value as appropriate. These
values are used to generate the final SDL2_LIBRARIES variable and the
SDL2::Core and SDL2::Main targets, but when these values are unset,
SDL2_LIBRARIES, SDL2::Core and SDL2::Main does not get created.


$SDL2DIR is an environment variable that would correspond to the
./configure --prefix=$SDL2DIR used in building SDL2.  l.e.galup 9-20-02



Created by Amine Ben Hassouna:
  Adapt FindSDL.cmake to SDL2 (FindSDL2.cmake).
  Add cache variables for more flexibility:
    SDL2_PATH, SDL2_NO_DEFAULT_PATH (for details, see doc above).
  Mark 'Threads' as a required dependency for non-OSX systems.
  Modernize the FindSDL2.cmake module by creating specific targets:
    SDL2::Core and SDL2::Main (for details, see doc above).


Original FindSDL.cmake module:
  Modified by Eric Wing.  Added code to assist with automated building
  by using environmental variables and providing a more
  controlled/consistent search behavior.  Added new modifications to
  recognize OS X frameworks and additional Unix paths (FreeBSD, etc).
  Also corrected the header search path to follow "proper" SDL
  guidelines.  Added a search for SDLmain which is needed by some
  platforms.  Added a search for threads which is needed by some
  platforms.  Added needed compile switches for MinGW.

On OSX, this will prefer the Framework version (if found) over others.
People will have to manually change the cache value of SDL2_LIBRARY to
override this selection or set the SDL2_PATH variable or the CMake
environment CMAKE_INCLUDE_PATH to modify the search paths.

Note that the header path has changed from SDL/SDL.h to just SDL.h
This needed to change because "proper" SDL convention is #include
"SDL.h", not <SDL/SDL.h>.  This is done for portability reasons
because not all systems place things in SDL/ (see FreeBSD).
#]=======================================================================]

# Define options for searching SDL2 Library in a custom path

set(SDL2_PATH "" CACHE STRING "Custom SDL2 Library path")

set(_SDL2_NO_DEFAULT_PATH OFF)
if(SDL2_PATH)
  set(_SDL2_NO_DEFAULT_PATH ON)
endif()

set(SDL2_NO_DEFAULT_PATH ${_SDL2_NO_DEFAULT_PATH}
    CACHE BOOL "Disable search SDL2 Library in default path")
unset(_SDL2_NO_DEFAULT_PATH)

set(SDL2_NO_DEFAULT_PATH_CMD)
if(SDL2_NO_DEFAULT_PATH)
  set(SDL2_NO_DEFAULT_PATH_CMD NO_DEFAULT_PATH)
endif()

# Search for the SDL2 include directory
find_path(SDL2_INCLUDE_DIR SDL.h
  HINTS
    ENV SDL2DIR
    ${SDL2_NO_DEFAULT_PATH_CMD}
  PATH_SUFFIXES SDL2
                # path suffixes to search inside ENV{SDL2DIR}
                include/SDL2 include
  PATHS ${SDL2_PATH}
  DOC "Where the SDL2 headers can be found"
)

set(SDL2_INCLUDE_DIRS "${SDL2_INCLUDE_DIR}")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(VC_LIB_PATH_SUFFIX lib/x64)
else()
  set(VC_LIB_PATH_SUFFIX lib/x86)
endif()

# SDL-2.0 is the name used by FreeBSD ports...
# don't confuse it for the version number.
find_library(SDL2_LIBRARY
  NAMES SDL2 SDL-2.0
  HINTS
    ENV SDL2DIR
    ${SDL2_NO_DEFAULT_PATH_CMD}
  PATH_SUFFIXES lib ${VC_LIB_PATH_SUFFIX}
  PATHS ${SDL2_PATH}
  DOC "Where the SDL2 Library can be found"
)

set(SDL2_LIBRARIES "${SDL2_LIBRARY}")

if(NOT SDL2_BUILDING_LIBRARY)
  if(NOT SDL2_INCLUDE_DIR MATCHES ".framework")
    # Non-OS X framework versions expect you to also dynamically link to
    # SDL2main. This is mainly for Windows and OS X. Other (Unix) platforms
    # seem to provide SDL2main for compatibility even though they don't
    # necessarily need it.

    if(SDL2_PATH)
      set(SDL2MAIN_LIBRARY_PATHS "${SDL2_PATH}")
    endif()

    if(NOT SDL2_NO_DEFAULT_PATH)
      set(SDL2MAIN_LIBRARY_PATHS
            /sw
            /opt/local
            /opt/csw
            /opt
            "${SDL2MAIN_LIBRARY_PATHS}"
      )
    endif()

    find_library(SDL2MAIN_LIBRARY
      NAMES SDL2main
      HINTS
        ENV SDL2DIR
        ${SDL2_NO_DEFAULT_PATH_CMD}
      PATH_SUFFIXES lib ${VC_LIB_PATH_SUFFIX}
      PATHS ${SDL2MAIN_LIBRARY_PATHS}
      DOC "Where the SDL2main library can be found"
    )
    unset(SDL2MAIN_LIBRARY_PATHS)
  endif()
endif()

# SDL2 may require threads on your system.
# The Apple build may not need an explicit flag because one of the
# frameworks may already provide it.
# But for non-OSX systems, I will use the CMake Threads package.
if(NOT APPLE)
  find_package(Threads QUIET)
  if(NOT Threads_FOUND AND NOT WIN32)
    set(SDL2_THREADS_NOT_FOUND "Could NOT find Threads (Threads is required by SDL2).")
    if(SDL2_FIND_REQUIRED)
      message(FATAL_ERROR ${SDL2_THREADS_NOT_FOUND})
    else()
        if(NOT SDL2_FIND_QUIETLY)
          message(STATUS ${SDL2_THREADS_NOT_FOUND})
        endif()
      return()
    endif()
    unset(SDL2_THREADS_NOT_FOUND)
  endif()
endif()

# MinGW needs an additional link flag, -mwindows
# It's total link flags should look like -lmingw32 -lSDL2main -lSDL2 -mwindows
if(MINGW)
  set(MINGW32_LIBRARY mingw32 "-mwindows" CACHE STRING "link flags for MinGW")
endif()

if(SDL2_LIBRARY)
  # For SDL2main
  if(SDL2MAIN_LIBRARY AND NOT SDL2_BUILDING_LIBRARY)
    list(FIND SDL2_LIBRARIES "${SDL2MAIN_LIBRARY}" _SDL2_MAIN_INDEX)
    if(_SDL2_MAIN_INDEX EQUAL -1)
      set(SDL2_LIBRARIES "${SDL2MAIN_LIBRARY}" ${SDL2_LIBRARIES})
    endif()
    unset(_SDL2_MAIN_INDEX)
  endif()

  # For OS X, SDL2 uses Cocoa as a backend so it must link to Cocoa.
  # CMake doesn't display the -framework Cocoa string in the UI even
  # though it actually is there if I modify a pre-used variable.
  # I think it has something to do with the CACHE STRING.
  # So I use a temporary variable until the end so I can set the
  # "real" variable in one-shot.
  if(APPLE)
    set(SDL2_LIBRARIES ${SDL2_LIBRARIES} "-framework Cocoa")
  endif()

  # For threads, as mentioned Apple doesn't need this.
  # In fact, there seems to be a problem if I used the Threads package
  # and try using this line, so I'm just skipping it entirely for OS X.
  if(NOT APPLE)
    set(SDL2_LIBRARIES ${SDL2_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
  endif()

  # For MinGW library
  if(MINGW)
    set(SDL2_LIBRARIES ${MINGW32_LIBRARY} ${SDL2_LIBRARIES})
  endif()

endif()

# Read SDL2 version
if(SDL2_INCLUDE_DIR AND EXISTS "${SDL2_INCLUDE_DIR}/SDL_version.h")
  file(STRINGS "${SDL2_INCLUDE_DIR}/SDL_version.h" SDL2_VERSION_MAJOR_LINE REGEX "^#define[ \t]+SDL_MAJOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL2_INCLUDE_DIR}/SDL_version.h" SDL2_VERSION_MINOR_LINE REGEX "^#define[ \t]+SDL_MINOR_VERSION[ \t]+[0-9]+$")
  file(STRINGS "${SDL2_INCLUDE_DIR}/SDL_version.h" SDL2_VERSION_PATCH_LINE REGEX "^#define[ \t]+SDL_PATCHLEVEL[ \t]+[0-9]+$")
  string(REGEX REPLACE "^#define[ \t]+SDL_MAJOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL2_VERSION_MAJOR "${SDL2_VERSION_MAJOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_MINOR_VERSION[ \t]+([0-9]+)$" "\\1" SDL2_VERSION_MINOR "${SDL2_VERSION_MINOR_LINE}")
  string(REGEX REPLACE "^#define[ \t]+SDL_PATCHLEVEL[ \t]+([0-9]+)$" "\\1" SDL2_VERSION_PATCH "${SDL2_VERSION_PATCH_LINE}")
  set(SDL2_VERSION_STRING ${SDL2_VERSION_MAJOR}.${SDL2_VERSION_MINOR}.${SDL2_VERSION_PATCH})
  unset(SDL2_VERSION_MAJOR_LINE)
  unset(SDL2_VERSION_MINOR_LINE)
  unset(SDL2_VERSION_PATCH_LINE)
  unset(SDL2_VERSION_MAJOR)
  unset(SDL2_VERSION_MINOR)
  unset(SDL2_VERSION_PATCH)
endif()

include(FindPackageHandleStandardArgs)

FIND_PACKAGE_HANDLE_STANDARD_ARGS(SDL2
                                  REQUIRED_VARS SDL2_LIBRARY SDL2_INCLUDE_DIR
                                  VERSION_VAR SDL2_VERSION_STRING)

if(SDL2MAIN_LIBRARY)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(SDL2main
                                    REQUIRED_VARS SDL2MAIN_LIBRARY SDL2_INCLUDE_DIR
                                    VERSION_VAR SDL2_VERSION_STRING)
endif()


mark_as_advanced(SDL2_PATH
                 SDL2_NO_DEFAULT_PATH
                 SDL2_LIBRARY
                 SDL2MAIN_LIBRARY
                 SDL2_INCLUDE_DIR
                 SDL2_BUILDING_LIBRARY)


# SDL2:: targets (SDL2::Core and SDL2::Main)
if(SDL2_FOUND)

  # SDL2::Core target
  if(SDL2_LIBRARY AND NOT TARGET SDL2::Core)
    add_library(SDL2::Core UNKNOWN IMPORTED)
    set_target_properties(SDL2::Core PROPERTIES
                          IMPORTED_LOCATION "${SDL2_LIBRARY}"
                          INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}")

    if(APPLE)
      # For OS X, SDL2 uses Cocoa as a backend so it must link to Cocoa.
      # For more details, please see above.
      set_property(TARGET SDL2::Core APPEND PROPERTY
                   INTERFACE_LINK_OPTIONS "-framework Cocoa")
    else()
      # For threads, as mentioned Apple doesn't need this.
      # For more details, please see above.
      set_property(TARGET SDL2::Core APPEND PROPERTY
                   INTERFACE_LINK_LIBRARIES Threads::Threads)
    endif()
  endif()

  # SDL2::Main target
  # Applications should link to SDL2::Main instead of SDL2::Core
  # For more details, please see above.
  if(NOT SDL2_BUILDING_LIBRARY AND NOT TARGET SDL2::Main)

    if(SDL2_INCLUDE_DIR MATCHES ".framework" OR NOT SDL2MAIN_LIBRARY)
      add_library(SDL2::Main INTERFACE IMPORTED)
      set_property(TARGET SDL2::Main PROPERTY
                   INTERFACE_LINK_LIBRARIES SDL2::Core)
    elseif(SDL2MAIN_LIBRARY)
      # MinGW requires that the mingw32 library is specified before the
      # libSDL2main.a static library when linking.
      # The SDL2::MainInternal target is used internally to make sure that
      # CMake respects this condition.
      add_library(SDL2::MainInternal UNKNOWN IMPORTED)
      set_property(TARGET SDL2::MainInternal PROPERTY
                   IMPORTED_LOCATION "${SDL2MAIN_LIBRARY}")
      set_property(TARGET SDL2::MainInternal PROPERTY
                   INTERFACE_LINK_LIBRARIES SDL2::Core)

      add_library(SDL2::Main INTERFACE IMPORTED)

      if(MINGW)
        # MinGW needs an additional link flag '-mwindows' and link to mingw32
        set_property(TARGET SDL2::Main PROPERTY
                     INTERFACE_LINK_LIBRARIES "mingw32" "-mwindows")
      endif()

      set_property(TARGET SDL2::Main APPEND PROPERTY
                   INTERFACE_LINK_LIBRARIES SDL2::MainInternal)
    endif()

  endif()
endif()
