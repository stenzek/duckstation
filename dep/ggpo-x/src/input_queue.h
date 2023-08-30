/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _INPUT_QUEUE_H
#define _INPUT_QUEUE_H

#include "game_input.h"

#define INPUT_QUEUE_LENGTH    128
#define DEFAULT_INPUT_SIZE      4

class InputQueue {
public:
   InputQueue(int input_size = DEFAULT_INPUT_SIZE);
   ~InputQueue();

public:
   void Init(int id, int input_size);
   int GetLastConfirmedFrame();
   int GetFirstIncorrectFrame();
   int GetLength() { return _length; }

   void SetFrameDelay(int delay) { _frame_delay = delay; }
   void ResetPrediction(int frame);
   void DiscardConfirmedFrames(int frame);
   bool GetConfirmedInput(int frame, GameInput *input);
   bool GetInput(int frame, GameInput *input);
   void AddInput(GameInput &input);

protected:
   int AdvanceQueueHead(int frame);
   void AddDelayedInputToQueue(GameInput &input, int i);
   void Log(const char *fmt, ...);

protected:
   int                  _id;
   int                  _head;
   int                  _tail;
   int                  _length;
   bool                 _first_frame;

   int                  _last_user_added_frame;
   int                  _last_added_frame;
   int                  _first_incorrect_frame;
   int                  _last_frame_requested;

   int                  _frame_delay;

   GameInput            _inputs[INPUT_QUEUE_LENGTH];
   GameInput            _prediction;
};

#endif



