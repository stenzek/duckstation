#pragma once
#include "types.h"

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
#ifdef WIN32
  void* m_event_handle;
#elif defined(__linux__) || defined(__APPLE__)
  int m_pipe_fds[2];
  bool m_auto_reset;
#else
#error Unknown platform.
#endif
};

} // namespace Common