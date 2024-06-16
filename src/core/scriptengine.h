#pragma once

#include "types.h"

#include <string_view>

class Error;

namespace ScriptEngine {

bool Initialize(Error* error);
void Shutdown();

using OutputCallback = void (*)(std::string_view, void*);
void SetOutputCallback(OutputCallback callback, void* userdata);

void EvalString(const char* str);

} // namespace ScriptEngine