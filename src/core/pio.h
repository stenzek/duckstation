// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <memory>

class Error;

class StateWrapper;

struct Settings;

enum class PIODevice : u8;

namespace PIO {

// Exposed so Bus can call the handlers directly. Otherwise calls should go through the functions below.

class Device
{
public:
  virtual ~Device();

  virtual bool Initialize(Error* error) = 0;
  virtual void UpdateSettings(const Settings& old_settings) = 0;

  virtual void Reset() = 0;

  virtual bool DoState(StateWrapper& sw) = 0;

  virtual u8 ReadHandler(u32 offset) = 0;
  virtual void CodeReadHandler(u32 offset, u32* words, u32 word_count) = 0;
  virtual void WriteHandler(u32 offset, u8 value) = 0;
};

extern bool Initialize(Error* error);
extern void UpdateSettings(const Settings& old_settings);
extern void Shutdown();

extern void Reset();
extern bool DoState(StateWrapper& sw);

} // namespace PIO

extern std::unique_ptr<PIO::Device> g_pio_device;
