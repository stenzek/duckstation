/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _UDP_MSG_H
#define _UDP_MSG_H

#define MAX_COMPRESSED_BITS       4096
#define UDP_MSG_MAX_PLAYERS          4
#pragma pack(push, 1)

struct UdpMsg
{
   enum MsgType {
      Invalid       = 0,
      SyncRequest   = 1,
      SyncReply     = 2,
      Input         = 3,
      QualityReport = 4,
      QualityReply  = 5,
      InputAck      = 6,
   };

   struct connect_status {
      unsigned int   disconnected:1;
      int            last_frame:31;
   };

   struct {
      uint16         magic;
      uint16         sequence_number;
      uint8          type;            /* packet type */
   } hdr;
   union {
      struct {
         uint32      random_request;  /* please reply back with this random data */
         uint16      remote_magic;
         uint8       remote_endpoint;
         uint8       remote_inputDelay;
      } sync_request;
      
      struct {
         uint32      random_reply;    /* OK, here's your random data back */
      } sync_reply;
      
      struct {
         int8        frame_advantage; /* what's the other guy's frame advantage? */
         uint32      ping;
      } quality_report;
      
      struct {
         uint32      pong;
      } quality_reply;

      struct {
         connect_status    peer_connect_status[UDP_MSG_MAX_PLAYERS];

         uint32            start_frame;

         int               ack_frame;

         uint16            num_bits;
         uint32            checksum32;
         uint8             input_size; // XXX: shouldn't be in every single packet!
         uint8             bits[MAX_COMPRESSED_BITS]; /* must be last */
      } input;

      struct {
         int               ack_frame;
      } input_ack;
   } u;

public:
   int PacketSize() {
      return sizeof(hdr) + PayloadSize();
   }

   int PayloadSize() {
      int size;

      switch (hdr.type) {
      case SyncRequest:   return sizeof(u.sync_request);
      case SyncReply:     return sizeof(u.sync_reply);
      case QualityReport: return sizeof(u.quality_report);
      case QualityReply:  return sizeof(u.quality_reply);
      case InputAck:      return sizeof(u.input_ack);
      case Input:
         size = (int)((char *)&u.input.bits - (char *)&u.input);
         size += (u.input.num_bits + 7) / 8;
         return size;
      }
   
      ASSERT(false);
      return 0;
   }

   UdpMsg(MsgType t) { hdr.type = (uint8)t; }
};

#pragma pack(pop)

#endif   
