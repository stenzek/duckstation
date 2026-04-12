// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "types.h"

#include <mutex>

/// LockedPtr holds a pointer alongside a mutex, locking it on construction and
/// unlocking on destruction. The mutex type is templated, defaulting to std::mutex.
template<typename T, typename MutexType>
class LockedPtr
{
public:
  ALWAYS_INLINE LockedPtr() = default;

  ALWAYS_INLINE LockedPtr(MutexType& mutex, T* ptr) : m_ptr(ptr), m_mutex(&mutex) { m_mutex->lock(); }

  ALWAYS_INLINE LockedPtr(MutexType& mutex, T* ptr, std::adopt_lock_t) : m_ptr(ptr), m_mutex(&mutex) {}

  ALWAYS_INLINE LockedPtr(std::unique_lock<MutexType>&& lock, T* ptr) : m_ptr(ptr), m_mutex(lock.release()) {}

  ALWAYS_INLINE ~LockedPtr()
  {
    if (m_mutex)
      m_mutex->unlock();
  }

  ALWAYS_INLINE LockedPtr(LockedPtr&& other) : m_ptr(other.m_ptr), m_mutex(other.m_mutex)
  {
    other.m_ptr = nullptr;
    other.m_mutex = nullptr;
  }

  LockedPtr(const LockedPtr&) = delete;
  LockedPtr& operator=(const LockedPtr&) = delete;

  ALWAYS_INLINE LockedPtr& operator=(LockedPtr&& other)
  {
    if (m_mutex)
      m_mutex->unlock();
    m_ptr = other.m_ptr;
    m_mutex = other.m_mutex;
    other.m_ptr = nullptr;
    other.m_mutex = nullptr;
    return *this;
  }

  ALWAYS_INLINE T& operator*() const { return *m_ptr; }
  ALWAYS_INLINE T* operator->() const { return m_ptr; }

  ALWAYS_INLINE explicit operator bool() const { return (m_ptr != nullptr); }

  ALWAYS_INLINE T* get_ptr() const { return m_ptr; }
  ALWAYS_INLINE MutexType* get_mutex() const { return m_mutex; }

private:
  T* m_ptr = nullptr;
  MutexType* m_mutex = nullptr;
};
