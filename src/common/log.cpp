// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "log.h"
#include "assert.h"
#include "file_system.h"
#include "small_string.h"
#include "timer.h"

#include "fmt/format.h"

#include <array>
#include <bitset>
#include <cstdio>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include "windows_headers.h"
#elif defined(__ANDROID__)
#include <android/log.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace std::string_view_literals;

namespace Log {
namespace {
struct RegisteredCallback
{
  Log::CallbackFunctionType Function;
  void* Parameter;
};
} // namespace

using ChannelBitSet = std::bitset<static_cast<size_t>(Channel::MaxCount)>;

static void RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam,
                             const std::unique_lock<std::mutex>& lock);
static void UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam,
                               const std::unique_lock<std::mutex>& lock);

static bool FilterTest(Channel channel, Level level);
static void ExecuteCallbacks(Channel channel, const char* functionName, Level level, std::string_view message,
                             const std::unique_lock<std::mutex>& lock);
static void FormatLogMessageForDisplay(fmt::memory_buffer& buffer, const char* channelName, const char* functionName,
                                       Level level, std::string_view message, bool timestamp, bool ansi_color_code);
static void ConsoleOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, Level level,
                                     std::string_view message);
static void DebugOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, Level level,
                                   std::string_view message);
static void FileOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, Level level,
                                  std::string_view message);
template<typename T>
static void FormatLogMessageAndPrint(const char* channelName, const char* functionName, Level level,
                                     std::string_view message, bool timestamp, bool ansi_color_code, const T& callback);
#ifdef _WIN32
template<typename T>
static void FormatLogMessageAndPrintW(const char* channelName, const char* functionName, Level level,
                                      std::string_view message, bool timestamp, bool ansi_color_code,
                                      const T& callback);
#endif

ALWAYS_INLINE static Channel UnpackLogChannel(PackedChannelAndLevel cat)
{
  return static_cast<Channel>(cat >> 3);
}

ALWAYS_INLINE static Level UnpackLogLevel(PackedChannelAndLevel cat)
{
  return static_cast<Level>(cat & 0x7);
}

static constexpr const std::array<char, static_cast<size_t>(Level::MaxCount)> s_log_level_characters = {
  {'X', 'E', 'W', 'I', 'V', 'D', 'B', 'T'}};

static constexpr const std::array<const char*, static_cast<size_t>(Channel::MaxCount)> s_log_channel_names = {{
#define LOG_CHANNEL_NAME(X) #X,
  ENUMERATE_LOG_CHANNELS(LOG_CHANNEL_NAME)
#undef LOG_CHANNEL_NAME
}};

namespace {

struct State
{
  Level log_level = Level::Trace;
  ChannelBitSet log_channels_enabled = ChannelBitSet().set();

  std::vector<RegisteredCallback> callbacks;
  std::mutex callbacks_mutex;

  Common::Timer::Value start_timestamp = Common::Timer::GetCurrentValue();

  FileSystem::ManagedCFilePtr file_handle;

  bool console_output_enabled = false;
  bool console_output_timestamps = false;
  bool file_output_enabled = false;
  bool file_output_timestamp = false;
  bool debug_output_enabled = false;

#ifdef _WIN32
  HANDLE hConsoleStdIn = NULL;
  HANDLE hConsoleStdOut = NULL;
  HANDLE hConsoleStdErr = NULL;
#endif
};

} // namespace

ALIGN_TO_CACHE_LINE static State s_state;

} // namespace Log

void Log::RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam)
{
  std::unique_lock lock(s_state.callbacks_mutex);
  RegisterCallback(callbackFunction, pUserParam, lock);
}

void Log::RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam,
                           const std::unique_lock<std::mutex>& lock)
{
  RegisteredCallback Callback;
  Callback.Function = callbackFunction;
  Callback.Parameter = pUserParam;

  s_state.callbacks.push_back(std::move(Callback));
}

