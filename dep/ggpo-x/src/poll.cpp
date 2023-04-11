/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "types.h"
#include "poll.h"

Poll::Poll(void) :
   _start_time(0)
{
}


//
//void
//Poll::RegisterMsgLoop(IPollSink *sink, void *cookie)
//{
//   _msg_sinks.push_back(PollSinkCb(sink, cookie));
//}

void
Poll::RegisterLoop(IPollSink *sink, void *cookie)
{
   _loop_sinks.push_back(PollSinkCb(sink, cookie));
}
//void
//Poll::RegisterPeriodic(IPollSink *sink, int interval, void *cookie)
//{
//   _periodic_sinks.push_back(PollPeriodicSinkCb(sink, cookie, interval));
//}

void
Poll::Run()
{
   while (Pump(100)) {
      continue;
   }
}

bool
Poll::Pump(int timeout)
{
   int i;
   bool finished = false;

   for (i = 0; i < _loop_sinks.size(); i++) {
      PollSinkCb &cb = _loop_sinks[i];
      finished = !cb.sink->OnLoopPoll(cb.cookie) || finished;
   }
   return finished;
}
//
//int
//Poll::ComputeWaitTime(int elapsed)
//{
//   int waitTime = INFINITE;
//   size_t count = _periodic_sinks.size();
//
//   if (count > 0) {
//      for (int i = 0; i < count; i++) {
//         PollPeriodicSinkCb &cb = _periodic_sinks[i];
//         int timeout = (cb.interval + cb.last_fired) - elapsed;
//         if (waitTime == INFINITE || (timeout < waitTime)) {
//            waitTime = MAX(timeout, 0);
//         }         
//      }
//   }
//   return waitTime;
//}
