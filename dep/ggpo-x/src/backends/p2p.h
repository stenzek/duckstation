/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _P2P_H
#define _P2P_H

#include "types.h"
#include "sync.h"
#include "backend.h"
#include "timesync.h"
#include "network/udp_proto.h"
#include <map>
class Peer2PeerBackend final : public GGPOSession
{
public:
   Peer2PeerBackend(GGPOSessionCallbacks *cb, int num_players, int input_size, int nframes);
   virtual ~Peer2PeerBackend();


public:
   virtual GGPOErrorCode DoPoll() override;
   virtual GGPOErrorCode NetworkIdle() override;
   virtual GGPOErrorCode AddPlayer(GGPOPlayer *player, GGPOPlayerHandle *handle) override;
   virtual GGPOErrorCode AddLocalInput(GGPOPlayerHandle player, void *values, int size) override;
   virtual GGPOErrorCode SyncInput(void *values, int size, int *disconnect_flags) override;
   virtual GGPOErrorCode IncrementFrame(uint16_t checksum) override;
   virtual GGPOErrorCode DisconnectPlayer(GGPOPlayerHandle handle) override;
   virtual GGPOErrorCode GetNetworkStats(GGPONetworkStats *stats, GGPOPlayerHandle handle) override;
   virtual GGPOErrorCode SetFrameDelay(GGPOPlayerHandle player, int delay) override;
   virtual GGPOErrorCode CurrentFrame(int& current) override;
   virtual GGPOErrorCode OnPacket(ENetPeer* peer, const ENetPacket* pkt) override;

protected:
   GGPOErrorCode PlayerHandleToQueue(GGPOPlayerHandle player, int *queue);
   GGPOPlayerHandle QueueToPlayerHandle(int queue) { return (GGPOPlayerHandle)(queue + 1); }
   GGPOPlayerHandle QueueToSpectatorHandle(int queue) { return (GGPOPlayerHandle)(queue + 1000); } /* out of range of the player array, basically */
   void DisconnectPlayerQueue(int queue, int syncto);
   void PollSyncEvents(void);
   void PollUdpProtocolEvents(void);
   void CheckInitialSync(void);
   int Poll2Players(int current_frame);
   int PollNPlayers(int current_frame);
   void AddRemotePlayer(ENetPeer* peer, int queue);
   GGPOErrorCode AddSpectator(ENetPeer* peer);
   virtual void OnSyncEvent(Sync::Event &e) { }
   virtual void OnUdpProtocolEvent(UdpProtocol::Event &e, GGPOPlayerHandle handle);
   virtual void OnUdpProtocolPeerEvent(UdpProtocol::Event &e, int queue);
   virtual void OnUdpProtocolSpectatorEvent(UdpProtocol::Event &e, int queue);

protected:
   GGPOSessionCallbacks  _callbacks;
   Sync                  _sync;
   std::vector<UdpProtocol> _endpoints;
   UdpProtocol           _spectators[GGPO_MAX_SPECTATORS];
   int                   _num_spectators;
   int                   _input_size;

   bool                  _synchronizing;
   int                   _num_players;
   int                   _next_recommended_sleep;

   int                   _next_spectator_frame;

   UdpMsg::connect_status _local_connect_status[UDP_MSG_MAX_PLAYERS];
   struct ChecksumEntry {
       int         nFrame;
       int         checkSum;;
   };
   std::map<int, uint32> _pendingCheckSums;
   std::map<int, uint32> _confirmedCheckSums;
   
 //  uint16 GetChecksumForConfirmedFrame(int frameNumber) const;
   void CheckRemoteChecksum(int framenumber, uint32 cs);
   int HowFarBackForChecksums()const;
   int _confirmedCheckSumFrame = -500;
   void CheckDesync();
};

#endif
