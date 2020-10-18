#pragma once
#include "types.h"

namespace Common::PageFaultHandler {
enum class HandlerResult
{
  ContinueExecution,
  ExecuteNextHandler,
};

using Callback = HandlerResult(*)(void* exception_pc, void* fault_address, bool is_write);
using Handle = void*;

bool InstallHandler(void* owner, Callback callback);
bool RemoveHandler(void* owner);

} // namespace Common::PageFaultHandler
