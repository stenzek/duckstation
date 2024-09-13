// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "types.h"

enum class GenericInputBinding : u8;

struct InputBindingInfo
{
  enum class Type : u8
  {
    Unknown,
    Button,
    Axis,
    HalfAxis,
    Motor,
    Pointer, // Receive relative mouse movement events, bind_index is offset by the axis.
    Macro,
  };

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

  SmallMotor, // High frequency vibration.
  LargeMotor, // Low frequency vibration.

  Count,
};
