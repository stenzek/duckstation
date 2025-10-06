// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "types.h"

class SettingsInterface;

enum class GenericInputBinding : u8;

struct InputBindingInfo
{
  enum class Type : u8
  {
    Unknown,
    Button,
    Axis,
    HalfAxis,
    Motor,           // Vibration motors, generic_mapping gets used for motor selection.
    LED,             // Status LEDs, e.g. analog/digital mode indicator.
    Pointer,         // Absolute pointer, does not receive any events, but is queryable.
    RelativePointer, // Receive relative mouse movement events, bind_index is offset by the axis.
    Device,          // Used for special-purpose device selection, e.g. force feedback.
    Macro,
  };

  ALWAYS_INLINE static bool IsEffectType(Type type) { return (type >= Type::Motor && type <= Type::LED); }

  const char* name;
  const char* display_name;
  Type bind_type;
  u16 bind_index;
  GenericInputBinding generic_mapping;
};

/// Generic input bindings. These roughly match a DualShock 4 or XBox One controller.
/// They are used for automatic binding to PS2 controller types, and for big picture mode navigation.
enum class GenericInputBinding : u8
{
  Unknown,

  DPadUp,
  DPadRight,
  DPadLeft,
  DPadDown,

  LeftStickUp,
  LeftStickRight,
  LeftStickDown,
  LeftStickLeft,
  L3,

  RightStickUp,
  RightStickRight,
  RightStickDown,
  RightStickLeft,
  R3,

  Triangle, // Y on XBox pads.
  Circle,   // B on XBox pads.
  Cross,    // A on XBox pads.
  Square,   // X on XBox pads.

  Select, // Share on DS4, View on XBox pads.
  Start,  // Options on DS4, Menu on XBox pads.
  System, // PS button on DS4, Guide button on XBox pads.

  L1, // LB on Xbox pads.
  L2, // Left trigger on XBox pads.
  R1, // RB on XBox pads.
  R2, // Right trigger on Xbox pads.

  LargeMotor, // Low frequency vibration.
  SmallMotor, // High frequency vibration.

  ModeLED, // Indicates Digital/Analog mode.

  Count,
};

struct SettingInfo
{
  enum class Type
  {
    Boolean,
    Integer,
    IntegerList,
    Float,
    String,
    Path,
  };

  Type type;
  const char* name;
  const char* display_name;
  const char* description;
  const char* default_value;
  const char* min_value;
  const char* max_value;
  const char* step_value;
  const char* format;
  const char* const* options;
  float multiplier;

  const char* StringDefaultValue() const;
  bool BooleanDefaultValue() const;
  s32 IntegerDefaultValue() const;
  s32 IntegerMinValue() const;
  s32 IntegerMaxValue() const;
  s32 IntegerStepValue() const;
  float FloatDefaultValue() const;
  float FloatMinValue() const;
  float FloatMaxValue() const;
  float FloatStepValue() const;

  void CopyValue(SettingsInterface* dest_si, const SettingsInterface& src_si, const char* section) const;
};
