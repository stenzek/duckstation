#pragma once
#include "common/bitfield.h"
#include "types.h"
#include <optional>
#include <string>
#include <vector>

struct CheatCode
{
  enum class Type : u8
  {
    Gameshark,
    Count
  };

  enum class Activation : u8
  {
    Manual,
    EndFrame,
    Count,
  };

  enum class InstructionCode : u8
  {
    Nop = 0x00,
    ConstantWrite8 = 0x30,
    ConstantWrite16 = 0x80,
    ScratchpadWrite16 = 0x1F,
    Increment16 = 0x10,
    Decrement16 = 0x11,
    Increment8 = 0x20,
    Decrement8 = 0x21,
    DelayActivation = 0xC1,
    SkipIfNotEqual16 = 0xC0,
    SkipIfButtonsNotEqual = 0xD5,
    SkipIfButtonsEqual = 0xD6,
    CompareButtons = 0xD4,
    CompareEqual16 = 0xD0,
    CompareNotEqual16 = 0xD1,
    CompareLess16 = 0xD2,
    CompareGreater16 = 0xD3,
    CompareEqual8 = 0xE0,
    CompareNotEqual8 = 0xE1,
    CompareLess8 = 0xE2,
    CompareGreater8 = 0xE3,
    Slide = 0x50,
    MemoryCopy = 0xC2,
    ExtImprovedSlide = 0x53,

    // Extension opcodes, not present on original GameShark.
    ExtConstantWrite32 = 0x90,
    ExtScratchpadWrite32 = 0xA5,
    ExtCompareEqual32 = 0xA0,
    ExtCompareNotEqual32 = 0xA1,
    ExtCompareLess32 = 0xA2,
    ExtCompareGreater32 = 0xA3,
    ExtSkipIfNotEqual32 = 0xA4,
    ExtIncrement32 = 0x60,
    ExtDecrement32 = 0x61,
    ExtConstantWriteIfMatch16 = 0xA6,
    ExtConstantWriteIfMatchWithRestore16 = 0xA7,
    ExtConstantForceRange8 = 0xF0,
    ExtConstantForceRangeLimits16 = 0xF1,
    ExtConstantForceRangeRollRound16 = 0xF2,
    ExtConstantForceRange16 = 0xF3,
    ExtFindAndReplace = 0xF4,
    ExtConstantSwap16 = 0xF5,

    ExtConstantBitSet8 = 0x31,
    ExtConstantBitClear8 = 0x32,
    ExtConstantBitSet16 = 0x81,
    ExtConstantBitClear16 = 0x82,
    ExtConstantBitSet32 = 0x91,
    ExtConstantBitClear32 = 0x92,

    ExtBitCompareButtons = 0xD7,
    ExtSkipIfNotLess8 = 0xC3,
    ExtSkipIfNotGreater8 = 0xC4,
    ExtSkipIfNotLess16 = 0xC5,
    ExtSkipIfNotGreater16 = 0xC6,
    ExtMultiConditionals = 0xF6,

    ExtCheatRegisters = 0x51,
    ExtCheatRegistersCompare = 0x52,
    
    ExtCompareBitsSet8 = 0xE4,   //Only used inside ExtMultiConditionals
    ExtCompareBitsClear8 = 0xE5, //Only used inside ExtMultiConditionals
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
    BitField<u64, u32, 0, 32> value32;
    BitField<u64, u16, 0, 16> value16;
    BitField<u64, u8, 0, 8> value8;
  };

  std::string group;
  std::string description;
  std::vector<Instruction> instructions;
  std::string comments;
  Type type = Type::Gameshark;
  Activation activation = Activation::EndFrame;
  bool enabled = false;

  ALWAYS_INLINE bool Valid() const { return !instructions.empty() && !description.empty(); }
  ALWAYS_INLINE bool IsManuallyActivated() const { return (activation == Activation::Manual); }

  std::string GetInstructionsAsString() const;
  bool SetInstructionsFromString(const std::string& str);

  u32 GetNextNonConditionalInstruction(u32 index) const;

  void Apply() const;
  void ApplyOnDisable() const;

  static const char* GetTypeName(Type type);
  static const char* GetTypeDisplayName(Type type);
  static std::optional<Type> ParseTypeName(const char* str);

  static const char* GetActivationName(Activation activation);
  static const char* GetActivationDisplayName(Activation activation);
  static std::optional<Activation> ParseActivationName(const char* str);
};

class CheatList final
{
public:
  enum class Format
  {
    Autodetect,
    PCSXR,
    Libretro,
    EPSXe,
    Count
  };

  CheatList();
  ~CheatList();

  ALWAYS_INLINE const CheatCode& GetCode(u32 i) const { return m_codes[i]; }
  ALWAYS_INLINE CheatCode& GetCode(u32 i) { return m_codes[i]; }
  ALWAYS_INLINE u32 GetCodeCount() const { return static_cast<u32>(m_codes.size()); }
  ALWAYS_INLINE bool IsCodeEnabled(u32 index) const { return m_codes[index].enabled; }

