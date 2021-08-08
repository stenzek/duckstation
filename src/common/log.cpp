#include "log.h"
#include "assert.h"
#include "file_system.h"
#include "string.h"
#include "timer.h"
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

namespace Log {

static const char s_log_level_characters[LOGLEVEL_COUNT] = {'X', 'E', 'W', 'P', 'I', 'V', 'D', 'R', 'B', 'T'};

struct RegisteredCallback
{
  CallbackFunctionType Function;
  void* Parameter;
};

std::vector<RegisteredCallback> s_callbacks;
static std::mutex s_callback_mutex;

static LOGLEVEL s_filter_level = LOGLEVEL_TRACE;

static Common::Timer::Value s_startTimeStamp = Common::Timer::GetValue();

static bool s_console_output_enabled = false;
static String s_console_output_channel_filter;
static LOGLEVEL s_console_output_level_filter = LOGLEVEL_TRACE;

#ifdef _WIN32
static HANDLE s_hConsoleStdIn = NULL;
static HANDLE s_hConsoleStdOut = NULL;
static HANDLE s_hConsoleStdErr = NULL;
#endif

static bool s_debug_output_enabled = false;
static String s_debug_output_channel_filter;
static LOGLEVEL s_debug_output_level_filter = LOGLEVEL_TRACE;

static bool s_file_output_enabled = false;
static bool s_file_output_timestamp = false;
static String s_file_output_channel_filter;
static LOGLEVEL s_file_output_level_filter = LOGLEVEL_TRACE;
std::unique_ptr<std::FILE, void (*)(std::FILE*)> s_fileOutputHandle(nullptr, [](std::FILE* fp) {
  if (fp)
  {
    std::fclose(fp);
  }
});

void RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam)
{
  RegisteredCallback Callback;
  Callback.Function = callbackFunction;
  Callback.Parameter = pUserParam;

  std::lock_guard<std::mutex> guard(s_callback_mutex);
  s_callbacks.push_back(std::move(Callback));
}

void UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam)
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

bool IsConsoleOutputEnabled()
{
  return s_console_output_enabled;
}

bool IsDebugOutputEnabled()
{
  return s_debug_output_enabled;
}

static void ExecuteCallbacks(const char* channelName, const char* functionName, LOGLEVEL level, const char* message)
{
  std::lock_guard<std::mutex> guard(s_callback_mutex);
  for (RegisteredCallback& callback : s_callbacks)
    callback.Function(callback.Parameter, channelName, functionName, level, message);
}

static int FormatLogMessageForDisplay(char* buffer, size_t buffer_size, const char* channelName,
                                      const char* functionName, LOGLEVEL level, const char* message, bool timestamp,
                                      bool ansi_color_code, bool newline)
{
  static const char* s_ansi_color_codes[LOGLEVEL_COUNT] = {
    "\033[0m",    // NONE
    "\033[1;31m", // ERROR
    "\033[1;33m", // WARNING
    "\033[1;35m", // PERF
    "\033[1;37m", // INFO
    "\033[1;32m", // VERBOSE
    "\033[0;37m", // DEV
    "\033[1;36m", // PROFILE
    "\033[0;32m", // DEBUG
    "\033[0;34m", // TRACE
  };

  const char* color_start = ansi_color_code ? s_ansi_color_codes[level] : "";
  const char* color_end = ansi_color_code ? s_ansi_color_codes[0] : "";
  const char* message_end = newline ? "\n" : "";

  if (timestamp)
  {
    // find time since start of process
    const float message_time =
      static_cast<float>(Common::Timer::ConvertValueToSeconds(Common::Timer::GetValue() - s_startTimeStamp));

    if (level <= LOGLEVEL_PERF)
    {
      return std::snprintf(buffer, buffer_size, "%s[%10.4f] %c(%s): %s%s%s", color_start, message_time,
                           s_log_level_characters[level], functionName, message, color_end, message_end);
    }
    else
    {
      return std::snprintf(buffer, buffer_size, "%s[%10.4f] %c/%s: %s%s%s", color_start, message_time,
                           s_log_level_characters[level], channelName, message, color_end, message_end);
    }
  }
  else
  {
    if (level <= LOGLEVEL_PERF)
    {
      return std::snprintf(buffer, buffer_size, "%s%c(%s): %s%s%s", color_start, s_log_level_characters[level],
                           functionName, message, color_end, message_end);
    }
    else
    {
      return std::snprintf(buffer, buffer_size, "%s%c/%s: %s%s%s", color_start, s_log_level_characters[level],
                           channelName, message, color_end, message_end);
    }
  }
}

template<typename T>
static ALWAYS_INLINE void FormatLogMessageAndPrint(const char* channelName, const char* functionName, LOGLEVEL level,
                                                   const char* message, bool timestamp, bool ansi_color_code,
                                                   bool newline, const T& callback)
{
  char buf[512];
  char* message_buf = buf;
  int message_len;
  if ((message_len = FormatLogMessageForDisplay(message_buf, sizeof(buf), channelName, functionName, level, message,
                                                timestamp, ansi_color_code, newline)) >
      static_cast<int>(sizeof(buf) - 1))
  {
    message_buf = static_cast<char*>(std::malloc(message_len + 1));
    message_len = FormatLogMessageForDisplay(message_buf, message_len + 1, channelName, functionName, level, message,
                                             timestamp, ansi_color_code, newline);
  }

  callback(message_buf, message_len);

  if (message_buf != buf)
    std::free(message_buf);
}

