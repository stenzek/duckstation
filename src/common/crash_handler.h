#include "types.h"
#include <string_view>

namespace CrashHandler {
bool Install();
void SetWriteDirectory(const std::string_view& dump_directory);
void Uninstall();
} // namespace CrashHandler
