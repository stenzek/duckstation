/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _GAMEINPUT_H
#define _GAMEINPUT_H

#include <stdio.h>
#include <memory.h>

// GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS * 8 must be less than
// 2^BITVECTOR_NIBBLE_SIZE (see bitvector.h)

#define GAMEINPUT_MAX_BYTES      4
#define GAMEINPUT_MAX_PLAYERS    6

struct GameInput {
   enum Constants {
      NullFrame = -1
   };
   int      frame;
   int      size; /* size in bytes of the entire input for all players */
   char     bits[GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS];
   uint32 checksum;
   bool is_null() { return frame == NullFrame; }
   void init(int frame, char *bits, int size, int offset);
   void init(int frame, char *bits, int size);
   bool value(int i) const { return (bits[i/8] & (1 << (i%8))) != 0; }
   void set(int i) { bits[i/8] |= (1 << (i%8)); }
   void clear(int i) { bits[i/8] &= ~(1 << (i%8)); }
   void erase() { memset(bits, 0, sizeof(bits)); }
   void desc(char *buf, size_t buf_size, bool show_frame = true) const;
   void log(char *prefix, bool show_frame = true) const;
   bool equal(GameInput &input, bool bitsonly = false);
};

#endif
