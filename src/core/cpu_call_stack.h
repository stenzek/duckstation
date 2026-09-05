// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <vector>

namespace CPU {

class CallStack final
{
public:
  struct Frame
  {
    VirtualMemoryAddress address;
    VirtualMemoryAddress stack_pointer;
  };
  using FrameList = std::vector<Frame>;

  CallStack(VirtualMemoryAddress pc, VirtualMemoryAddress sp, VirtualMemoryAddress ra);

  const FrameList& GetFrames() const { return m_frames; }
  FrameList TakeFrames();

  void Walk();

private:
  VirtualMemoryAddress m_initial_pc;
  VirtualMemoryAddress m_initial_sp;
  VirtualMemoryAddress m_initial_ra;
  FrameList m_frames;
};

} // namespace CPU
