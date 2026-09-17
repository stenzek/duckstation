// SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "common/threading.h"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

static_assert(!std::is_copy_constructible_v<Threading::Mutex>);
static_assert(!std::is_copy_assignable_v<Threading::Mutex>);
static_assert(!std::is_copy_constructible_v<Threading::ConditionVariable>);
static_assert(!std::is_copy_assignable_v<Threading::ConditionVariable>);

TEST(ThreadingMutex, TryLock)
{
  Threading::Mutex mutex;
  EXPECT_TRUE(mutex.try_lock());
  EXPECT_FALSE(mutex.try_lock());
  mutex.unlock();
  EXPECT_TRUE(mutex.try_lock());
  mutex.unlock();
}

TEST(ThreadingMutex, StandardLockWrappers)
{
  Threading::Mutex mutex;

  {
    const std::lock_guard lock(mutex);
    EXPECT_FALSE(mutex.try_lock());
  }

  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(lock.owns_lock());
    EXPECT_FALSE(mutex.try_lock());
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