void Log::UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam)
{
  std::unique_lock lock(s_state.callbacks_mutex);
  UnregisterCallback(callbackFunction, pUserParam, lock);
}

void Log::UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam,
                             const std::unique_lock<std::mutex>& lock)
{
  for (auto iter = s_state.callbacks.begin(); iter != s_state.callbacks.end(); ++iter)
  {
    if (iter->Function == callbackFunction && iter->Parameter == pUserParam)
    {
      s_state.callbacks.erase(iter);
      break;
    }
  }
}

const std::array<const char*, static_cast<size_t>(Log::Channel::MaxCount)>& Log::GetChannelNames()
{
  return s_log_channel_names;
}

float Log::GetCurrentMessageTime()
{
  return static_cast<float>(
    Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_state.start_timestamp));
}

bool Log::AreTimestampsEnabled()
{
  return s_state.console_output_timestamps || s_state.file_output_timestamp;
}

bool Log::IsConsoleOutputCurrentlyAvailable()
{
#ifdef _WIN32
  const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  return (h != NULL && h != INVALID_HANDLE_VALUE);
#elif defined(__ANDROID__)
  return false;
#else
  // standard output isn't really reliable because it could be redirected to a file. check standard input for tty.
  struct termios attr;
  return (tcgetattr(STDIN_FILENO, &attr) == 0);
#endif
}

bool Log::IsConsoleOutputEnabled()
{
  return s_state.console_output_enabled;
}

bool Log::IsDebugOutputEnabled()
{
  return s_state.debug_output_enabled;
}

void Log::ExecuteCallbacks(Channel channel, const char* functionName, Level level, std::string_view message,
                           const std::unique_lock<std::mutex>& lock)
{
  for (RegisteredCallback& callback : s_state.callbacks)
    callback.Function(callback.Parameter, s_log_channel_names[static_cast<size_t>(channel)], functionName, level,
                      message);
}