#ifdef _WIN32

template<typename T>
static ALWAYS_INLINE void FormatLogMessageAndPrintW(const char* channelName, const char* functionName, LOGLEVEL level,
                                                    const char* message, bool timestamp, bool ansi_color_code,
                                                    bool newline, const T& callback)
{
  char buf[512];
  char* message_buf = buf;
  int message_len;
  if ((message_len = FormatLogMessageForDisplay(message_buf, sizeof(buf), channelName, functionName, level, message,
                                                timestamp, ansi_color_code, newline)) > (sizeof(buf) - 1))
  {
    message_buf = static_cast<char*>(std::malloc(message_len + 1));
    message_len = FormatLogMessageForDisplay(message_buf, message_len + 1, channelName, functionName, level, message,
                                             timestamp, ansi_color_code, newline);
  }
  if (message_len <= 0)
    return;

  // Convert to UTF-16 first so unicode characters display correctly. NT is going to do it
  // anyway...
  wchar_t wbuf[512];
  wchar_t* wmessage_buf = wbuf;
  int wmessage_buflen = countof(wbuf) - 1;
  if (message_len >= countof(wbuf))
  {
    wmessage_buflen = message_len;
    wmessage_buf = static_cast<wchar_t*>(std::malloc((wmessage_buflen + 1) * sizeof(wchar_t)));
  }

  wmessage_buflen = MultiByteToWideChar(CP_UTF8, 0, message_buf, message_len, wmessage_buf, wmessage_buflen);
  if (wmessage_buflen <= 0)
    return;

  wmessage_buf[wmessage_buflen] = '\0';
  callback(wmessage_buf, wmessage_buflen);

  if (wmessage_buf != wbuf)
    std::free(wbuf);

  if (message_buf != buf)
    std::free(message_buf);
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
                                     LOGLEVEL level, const char* message)
{
  if (!s_console_output_enabled || level > s_console_output_level_filter ||
      s_console_output_channel_filter.Find(channelName) >= 0)
  {
    return;
  }

#if defined(_WIN32)
  FormatLogMessageAndPrintW(channelName, functionName, level, message, true, true, true,
                            [level](const wchar_t* message, int message_len) {
                              HANDLE hOutput = (level <= LOGLEVEL_WARNING) ? s_hConsoleStdErr : s_hConsoleStdOut;
                              DWORD chars_written;
                              WriteConsoleW(hOutput, message, message_len, &chars_written, nullptr);
                            });
#elif !defined(__ANDROID__)
  FormatLogMessageAndPrint(channelName, functionName, level, message, true, true, true,
                           [level](const char* message, int message_len) {
                             const int outputFd = (level <= LOGLEVEL_WARNING) ? STDERR_FILENO : STDOUT_FILENO;
                             write(outputFd, message, message_len);
                           });
#endif
}

static void DebugOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, LOGLEVEL level,
                                   const char* message)
{
  if (!s_debug_output_enabled || level > s_debug_output_level_filter ||
      s_debug_output_channel_filter.Find(functionName) >= 0)
  {
    return;
  }

#if defined(_WIN32)
  FormatLogMessageAndPrintW(channelName, functionName, level, message, true, false, true,
                            [](const wchar_t* message, int message_len) { OutputDebugStringW(message); });
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

void SetConsoleOutputParams(bool Enabled, const char* ChannelFilter, LOGLEVEL LevelFilter)
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

void SetDebugOutputParams(bool enabled, const char* channelFilter /* = nullptr */,
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
                                  const char* message)
{
  if (level > s_file_output_level_filter || s_file_output_channel_filter.Find(channelName) >= 0)
    return;

  FormatLogMessageAndPrint(
    channelName, functionName, level, message, true, false, true,
    [](const char* message, int message_len) { std::fwrite(message, 1, message_len, s_fileOutputHandle.get()); });
}

void SetFileOutputParams(bool enabled, const char* filename, bool timestamps /* = true */,
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

void SetFilterLevel(LOGLEVEL level)
{
  DebugAssert(level < LOGLEVEL_COUNT);
  s_filter_level = level;
}

void Write(const char* channelName, const char* functionName, LOGLEVEL level, const char* message)
{
  if (level > s_filter_level)
    return;

  ExecuteCallbacks(channelName, functionName, level, message);
}

void Writef(const char* channelName, const char* functionName, LOGLEVEL level, const char* format, ...)
{
  if (level > s_filter_level)
    return;

  va_list ap;
  va_start(ap, format);
  Writev(channelName, functionName, level, format, ap);
  va_end(ap);
}

void Writev(const char* channelName, const char* functionName, LOGLEVEL level, const char* format, va_list ap)
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

} // namespace Log