  ALWAYS_INLINE bool GetMasterEnable() const { return m_master_enable; }
  ALWAYS_INLINE void SetMasterEnable(bool enable) { m_master_enable = enable; }

  const CheatCode* FindCode(const char* name) const;
  const CheatCode* FindCode(const char* group, const char* name) const;

  void AddCode(CheatCode cc);
  void SetCode(u32 index, CheatCode cc);
  void RemoveCode(u32 i);

  u32 GetEnabledCodeCount() const;
  std::vector<std::string> GetCodeGroups() const;
  void EnableCode(u32 index);
  void DisableCode(u32 index);
  void SetCodeEnabled(u32 index, bool state);

  static std::optional<Format> DetectFileFormat(const char* filename);
  static Format DetectFileFormat(const std::string& str);
  static bool ParseLibretroCheat(CheatCode* cc, const char* line);

  bool LoadFromFile(const char* filename, Format format);
  bool LoadFromPCSXRFile(const char* filename);
  bool LoadFromLibretroFile(const char* filename);

  bool LoadFromString(const std::string& str, Format format);
  bool LoadFromPCSXRString(const std::string& str);
  bool LoadFromLibretroString(const std::string& str);
  bool LoadFromEPSXeString(const std::string& str);

  bool SaveToPCSXRFile(const char* filename);

  bool LoadFromPackage(const std::string& game_code);

  void Apply();

  void ApplyCode(u32 index);

  void MergeList(const CheatList& cl);

private:
  std::vector<CheatCode> m_codes;
  bool m_master_enable = true;
};

class MemoryScan
{
public:
  enum class Operator
  {
    Equal,
    NotEqual,
    GreaterThan,
    GreaterEqual,
    LessThan,
    LessEqual,
    IncreasedBy,
    DecreasedBy,
    ChangedBy,
    EqualLast,
    NotEqualLast,
    GreaterThanLast,
    GreaterEqualLast,
    LessThanLast,
    LessEqualLast,
    Any
  };

  struct Result
  {
    PhysicalMemoryAddress address;
    u32 value;
    u32 last_value;
    bool value_changed;

    bool Filter(Operator op, u32 comp_value, bool is_signed) const;
    void UpdateValue(MemoryAccessSize size, bool is_signed);
  };

  using ResultVector = std::vector<Result>;

  MemoryScan();
  ~MemoryScan();

  u32 GetValue() const { return m_value; }
  bool GetValueSigned() const { return m_signed; }
  MemoryAccessSize GetSize() const { return m_size; }
  Operator GetOperator() const { return m_operator; }
  PhysicalMemoryAddress GetStartAddress() const { return m_start_address; }
  PhysicalMemoryAddress GetEndAddress() const { return m_end_address; }
  const ResultVector& GetResults() const { return m_results; }
  const Result& GetResult(u32 index) const { return m_results[index]; }
  u32 GetResultCount() const { return static_cast<u32>(m_results.size()); }

  void SetValue(u32 value) { m_value = value; }
  void SetValueSigned(bool s) { m_signed = s; }
  void SetSize(MemoryAccessSize size) { m_size = size; }
  void SetOperator(Operator op) { m_operator = op; }
  void SetStartAddress(PhysicalMemoryAddress addr) { m_start_address = addr; }
  void SetEndAddress(PhysicalMemoryAddress addr) { m_end_address = addr; }

  void ResetSearch();
  void Search();
  void SearchAgain();
  void UpdateResultsValues();

  void SetResultValue(u32 index, u32 value);

private:
  void SearchBytes();
  void SearchHalfwords();
  void SearchWords();

  u32 m_value = 0;
  MemoryAccessSize m_size = MemoryAccessSize::HalfWord;
  Operator m_operator = Operator::Equal;
  PhysicalMemoryAddress m_start_address = 0;
  PhysicalMemoryAddress m_end_address = 0x200000;
  ResultVector m_results;
  bool m_signed = false;
};

class MemoryWatchList
{
public:
  MemoryWatchList();
  ~MemoryWatchList();

  struct Entry
  {
    std::string description;
    u32 address;
    u32 value;
    MemoryAccessSize size;
    bool is_signed;
    bool freeze;
    bool changed;
  };

  using EntryVector = std::vector<Entry>;

  const Entry* GetEntryByAddress(u32 address) const;
  const EntryVector& GetEntries() const { return m_entries; }
  const Entry& GetEntry(u32 index) const { return m_entries[index]; }
  u32 GetEntryCount() const { return static_cast<u32>(m_entries.size()); }

  bool AddEntry(std::string description, u32 address, MemoryAccessSize size, bool is_signed, bool freeze);
  void RemoveEntry(u32 index);
  bool RemoveEntryByDescription(const char* description);
  bool RemoveEntryByAddress(u32 address);

  void SetEntryDescription(u32 index, std::string description);
  void SetEntryFreeze(u32 index, bool freeze);
  void SetEntryValue(u32 index, u32 value);

  void UpdateValues();

private:
  static void SetEntryValue(Entry* entry, u32 value);
  static void UpdateEntryValue(Entry* entry);

  EntryVector m_entries;
};
