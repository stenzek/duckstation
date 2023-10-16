/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _TIMESYNC_H
#define _TIMESYNC_H

#include "types.h"
#include "game_input.h"

#define FRAME_WINDOW_SIZE           120
#define MIN_UNIQUE_FRAMES           10
#define MIN_FRAME_ADVANTAGE          3
#define MAX_FRAME_ADVANTAGE          9

class TimeSync {
public:
   TimeSync();
   virtual ~TimeSync ();

   void advance_frame(GameInput &input, float advantage, float radvantage);
   float recommend_frame_wait_duration(bool require_idle_input);
   float LocalAdvantage() const;
   float RemoteAdvantage() const;
   float AvgLocalAdvantageSinceStart() const { return _avgLocal; }
   float AvgRemoteAdvantageSinceStart() const { return _avgRemote; }
   void SetFrameDelay(int frame);
   int _frameDelay2 ;
   int _remoteFrameDelay = 0;;
protected:
   float         _local[FRAME_WINDOW_SIZE];
   float         _remote[FRAME_WINDOW_SIZE];
   GameInput   _last_inputs[MIN_UNIQUE_FRAMES];
   int         _next_prediction;
   int nFrame=0;
   float _avgLocal = 0;
   float _avgRemote = 0;
   bool clearedInitial = false;
  };

#endif
