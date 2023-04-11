/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "timesync.h"
#include <string>
TimeSync::TimeSync()
{
   memset(_local, 0, sizeof(_local));
   memset(_remote, 0, sizeof(_remote));
   _next_prediction = FRAME_WINDOW_SIZE * 3;
}

TimeSync::~TimeSync()
{
}
void TimeSync::SetFrameDelay(int frame)
{
    _frameDelay2 = frame;
}
void
TimeSync::advance_frame(GameInput &input, float advantage, float radvantage)
{
   // Remember the last frame and frame advantage
   _last_inputs[input.frame % ARRAY_SIZE(_last_inputs)] = input;
   _local[input.frame % ARRAY_SIZE(_local)] = advantage;
   _remote[input.frame % ARRAY_SIZE(_remote)] = radvantage;
   
  
   _avgLocal = ((nFrame * _avgLocal) + advantage) / (nFrame + 1);
   _avgRemote = ((nFrame * _avgRemote) + radvantage) / (nFrame + 1);
  
   nFrame++;   
   //Clear after first 3 seconds, as this is a bit crazy
   if (!clearedInitial && nFrame == 240)
   {
       clearedInitial = true;
       nFrame = 0;
   }
}
float TimeSync::LocalAdvantage() const
{
    int i ;
    float advantage=0;
    for (i = 0; i < ARRAY_SIZE(_local); i++) {
        advantage += _local[i];
    }
    advantage /=(float)ARRAY_SIZE(_local);
    return (advantage);
}

float TimeSync::RemoteAdvantage() const
{
    int i;
    float advantage = 0;;
    for (i = 0; i < ARRAY_SIZE(_local); i++) {
        advantage += _remote[i];
    }
    advantage /= (float)ARRAY_SIZE(_local);
    return (advantage);
}
float
TimeSync::recommend_frame_wait_duration(bool require_idle_input)
{
   
   auto advantage = LocalAdvantage();

   auto radvantage = RemoteAdvantage();


   // See if someone should take action.  The person furthest ahead
   // needs to slow down so the other user can catch up.
   // Only do this if both clients agree on who's ahead!!
  
 
 //  if (advantage  >= radvantage) {
      
   //   return 0;
  // }
   float sleep_frames = (((radvantage - advantage) / 2.0f));

   // Both clients agree that we're the one ahead.  Split
   // the difference between the two to figure out how long to
   // sleep for.
  /* char logMessage[256];
   sprintf_s<256>(logMessage, "Local Adv: %.2f, remoate adv %.2f", advantage, radvantage);
   OutputDebugString(logMessage);
  
   sprintf_s<256>(logMessage, ": Sleep for %.2f frames\n", sleep_frames);
   OutputDebugString(logMessage);
   Log("iteration %d:  sleep frames is %d\n", count, sleep_frames);*/

   // Some things just aren't worth correcting for.  Make sure
   // the difference is relevant before proceeding.
  // if (sleep_frames < 0.2f){//{ MIN_FRAME_ADVANTAGE) {
  //    return 0;
  // }

   // Make sure our input had been "idle enough" before recommending
   // a sleep.  This tries to make the emulator sleep while the
   // user's input isn't sweeping in arcs (e.g. fireball motions in
   // Street Fighter), which could cause the player to miss moves.
   //if (require_idle_input) {
   //   for (i = 1; i < ARRAY_SIZE(_last_inputs); i++) {
   //      if (!_last_inputs[i].equal(_last_inputs[0], true)) {
   //         Log("iteration %d:  rejecting due to input stuff at position %d...!!!\n", count, i);
   //         return 0;
   //      }
   //   }
   //}
   //require_idle_input;
   // Success!!! Recommend the number of frames to sleep and adjust
   return sleep_frames > 0  ? (float)MIN(sleep_frames, MAX_FRAME_ADVANTAGE) : (float)MAX(sleep_frames, -MAX_FRAME_ADVANTAGE);
}
