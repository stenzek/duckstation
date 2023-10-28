// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#pragma once
#include "common/types.h"

namespace Common::PageFaultHandler {
enum class HandlerResult
{
  ContinueExecution,
  ExecuteNextHandler,
};

using Handler = HandlerResult (*)(void* exception_pc, void* fault_address, bool is_write);

bool InstallHandler(Handler callback);
bool RemoveHandler(Handler callback);

} // namespace Common::PageFaultHandler
