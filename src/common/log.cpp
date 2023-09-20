// SPDX-FileCopyrightText: 2019-2022 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "log.h"
#include "assert.h"
#include "file_system.h"
#include "small_string.h"
#include "timer.h"

#include "fmt/format.h"

#include <cstdio>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include "windows_headers.h"
#elif defined(__ANDROID__)
#include <android/log.h>
#else
#include <unistd.h>
#endif

using namespace std::string_view_literals;

static const char s_log_level_characters[LOGLEVEL_COUNT] = {'X', 'E', 'W', 'P', 'I', 'V', 'D', 'R', 'B', 'T'};

struct RegisteredCallback
{
  Log::CallbackFunctionType Function;
  void* Parameter;
};

std::vector<RegisteredCallback> s_callbacks;
static std::mutex s_callback_mutex;

static LOGLEVEL s_filter_level = LOGLEVEL_TRACE;

static Common::Timer::Value s_startTimeStamp = Common::Timer::GetCurrentValue();

static bool s_console_output_enabled = false;
static std::string s_console_output_channel_filter;
static LOGLEVEL s_console_output_level_filter = LOGLEVEL_TRACE;

#ifdef _WIN32
static HANDLE s_hConsoleStdIn = NULL;
static HANDLE s_hConsoleStdOut = NULL;
static HANDLE s_hConsoleStdErr = NULL;
#endif

static bool s_debug_output_enabled = false;
static std::string s_debug_output_channel_filter;
static LOGLEVEL s_debug_output_level_filter = LOGLEVEL_TRACE;

static bool s_file_output_enabled = false;
static bool s_file_output_timestamp = false;
static std::string s_file_output_channel_filter;
static LOGLEVEL s_file_output_level_filter = LOGLEVEL_TRACE;
std::unique_ptr<std::FILE, void (*)(std::FILE*)> s_fileOutputHandle(nullptr, [](std::FILE* fp) {
  if (fp)
  {
    std::fclose(fp);
  }
});

void Log::RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam)
{
  RegisteredCallback Callback;
  Callback.Function = callbackFunction;
  Callback.Parameter = pUserParam;

  std::lock_guard<std::mutex> guard(s_callback_mutex);
  s_callbacks.push_back(std::move(Callback));
}

void Log::UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam)
{
  std::lock_guard<std::mutex> guard(s_callback_mutex);

  for (auto iter = s_callbacks.begin(); iter != s_callbacks.end(); ++iter)
  {
    if (iter->Function == callbackFunction && iter->Parameter == pUserParam)
    {
      s_callbacks.erase(iter);
      break;
    }
  }
}

bool Log::IsConsoleOutputEnabled()
{
  return s_console_output_enabled;
}

bool Log::IsDebugOutputEnabled()
{
  return s_debug_output_enabled;
}

static void ExecuteCallbacks(const char* channelName, const char* functionName, LOGLEVEL level,
                             std::string_view message)
{
  std::lock_guard<std::mutex> guard(s_callback_mutex);
  for (RegisteredCallback& callback : s_callbacks)
    callback.Function(callback.Parameter, channelName, functionName, level, message);
}

ALWAYS_INLINE_RELEASE static void FormatLogMessageForDisplay(fmt::memory_buffer& buffer, const char* channelName,
                                                             const char* functionName, LOGLEVEL level,
                                                             std::string_view message, bool timestamp,
                                                             bool ansi_color_code, bool newline)
{
  static constexpr std::string_view s_ansi_color_codes[LOGLEVEL_COUNT] = {
    "\033[0m"sv,    // NONE
    "\033[1;31m"sv, // ERROR
    "\033[1;33m"sv, // WARNING
    "\033[1;35m"sv, // PERF
    "\033[1;37m"sv, // INFO
    "\033[1;32m"sv, // VERBOSE
    "\033[0;37m"sv, // DEV
    "\033[1;36m"sv, // PROFILE
    "\033[0;32m"sv, // DEBUG
    "\033[0;34m"sv, // TRACE
  };

  std::string_view color_start = ansi_color_code ? s_ansi_color_codes[level] : ""sv;
  std::string_view color_end = ansi_color_code ? s_ansi_color_codes[0] : ""sv;
  std::string_view message_end = newline ? "\n"sv : ""sv;

  auto appender = std::back_inserter(buffer);

  if (timestamp)
  {
    // find time since start of process
    const float message_time =
      static_cast<float>(Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - s_startTimeStamp));

    if (level <= LOGLEVEL_PERF)
    {
      fmt::format_to(appender, "[{:10.4f}] {}{}({}): {}{}{}", message_time, color_start, s_log_level_characters[level],
                     functionName, message, color_end, message_end);
    }
    else
    {
      fmt::format_to(appender, "[{:10.4f}] {}{}/{}: {}{}{}", message_time, color_start, s_log_level_characters[level],
                     channelName, message, color_end, message_end);
    }
  }
  else
  {
    if (level <= LOGLEVEL_PERF)
    {
      fmt::format_to(appender, "{}{}({}): {}{}{}", color_start, s_log_level_characters[level], functionName, message,
                     color_end, message_end);
    }
    else
    {
      fmt::format_to(appender, "{}{}/{}: {}{}{}", color_start, s_log_level_characters[level], channelName, message,
                     color_end, message_end);
    }
  }
}

