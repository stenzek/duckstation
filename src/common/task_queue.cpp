// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "task_queue.h"
#include "assert.h"

#include "common/log.h"

LOG_CHANNEL(Threading);

TaskQueue::TaskQueue() = default;

TaskQueue::~TaskQueue()
{
  SetWorkerCount(0, 0);
  Assert(m_tasks.empty());
}

void TaskQueue::SetWorkerCount(u16 count, u16 max_threads)
{
  std::unique_lock lock(m_mutex);

  WaitForAll(lock);

  if (!m_threads.empty())
  {
    m_threads_done = true;
    m_task_wait_cv.notify_all();

    auto threads = std::move(m_threads);
    m_threads = decltype(threads)();

    lock.unlock();
    for (std::thread& t : threads)
      t.join();
    lock.lock();
  }

  if (count > 0)
  {
    m_threads_done = false;
    for (u32 i = 0; i < count; i++)
      m_threads.emplace_back(&TaskQueue::WorkerThreadEntryPoint, this);
  }

  m_max_threads = max_threads;
}

size_t TaskQueue::GetOutstandingTasks()
{
  std::unique_lock lock(m_mutex);
  return m_tasks_outstanding;
}

void TaskQueue::SubmitTask(TaskFunctionType func)
{
  std::unique_lock lock(m_mutex);

  if (m_max_threads == 0) [[unlikely]]
  {
    lock.unlock();
    func();
    return;
  }

  m_tasks.push_back(std::move(func));
  m_tasks_outstanding++;

  // If we're under pressure and all threads are busy, spin up another one.
  if (m_threads_busy == m_threads.size() && m_threads.size() < m_max_threads)
  {
    m_threads.emplace_back(&TaskQueue::WorkerThreadEntryPoint, this);
    DEV_LOG("Spawning TaskQueue worker thread, now {} threads", m_threads.size());
  }

  m_task_wait_cv.notify_one();
}

void TaskQueue::WaitForAll()
{
  std::unique_lock lock(m_mutex);
  WaitForAll(lock);
}

void TaskQueue::WaitForAll(std::unique_lock<Threading::Mutex>& lock)
{
  // while we're waiting, execute work on the calling thread
  m_tasks_done_cv.wait(lock, [this, &lock]() {
    if (m_tasks_outstanding == 0)
      return true;

    while (!m_tasks.empty())
      ExecuteOneTask(lock);

    return (m_tasks_outstanding == 0);
  });
}

bool TaskQueue::ExecuteOneTask()
{
  std::unique_lock lock(m_mutex);
  if (m_tasks.empty())
    return false;

  ExecuteOneTask(lock);
  return true;
}

void TaskQueue::ExecuteOneTask(std::unique_lock<Threading::Mutex>& lock)
{
  TaskFunctionType func = std::move(m_tasks.front());
  m_tasks.pop_front();
  m_threads_busy++;
  lock.unlock();
  func();
  lock.lock();
  DebugAssert(m_threads_busy > 0);
  m_threads_busy--;
  m_tasks_outstanding--;
  if (m_tasks_outstanding == 0)
    m_tasks_done_cv.notify_all();
}

void TaskQueue::WorkerThreadEntryPoint()
{
  Threading::SetNameOfCurrentThread("TaskQueue Worker");

  std::unique_lock lock(m_mutex);
  while (!m_threads_done)
  {
    if (m_tasks.empty())
    {
      m_task_wait_cv.wait(lock);
      continue;
    }

    ExecuteOneTask(lock);
  }
}
