#include "common/event.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <thread>

TEST(Event, InitialStateUnsignaled)
{
  Common::Event e;
  ASSERT_FALSE(e.TryWait(1));
}

TEST(Event, SignalOnSameThread)
{
  Common::Event e;
  e.Signal();
  ASSERT_TRUE(e.TryWait(1));
}

TEST(Event, SignalOnSecondThread)
{
  Common::Event e;

  std::thread thr([&e]() { e.Signal(); });
  e.Wait();
  thr.join();
}

TEST(Event, SignalOnSecondThreadWithDelay)
{
  Common::Event e;
  std::atomic_bool fl{false};

  std::thread thr([&e, &fl]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    e.Signal();
    fl.store(true);
  });
  ASSERT_FALSE(fl.load());
  e.Wait();
  ASSERT_TRUE(fl.load());
  thr.join();
}

TEST(Event, ResetAfterSignaling)
{
  Common::Event e;

  e.Signal();
  e.Reset();

  ASSERT_FALSE(e.TryWait(1));
}

TEST(Event, WaitForMultiple)
{
  Common::Event e1, e2;
  e1.Signal();
  e2.Signal();

  Common::Event* events[] = { &e1, &e2 };
  Common::Event::WaitForMultiple(events, countof(events));
}

TEST(Event, AutoReset)
{
  Common::Event e(true);
  e.Signal();
  ASSERT_TRUE(e.TryWait(1));
  ASSERT_FALSE(e.TryWait(1));
}

TEST(Event, NoAutoReset)
{
  Common::Event e(false);
  e.Signal();
  ASSERT_TRUE(e.TryWait(1));
  ASSERT_TRUE(e.TryWait(1));
}