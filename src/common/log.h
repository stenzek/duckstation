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

enum class Color : u32
{
  Default,
  Black,
  Red,
  Green,
  Blue,
  Magenta,
  Orange,
  Cyan,
  Yellow,
  White,
  StrongBlack,
  StrongRed,
  StrongGreen,
  StrongBlue,
  StrongMagenta,
  StrongOrange,
  StrongCyan,
  StrongYellow,
  StrongWhite,

  MaxCount
};

enum class Channel : u32
{
#define LOG_CHANNEL_ENUM(X) X,
  ENUMERATE_LOG_CHANNELS(LOG_CHANNEL_ENUM)
#undef LOG_CHANNEL_ENUM

    MaxCount
};

// Default log level.
static constexpr Log::Level DEFAULT_LOG_LEVEL = Log::Level::Info;

// Packs a level and channel into one 16-bit number.
using MessageCategory = u32;
[[maybe_unused]] ALWAYS_INLINE constexpr u32 PackCategory(Channel channel, Level level, Color color)
{
  return ((static_cast<MessageCategory>(color) << 10) | (static_cast<MessageCategory>(channel) << 3) |
          static_cast<MessageCategory>(level));
}
[[maybe_unused]] ALWAYS_INLINE constexpr Color UnpackColor(MessageCategory cat)
{
  return static_cast<Color>((cat >> 10) & 0x1f);
}
[[maybe_unused]] ALWAYS_INLINE constexpr Channel UnpackChannel(MessageCategory cat)
{
  return static_cast<Channel>((cat >> 3) & 0x7f);
}
[[maybe_unused]] ALWAYS_INLINE constexpr Level UnpackLevel(MessageCategory cat)
{
  return static_cast<Level>(cat & 0x7);
}

// log message callback type
using CallbackFunctionType = void (*)(void* pUserParam, MessageCategory category, const char* functionName,
                                      std::string_view message);

// registers a log callback
void RegisterCallback(CallbackFunctionType callbackFunction, void* pUserParam);

// unregisters a log callback
void UnregisterCallback(CallbackFunctionType callbackFunction, void* pUserParam);

// returns a list of all log channels
const std::array<const char*, static_cast<size_t>(Channel::MaxCount)>& GetChannelNames();

// returns the time in seconds since the start of the process
float GetCurrentMessageTime();
bool AreConsoleOutputTimestampsEnabled();

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

// Returns the name of the specified log channel.
const char* GetChannelName(Channel channel);

// Returns the default color for a log level.
Color GetColorForLevel(Level level);

// writes a message to the log
void Write(MessageCategory cat, std::string_view message);
void WriteFuncName(MessageCategory cat, const char* function_name, std::string_view message);
void WriteFmtArgs(MessageCategory cat, fmt::string_view fmt, fmt::format_args args);
void WriteFuncNameFmtArgs(MessageCategory cat, const char* function_name, fmt::string_view fmt, fmt::format_args args);

template<typename... T>
ALWAYS_INLINE void Write(MessageCategory cat, fmt::format_string<T...> fmt, T&&... args)
{
  WriteFmtArgs(cat, fmt, fmt::make_format_args(args...));
}

template<typename... T>
ALWAYS_INLINE void WriteFuncName(MessageCategory cat, const char* function_name, fmt::format_string<T...> fmt,
                                 T&&... args)
{
  WriteFuncNameFmtArgs(cat, function_name, fmt, fmt::make_format_args(args...));
}

} // namespace Log

// log wrappers
#define LOG_CHANNEL(name) [[maybe_unused]] static constexpr Log::Channel ___LogChannel___ = Log::Channel::name;

#define GENERIC_LOG(channel, level, color, ...)                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((level) <= Log::GetLogLevel()) [[unlikely]]                                                                    \
      Log::Write(Log::PackCategory((channel), (level), (color)), __VA_ARGS__);                                         \
  } while (0)

#define GENERIC_FUNC_LOG(channel, level, color, ...)                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    if ((level) <= Log::GetLogLevel()) [[unlikely]]                                                                    \
      Log::WriteFuncName(Log::PackCategory((channel), (level), (color)), __func__, __VA_ARGS__);                       \
  } while (0)

// clang-format off

#define ERROR_LOG(...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Error, Log::Color::Default, __VA_ARGS__)
#define WARNING_LOG(...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Warning, Log::Color::Default, __VA_ARGS__)
#define INFO_LOG(...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Info, Log::Color::Default, __VA_ARGS__)
#define VERBOSE_LOG(...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Verbose, Log::Color::Default, __VA_ARGS__)
#define DEV_LOG(...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Dev, Log::Color::Default, __VA_ARGS__)

#if defined(_DEBUG) || defined(_DEVEL)
#define DEBUG_LOG(...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Debug, Log::Color::Default, __VA_ARGS__)
#define TRACE_LOG(...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Trace, Log::Color::Default, __VA_ARGS__)
#else
#define DEBUG_LOG(...) do { } while (0)
#define TRACE_LOG(...) do { } while (0)
#endif

#define ERROR_COLOR_LOG(color, ...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Error, Log::Color::color, __VA_ARGS__)
#define WARNING_COLOR_LOG(color, ...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Warning, Log::Color::color, __VA_ARGS__)
#define INFO_COLOR_LOG(color, ...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Info, Log::Color::color, __VA_ARGS__)
#define VERBOSE_COLOR_LOG(color, ...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Verbose, Log::Color::color, __VA_ARGS__)
#define DEV_COLOR_LOG(color, ...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Dev, Log::Color::color, __VA_ARGS__)

#if defined(_DEBUG) || defined(_DEVEL)
#define DEBUG_COLOR_LOG(color, ...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Debug, Log::Color::color, __VA_ARGS__)
#define TRACE_COLOR_LOG(color, ...) GENERIC_FUNC_LOG(___LogChannel___, Log::Level::Trace, Log::Color::color, __VA_ARGS__)
#else
#define DEBUG_COLOR_LOG(color, ...) do { } while (0)
#define TRACE_COLOR_LOG(color, ...) do { } while (0)
#endif

// clang-format on
