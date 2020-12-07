# TODO
# - backend selection via command line, rather than simply detecting headers.

cmake_minimum_required(VERSION 3.1 FATAL_ERROR)
project(cubeb
  VERSION 0.0.0)

if(POLICY CMP0063)
  cmake_policy(SET CMP0063 NEW)
endif()
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# On OS/2, visibility attribute is not supported.
if(NOT OS2)
  set(CMAKE_C_VISIBILITY_PRESET hidden)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)
endif()

set(CMAKE_CXX_WARNING_LEVEL 4)
if(NOT MSVC)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti")
else()
  string(REPLACE "/GR" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS}) # Disable RTTI
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS}) # Disable Exceptions
endif()

add_library(cubeb
  src/cubeb.c
  src/cubeb_mixer.cpp
  src/cubeb_resampler.cpp
  src/cubeb_log.cpp
  src/cubeb_strings.c
  src/cubeb_utils.cpp
   $<TARGET_OBJECTS:speex>)
target_include_directories(cubeb
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_include_directories(cubeb PRIVATE src)
target_compile_definitions(cubeb PRIVATE OUTSIDE_SPEEX)
target_compile_definitions(cubeb PRIVATE FLOATING_POINT)
target_compile_definitions(cubeb PRIVATE EXPORT=)
target_compile_definitions(cubeb PRIVATE RANDOM_PREFIX=speex)

add_library(speex OBJECT
  src/speex/resample.c)
set_target_properties(speex PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
target_compile_definitions(speex PRIVATE OUTSIDE_SPEEX)
target_compile_definitions(speex PRIVATE FLOATING_POINT)
target_compile_definitions(speex PRIVATE EXPORT=)
target_compile_definitions(speex PRIVATE RANDOM_PREFIX=speex)

include(CheckIncludeFiles)

check_include_files(AudioUnit/AudioUnit.h USE_AUDIOUNIT)
if(USE_AUDIOUNIT)
  target_sources(cubeb PRIVATE
    src/cubeb_audiounit.cpp
    src/cubeb_osx_run_loop.cpp)
  target_compile_definitions(cubeb PRIVATE USE_AUDIOUNIT)
  target_link_libraries(cubeb PRIVATE "-framework AudioUnit" "-framework CoreAudio" "-framework CoreServices")
endif()

check_include_files(pulse/pulseaudio.h USE_PULSE)
if(USE_PULSE)
  target_sources(cubeb PRIVATE
    src/cubeb_pulse.c)
  target_compile_definitions(cubeb PRIVATE USE_PULSE)
  target_link_libraries(cubeb PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

check_include_files(alsa/asoundlib.h USE_ALSA)
if(USE_ALSA)
  target_sources(cubeb PRIVATE
    src/cubeb_alsa.c)
  target_compile_definitions(cubeb PRIVATE USE_ALSA)
  target_link_libraries(cubeb PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

check_include_files(jack/jack.h USE_JACK)
if(USE_JACK)
  target_sources(cubeb PRIVATE
    src/cubeb_jack.cpp)
  target_compile_definitions(cubeb PRIVATE USE_JACK)
  target_link_libraries(cubeb PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

check_include_files(audioclient.h USE_WASAPI)
if(USE_WASAPI)
  target_sources(cubeb PRIVATE
    src/cubeb_wasapi.cpp)
  target_compile_definitions(cubeb PRIVATE USE_WASAPI)
  target_link_libraries(cubeb PRIVATE avrt ole32)
endif()

check_include_files("windows.h;mmsystem.h" USE_WINMM)
if(USE_WINMM)
  target_sources(cubeb PRIVATE
    src/cubeb_winmm.c)
  target_compile_definitions(cubeb PRIVATE USE_WINMM)
  target_link_libraries(cubeb PRIVATE winmm)
endif()

check_include_files(SLES/OpenSLES.h USE_OPENSL)
if(USE_OPENSL)
  target_sources(cubeb PRIVATE
    src/cubeb_opensl.c
    src/cubeb-jni.cpp)
  target_compile_definitions(cubeb PRIVATE USE_OPENSL)
  target_link_libraries(cubeb PRIVATE OpenSLES)
endif()

check_include_files(sys/soundcard.h HAVE_SYS_SOUNDCARD_H)
if(HAVE_SYS_SOUNDCARD_H)
  try_compile(USE_OSS "${PROJECT_BINARY_DIR}/compile_tests"
    ${PROJECT_SOURCE_DIR}/cmake/compile_tests/oss_is_v4.c)
  if(USE_OSS)
    target_sources(cubeb PRIVATE
      src/cubeb_oss.c)
    target_compile_definitions(cubeb PRIVATE USE_OSS)
    target_link_libraries(cubeb PRIVATE pthread)
  endif()
endif()

check_include_files(aaudio/AAudio.h USE_AAUDIO)
if(USE_AAUDIO)
  target_sources(cubeb PRIVATE
    src/cubeb_aaudio.cpp)
  target_compile_definitions(cubeb PRIVATE USE_AAUDIO)

  # set this definition to enable low latency mode. Possibly bad for battery
  target_compile_definitions(cubeb PRIVATE CUBEB_AAUDIO_LOW_LATENCY)

  # set this definition to enable power saving mode. Possibly resulting
  # in high latency
  # target_compile_definitions(cubeb PRIVATE CUBEB_AAUDIO_LOW_POWER_SAVING)

  # set this mode to make the backend use an exclusive stream.
  # will decrease latency.
  # target_compile_definitions(cubeb PRIVATE CUBEB_AAUDIO_EXCLUSIVE_STREAM)

  target_link_libraries(cubeb PRIVATE ${CMAKE_DL_LIBS})
endif()

check_include_files(android/log.h USE_AUDIOTRACK)
if(USE_AUDIOTRACK)
  target_sources(cubeb PRIVATE
    src/cubeb_audiotrack.c)
  target_compile_definitions(cubeb PRIVATE USE_AUDIOTRACK)
  target_link_libraries(cubeb PRIVATE log)
endif()

check_include_files(sndio.h USE_SNDIO)
if(USE_SNDIO)
  target_sources(cubeb PRIVATE
    src/cubeb_sndio.c)
  target_compile_definitions(cubeb PRIVATE USE_SNDIO)
  target_link_libraries(cubeb PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

check_include_files(sys/audioio.h USE_SUN)
if(USE_SUN)
  target_sources(cubeb PRIVATE
    src/cubeb_sun.c)
  target_compile_definitions(cubeb PRIVATE USE_SUN)
  target_link_libraries(cubeb PRIVATE pthread)
endif()

check_include_files(kai.h USE_KAI)
if(USE_KAI)
  target_sources(cubeb PRIVATE
    src/cubeb_kai.c)
  target_compile_definitions(cubeb PRIVATE USE_KAI)
  target_link_libraries(cubeb PRIVATE kai)
endif()

