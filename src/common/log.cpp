#include "log.h"
#include "assert.h"
#include "file_system.h"
#include "string.h"
#include "timer.h"
#include <cstdio>
#include <mutex>
#include <vector>

#if defined(WIN32)
#include "windows_headers.h"
#elif defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
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

static bool s_consoleOutputEnabled = false;
static String s_consoleOutputChannelFilter;
static LOGLEVEL s_consoleOutputLevelFilter = LOGLEVEL_TRACE;

static bool s_debugOutputEnabled = false;
static String s_debugOutputChannelFilter;
static LOGLEVEL s_debugOutputLevelFilter = LOGLEVEL_TRACE;

static bool s_fileOutputEnabled = false;
static bool s_fileOutputTimestamp = false;
static String s_fileOutputChannelFilter;
static LOGLEVEL s_fileOutputLevelFilter = LOGLEVEL_TRACE;
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
  return s_consoleOutputEnabled;
}

bool IsDebugOutputEnabled()
{
  return s_debugOutputEnabled;
}

static void ExecuteCallbacks(const char* channelName, const char* functionName, LOGLEVEL level, const char* message)
{
  std::lock_guard<std::mutex> guard(s_callback_mutex);
  for (RegisteredCallback& callback : s_callbacks)
    callback.Function(callback.Parameter, channelName, functionName, level, message);
}

static void FormatLogMessageForDisplay(const char* channelName, const char* functionName, LOGLEVEL level,
                                       const char* message, void (*printCallback)(const char*, void*),
                                       void* pCallbackUserData, bool timestamp = true)
{
  if (timestamp)
  {
    // find time since start of process
    float messageTime =
      static_cast<float>(Common::Timer::ConvertValueToSeconds(Common::Timer::GetValue() - s_startTimeStamp));

    // write prefix
    char prefix[256];
    if (level <= LOGLEVEL_PERF)
      std::snprintf(prefix, countof(prefix), "[%10.4f] %c(%s): ", messageTime, s_log_level_characters[level],
                    functionName);
    else
      std::snprintf(prefix, countof(prefix), "[%10.4f] %c/%s: ", messageTime, s_log_level_characters[level],
                    channelName);

    printCallback(prefix, pCallbackUserData);
  }
  else
  {
    // write prefix
    char prefix[256];
    if (level <= LOGLEVEL_PERF)
      std::snprintf(prefix, countof(prefix), "%c(%s): ", s_log_level_characters[level], functionName);
    else
      std::snprintf(prefix, countof(prefix), "%c/%s: ", s_log_level_characters[level], channelName);

    printCallback(prefix, pCallbackUserData);
  }

  // write message
  printCallback(message, pCallbackUserData);
}

#if defined(_WIN32)
// Windows obscures access to POSIX style write() and file handles.
#include <io.h>
#define STDOUT_FILENO (_fileno(stdout))
#define STDERR_FILENO (_fileno(stderr))
#define write(fd, buf, count) _write(fd, buf, (int)count)
#endif

