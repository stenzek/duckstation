// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once
#include "common/types.h"

class Error;

namespace PageFaultHandler {
enum class HandlerResult
{
  ContinueExecution,
  ExecuteNextHandler,
};

HandlerResult HandlePageFault(void* exception_pc, void* fault_address, bool is_write);
bool Install(Error* error = nullptr);
} // namespace PageFaultHandler
