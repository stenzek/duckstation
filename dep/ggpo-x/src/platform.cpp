#include "platform.h"

#include <cstdio>
#include <cstdint>

#ifdef _WIN32
#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1
#include <Windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

void GGPOAssertFailed(const char* expr, const char* filename, int line)
{
#ifdef _WIN32
  const uint32 pid = GetCurrentProcessId();
#else
  const uint32 pid = getpid();
#endif

  char assert_buf[1024];
  std::snprintf(assert_buf, sizeof(assert_buf), "Assertion: %s @ %s:%d (pid:%d)", expr, __FILE__, __LINE__, pid);

#ifdef _WIN32
  MessageBoxA(nullptr, assert_buf, "Assertion Failed", MB_ICONERROR | MB_OK);
#else
  std::fprintf(stderr, "\n%s\n", assert_buf);
#endif
}

uint32 GGPOGetCurrentTimeMS()
{
#ifdef _WIN32
  static std::uint64_t freq;
  static std::uint64_t start_time;
  static bool start_time_initialized = false;

  if (!start_time_initialized)
  {
    start_time_initialized = true;
    LARGE_INTEGER pfreq = {};
    QueryPerformanceFrequency(&pfreq);
    freq = pfreq.QuadPart / 1000u;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&start_time));
  }

  std::uint64_t current_time;
  QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&current_time));
  return (current_time - start_time) / freq;
#else
  static constexpr auto get_current_time_ns = []() {
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return (static_cast<std::uint64_t>(tv.tv_nsec) + static_cast<std::uint64_t>(tv.tv_sec) * 1000000000);
  };

  static std::uint64_t start_time;
  static bool start_time_initialized = false;

  if (!start_time_initialized)
  {
    start_time_initialized = true;
    start_time = get_current_time_ns();
  }

  const std::uint64_t current_time = get_current_time_ns();
  return (current_time - start_time) / 1000000;
#endif
}
