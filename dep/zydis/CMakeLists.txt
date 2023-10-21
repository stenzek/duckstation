add_library(zydis
  dependencies/zycore/include/Zycore/Allocator.h
  dependencies/zycore/include/Zycore/Comparison.h
  dependencies/zycore/include/Zycore/Defines.h
  dependencies/zycore/include/Zycore/Format.h
  dependencies/zycore/include/Zycore/LibC.h
  dependencies/zycore/include/Zycore/Object.h
  dependencies/zycore/include/Zycore/Status.h
  dependencies/zycore/include/Zycore/String.h
  dependencies/zycore/include/Zycore/Types.h
  dependencies/zycore/include/Zycore/Vector.h
  dependencies/zycore/src/Allocator.c
  dependencies/zycore/src/Format.c
  dependencies/zycore/src/String.c
  dependencies/zycore/src/Vector.c
  include/Zydis/Decoder.h
  include/Zydis/DecoderTypes.h
  include/Zydis/Defines.h
  include/Zydis/Disassembler.h
  include/Zydis/FormatterBuffer.h
  include/Zydis/Formatter.h
  include/Zydis/Generated/EnumInstructionCategory.h
  include/Zydis/Generated/EnumISAExt.h
  include/Zydis/Generated/EnumISASet.h
  include/Zydis/Generated/EnumMnemonic.h
  include/Zydis/Generated/EnumRegister.h
  include/Zydis/Internal/DecoderData.h
  include/Zydis/Internal/FormatterATT.h
  include/Zydis/Internal/FormatterBase.h
  include/Zydis/Internal/FormatterIntel.h
  include/Zydis/Internal/SharedData.h
  include/Zydis/Internal/String.h
  include/Zydis/MetaInfo.h
  include/Zydis/Mnemonic.h
  include/Zydis/Register.h
  include/Zydis/Segment.h
  include/Zydis/SharedTypes.h
  include/Zydis/ShortString.h
  include/Zydis/Status.h
  include/Zydis/Utils.h
  include/Zydis/Zydis.h
  src/Decoder.c
  src/DecoderData.c
  src/Disassembler.c
  src/FormatterATT.c
  src/FormatterBase.c
  src/FormatterBuffer.c
  src/Formatter.c
  src/FormatterIntel.c
  src/MetaInfo.c
  src/Mnemonic.c
  src/Register.c
  src/Segment.c
  src/SharedData.c
  src/String.c
  src/Utils.c
  src/Zydis.c
)

target_compile_definitions(zydis PUBLIC
  ZYCORE_STATIC_BUILD
  ZYDIS_DISABLE_ENCODER
  ZYDIS_DISABLE_AVX512
  ZYDIS_DISABLE_KNC
  ZYDIS_STATIC_BUILD
)

target_include_directories(zydis PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/zycore/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(zydis PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/zycore/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

