/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _UDP_PROTO_H_
#define _UDP_PROTO_H_

#include "enet/enet.h"
#include "udp_msg.h"
#include "game_input.h"
#include "timesync.h"
#include "ggponet.h"
#include "ring_buffer.h"
#include <functional>
#include <vector>
#include <string>
#include <map>
class UdpProtocol
{
public:
   struct Stats {
      int                 ping;
      float                 remote_frame_advantage;
      float                 local_frame_advantage;
      float                 av_remote_frame_advantage;
      float                 av_local_frame_advantage;
      int                 send_queue_len;
   };

   struct Event {
      enum Type {
         Unknown = -1,
         Connected,
         Synchronizing,
         Synchronzied,
         Input,
      };

      Type      type;
      union {
         struct {
            GameInput   input;
         } input;
         struct {
            int         total;
            int         count;
         } synchronizing;
         struct {
            int         disconnect_timeout;
         } network_interrupted;
      } u;

      UdpProtocol::Event(Type t = Unknown) : type(t) { }
   };

public:
   bool NetworkIdle();

public:
   UdpProtocol();
   ~UdpProtocol();

   ENetPeer* GetENetPeer() const { return _peer; }

   void Init(ENetPeer* peer, int queue, UdpMsg::connect_status *status);

   void Synchronize();
   bool GetPeerConnectStatus(int id, int *frame);
   bool IsInitialized() { return _peer != nullptr; }
   bool IsSynchronized() { return _current_state == Running; }
   bool IsRunning() { return _current_state == Running; }
   void SendInput(GameInput &input);
   void SendInputAck();
   void OnMsg(UdpMsg *msg, int len);
   void Disconnect();
  
   void GetNetworkStats(struct GGPONetworkStats *stats);
   bool GetEvent(UdpProtocol::Event &e);
   void SetLocalFrameNumber(int num);
   float RecommendFrameDelay();
   int RemoteFrameDelay()const;
   void SetFrameDelay(int delay);
   void ApplyToEvents(std::function<void(UdpProtocol::Event&)> cb);
   void StartPollLoop();
   void EndPollLoop();
   std::map<int, uint32> _remoteCheckSums;
   std::map<int, uint32> _remoteCheckSumsThisFrame;
protected:
   enum State {
      Syncing,
      Synchronzied,
      Running,
      Disconnected
   };
   struct QueueEntry {
      int         queue_time;
      sockaddr_in dest_addr;
      UdpMsg      *msg;

      QueueEntry() {}
      QueueEntry(int time, sockaddr_in &dst, UdpMsg *m) : queue_time(time), dest_addr(dst), msg(m) { }
   };

   void UpdateNetworkStats(void);
   void QueueEvent(const UdpProtocol::Event &evt);
   void Log(const char *fmt, ...);
   void LogMsg(const char *prefix, UdpMsg *msg);
   void LogEvent(const char *prefix, const UdpProtocol::Event &evt);
   void SendSyncRequest();
   void SendMsg(UdpMsg *msg);
   void SendPendingOutput();
   bool OnInvalid(UdpMsg *msg, int len);
   bool OnSyncRequest(UdpMsg *msg, int len);
   bool OnSyncReply(UdpMsg *msg, int len);
   bool OnInput(UdpMsg *msg, int len);
   bool OnInputAck(UdpMsg *msg, int len);
   bool OnQualityReport(UdpMsg *msg, int len);
   bool OnQualityReply(UdpMsg *msg, int len);
  
protected:
   /*
    * Network transmission information
    */
   ENetPeer       *_peer;
   uint16         _magic_number;
   int            _queue;
   uint16         _remote_magic_number;
   bool           _connected;

   /*
    * Stats
    */
   int            _round_trip_time = 0;
   int            _packets_sent;
   int            _bytes_sent;
   int            _kbps_sent;
   int            _stats_start_time;

   /*
    * The state machine
    */
   UdpMsg::connect_status *_local_connect_status;
   UdpMsg::connect_status _peer_connect_status[UDP_MSG_MAX_PLAYERS];

   State          _current_state;
   union {
      struct {
         uint32   roundtrips_remaining;
         uint32   random;
      } sync;
      struct {
         uint32   last_quality_report_time;
         uint32   last_network_stats_interval;
         uint32   last_input_packet_recv_time;
      } running;
   } _state;

   /*
    * Fairness.
    */
   float               _local_frame_advantage;
   float               _remote_frame_advantage;

   /*
    * Packet loss...
    */
   RingBuffer<GameInput, 64>  _pending_output;
   GameInput                  _last_received_input;
   GameInput                  _last_sent_input;
   GameInput                  _last_acked_input;
   unsigned int               _last_send_time;
   unsigned int               _last_recv_time;

   uint16                     _next_send_seq;
   uint16                     _next_recv_seq;

   /*
    * Rift synchronization.
    */
   TimeSync                   _timesync;

   /*
    * Event queue
    */
   RingBuffer<UdpProtocol::Event, 64>  _event_queue;
};

#endif
