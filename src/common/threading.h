// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <atomic>
#include <functional>

namespace Threading {
extern u64 GetThreadCpuTime();
extern u64 GetThreadTicksPerSecond();
extern u32 GetProcessorCount();

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

  bool operator==(const ThreadHandle& other) const;
  bool operator!=(const ThreadHandle& other) const;

  /// Returns the amount of CPU time consumed by the thread, at the GetThreadTicksPerSecond() frequency.
  u64 GetCPUTime() const;

  /// Sets the affinity for a thread to the specified processors.
  /// Obviously, only works up to 64 processors.
  bool SetAffinity(u64 processor_mask) const;

  /// Returns true if the calling thread matches this handle.
  bool IsCallingThread() const;

#ifdef __APPLE__
  /// Only available on MacOS, sets a period/maximum time for the scheduler.
  bool SetTimeConstraints(bool enabled, u64 period, u64 typical_time, u64 maximum_time) const;
#endif

protected:
  void* m_native_handle = nullptr;

  // We need the thread ID for affinity adjustments on Linux.
#if defined(_WIN32) || defined(__linux__)
  unsigned int m_native_id = 0;
  u32 m_stack_size = 0;
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

#if !defined(_WIN32) && !defined(__linux__)
  // Stored in ThreadHandle to save 8 bytes.
  u32 m_stack_size = 0;
#endif
};

// --------------------------------------------------------------------------------------
//  Mutex
// --------------------------------------------------------------------------------------
// A lightweight replacement for std::mutex. The native object is stored inline to avoid
// the oversized standard library representation on Windows, without exposing platform
// headers to users of this header.
//
class Mutex
{
public:
#ifdef _WIN32
  Mutex() = default;
#else
  Mutex();
  ~Mutex();
#endif

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  void lock();
  bool try_lock();
  void unlock();

private:
  friend class ConditionVariable;

#if defined(_WIN32)
  void* m_data = nullptr;
#elif defined(__APPLE__)
  static constexpr u32 NATIVE_STORAGE_SIZE = 64;
#elif defined(__linux__) && defined(CPU_ARCH_ARM64)
  static constexpr u32 NATIVE_STORAGE_SIZE = 48;
#elif defined(__linux__)
  static constexpr u32 NATIVE_STORAGE_SIZE = (sizeof(void*) == 8) ? 40 : 24;
#else
#error Unsupported platform.
#endif

#if !defined(_WIN32)
  alignas(void*) u8 m_data[NATIVE_STORAGE_SIZE];
#endif
};

// --------------------------------------------------------------------------------------
//  ConditionVariable
// --------------------------------------------------------------------------------------
// A lightweight replacement for the subset of std::condition_variable used by the
// project. Spurious wakeups are permitted, matching std::condition_variable.
//
class ConditionVariable
{
public:
#ifdef _WIN32
  ConditionVariable() = default;
#else
  ConditionVariable();
  ~ConditionVariable();
#endif

  ConditionVariable(const ConditionVariable&) = delete;
  ConditionVariable& operator=(const ConditionVariable&) = delete;

  void notify_one();
  void notify_all();

  template<typename LockType>
  void wait(LockType& lock)
  {
    Wait(*lock.mutex());
  }

  template<typename LockType, typename Predicate>
  void wait(LockType& lock, Predicate predicate)
  {
    while (!predicate())
      wait(lock);
  }

private:
  void Wait(Mutex& mutex);

#if defined(_WIN32)
  void* m_data = nullptr;
#elif defined(__APPLE__) || defined(__linux__)
  static constexpr u32 NATIVE_STORAGE_SIZE = 48;
  static constexpr u32 NATIVE_STORAGE_ALIGNMENT = 8;
#else
#error Unsupported platform.
#endif

#if !defined(_WIN32)
  alignas(NATIVE_STORAGE_ALIGNMENT) u8 m_data[NATIVE_STORAGE_SIZE];
#endif
};

/// A semaphore that requires a system call to wake/sleep.
class KernelSemaphore
{
public:
  KernelSemaphore();
  KernelSemaphore(const KernelSemaphore&) = delete;
  ~KernelSemaphore();

  KernelSemaphore& operator=(const KernelSemaphore&) = delete;

  void Post();
  void Wait();
  bool TryWait();

private:
#if defined(_WIN32) || defined(__APPLE__)
  void* m_data = nullptr;
#elif defined(__linux__)
  static constexpr u32 NATIVE_STORAGE_SIZE = (sizeof(void*) == 8) ? 32 : 16;
#else
#error Unsupported platform.
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
  alignas(void*) u8 m_data[NATIVE_STORAGE_SIZE] = {};
#endif
};

} // namespace Threading
