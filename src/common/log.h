// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include "fmt/base.h"

#include <cinttypes>
#include <cstdarg>
#include <mutex>
#include <string_view>

enum LOGLEVEL
{
  LOGLEVEL_NONE, // Silences all log traffic
  LOGLEVEL_ERROR,
  LOGLEVEL_WARNING,
  LOGLEVEL_INFO,
  LOGLEVEL_VERBOSE,
  LOGLEVEL_DEV,
  LOGLEVEL_DEBUG,
  LOGLEVEL_TRACE,

  LOGLEVEL_COUNT
};

namespace Log {
// log message callback type
using CallbackFunctionType = void (*)(void* pUserParam, const char* channelName, const char* functionName,
                                      LOGLEVEL level, std::string_view message);

// registers a log callback
void RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam);

// unregisters a log callback
void UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam);

// returns the time in seconds since the start of the process
float GetCurrentMessageTime();

// adds a standard console output
bool IsConsoleOutputCurrentlyAvailable();
bool IsConsoleOutputEnabled();
void SetConsoleOutputParams(bool enabled, bool timestamps = true);

// adds a debug console output [win32/android only]
bool IsDebugOutputEnabled();
void SetDebugOutputParams(bool enabled);

// adds a file output
void SetFileOutputParams(bool enabled, const char* filename, bool timestamps = true);

// Returns the current global filtering level.
LOGLEVEL GetLogLevel();

// Returns true if log messages for the specified log level/filter would not be filtered (and visible).
bool IsLogVisible(LOGLEVEL level, const char* channelName);

// Sets global filtering level, messages below this level won't be sent to any of the logging sinks.
void SetLogLevel(LOGLEVEL level);

// Sets global filter, any messages from these channels won't be sent to any of the logging sinks.
void SetLogFilter(std::string_view filter);

// writes a message to the log
void Write(const char* channelName, LOGLEVEL level, std::string_view message);
void Write(const char* channelName, const char* functionName, LOGLEVEL level, std::string_view message);
void WriteFmtArgs(const char* channelName, LOGLEVEL level, fmt::string_view fmt, fmt::format_args args);
void WriteFmtArgs(const char* channelName, const char* functionName, LOGLEVEL level, fmt::string_view fmt,
                  fmt::format_args args);

ALWAYS_INLINE static void FastWrite(const char* channelName, LOGLEVEL level, std::string_view message)
{
  if (level <= GetLogLevel()) [[unlikely]]
    Write(channelName, level, message);
}
ALWAYS_INLINE static void FastWrite(const char* channelName, const char* functionName, LOGLEVEL level,
                                    std::string_view message)
{
  if (level <= GetLogLevel()) [[unlikely]]
    Write(channelName, functionName, level, message);
}
template<typename... T>
ALWAYS_INLINE static void FastWrite(const char* channelName, LOGLEVEL level, fmt::format_string<T...> fmt, T&&... args)
{
  if (level <= GetLogLevel()) [[unlikely]]
    WriteFmtArgs(channelName, level, fmt, fmt::make_format_args(args...));
}
template<typename... T>
ALWAYS_INLINE static void FastWrite(const char* channelName, const char* functionName, LOGLEVEL level,
                                    fmt::format_string<T...> fmt, T&&... args)
{
  if (level <= GetLogLevel()) [[unlikely]]
    WriteFmtArgs(channelName, functionName, level, fmt, fmt::make_format_args(args...));
}
} // namespace Log

// log wrappers
#define Log_SetChannel(ChannelName) [[maybe_unused]] static const char* ___LogChannel___ = #ChannelName;

#define ERROR_LOG(...) Log::FastWrite(___LogChannel___, __func__, LOGLEVEL_ERROR, __VA_ARGS__)
#define WARNING_LOG(...) Log::FastWrite(___LogChannel___, __func__, LOGLEVEL_WARNING, __VA_ARGS__)
#define INFO_LOG(...) Log::FastWrite(___LogChannel___, LOGLEVEL_INFO, __VA_ARGS__)
#define VERBOSE_LOG(...) Log::FastWrite(___LogChannel___, LOGLEVEL_VERBOSE, __VA_ARGS__)
#define DEV_LOG(...) Log::FastWrite(___LogChannel___, LOGLEVEL_DEV, __VA_ARGS__)

#ifdef _DEBUG
#define DEBUG_LOG(...) Log::FastWrite(___LogChannel___, LOGLEVEL_DEBUG, __VA_ARGS__)
#define TRACE_LOG(...) Log::FastWrite(___LogChannel___, LOGLEVEL_TRACE, __VA_ARGS__)
#else
#define DEBUG_LOG(...)                                                                                                 \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#define TRACE_LOG(...)                                                                                                 \
  do                                                                                                                   \
  {                                                                                                                    \
  } while (0)
#endif
