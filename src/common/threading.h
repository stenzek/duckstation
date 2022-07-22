#pragma once
#include "types.h"

#if defined(__APPLE__)
#include <mach/semaphore.h>
#elif !defined(_WIN32)
#include <semaphore.h>
#endif

#include <atomic>
#include <functional>

namespace Threading {
extern u64 GetThreadCpuTime();
extern u64 GetThreadTicksPerSecond();

/// Set the name of the current thread
extern void SetNameOfCurrentThread(const char* name);

// Releases a timeslice to other threads.
extern void Timeslice();

// --------------------------------------------------------------------------------------
//  ThreadHandle
// --------------------------------------------------------------------------------------
// Abstracts an OS's handle to a thread, closing the handle when necessary. Currently,
// only used for getting the CPU time for a thread.
//
class ThreadHandle
{
public:
  ThreadHandle();
  ThreadHandle(ThreadHandle&& handle);
  ThreadHandle(const ThreadHandle& handle);
  ~ThreadHandle();

  /// Returns a new handle for the calling thread.
  static ThreadHandle GetForCallingThread();

  ThreadHandle& operator=(ThreadHandle&& handle);
  ThreadHandle& operator=(const ThreadHandle& handle);

  operator void*() const { return m_native_handle; }
  operator bool() const { return (m_native_handle != nullptr); }

  /// Returns the amount of CPU time consumed by the thread, at the GetThreadTicksPerSecond() frequency.
  u64 GetCPUTime() const;

  /// Sets the affinity for a thread to the specified processors.
  /// Obviously, only works up to 64 processors.
  bool SetAffinity(u64 processor_mask) const;

protected:
  void* m_native_handle = nullptr;

  // We need the thread ID for affinity adjustments on Linux.
#if defined(__linux__)
  unsigned int m_native_id = 0;
#endif
};

// --------------------------------------------------------------------------------------
//  Thread
// --------------------------------------------------------------------------------------
// Abstracts a native thread in a lightweight manner. Provides more functionality than
// std::thread (allowing stack size adjustments).
//
class Thread : public ThreadHandle
{
public:
  using EntryPoint = std::function<void()>;

  Thread();
  Thread(Thread&& thread);
  Thread(const Thread&) = delete;
  Thread(EntryPoint func);
  ~Thread();

  ThreadHandle& operator=(Thread&& thread);
  ThreadHandle& operator=(const Thread& handle) = delete;

  ALWAYS_INLINE bool Joinable() const { return (m_native_handle != nullptr); }
  ALWAYS_INLINE u32 GetStackSize() const { return m_stack_size; }

  /// Sets the stack size for the thread. Do not call if the thread has already been started.
  void SetStackSize(u32 size);

  bool Start(EntryPoint func);
  void Detach();
  void Join();

protected:
#ifdef _WIN32
  static unsigned __stdcall ThreadProc(void* param);
#else
  static void* ThreadProc(void* param);
#endif

  u32 m_stack_size = 0;
};

/// A semaphore that may not have a fast userspace path
/// (Used in other semaphore-based algorithms where the semaphore is just used for its thread sleep/wake ability)
class KernelSemaphore
{
#if defined(_WIN32)
  void* m_sema;
#elif defined(__APPLE__)
  semaphore_t m_sema;
#else
  sem_t m_sema;
#endif
public:
  KernelSemaphore();
  ~KernelSemaphore();
  void Post();
  void Wait();
  bool TryWait();
};

} // namespace Threading