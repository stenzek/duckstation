/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _BACKEND_H
#define _BACKEND_H

#include "ggponet.h"
#include "types.h"

class GGPOSession {
public:
   virtual ~GGPOSession() { }
   virtual GGPOErrorCode DoPoll() = 0;
   virtual GGPOErrorCode NetworkIdle() = 0;
   virtual GGPOErrorCode AddPlayer(GGPOPlayer *player, GGPOPlayerHandle *handle) = 0;
   virtual GGPOErrorCode AddLocalInput(GGPOPlayerHandle player, void *values, int size) = 0;
   virtual GGPOErrorCode SyncInput(void *values, int size, int *disconnect_flags) = 0;
   virtual GGPOErrorCode IncrementFrame(uint16_t checksum) = 0;
   virtual GGPOErrorCode CurrentFrame(int& current) =0;
   virtual GGPOErrorCode DisconnectPlayer(GGPOPlayerHandle handle) = 0;// { return GGPO_OK; }
   virtual GGPOErrorCode GetNetworkStats(GGPONetworkStats *stats, GGPOPlayerHandle handle) { return GGPO_OK; }
   virtual GGPOErrorCode Logv(const char *fmt, va_list list) { ::Logv(fmt, list); return GGPO_OK; }

   virtual GGPOErrorCode SetFrameDelay(GGPOPlayerHandle player, int delay) { return GGPO_ERRORCODE_UNSUPPORTED; }
   virtual GGPOErrorCode SetDisconnectTimeout(int timeout) { return GGPO_ERRORCODE_UNSUPPORTED; }
   virtual GGPOErrorCode SetDisconnectNotifyStart(int timeout) { return GGPO_ERRORCODE_UNSUPPORTED; }
   virtual GGPOErrorCode OnPacket(ENetPeer* peer, const ENetPacket* pkt) = 0;
};


#endif

