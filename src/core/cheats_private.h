// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com> and contributors.
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "cheats.h"

#include "common/bitfield.h"

#include <cstdint>
#include <span>
#include <tuple>

namespace Cheats {

/// Represents a cheat code, after being parsed.
class CheatCode
{
public:
  /// Additional metadata to a cheat code, present for all types.
  struct Metadata
  {
    std::string name;
    CodeType type = CodeType::Gameshark;
    CodeActivation activation = CodeActivation::EndFrame;
    std::optional<u32> override_cpu_overclock;
    std::optional<DisplayAspectRatio> override_aspect_ratio;
    bool has_options : 1;
    bool disable_widescreen_rendering : 1;
    bool enable_8mb_ram : 1;
    bool disallow_for_achievements : 1;
  };

public:
  explicit CheatCode(Metadata metadata);
  virtual ~CheatCode();

  ALWAYS_INLINE const Metadata& GetMetadata() const { return m_metadata; }
  ALWAYS_INLINE const std::string& GetName() const { return m_metadata.name; }
  ALWAYS_INLINE CodeActivation GetActivation() const { return m_metadata.activation; }
  ALWAYS_INLINE bool IsManuallyActivated() const { return (m_metadata.activation == CodeActivation::Manual); }
  ALWAYS_INLINE bool HasOptions() const { return m_metadata.has_options; }

  bool HasAnySettingOverrides() const;
  void ApplySettingOverrides();

  virtual void SetOptionValue(u32 value) = 0;

  virtual void Apply() const = 0;
  virtual void ApplyOnDisable(RollbackLog* rollback_list) const = 0;

  virtual bool HasRestorableOnDisableEffects() const = 0;

protected:
  Metadata m_metadata;
};

class GamesharkCheatCode final : public CheatCode
{
public:
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
    ExtConstantWriteIfMatchWithRestore8 = 0xA8,
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

    ExtCompareBitsSet8 = 0xE4,   // Only used inside ExtMultiConditionals
    ExtCompareBitsClear8 = 0xE5, // Only used inside ExtMultiConditionals
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

  explicit GamesharkCheatCode(Metadata metadata);
  ~GamesharkCheatCode() override;

  static std::unique_ptr<GamesharkCheatCode> Parse(Metadata metadata, const std::string_view data, Error* error);

  std::span<const Instruction> GetInstructions() const;

  void SetOptionValue(u32 value) override;

  void Apply() const override;
  void ApplyOnDisable(RollbackLog* rollback_list) const override;

  bool HasRestorableOnDisableEffects() const override;

private:
  std::vector<Instruction> instructions;
  std::vector<std::tuple<u32, u8, u8>> option_instruction_values;

  u32 GetNextNonConditionalInstruction(u32 index) const;

  static bool IsConditionalInstruction(InstructionCode code);
};

class AssemblyCheatCode final : public CheatCode
{
public:
  static constexpr u64 UNINITIALIZED_OLD_VALUE = UINT64_C(0xFFFFFFFFFFFFFFFF);

  struct Instruction
  {
    u32 pc;
    u32 new_value;
    mutable u64 old_value; // higher order bits are set so fresh codes apply
  };
  static_assert(std::is_trivially_copyable_v<Instruction>);

  explicit AssemblyCheatCode(Metadata metadata);
  ~AssemblyCheatCode() override;

  static std::unique_ptr<AssemblyCheatCode> Parse(Metadata metadata, std::string_view data, Error* error);

  std::span<const Instruction> GetInstructions() const;

  void SetOptionValue(u32 value) override;

  void Apply() const override;
  void ApplyOnDisable(RollbackLog* rollback_list) const override;

  bool HasRestorableOnDisableEffects() const override;

private:
  std::vector<Instruction> instructions;
  std::vector<std::tuple<u32, u8, u8>> option_instruction_values;
};

std::unique_ptr<CheatCode> ParseCode(CheatCode::Metadata metadata, std::string_view data, Error* error);

} // namespace Cheats