template<typename T>
ALWAYS_INLINE_RELEASE static void FormatLogMessageAndPrint(const char* channelName, const char* functionName,
                                                           LOGLEVEL level, std::string_view message, bool timestamp,
                                                           bool ansi_color_code, bool newline, const T& callback)
{
  fmt::memory_buffer buffer;
  FormatLogMessageForDisplay(buffer, channelName, functionName, level, message, timestamp, ansi_color_code, newline);
  callback(std::string_view(buffer.data(), buffer.size()));
}

#ifdef _WIN32

template<typename T>
ALWAYS_INLINE_RELEASE static void FormatLogMessageAndPrintW(const char* channelName, const char* functionName,
                                                            LOGLEVEL level, std::string_view message, bool timestamp,
                                                            bool ansi_color_code, bool newline, const T& callback)
{
  fmt::memory_buffer buffer;
  FormatLogMessageForDisplay(buffer, channelName, functionName, level, message, timestamp, ansi_color_code, newline);

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
  if (wmessage_buflen <= 0)
    return;

  wmessage_buf[wmessage_buflen] = '\0';
  callback(std::wstring_view(wmessage_buf, wmessage_buflen));

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

static void ConsoleOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName,
                                     LOGLEVEL level, std::string_view message)
{
  if (!s_console_output_enabled || level > s_console_output_level_filter ||
      s_console_output_channel_filter.find(channelName) != std::string::npos)
  {
    return;
  }

#if defined(_WIN32)
  FormatLogMessageAndPrintW(
    channelName, functionName, level, message, true, true, true, [level](const std::wstring_view& message) {
      HANDLE hOutput = (level <= LOGLEVEL_WARNING) ? s_hConsoleStdErr : s_hConsoleStdOut;
      DWORD chars_written;
      WriteConsoleW(hOutput, message.data(), static_cast<DWORD>(message.length()), &chars_written, nullptr);
    });
#elif !defined(__ANDROID__)
  FormatLogMessageAndPrint(channelName, functionName, level, message, true, true, true,
                           [level](const std::string_view& message) {
                             const int outputFd = (level <= LOGLEVEL_WARNING) ? STDERR_FILENO : STDOUT_FILENO;
                             write(outputFd, message.data(), message.length());
                           });
#endif
}

static void DebugOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, LOGLEVEL level,
                                   std::string_view message)
{
  if (!s_debug_output_enabled || level > s_debug_output_level_filter ||
      s_debug_output_channel_filter.find(functionName) != std::string::npos)
  {
    return;
  }

#if defined(_WIN32)
  FormatLogMessageAndPrintW(channelName, functionName, level, message, true, false, true,
                            [](const std::wstring_view& message) { OutputDebugStringW(message.data()); });
#elif defined(__ANDROID__)
  static const int logPriority[LOGLEVEL_COUNT] = {
    ANDROID_LOG_INFO,  // NONE
    ANDROID_LOG_ERROR, // ERROR
    ANDROID_LOG_WARN,  // WARNING
    ANDROID_LOG_INFO,  // PERF
    ANDROID_LOG_INFO,  // INFO
    ANDROID_LOG_INFO,  // VERBOSE
    ANDROID_LOG_DEBUG, // DEV
    ANDROID_LOG_DEBUG, // PROFILE
    ANDROID_LOG_DEBUG, // DEBUG
    ANDROID_LOG_DEBUG, // TRACE
  };

  __android_log_write(logPriority[level], channelName, message);
#else
#endif
}

void Log::SetConsoleOutputParams(bool Enabled, const char* ChannelFilter, LOGLEVEL LevelFilter)
{
  s_console_output_channel_filter = (ChannelFilter != NULL) ? ChannelFilter : "";
  s_console_output_level_filter = LevelFilter;

  if (s_console_output_enabled == Enabled)
    return;

  s_console_output_enabled = Enabled;

#if defined(_WIN32)
  // On windows, no console is allocated by default on a windows based application
  static bool console_was_allocated = false;
  static HANDLE old_stdin = NULL;
  static HANDLE old_stdout = NULL;
  static HANDLE old_stderr = NULL;

  if (Enabled)
  {
    old_stdin = GetStdHandle(STD_INPUT_HANDLE);
    old_stdout = GetStdHandle(STD_OUTPUT_HANDLE);
    old_stderr = GetStdHandle(STD_ERROR_HANDLE);

    if (!old_stdout)
    {
      // Attach to the parent console if we're running from a command window
      if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole())
        return;

      s_hConsoleStdIn = GetStdHandle(STD_INPUT_HANDLE);
      s_hConsoleStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
      s_hConsoleStdErr = GetStdHandle(STD_ERROR_HANDLE);

      EnableVirtualTerminalProcessing(s_hConsoleStdOut);
      EnableVirtualTerminalProcessing(s_hConsoleStdErr);

      std::FILE* fp;
      freopen_s(&fp, "CONIN$", "r", stdin);
      freopen_s(&fp, "CONOUT$", "w", stdout);
      freopen_s(&fp, "CONOUT$", "w", stderr);

      console_was_allocated = true;
    }
    else
    {
      s_hConsoleStdIn = old_stdin;
      s_hConsoleStdOut = old_stdout;
      s_hConsoleStdErr = old_stderr;
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

      s_hConsoleStdIn = NULL;
      s_hConsoleStdOut = NULL;
      s_hConsoleStdErr = NULL;

      FreeConsole();
    }
  }