static void StandardOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName,
                                      LOGLEVEL level, const char* message)
{
  if (!s_consoleOutputEnabled || level > s_consoleOutputLevelFilter ||
      s_consoleOutputChannelFilter.Find(channelName) >= 0)
    return;

  static const char* const colorCodes[LOGLEVEL_COUNT] = {
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

  if (int outputFd = (level <= LOGLEVEL_WARNING) ? STDERR_FILENO : STDOUT_FILENO; outputFd >= 0)
  {
    write(outputFd, colorCodes[level], std::strlen(colorCodes[level]));

    Log::FormatLogMessageForDisplay(
      channelName, functionName, level, message,
      [](const char* text, void* outputFd) { write((int)(intptr_t)outputFd, text, std::strlen(text)); },
      (void*)(intptr_t)outputFd);

    write(outputFd, colorCodes[0], std::strlen(colorCodes[0]));
    write(outputFd, "\n", 1);
  }
}

#if defined(_WIN32)
static bool s_msw_console_allocated = false;
static HANDLE s_msw_prev_stdin = {};
static HANDLE s_msw_prev_stdout = {};
static HANDLE s_msw_prev_stderr = {};

#include <fcntl.h>

static void msw_ReopenStandardPipes()
{
  if (s_msw_console_allocated)
    return;

  s_msw_console_allocated = true;

  // By affecting only unbound pipes, it allows the program to accept input from stdin or honor
  // tee of stdout/stderr. Typical use case from GitBash terminal is to use `tee` to filter and pipe
  // several different levels of trace into various files, all very neat and fast and not requiring
  // any modification to the emulator beyond allowing for basic standard pipe redirection to work in
  // the way it was designed to work over 40 yrs ago.

  // open outputs as binary to suppress Windows newline corruption (\r mess)
  std::FILE* fp;
  if (!s_msw_prev_stdin)
  {
    freopen_s(&fp, "CONIN$", "r", stdin);
  }
  if (!s_msw_prev_stdout)
  {
    freopen_s(&fp, "CONOUT$", "wb", stdout);
  }
  if (!s_msw_prev_stderr)
  {
    freopen_s(&fp, "CONOUT$", "wb", stderr);
  }

  // Windows Console Oddities - The only way to get windows built-in console is to render UTF chars from
  // the correct alt. fonts is to set either _O_U8TEXT or _O_U16TEXT. However, this imposes a requirement
  // that we must write UTF16 to the console using widechar versions of printf and friends (eg, wprintf)...
  // EVEN IF YOU WANT TO USE UTF8. Worse, printf() doesn't do the smart thing and assume UTF8 and then
  // convert it to UTF16 for us when the output file is in U16TEXT mode. Nope! It throws an ASSERTION and
  // forces us to call wprintf, which makes this all totally useless and not cross-platform.

  // Lesson: if you want nice UTF font display in your console window, don't use Windows Console.
  // Use mintty or conemu instead.

  //_setmode(_fileno(stdout), _O_U8TEXT);
  //_setmode(_fileno(stderr), _O_U8TEXT);
}

static void msw_FreeLegacyConsole()
{
  if (!s_msw_console_allocated)
    return;

  s_msw_console_allocated = false;

  // clear C file handles to the console, otherwise FreeConsole() fails.
  std::FILE* fp;
  if (!s_msw_prev_stdin)
    freopen_s(&fp, "NUL:", "w", stdin);
  if (!s_msw_prev_stdout)
    freopen_s(&fp, "NUL:", "w", stdout);
  if (!s_msw_prev_stderr)
    freopen_s(&fp, "NUL:", "w", stderr);

  // restore previous handles prior to creating the console.
  ::SetStdHandle(STD_INPUT_HANDLE, s_msw_prev_stdin);
  ::SetStdHandle(STD_OUTPUT_HANDLE, s_msw_prev_stdout);
  ::SetStdHandle(STD_ERROR_HANDLE, s_msw_prev_stderr);

  ::FreeConsole();
}

static bool msw_AttachLegacyConsole()
{
  if (::AttachConsole(ATTACH_PARENT_PROCESS))
    return true;

  // ERROR_ACCESS_DENIED means a windows Console is already attached.
  if (auto err = ::GetLastError(); err == ERROR_ACCESS_DENIED)
  {
    return true;
  }
  return false;
}

static bool msw_EnableVirtualTerminalProcessing()
{
  HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!hConsole)
    return false;

  DWORD old_mode;
  if (!GetConsoleMode(hConsole, &old_mode))
    return false;

  // already enabled?
  if (old_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
    return true;

  return SetConsoleMode(hConsole, old_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

// Creates an old-fashioned console window.
static bool msw_AllocLegacyConsole()
{
  // A potentially fancy solution which I haven't had time to experiment with yet is to spawn our own
  // terminal application and bind our standard pipes to it, instead of using AllocConsole(). This would
  // allow binding to any number of more modern terminal/console apps, all of which handle UTF8 better
  // than the windows legacy console (but would also depend on the user having them installed and PATH
  // accessible, so definitely not without annoying caveats) --jstine

  if (!::AllocConsole())
  {
    // Console could fail to allocate on an Appveyor/Jenkins environment, for example, because
    // when being run as a service the console may be unable to bind itself to a user login session.
    // It may also fail if a console is already allocated <-- this is a problem since in this case
    // we still want to set

    if (auto err = ::GetLastError(); err == ERROR_ACCESS_DENIED)
    {
      // ERROR_ACCESS_DENIED means a windows Console is already attached.
      // whatever the console is, who knows, so let's early-out, and not mess with its font settings.
      return true;
    }
  }

  return true;
}

static void msw_DebugOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName,
                                       LOGLEVEL level, const char* message)
{
  FormatLogMessageForDisplay(
    channelName, functionName, level, message, [](const char* text, void*) { OutputDebugStringA(text); }, nullptr);

  OutputDebugStringA("\n");
}
#endif

#if defined(__ANDROID__)
static void android_DebugOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName,
                                           LOGLEVEL level, const char* message)
{
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
}
#endif

static void DebugOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, LOGLEVEL level,
                                   const char* message)
{
  if (!s_debugOutputEnabled || level > s_debugOutputLevelFilter || s_debugOutputChannelFilter.Find(channelName) >= 0)
    return;

#if defined(_WIN32)
  msw_DebugOutputLogCallback(pUserParam, channelName, functionName, level, message);
#endif

#if defined(__ANDROID__)
  android_DebugOutputLogCallback(pUserParam, channelName, functionName, level, message);
#endif
}

