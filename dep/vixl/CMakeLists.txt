add_library(vixl
  include/vixl/assembler-base-vixl.h
  include/vixl/code-buffer-vixl.h
  include/vixl/code-generation-scopes-vixl.h
  include/vixl/compiler-intrinsics-vixl.h
  include/vixl/cpu-features.h
  include/vixl/globals-vixl.h
  include/vixl/invalset-vixl.h
  include/vixl/macro-assembler-interface.h
  include/vixl/platform-vixl.h
  include/vixl/pool-manager-impl.h
  include/vixl/pool-manager.h
  include/vixl/utils-vixl.h
  src/code-buffer-vixl.cc
  src/compiler-intrinsics-vixl.cc
  src/cpu-features.cc
  src/utils-vixl.cc
)

target_include_directories(vixl PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(vixl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include/vixl
)
target_compile_definitions(vixl PUBLIC
  VIXL_CODE_BUFFER_MALLOC
)

if(${CPU_ARCH} STREQUAL "aarch32")
  target_sources(vixl PRIVATE
    include/vixl/aarch32/assembler-aarch32.h
    include/vixl/aarch32/constants-aarch32.h
    include/vixl/aarch32/disasm-aarch32.h
    include/vixl/aarch32/instructions-aarch32.h
    include/vixl/aarch32/location-aarch32.h
    include/vixl/aarch32/macro-assembler-aarch32.h
    include/vixl/aarch32/operands-aarch32.h
    src/aarch32/assembler-aarch32.cc
    src/aarch32/constants-aarch32.cc
    src/aarch32/disasm-aarch32.cc
    src/aarch32/instructions-aarch32.cc
    src/aarch32/location-aarch32.cc
    src/aarch32/macro-assembler-aarch32.cc
    src/aarch32/operands-aarch32.cc
  )
  target_include_directories(vixl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vixl/aarch32
  )
  target_compile_definitions(vixl PUBLIC
    VIXL_INCLUDE_TARGET_A32
  )
endif()

if(${CPU_ARCH} STREQUAL "aarch64")
  target_sources(vixl PRIVATE
    include/vixl/aarch64/abi-aarch64.h
    include/vixl/aarch64/assembler-aarch64.h
    include/vixl/aarch64/constants-aarch64.h
    include/vixl/aarch64/cpu-aarch64.h
    include/vixl/aarch64/cpu-features-auditor-aarch64.h
    include/vixl/aarch64/decoder-aarch64.h
    include/vixl/aarch64/disasm-aarch64.h
    include/vixl/aarch64/instructions-aarch64.h
    include/vixl/aarch64/instrument-aarch64.h
    include/vixl/aarch64/macro-assembler-aarch64.h
    include/vixl/aarch64/operands-aarch64.h
    include/vixl/aarch64/simulator-aarch64.h
    include/vixl/aarch64/simulator-constants-aarch64.h
    src/aarch64/assembler-aarch64.cc
    src/aarch64/cpu-aarch64.cc
    src/aarch64/cpu-features-auditor-aarch64.cc
    src/aarch64/decoder-aarch64.cc
    src/aarch64/disasm-aarch64.cc
    src/aarch64/instructions-aarch64.cc
    src/aarch64/instrument-aarch64.cc
    src/aarch64/logic-aarch64.cc
    src/aarch64/macro-assembler-aarch64.cc
    src/aarch64/operands-aarch64.cc
    src/aarch64/pointer-auth-aarch64.cc
    src/aarch64/simulator-aarch64.cc
  )
  target_include_directories(vixl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/vixl/aarch64
  )
  target_compile_definitions(vixl PUBLIC
    VIXL_INCLUDE_TARGET_A64
  )
endif()