ALWAYS_INLINE_RELEASE void Log::FormatLogMessageForDisplay(fmt::memory_buffer& buffer, const char* channelName,
                                                           const char* functionName, Level level,
                                                           std::string_view message, bool timestamp,
                                                           bool ansi_color_code)
{
  static constexpr const std::array s_ansi_color_codes = {
    "\033[0m"sv,    // None
    "\033[1;31m"sv, // Error
    "\033[1;33m"sv, // Warning
    "\033[1;37m"sv, // Info
    "\033[1;32m"sv, // Verbose
    "\033[0;37m"sv, // Dev
    "\033[0;32m"sv, // Debug
    "\033[0;34m"sv, // Trace
  };

  std::string_view color_start = ansi_color_code ? s_ansi_color_codes[static_cast<size_t>(level)] : ""sv;
  std::string_view color_end = ansi_color_code ? s_ansi_color_codes[0] : ""sv;

  auto appender = std::back_inserter(buffer);

  if (timestamp)
  {
    // find time since start of process
    const float message_time = Log::GetCurrentMessageTime();

    // have to break it up into lines
    std::string_view::size_type start = 0;
    for (;;)
    {
      const std::string_view::size_type pos = message.find('\n', start);
      const std::string_view sub_message =
        (pos == std::string_view::npos) ? message.substr(start) : message.substr(start, pos - start);
      const std::string_view end_message = sub_message.ends_with('\n') ? ""sv : "\n"sv;

      if (functionName)
      {
        fmt::format_to(appender, "[{:10.4f}] {}{}({}): {}{}{}", message_time, color_start,
                       s_log_level_characters[static_cast<size_t>(level)], functionName, sub_message, color_end,
                       end_message);
      }
      else
      {
        fmt::format_to(appender, "[{:10.4f}] {}{}/{}: {}{}{}", message_time, color_start,
                       s_log_level_characters[static_cast<size_t>(level)], channelName, sub_message, color_end,
                       end_message);
      }

      if (pos != std::string_view::npos)
        start = pos + 1;
      else
        break;
    }
  }
  else
  {
    if (functionName)
    {
      fmt::format_to(appender, "{}{}({}): {}{}\n", color_start, s_log_level_characters[static_cast<size_t>(level)],
                     functionName, message, color_end);
    }
    else
    {
      fmt::format_to(appender, "{}{}/{}: {}{}\n", color_start, s_log_level_characters[static_cast<size_t>(level)],
                     channelName, message, color_end);
    }
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE void Log::FormatLogMessageAndPrint(const char* channelName, const char* functionName, Level level,
                                                         std::string_view message, bool timestamp, bool ansi_color_code,
                                                         const T& callback)
{
  fmt::memory_buffer buffer;
  FormatLogMessageForDisplay(buffer, channelName, functionName, level, message, timestamp, ansi_color_code);
  callback(std::string_view(buffer.data(), buffer.size()));
}

#ifdef _WIN32

template<typename T>
ALWAYS_INLINE_RELEASE void Log::FormatLogMessageAndPrintW(const char* channelName, const char* functionName,
                                                          Level level, std::string_view message, bool timestamp,
                                                          bool ansi_color_code, const T& callback)
{
  fmt::memory_buffer buffer;
  FormatLogMessageForDisplay(buffer, channelName, functionName, level, message, timestamp, ansi_color_code);

  // Convert to UTF-16 first so unicode characters display correctly. NT is going to do it
  // anyway...
  wchar_t wbuf[512];
  wchar_t* wmessage_buf = wbuf;
  int wmessage_buflen = static_cast<int>(std::size(wbuf) - 1);
  if (buffer.size() >= std::size(wbuf))
  {
    wmessage_buflen = static_cast<int>(buffer.size());
    wmessage_buf = static_cast<wchar_t*>(std::malloc((buffer.size() + 1) * sizeof(wchar_t)));
  }

  wmessage_buflen =
    MultiByteToWideChar(CP_UTF8, 0, buffer.data(), static_cast<int>(buffer.size()), wmessage_buf, wmessage_buflen);
  if (wmessage_buflen > 0) [[likely]]
  {
    wmessage_buf[wmessage_buflen] = '\0';
    callback(std::wstring_view(wmessage_buf, wmessage_buflen));
  }

  if (wmessage_buf != wbuf)
    std::free(wmessage_buf);
}

static bool EnableVirtualTerminalProcessing(HANDLE hConsole)
{
  DWORD old_mode;
  if (!GetConsoleMode(hConsole, &old_mode))
    return false;

  // already enabled?
  if (old_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
    return true;

  return SetConsoleMode(hConsole, old_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

#endif

void Log::ConsoleOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, Level level,
                                   std::string_view message)
{
  if (!s_state.console_output_enabled)
    return;

#if defined(_WIN32)
  FormatLogMessageAndPrintW(
    channelName, functionName, level, message, s_state.console_output_timestamps, true,
    [level](const std::wstring_view& message) {
      HANDLE hOutput = (level <= Level::Warning) ? s_state.hConsoleStdErr : s_state.hConsoleStdOut;
      DWORD chars_written;
      WriteConsoleW(hOutput, message.data(), static_cast<DWORD>(message.length()), &chars_written, nullptr);
    });
#elif !defined(__ANDROID__)
  FormatLogMessageAndPrint(channelName, functionName, level, message, s_state.console_output_timestamps, true,
                           [level](std::string_view message) {
                             const int outputFd = (level <= Log::Level::Warning) ? STDERR_FILENO : STDOUT_FILENO;
                             write(outputFd, message.data(), message.length());
                           });
#endif
}

void Log::DebugOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, Level level,
                                 std::string_view message)
{
  if (!s_state.debug_output_enabled)
    return;

#if defined(_WIN32)
  FormatLogMessageAndPrintW(channelName, functionName, level, message, false, false,
                            [](const std::wstring_view& message) { OutputDebugStringW(message.data()); });
#elif defined(__ANDROID__)
  if (message.empty())
    return;

  static constexpr int logPriority[static_cast<size_t>(Level::Count)] = {
    ANDROID_LOG_INFO,  // None
    ANDROID_LOG_ERROR, // Error
    ANDROID_LOG_WARN,  // Warning
    ANDROID_LOG_INFO,  // Info
    ANDROID_LOG_INFO,  // Verbose
    ANDROID_LOG_DEBUG, // Dev
    ANDROID_LOG_DEBUG, // Debug
    ANDROID_LOG_DEBUG, // Trace
  };

  __android_log_print(logPriority[static_cast<size_t>(level)], channelName, "%.*s", static_cast<int>(message.length()),
                      message.data());
#endif
}

void Log::SetConsoleOutputParams(bool enabled, bool timestamps)
{
  std::unique_lock lock(s_state.callbacks_mutex);

  s_state.console_output_timestamps = timestamps;
  if (s_state.console_output_enabled == enabled)
    return;

  s_state.console_output_enabled = enabled;

#if defined(_WIN32)
  // On windows, no console is allocated by default on a windows based application
  static bool console_was_allocated = false;
  static HANDLE old_stdin = NULL;
  static HANDLE old_stdout = NULL;
  static HANDLE old_stderr = NULL;

  if (enabled)
  {
    old_stdin = GetStdHandle(STD_INPUT_HANDLE);
    old_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    old_stderr = GetStdHandle(STD_ERROR_HANDLE);

    if (!old_stdout)
    {
      // Attach to the parent console if we're running from a command window
      if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole())
        return;

      s_state.hConsoleStdIn = GetStdHandle(STD_INPUT_HANDLE);
      s_state.hConsoleStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
      s_state.hConsoleStdErr = GetStdHandle(STD_ERROR_HANDLE);

      EnableVirtualTerminalProcessing(s_state.hConsoleStdOut);
      EnableVirtualTerminalProcessing(s_state.hConsoleStdErr);

      std::FILE* fp;
      freopen_s(&fp, "CONIN$", "r", stdin);
      freopen_s(&fp, "CONOUT$", "w", stdout);
      freopen_s(&fp, "CONOUT$", "w", stderr);

      console_was_allocated = true;
    }
    else
    {
      s_state.hConsoleStdIn = old_stdin;
      s_state.hConsoleStdOut = old_stdout;
      s_state.hConsoleStdErr = old_stderr;
    }
  }
  else
  {
    if (console_was_allocated)
    {
      console_was_allocated = false;

      std::FILE* fp;
      freopen_s(&fp, "NUL:", "w", stderr);
      freopen_s(&fp, "NUL:", "w", stdout);
      freopen_s(&fp, "NUL:", "w", stdin);

      SetStdHandle(STD_ERROR_HANDLE, old_stderr);
      SetStdHandle(STD_OUTPUT_HANDLE, old_stdout);
      SetStdHandle(STD_INPUT_HANDLE, old_stdin);

      s_state.hConsoleStdIn = NULL;
      s_state.hConsoleStdOut = NULL;
      s_state.hConsoleStdErr = NULL;

      FreeConsole();
    }
  }
#endif

  if (enabled)
    RegisterCallback(ConsoleOutputLogCallback, nullptr, lock);
  else
    UnregisterCallback(ConsoleOutputLogCallback, nullptr, lock);
}

void Log::SetDebugOutputParams(bool enabled)
{
  std::unique_lock lock(s_state.callbacks_mutex);
  if (s_state.debug_output_enabled == enabled)
    return;

  s_state.debug_output_enabled = enabled;
  if (enabled)
    RegisterCallback(DebugOutputLogCallback, nullptr, lock);
  else
    UnregisterCallback(DebugOutputLogCallback, nullptr, lock);
}

void Log::FileOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, Level level,
                                std::string_view message)
{
  if (!s_state.file_output_enabled)
    return;

  FormatLogMessageAndPrint(channelName, functionName, level, message, true, false, [](std::string_view message) {
    std::fwrite(message.data(), 1, message.size(), s_state.file_handle.get());
    std::fflush(s_state.file_handle.get());
  });
}

void Log::SetFileOutputParams(bool enabled, const char* filename, bool timestamps /* = true */)
{
  std::unique_lock lock(s_state.callbacks_mutex);
  if (s_state.file_output_enabled == enabled)
    return;

  if (enabled)
  {
    s_state.file_handle = FileSystem::OpenManagedCFile(filename, "wb");
    if (!s_state.file_handle) [[unlikely]]
    {
      ExecuteCallbacks(Log::Channel::Log, __FUNCTION__, Level::Error,
                       TinyString::from_format("Failed to open log file '{}'", filename), lock);
      return;
    }

    RegisterCallback(FileOutputLogCallback, nullptr, lock);
  }
  else
  {
    UnregisterCallback(FileOutputLogCallback, nullptr, lock);
    s_state.file_handle.reset();
  }

  s_state.file_output_enabled = enabled;
  s_state.file_output_timestamp = timestamps;
}

Log::Level Log::GetLogLevel()
{
  return s_state.log_level;
}

bool Log::IsLogVisible(Level level, Channel channel)
{
  return FilterTest(channel, level);
}

void Log::SetLogLevel(Level level)
{
  std::unique_lock lock(s_state.callbacks_mutex);
  DebugAssert(level < Level::MaxCount);
  s_state.log_level = level;
}

void Log::SetLogChannelEnabled(Channel channel, bool enabled)
{
  std::unique_lock lock(s_state.callbacks_mutex);
  DebugAssert(channel < Channel::MaxCount);
  s_state.log_channels_enabled[static_cast<size_t>(channel)] = enabled;
}

ALWAYS_INLINE_RELEASE bool Log::FilterTest(Channel channel, Level level)
{
  return (level <= s_state.log_level && s_state.log_channels_enabled[static_cast<size_t>(channel)]);
}

void Log::Write(PackedChannelAndLevel cat, std::string_view message)
{
  const Channel channel = UnpackLogChannel(cat);
  const Level level = UnpackLogLevel(cat);
  if (!FilterTest(channel, level))
    return;

  std::unique_lock lock(s_state.callbacks_mutex);
  ExecuteCallbacks(channel, nullptr, level, message, lock);
}

void Log::Write(PackedChannelAndLevel cat, const char* functionName, std::string_view message)
{
  const Channel channel = UnpackLogChannel(cat);
  const Level level = UnpackLogLevel(cat);
  if (!FilterTest(channel, level))
    return;

  std::unique_lock lock(s_state.callbacks_mutex);
  ExecuteCallbacks(channel, functionName, level, message, lock);
}

void Log::WriteFmtArgs(PackedChannelAndLevel cat, fmt::string_view fmt, fmt::format_args args)
{
  const Channel channel = UnpackLogChannel(cat);
  const Level level = UnpackLogLevel(cat);
  if (!FilterTest(channel, level))
    return;

  fmt::memory_buffer buffer;
  fmt::vformat_to(std::back_inserter(buffer), fmt, args);

  std::unique_lock lock(s_state.callbacks_mutex);
  ExecuteCallbacks(channel, nullptr, level, std::string_view(buffer.data(), buffer.size()), lock);
}

void Log::WriteFmtArgs(PackedChannelAndLevel cat, const char* functionName, fmt::string_view fmt, fmt::format_args args)
{
  const Channel channel = UnpackLogChannel(cat);
  const Level level = UnpackLogLevel(cat);
  if (!FilterTest(channel, level))
    return;

  fmt::memory_buffer buffer;
  fmt::vformat_to(std::back_inserter(buffer), fmt, args);

  std::unique_lock lock(s_state.callbacks_mutex);
  ExecuteCallbacks(channel, functionName, level, std::string_view(buffer.data(), buffer.size()), lock);
}
