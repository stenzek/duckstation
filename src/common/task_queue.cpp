// SPDX-FileCopyrightText: 2019-2025 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: CC-BY-NC-ND-4.0

#include "task_queue.h"
#include "assert.h"

TaskQueue::TaskQueue() = default;

TaskQueue::~TaskQueue()
{
  SetWorkerCount(0);
  Assert(m_tasks.empty());
}

void TaskQueue::SetWorkerCount(u32 count)
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
}

void TaskQueue::SubmitTask(TaskFunctionType func)
{
  std::unique_lock lock(m_mutex);
  m_tasks.push_back(std::move(func));
  m_tasks_outstanding++;
  m_task_wait_cv.notify_one();
}

void TaskQueue::WaitForAll()
{
  std::unique_lock lock(m_mutex);
  WaitForAll(lock);
}

void TaskQueue::WaitForAll(std::unique_lock<std::mutex>& lock)
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

void TaskQueue::ExecuteOneTask(std::unique_lock<std::mutex>& lock)
{
  TaskFunctionType func = std::move(m_tasks.front());
  m_tasks.pop_front();
  lock.unlock();
  func();
  lock.lock();
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
