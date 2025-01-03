// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#pragma once

#include "threading.h"
#include "types.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/// Implements a simple task queue with multiple worker threads.
class TaskQueue
{
public:
  using TaskFunctionType = std::function<void()>;

  TaskQueue();
  ~TaskQueue();

  /// Sets the number of worker threads to be used by the task queue.
  /// Setting this to zero threads completes tasks on the calling thread.
  /// @param count The desired number of worker threads.
  void SetWorkerCount(u32 count);

  /// Submits a task to the queue for execution.
  /// @param func The task function to execute.
  void SubmitTask(TaskFunctionType func);

  /// Waits for all submitted tasks to complete execution.
  void WaitForAll();

private:
  /// Waits for all submitted tasks to complete execution.
  /// This is a helper function that assumes a lock is already held.
  /// @param lock A unique_lock object holding the mutex.
  void WaitForAll(std::unique_lock<std::mutex>& lock);

  /// Executes one task from the queue.
  /// This is a helper function that assumes a lock is already held.
  /// @param lock A unique_lock object holding the mutex.
  void ExecuteOneTask(std::unique_lock<std::mutex>& lock);

  /// Entry point for worker threads. Executes tasks from the queue until termination is signaled.
  void WorkerThreadEntryPoint();

  std::mutex m_mutex;
  std::deque<TaskFunctionType> m_tasks;
  size_t m_tasks_outstanding = 0;
  std::condition_variable m_task_wait_cv;
  std::condition_variable m_tasks_done_cv;
  std::vector<std::thread> m_threads;
  bool m_threads_done = false;
};
