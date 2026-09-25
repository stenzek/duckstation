// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/threading.h"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <vector>

static_assert(!std::is_copy_constructible_v<Threading::Mutex>);
static_assert(!std::is_copy_assignable_v<Threading::Mutex>);
static_assert(!std::is_copy_constructible_v<Threading::SharedMutex>);
static_assert(!std::is_copy_assignable_v<Threading::SharedMutex>);
static_assert(!std::is_copy_constructible_v<Threading::SharedLockGuard>);
static_assert(!std::is_copy_assignable_v<Threading::SharedLockGuard>);
static_assert(!std::is_move_constructible_v<Threading::SharedLockGuard>);
static_assert(!std::is_move_assignable_v<Threading::SharedLockGuard>);
static_assert(!std::is_copy_constructible_v<Threading::ConditionVariable>);
static_assert(!std::is_copy_assignable_v<Threading::ConditionVariable>);

TEST(ThreadingMutex, TryLock)
{
  Threading::Mutex mutex;
  EXPECT_TRUE(mutex.try_lock());

  bool acquired_in_thread = true;
  std::thread thread([&]() { acquired_in_thread = mutex.try_lock(); });
  thread.join();
  EXPECT_FALSE(acquired_in_thread);

  mutex.unlock();
  EXPECT_TRUE(mutex.try_lock());
  mutex.unlock();
}

TEST(ThreadingMutex, StandardLockWrappers)
{
  Threading::Mutex mutex;

  {
    const std::lock_guard lock(mutex);
  }

  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(lock.owns_lock());
  }

  EXPECT_TRUE(mutex.try_lock());
  mutex.unlock();
}

TEST(ThreadingMutex, MutualExclusion)
{
  static constexpr u32 NUM_THREADS = 4;
  static constexpr u32 NUM_INCREMENTS = 10000;

  Threading::Mutex mutex;
  u32 value = 0;
  std::vector<std::thread> threads;
  threads.reserve(NUM_THREADS);
  for (u32 i = 0; i < NUM_THREADS; i++)
  {
    threads.emplace_back([&mutex, &value]() {
      for (u32 j = 0; j < NUM_INCREMENTS; j++)
      {
        const std::lock_guard lock(mutex);
        value++;
      }
    });
  }

  for (std::thread& thread : threads)
    thread.join();

  EXPECT_EQ(value, NUM_THREADS * NUM_INCREMENTS);
}

TEST(ThreadingSharedMutex, TryLock)
{
  Threading::SharedMutex mutex;
  EXPECT_TRUE(mutex.try_lock());

  bool exclusive_locked = true;
  bool shared_locked = true;
  std::thread exclusive_test_thread([&]() {
    exclusive_locked = mutex.try_lock();
    shared_locked = mutex.try_lock_shared();
  });
  exclusive_test_thread.join();
  EXPECT_FALSE(exclusive_locked);
  EXPECT_FALSE(shared_locked);
  mutex.unlock();

  EXPECT_TRUE(mutex.try_lock_shared());
  std::thread shared_test_thread([&]() {
    shared_locked = mutex.try_lock_shared();
    if (shared_locked)
      mutex.unlock_shared();
    exclusive_locked = mutex.try_lock();
  });
  shared_test_thread.join();
  EXPECT_TRUE(shared_locked);
  EXPECT_FALSE(exclusive_locked);
  mutex.unlock_shared();

  EXPECT_TRUE(mutex.try_lock());
  mutex.unlock();
}

TEST(ThreadingSharedMutex, StandardLockWrappers)
{
  Threading::SharedMutex mutex;

  {
    const std::lock_guard lock(mutex);
  }

  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(lock.owns_lock());
  }

  {
    std::shared_lock lock(mutex);
    EXPECT_TRUE(lock.owns_lock());
  }
}

TEST(ThreadingSharedLockGuard, LocksSharedAndUnlocksOnDestruction)
{
  Threading::SharedMutex mutex;

  {
    const Threading::SharedLockGuard guard(mutex);

    bool shared_locked = false;
    bool exclusive_locked = false;
    std::thread thread([&]() {
      shared_locked = mutex.try_lock_shared();
      if (shared_locked)
        mutex.unlock_shared();

      exclusive_locked = mutex.try_lock();
      if (exclusive_locked)
        mutex.unlock();
    });
    thread.join();

    EXPECT_TRUE(shared_locked);
    EXPECT_FALSE(exclusive_locked);
  }

  ASSERT_TRUE(mutex.try_lock());
  mutex.unlock();
}

TEST(ThreadingSharedMutex, MutualExclusion)
{
  static constexpr u32 NUM_THREADS = 4;
  static constexpr u32 NUM_INCREMENTS = 10000;

  Threading::SharedMutex mutex;
  u32 value = 0;
  std::vector<std::thread> threads;
  threads.reserve(NUM_THREADS);
  for (u32 i = 0; i < NUM_THREADS; i++)
  {
    threads.emplace_back([&mutex, &value]() {
      for (u32 j = 0; j < NUM_INCREMENTS; j++)
      {
        const std::lock_guard lock(mutex);
        value++;
      }
    });
  }

  for (std::thread& thread : threads)
    thread.join();

  EXPECT_EQ(value, NUM_THREADS * NUM_INCREMENTS);
}

TEST(ThreadingConditionVariable, NotifyOneAndPredicateWait)
{
  Threading::Mutex mutex;
  Threading::ConditionVariable condition;
  Threading::KernelSemaphore waiting;
  bool wake = false;
  bool woke = false;

  std::thread thread([&]() {
    std::unique_lock lock(mutex);
    waiting.Post();
    condition.wait(lock, [&wake]() { return wake; });
    woke = true;
  });

  waiting.Wait();
  {
    const std::lock_guard lock(mutex);
    EXPECT_FALSE(woke);
    wake = true;
  }
  condition.notify_one();
  thread.join();
  EXPECT_TRUE(woke);
}

TEST(ThreadingConditionVariable, NotifyAll)
{
  static constexpr u32 NUM_THREADS = 4;

  Threading::Mutex mutex;
  Threading::ConditionVariable condition;
  Threading::KernelSemaphore waiting;
  bool wake = false;
  u32 num_woken = 0;
  std::vector<std::thread> threads;
  threads.reserve(NUM_THREADS);
  for (u32 i = 0; i < NUM_THREADS; i++)
  {
    threads.emplace_back([&]() {
      std::unique_lock lock(mutex);
      waiting.Post();
      condition.wait(lock, [&wake]() { return wake; });
      num_woken++;
    });
  }

  for (u32 i = 0; i < NUM_THREADS; i++)
    waiting.Wait();

  {
    const std::lock_guard lock(mutex);
    wake = true;
  }
  condition.notify_all();

  for (std::thread& thread : threads)
    thread.join();

  EXPECT_EQ(num_woken, NUM_THREADS);
}

TEST(ThreadingKernelSemaphore, PostWaitAndTryWait)
{
  Threading::KernelSemaphore semaphore;
  EXPECT_FALSE(semaphore.TryWait());
  semaphore.Post();
  EXPECT_TRUE(semaphore.TryWait());
  EXPECT_FALSE(semaphore.TryWait());

  Threading::KernelSemaphore started;
  std::atomic_bool finished = false;
  std::thread thread([&]() {
    started.Post();
    semaphore.Wait();
    finished.store(true, std::memory_order_release);
  });

  started.Wait();
  EXPECT_FALSE(finished.load(std::memory_order_acquire));
  semaphore.Post();
  thread.join();
  EXPECT_TRUE(finished.load(std::memory_order_acquire));
}
