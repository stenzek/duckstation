// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "log_channels.h"
#include "types.h"

#include "fmt/base.h"

#include <array>
#include <cinttypes>
#include <cstdarg>
#include <mutex>
#include <string_view>

namespace Log {
enum class Level : u32
{
  None, // Silences all log traffic
  Error,
  Warning,
  Info,
  Verbose,
  Dev,
  Debug,
  Trace,

  MaxCount
};

enum class Channel : u32
{
#define LOG_CHANNEL_ENUM(X) X,
  ENUMERATE_LOG_CHANNELS(LOG_CHANNEL_ENUM)
#undef LOG_CHANNEL_ENUM

    MaxCount
};

// log message callback type
using CallbackFunctionType = void (*)(void* pUserParam, const char* channelName, const char* functionName, Level level,
                                      std::string_view message);

// registers a log callback
void RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam);

// unregisters a log callback
void UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam);

// returns a list of all log channels
const std::array<const char*, static_cast<size_t>(Channel::MaxCount)>& GetChannelNames();

// returns the time in seconds since the start of the process
float GetCurrentMessageTime();
bool AreTimestampsEnabled();

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
Level GetLogLevel();

// Returns true if log messages for the specified log level/filter would not be filtered (and visible).
bool IsLogVisible(Level level, Channel channel);

// Sets global filtering level, messages below this level won't be sent to any of the logging sinks.
void SetLogLevel(Level level);

// Sets global filter, any messages from these channels won't be sent to any of the logging sinks.
void SetLogChannelEnabled(Channel channel, bool enabled);

// Packs a level and channel into one 16-bit number.
using PackedChannelAndLevel = u32;
[[maybe_unused]] ALWAYS_INLINE static u32 PackChannelAndLevel(Channel channel, Level level)
{
  return ((static_cast<PackedChannelAndLevel>(channel) << 3) | static_cast<PackedChannelAndLevel>(level));
}

// writes a message to the log
void Write(PackedChannelAndLevel cat, std::string_view message);
void Write(PackedChannelAndLevel cat, const char* functionName, std::string_view message);
void WriteFmtArgs(PackedChannelAndLevel cat, fmt::string_view fmt, fmt::format_args args);
void WriteFmtArgs(PackedChannelAndLevel cat, const char* functionName, fmt::string_view fmt, fmt::format_args args);

ALWAYS_INLINE static void FastWrite(Channel channel, Level level, std::string_view message)
{
  if (level <= GetLogLevel()) [[unlikely]]
    Write(PackChannelAndLevel(channel, level), message);
}
ALWAYS_INLINE static void FastWrite(Channel channel, const char* functionName, Level level, std::string_view message)
{
  if (level <= GetLogLevel()) [[unlikely]]
    Write(PackChannelAndLevel(channel, level), functionName, message);
}
template<typename... T>
ALWAYS_INLINE static void FastWrite(Channel channel, Level level, fmt::format_string<T...> fmt, T&&... args)
{
  if (level <= GetLogLevel()) [[unlikely]]
    WriteFmtArgs(PackChannelAndLevel(channel, level), fmt, fmt::make_format_args(args...));
}
template<typename... T>
ALWAYS_INLINE static void FastWrite(Channel channel, const char* functionName, Level level,
                                    fmt::format_string<T...> fmt, T&&... args)
{
  if (level <= GetLogLevel()) [[unlikely]]
    WriteFmtArgs(PackChannelAndLevel(channel, level), functionName, fmt, fmt::make_format_args(args...));
}
} // namespace Log

// log wrappers
#define LOG_CHANNEL(name) [[maybe_unused]] static constexpr Log::Channel ___LogChannel___ = Log::Channel::name;

#define ERROR_LOG(...) Log::FastWrite(___LogChannel___, __func__, Log::Level::Error, __VA_ARGS__)
#define WARNING_LOG(...) Log::FastWrite(___LogChannel___, __func__, Log::Level::Warning, __VA_ARGS__)
#define INFO_LOG(...) Log::FastWrite(___LogChannel___, Log::Level::Info, __VA_ARGS__)
#define VERBOSE_LOG(...) Log::FastWrite(___LogChannel___, Log::Level::Verbose, __VA_ARGS__)
#define DEV_LOG(...) Log::FastWrite(___LogChannel___, Log::Level::Dev, __VA_ARGS__)

#ifdef _DEBUG
#define DEBUG_LOG(...) Log::FastWrite(___LogChannel___, Log::Level::Debug, __VA_ARGS__)
#define TRACE_LOG(...) Log::FastWrite(___LogChannel___, Log::Level::Trace, __VA_ARGS__)
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
