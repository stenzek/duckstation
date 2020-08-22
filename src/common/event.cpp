#include "event.h"
#include "assert.h"

#if defined(WIN32)
#include <malloc.h>
#include "windows_headers.h"
#elif defined(__linux__) || defined(__APPLE__) || defined(__HAIKU__)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#ifdef __APPLE__
#include <stdlib.h>
#else
#include <malloc.h>
#endif
#endif

namespace Common {

#if defined(WIN32)

Event::Event(bool auto_reset /* = false */)
{
  m_event_handle = reinterpret_cast<void*>(CreateEvent(nullptr, auto_reset ? FALSE : TRUE, FALSE, nullptr));
  Assert(m_event_handle != nullptr);
}

Event::~Event()
{
  CloseHandle(reinterpret_cast<HANDLE>(m_event_handle));
}

void Event::Signal()
{
  SetEvent(reinterpret_cast<HANDLE>(m_event_handle));
}

void Event::Wait()
{
  WaitForSingleObject(reinterpret_cast<HANDLE>(m_event_handle), INFINITE);
}

bool Event::TryWait(u32 timeout_in_ms)
{
  return (WaitForSingleObject(reinterpret_cast<HANDLE>(m_event_handle), timeout_in_ms) == WAIT_OBJECT_0);
}

void Event::Reset()
{
  ResetEvent(reinterpret_cast<HANDLE>(m_event_handle));
}

void Event::WaitForMultiple(Event** events, u32 num_events)
{
  DebugAssert(num_events > 0);

  HANDLE* event_handles = (HANDLE*)alloca(sizeof(HANDLE) * num_events);
  for (u32 i = 0; i < num_events; i++)
    event_handles[i] = reinterpret_cast<HANDLE>(events[i]->m_event_handle);

  WaitForMultipleObjects(num_events, event_handles, TRUE, INFINITE);
}

#elif defined(__linux__) || defined(__APPLE__) || defined(__HAIKU__)

Event::Event(bool auto_reset /*= false*/) : m_auto_reset(auto_reset)
{
  m_pipe_fds[0] = m_pipe_fds[1] = -1;
#if defined(__linux__)
  pipe2(m_pipe_fds, O_NONBLOCK);
#else
  pipe(m_pipe_fds);
  fcntl(m_pipe_fds[0], F_SETFL, fcntl(m_pipe_fds[0], F_GETFL) | O_NONBLOCK);
  fcntl(m_pipe_fds[1], F_SETFL, fcntl(m_pipe_fds[1], F_GETFL) | O_NONBLOCK);
#endif
  Assert(m_pipe_fds[0] >= 0 && m_pipe_fds[1] >= 0);
}

Event::~Event()
{
  close(m_pipe_fds[0]);
  close(m_pipe_fds[1]);
}

void Event::Signal()
{
  char buf[1] = {0};
  write(m_pipe_fds[1], buf, sizeof(buf));
}

void Event::Wait()
{
  pollfd pd = {};
  pd.fd = m_pipe_fds[0];
  pd.events = POLLRDNORM;
  poll(&pd, 1, -1);

  if (m_auto_reset)
    Reset();
}

bool Event::TryWait(u32 timeout_in_ms)
{
  pollfd pd;
  pd.fd = m_pipe_fds[0];
  pd.events = POLLRDNORM;
  if (poll(&pd, 1, timeout_in_ms) == 0)
    return false;

  if (m_auto_reset)
    Reset();

  return true;
}

void Event::Reset()
{
  char buf[1];
  while (read(m_pipe_fds[0], buf, sizeof(buf)) > 0)
    ;
}

void Event::WaitForMultiple(Event** events, u32 num_events)
{
  DebugAssert(num_events > 0);

  pollfd pd = {};
  pd.events = POLLRDNORM;
  for (u32 i = 0; i < num_events; i++)
  {
    pd.fd = events[i]->m_pipe_fds[0];
    poll(&pd, 1, -1);

    if (events[i]->m_auto_reset)
      events[i]->Reset();
  }
}

#endif

} // namespace Common
