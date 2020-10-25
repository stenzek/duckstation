#include "event.h"
#include "assert.h"

#if defined(WIN32)
#include "windows_headers.h"
#include <malloc.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__HAIKU__)
#include <time.h>
#endif

namespace Common {

#if defined(WIN32) && defined(USE_WIN32_EVENT_OBJECTS)

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

#elif defined(WIN32)

Event::Event(bool auto_reset /* = false */) : m_auto_reset(auto_reset)
{
  InitializeCriticalSection(&m_cs);
  InitializeConditionVariable(&m_cv);
}

Event::~Event()
{
  DeleteCriticalSection(&m_cs);
}

void Event::Signal()
{
  EnterCriticalSection(&m_cs);
  m_signaled.store(true);
  WakeAllConditionVariable(&m_cv);
  LeaveCriticalSection(&m_cs);
}

void Event::Wait()
{
  m_waiters.fetch_add(1);

  EnterCriticalSection(&m_cs);
  while (!m_signaled.load())
    SleepConditionVariableCS(&m_cv, &m_cs, INFINITE);

  if (m_waiters.fetch_sub(1) == 1 && m_auto_reset)
    m_signaled.store(false);

  LeaveCriticalSection(&m_cs);
}

bool Event::TryWait(u32 timeout_in_ms)
{
  m_waiters.fetch_add(1);

  const u32 start = GetTickCount();

  EnterCriticalSection(&m_cs);
  while (!m_signaled.load() && (GetTickCount() - start) < timeout_in_ms)
    SleepConditionVariableCS(&m_cv, &m_cs, INFINITE);

  const bool result = m_signaled.load();

  if (m_waiters.fetch_sub(1) == 1 && result && m_auto_reset)
    m_signaled.store(false);

  LeaveCriticalSection(&m_cs);

  return result;
}

void Event::Reset()
{
  EnterCriticalSection(&m_cs);
  m_signaled.store(false);
  LeaveCriticalSection(&m_cs);
}

void Event::WaitForMultiple(Event** events, u32 num_events)
{
  for (u32 i = 0; i < num_events; i++)
    events[i]->Wait();
}

#elif defined(__linux__) || defined(__APPLE__) || defined(__HAIKU__)

Event::Event(bool auto_reset /* = false */) : m_auto_reset(auto_reset)
{
  pthread_mutex_init(&m_mutex, nullptr);
  pthread_cond_init(&m_cv, nullptr);
}

Event::~Event()
{
  pthread_cond_destroy(&m_cv);
  pthread_mutex_destroy(&m_mutex);
}

void Event::Signal()
{
  pthread_mutex_lock(&m_mutex);
  m_signaled.store(true);
  pthread_cond_broadcast(&m_cv);
  pthread_mutex_unlock(&m_mutex);
}

void Event::Wait()
{
  m_waiters.fetch_add(1);

  pthread_mutex_lock(&m_mutex);
  while (!m_signaled.load())
    pthread_cond_wait(&m_cv, &m_mutex);

  if (m_waiters.fetch_sub(1) == 1 && m_auto_reset)
    m_signaled.store(false);

  pthread_mutex_unlock(&m_mutex);
}

bool Event::TryWait(u32 timeout_in_ms)
{
  m_waiters.fetch_add(1);

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += timeout_in_ms / 1000;
  ts.tv_nsec += (timeout_in_ms % 1000) * 1000000;

  pthread_mutex_lock(&m_mutex);
  while (!m_signaled.load())
    pthread_cond_timedwait(&m_cv, &m_mutex, &ts);

  const bool result = m_signaled.load();

  if (m_waiters.fetch_sub(1) == 1 && result && m_auto_reset)
    m_signaled.store(false);

  pthread_mutex_unlock(&m_mutex);

  return result;
}

void Event::Reset()
{
  pthread_mutex_lock(&m_mutex);
  m_signaled.store(false);
  pthread_mutex_unlock(&m_mutex);
}

void Event::WaitForMultiple(Event** events, u32 num_events)
{
  for (u32 i = 0; i < num_events; i++)
    events[i]->Wait();
}

#else

Event::Event(bool auto_reset /* = false */) : m_auto_reset(auto_reset) {}

Event::~Event() = default;

void Event::Signal()
{
  std::unique_lock lock(m_mutex);
  m_signaled.store(true);
  m_cv.notify_all();
}

void Event::Wait()
{
  m_waiters.fetch_add(1);

  std::unique_lock lock(m_mutex);
  m_cv.wait(lock, [this]() { return m_signaled.load(); });

  if (m_waiters.fetch_sub(1) == 1 && m_auto_reset)
    m_signaled.store(false);
}

bool Event::TryWait(u32 timeout_in_ms)
{
  m_waiters.fetch_add(1);

  std::unique_lock lock(m_mutex);
  const bool result =
    m_cv.wait_for(lock, std::chrono::milliseconds(timeout_in_ms), [this]() { return m_signaled.load(); });

  if (m_waiters.fetch_sub(1) == 1 && result && m_auto_reset)
    m_signaled.store(false);

  return result;
}

void Event::Reset()
{
  std::unique_lock lock(m_mutex);
  m_signaled.store(false);
}

void Event::WaitForMultiple(Event** events, u32 num_events)
{
  for (u32 i = 0; i < num_events; i++)
    events[i]->Wait();
}

#endif

} // namespace Common
