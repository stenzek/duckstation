#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <optional>
#include <string>
#include <vector>

struct CheatCode
{
  enum class InstructionCode : u8
  {
    ConstantWrite8 = 0x30,
    ConstantWrite16 = 0x80,
    Increment16 = 0x10,
    Decrement16 = 0x11,
    Increment8 = 0x20,
    Decrement8 = 0x21,
    CompareEqual16 = 0xD0,
    CompareNotEqual16 = 0xD1,
    CompareLess16 = 0xD2,
    CompareGreater16 = 0xD3,
    CompareEqual8 = 0xE0,
    CompareNotEqual8 = 0xE1,
    CompareLess8 = 0xE2,
    CompareGreater8 = 0xE3,
    Slide = 0x50
  };

  union Instruction
  {
    u64 bits;

    struct
    {
      u32 second;
      u32 first;
    };

    BitField<u64, InstructionCode, 32 + 24, 8> code;
    BitField<u64, u32, 32, 24> address;
    BitField<u64, u16, 0, 16> value16;
    BitField<u64, u8, 0, 8> value8;
  };

  std::string description;
  std::vector<Instruction> instructions;
  bool enabled;

  ALWAYS_INLINE bool Valid() const { return !instructions.empty() && !description.empty(); }

  void Apply() const;
};

class CheatList final
{
public:
  enum class Format
  {
    Autodetect,
    PCSXR,
    Libretro,
    Count
  };

  CheatList();
  ~CheatList();

  ALWAYS_INLINE const CheatCode& GetCode(u32 i) const { return m_codes[i]; }
  ALWAYS_INLINE CheatCode& GetCode(u32 i) { return m_codes[i]; }
  ALWAYS_INLINE u32 GetCodeCount() const { return static_cast<u32>(m_codes.size()); }

  void AddCode(CheatCode cc);
  void RemoveCode(u32 i);

  static std::optional<Format> DetectFileFormat(const char* filename);

  bool LoadFromFile(const char* filename, Format format);
  bool LoadFromPCSXRFile(const char* filename);
  bool LoadFromLibretroFile(const char* filename);

  bool SaveToPCSXRFile(const char* filename);

  void Apply();

private:
  std::vector<CheatCode> m_codes;
};
