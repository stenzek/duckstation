#pragma once
#include "types.h"

// #define USE_WIN32_EVENT_OBJECTS 1

#if defined(WIN32) && !defined(USE_WIN32_EVENT_OBJECTS)
#include "windows_headers.h"
#include <atomic>
#elif defined(__linux__) || defined(__APPLE__) || defined(__HAIKU__)
#include <atomic>
#include <pthread.h>
#else
#include <atomic>
#include <condition_variable>
#include <mutex>
#endif

namespace Common {

class Event
{
public:
  Event(bool auto_reset = false);
  ~Event();

  void Reset();
  void Signal();
  void Wait();
  bool TryWait(u32 timeout_in_ms);

  static void WaitForMultiple(Event** events, u32 num_events);

private:
#if defined(WIN32) && defined(USE_WIN32_EVENT_OBJECTS)
  void* m_event_handle;
#elif defined(WIN32)
  CRITICAL_SECTION m_cs;
  CONDITION_VARIABLE m_cv;
  std::atomic_uint32_t m_waiters{0};
  std::atomic_bool m_signaled{false};
  bool m_auto_reset = false;
#elif defined(__linux__) || defined(__APPLE__) || defined(__HAIKU__)
  pthread_mutex_t m_mutex;
  pthread_cond_t m_cv;
  std::atomic_uint32_t m_waiters{0};
  std::atomic_bool m_signaled{false};
  bool m_auto_reset = false;
#else
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::atomic_uint32_t m_waiters{0};
  std::atomic_bool m_signaled{false};
  bool m_auto_reset = false;
#endif
};

} // namespace Common
