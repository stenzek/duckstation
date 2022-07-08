#pragma once
#include "common/types.h"

namespace Common::PageFaultHandler {
enum class HandlerResult
{
  ContinueExecution,
  ExecuteNextHandler,
};

using Callback = HandlerResult (*)(void* exception_pc, void* fault_address, bool is_write);
using Handle = void*;

u32 GetHandlerCodeSize();

bool InstallHandler(const void* owner, void* start_pc, u32 code_size, Callback callback);
bool RemoveHandler(const void* owner);

} // namespace Common::PageFaultHandler