#endif

  if (Enabled)
    RegisterCallback(ConsoleOutputLogCallback, nullptr);
  else
    UnregisterCallback(ConsoleOutputLogCallback, nullptr);
}

void Log::SetDebugOutputParams(bool enabled, const char* channelFilter /* = nullptr */,
                               LOGLEVEL levelFilter /* = LOGLEVEL_TRACE */)
{
  if (s_debug_output_enabled != enabled)
  {
    s_debug_output_enabled = enabled;
    if (enabled)
      RegisterCallback(DebugOutputLogCallback, nullptr);
    else
      UnregisterCallback(DebugOutputLogCallback, nullptr);
  }

  s_debug_output_channel_filter = (channelFilter != nullptr) ? channelFilter : "";
  s_debug_output_level_filter = levelFilter;
}

static void FileOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, LOGLEVEL level,
                                  std::string_view message)
{
  if (level > s_file_output_level_filter || s_file_output_channel_filter.find(channelName) != std::string::npos)
    return;

  FormatLogMessageAndPrint(
    channelName, functionName, level, message, true, false, true,
    [](const std::string_view& message) { std::fwrite(message.data(), 1, message.size(), s_fileOutputHandle.get()); });
}

void Log::SetFileOutputParams(bool enabled, const char* filename, bool timestamps /* = true */,
                              const char* channelFilter /* = nullptr */, LOGLEVEL levelFilter /* = LOGLEVEL_TRACE */)
{
  if (s_file_output_enabled != enabled)
  {
    if (enabled)
    {
      s_fileOutputHandle.reset(FileSystem::OpenCFile(filename, "wb"));
      if (!s_fileOutputHandle)
      {
        Log::Writef("Log", __FUNCTION__, LOGLEVEL_ERROR, "Failed to open log file '%s'", filename);
        return;
      }

      RegisterCallback(FileOutputLogCallback, nullptr);
    }
    else
    {
      UnregisterCallback(FileOutputLogCallback, nullptr);
      s_fileOutputHandle.reset();
    }

    s_file_output_enabled = enabled;
  }

  std::lock_guard<std::mutex> guard(s_callback_mutex);
  s_file_output_channel_filter = (channelFilter != nullptr) ? channelFilter : "";
  s_file_output_level_filter = levelFilter;
  s_file_output_timestamp = timestamps;
}

void Log::SetFilterLevel(LOGLEVEL level)
{
  DebugAssert(level < LOGLEVEL_COUNT);
  s_filter_level = level;
}

void Log::Write(const char* channelName, const char* functionName, LOGLEVEL level, std::string_view message)
{
  if (level > s_filter_level)
    return;

  ExecuteCallbacks(channelName, functionName, level, message);
}

void Log::Writef(const char* channelName, const char* functionName, LOGLEVEL level, const char* format, ...)
{
  if (level > s_filter_level)
    return;

  va_list ap;
  va_start(ap, format);
  Writev(channelName, functionName, level, format, ap);
  va_end(ap);
}

void Log::Writev(const char* channelName, const char* functionName, LOGLEVEL level, const char* format, va_list ap)
{
  if (level > s_filter_level)
    return;

  va_list apCopy;
  va_copy(apCopy, ap);

#ifdef _WIN32
  u32 requiredSize = static_cast<u32>(_vscprintf(format, apCopy));
#else
  u32 requiredSize = std::vsnprintf(nullptr, 0, format, apCopy);
#endif
  va_end(apCopy);

  if (requiredSize < 256)
  {
    char buffer[256];
    std::vsnprintf(buffer, countof(buffer), format, ap);
    ExecuteCallbacks(channelName, functionName, level, buffer);
  }
  else
  {
    char* buffer = new char[requiredSize + 1];
    std::vsnprintf(buffer, requiredSize + 1, format, ap);
    ExecuteCallbacks(channelName, functionName, level, buffer);
    delete[] buffer;
  }
}

void Log::WriteFmtArgs(const char* channelName, const char* functionName, LOGLEVEL level, fmt::string_view fmt,
                       fmt::format_args args)
{
  if (level > s_filter_level)
    return;

  fmt::memory_buffer buffer;
  fmt::vformat_to(std::back_inserter(buffer), fmt, args);

  ExecuteCallbacks(channelName, functionName, level, std::string_view(buffer.data(), buffer.size()));
}