void SetConsoleOutputParams(bool Enabled, const char* ChannelFilter, LOGLEVEL LevelFilter)
{
  s_consoleOutputChannelFilter = (ChannelFilter != NULL) ? ChannelFilter : "";
  s_consoleOutputLevelFilter = LevelFilter;

  if (s_consoleOutputEnabled == Enabled)
    return;

  s_consoleOutputEnabled = Enabled;

  if (Enabled)
    RegisterCallback(StandardOutputLogCallback, NULL);
  else
    UnregisterCallback(StandardOutputLogCallback, NULL);

#if defined(_WIN32) && !defined(_CONSOLE)
  if (Enabled)
  {
    // Windows Console behavior is very tricky, and depends on:
    //  - Whether the application is built with defined(_CONSOLE) or not.
    //  - Whether the application is started via a Microsoft shell (Cmd.exe) or a Unix'y shell
    //    (MSYS2, Git Bash, ConEum, ConsoleX, etc)
    //  - The instance of the MSVCRT currently in-use, which depends on whether the code is run
    //    from a DLL and whether that DLL was linked with static or dynamic CRT runtimes.
    //  - if the DLL uses dynamic CRT, then behavior also depends on whether that dynamic CRT version
    //    matches the one used by the main program.
    //
    // To maintain some level of personal sanity, I'll disregard all the DLL/CRT caveats for now.
    //
    // Console Mode (_CONSOLE) vs Windowed Application
    //   Microsoft CMD.EXE "does us a favor" and DETACHES the standard console pipes when it spawns
    //   windowed applications, but only if redirections are not specified at the command line.
    //   This creates all kinds of confusion and havok that could easy fill pages of the screen with
    //   comments. The TL;DR version is:
    //    - only call AllocConsole() if the stdout/stderr pipes are DETACHED (null) - this avoids
    //      clobbering pipe redirections specified from any shell (cmd/bash) and avoids creating
    //      spurious console windows when running from MSYS/ConEmu/GitBash.
    //    - Only use Microsoft's over-engineered Console text-coloring APIs if we called AllocConsole,
    //      because those APIs result in a whole lot of black screen if you call them while attached to
    //      a terminal app (ConEmu, ConsoleX, etc).
    //    - Ignore all of this if defined(_CONSOLE), in that case the OS behavior straightforward and a
    //      console is always allocated/attached. This is its own annoyance, and thus why few devs use
    //      it, even for console apps, because actually we DON'T want the console window popping up
    //      every time we run some console app in the background. --jstine

    s_msw_prev_stdin = ::GetStdHandle(STD_INPUT_HANDLE);
    s_msw_prev_stdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
    s_msw_prev_stderr = ::GetStdHandle(STD_ERROR_HANDLE);

    if (!s_msw_prev_stdout || !s_msw_prev_stdin)
    {
      if (msw_AttachLegacyConsole() || msw_AllocLegacyConsole())
      {
        msw_EnableVirtualTerminalProcessing();
        msw_ReopenStandardPipes();
      }
    }
  }
  else
  {
    msw_FreeLegacyConsole();
  }
#endif
}

void SetDebugOutputParams(bool enabled, const char* channelFilter /* = nullptr */,
                          LOGLEVEL levelFilter /* = LOGLEVEL_TRACE */)
{
  if (s_debugOutputEnabled != enabled)
  {
    s_debugOutputEnabled = enabled;
    if (enabled)
      RegisterCallback(DebugOutputLogCallback, nullptr);
    else
      UnregisterCallback(DebugOutputLogCallback, nullptr);
  }

  s_debugOutputChannelFilter = (channelFilter != nullptr) ? channelFilter : "";
  s_debugOutputLevelFilter = levelFilter;
}

static void FileOutputLogCallback(void* pUserParam, const char* channelName, const char* functionName, LOGLEVEL level,
                                  const char* message)
{
  if (level > s_fileOutputLevelFilter || s_fileOutputChannelFilter.Find(channelName) >= 0)
    return;

  if (s_fileOutputTimestamp)
  {
    // find time since start of process
    float messageTime =
      static_cast<float>(Common::Timer::ConvertValueToSeconds(Common::Timer::GetValue() - s_startTimeStamp));

    // write prefix
    if (level <= LOGLEVEL_PERF)
    {
      std::fprintf(s_fileOutputHandle.get(), "[%10.4f] %c(%s): %s\n", messageTime, s_log_level_characters[level],
                   functionName, message);
    }
    else
    {
      std::fprintf(s_fileOutputHandle.get(), "[%10.4f] %c/%s: %s\n", messageTime, s_log_level_characters[level],
                   channelName, message);
    }
  }
  else
  {
    if (level <= LOGLEVEL_PERF)
    {
      std::fprintf(s_fileOutputHandle.get(), "%c(%s): %s\n", s_log_level_characters[level], functionName, message);
    }
    else
    {
      std::fprintf(s_fileOutputHandle.get(), "%c/%s: %s\n", s_log_level_characters[level], channelName, message);
    }
  }
}

void SetFileOutputParams(bool enabled, const char* filename, bool timestamps /* = true */,
                         const char* channelFilter /* = nullptr */, LOGLEVEL levelFilter /* = LOGLEVEL_TRACE */)
{
  if (s_fileOutputEnabled != enabled)
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

    s_fileOutputEnabled = enabled;
  }

  std::lock_guard<std::mutex> guard(s_callback_mutex);
  s_fileOutputChannelFilter = (channelFilter != nullptr) ? channelFilter : "";
  ;
  s_fileOutputLevelFilter = levelFilter;
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

#ifdef WIN32
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
