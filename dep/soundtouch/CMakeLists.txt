if(MSVC)
  set(COMPILE_DEFINITIONS /O2 /fp:fast)
  set(COMPILE_OPTIONS )
else()
  set(COMPILE_OPTIONS -Ofast)
endif()

if(NOT ANDROID)
  add_library(soundtouch STATIC)
else()
  add_library(soundtouch SHARED)
  set(COMPILE_DEFINITIONS "${COMPILE_DEFINITIONS}" "ST_EXPORT")
endif()

target_sources(soundtouch PRIVATE
  source/SoundTouch/AAFilter.cpp
  source/SoundTouch/BPMDetect.cpp
  source/SoundTouch/cpu_detect_x86.cpp
  source/SoundTouch/FIFOSampleBuffer.cpp
  source/SoundTouch/FIRFilter.cpp
  source/SoundTouch/InterpolateCubic.cpp
  source/SoundTouch/InterpolateLinear.cpp
  source/SoundTouch/InterpolateShannon.cpp
  source/SoundTouch/mmx_optimized.cpp
  source/SoundTouch/PeakFinder.cpp
  source/SoundTouch/RateTransposer.cpp
  source/SoundTouch/SoundTouch.cpp
  source/SoundTouch/sse_optimized.cpp
  source/SoundTouch/TDStretch.cpp
)
target_include_directories(soundtouch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(soundtouch PRIVATE ${COMPILE_DEFINITIONS})
target_compile_options(soundtouch PRIVATE ${COMPILE_OPTIONS})
target_compile_definitions(soundtouch PRIVATE SOUNDTOUCH_FLOAT_SAMPLES ST_NO_EXCEPTION_HANDLING=1)

if("${CPU_ARCH}" STREQUAL "aarch32" OR "${CPU_ARCH}" STREQUAL "aarch64")
  target_compile_definitions(soundtouch PRIVATE SOUNDTOUCH_USE_NEON)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7.*)$")
    target_compile_options(soundtouch PRIVATE -mfpu=neon)
  endif()
endif()
