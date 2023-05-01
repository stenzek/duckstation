/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "p2p.h"

static const int RECOMMENDATION_INTERVAL           = 120;
static const int DEFAULT_DISCONNECT_TIMEOUT        = 5000;
static const int DEFAULT_DISCONNECT_NOTIFY_START   = 750;

Peer2PeerBackend::Peer2PeerBackend(GGPOSessionCallbacks *cb,
                                   const char *gamename,
                                   uint16 localport,
                                   int num_players,
                                   int input_size, int nframes) :
    _num_players(num_players),
    _input_size(input_size),
    _sync(_local_connect_status, nframes),
    _disconnect_timeout(DEFAULT_DISCONNECT_TIMEOUT),
    _disconnect_notify_start(DEFAULT_DISCONNECT_NOTIFY_START),
    _num_spectators(0),
    _next_spectator_frame(0)
{
   _callbacks = *cb;
   _synchronizing = true;
   _next_recommended_sleep = 0;
   _manual_network_polling = false;

   /*
    * Initialize the synchronziation layer
    */
   Sync::Config config = { 0 };
   config.num_players = num_players;
   config.input_size = input_size;
   config.callbacks = _callbacks;
   config.num_prediction_frames = nframes;
   _sync.Init(config);

   /*
    * Initialize the UDP port
    */
   _udp.Init(localport, &_poll, this);

   _endpoints.resize(_num_players);
   memset(_local_connect_status, 0, sizeof(_local_connect_status));
   for (int i = 0; i < ARRAY_SIZE(_local_connect_status); i++) {
      _local_connect_status[i].last_frame = -1;
   }

   /*
    * Preload the ROM
    */
   _callbacks.begin_game(_callbacks.context, gamename);
}
  
Peer2PeerBackend::~Peer2PeerBackend()
{
}

void
Peer2PeerBackend::AddRemotePlayer(char *ip,
                                  uint16 port,
                                  int queue)
{
   /*
    * Start the state machine (xxx: no)
    */
   _synchronizing = true;
   
   _endpoints[queue].Init(&_udp, _poll, queue, ip, port, _local_connect_status);
   _endpoints[queue].SetDisconnectTimeout(_disconnect_timeout);
   _endpoints[queue].SetDisconnectNotifyStart(_disconnect_notify_start);
   _endpoints[queue].Synchronize();
}

GGPOErrorCode Peer2PeerBackend::AddSpectator(char *ip,
                                             uint16 port)
{
   if (_num_spectators == GGPO_MAX_SPECTATORS) {
      return GGPO_ERRORCODE_TOO_MANY_SPECTATORS;
   }
   /*
    * Currently, we can only add spectators before the game starts.
    */
   if (!_synchronizing) {
      return GGPO_ERRORCODE_INVALID_REQUEST;
   }
   int queue = _num_spectators++;

   _spectators[queue].Init(&_udp, _poll, queue + 1000, ip, port, _local_connect_status);
   _spectators[queue].SetDisconnectTimeout(_disconnect_timeout);
   _spectators[queue].SetDisconnectNotifyStart(_disconnect_notify_start);
   _spectators[queue].Synchronize();

   return GGPO_OK;
}
void Peer2PeerBackend::CheckDesync()
{
    std::vector<int> keysToRemove;
    for (auto& ep : _endpoints) 
    {
        for (const auto& pair : ep._remoteCheckSums)
        {
            auto checkSumFrame = pair.first;
            auto remoteChecksum = pair.second;

            if (_confirmedCheckSums.count(checkSumFrame))
            {
                keysToRemove.push_back(checkSumFrame);
                auto localChecksum = _confirmedCheckSums.at(checkSumFrame);

                if (remoteChecksum != localChecksum)
                {
                    GGPOEvent info;
                    info.code = GGPO_EVENTCODE_DESYNC;
                    info.u.desync.nFrameOfDesync = checkSumFrame;
                    info.u.desync.ourCheckSum = localChecksum;
                    info.u.desync.remoteChecksum = remoteChecksum;
                    _callbacks.on_event(_callbacks.context, &info);

                    char buf[256];
                    sprintf_s<256>(buf, "DESYNC Checksum frame %d, local: %d, remote %d, size of checksum maps: %d,%d", checkSumFrame, localChecksum, remoteChecksum, (int)_confirmedCheckSums.size(), (int)ep._remoteCheckSums.size());
                    //  OutputDebugStringA(buf);
                }

                if (checkSumFrame % 100 == 0)
                {
                    char buf[256];
                    sprintf_s<256>(buf, "Checksum frame %d, local: %d, remote %d, size of checksum maps: %d,%d\n", checkSumFrame, localChecksum, remoteChecksum, (int)_confirmedCheckSums.size(), (int)ep._remoteCheckSums.size());
                    //OutputDebugStringA(buf);
                }
            }
        }
        for (auto k : keysToRemove)
        {
            ep._remoteCheckSums.erase(k);
        }
    }
    for (auto k : keysToRemove)
    {
        char buf[256];
        sprintf_s<256>(buf, "Erase checksums for frame %d\n",k);
       // OutputDebugStringA(buf);
        for (auto itr = _confirmedCheckSums.cbegin(); itr != _confirmedCheckSums.cend(); )
            itr = (itr->first <=k) ? _confirmedCheckSums.erase(itr) : std::next(itr);
       
       // ep._remoteCheckSums.erase(k);
    }
    
}

GGPOErrorCode
Peer2PeerBackend::DoPoll()
{
    // Pass on chat
    for (int i = 0; i < _num_players; i++) {
        _endpoints[i].ConsumeChat([&](const char* msg) {
            GGPOEvent info;
            info.u.chat.senderID = i;
            info.code = GGPO_EVENTCODE_CHAT;
            info.u.chat.msg = msg;
            _callbacks.on_event(_callbacks.context, &info);
            });
    }

   if (!_sync.InRollback()) {
      if (!_manual_network_polling)
        _poll.Pump(0);

      PollUdpProtocolEvents();
      CheckDesync();
      if (!_synchronizing) {
         _sync.CheckSimulation();

         // notify all of our endpoints of their local frame number for their
         // next connection quality report
         int current_frame = _sync.GetFrameCount();
         for (int i = 0; i < _num_players; i++) {
            _endpoints[i].SetLocalFrameNumber(current_frame);
         }

         int total_min_confirmed;
         if (_num_players <= 2) {
            total_min_confirmed = Poll2Players(current_frame);
         } else {
            total_min_confirmed = PollNPlayers(current_frame);
         }

         Log("last confirmed frame in p2p backend is %d.\n", total_min_confirmed);
         if (total_min_confirmed >= 0) {
            ASSERT(total_min_confirmed != INT_MAX);
            if (_num_spectators > 0) {
               while (_next_spectator_frame <= total_min_confirmed) {
                  Log("pushing frame %d to spectators.\n", _next_spectator_frame);
   
                  GameInput input;
                  input.frame = _next_spectator_frame;
                  input.size = _input_size * _num_players;
                  _sync.GetConfirmedInputs(input.bits, _input_size * _num_players, _next_spectator_frame);
                  for (int i = 0; i < _num_spectators; i++) {
                     _spectators[i].SendInput(input);
                  }
                  _next_spectator_frame++;
               }
            }
            Log("setting confirmed frame in sync to %d.\n", total_min_confirmed);
            _sync.SetLastConfirmedFrame(total_min_confirmed);
         }

         // send timesync notifications if now is the proper time
         if (current_frame > _next_recommended_sleep) {
            float interval = 0;
            for (int i = 0; i < _num_players; i++) {
               interval = BIGGEST(interval, _endpoints[i].RecommendFrameDelay());
            }

            //if (interval > 0) 
            {
               GGPOEvent info;
               info.code = GGPO_EVENTCODE_TIMESYNC;
               info.u.timesync.frames_ahead = interval;
               info.u.timesync.timeSyncPeriodInFrames = RECOMMENDATION_INTERVAL;
               _callbacks.on_event(_callbacks.context, &info);
               _next_recommended_sleep = current_frame + RECOMMENDATION_INTERVAL;// RECOMMENDATION_INTERVAL;// RECOMMENDATION_INTERVAL;
            }
         }
      }
   }
   return GGPO_OK;
}

int Peer2PeerBackend::Poll2Players(int current_frame)
{
   int i;

   // discard confirmed frames as appropriate
   int total_min_confirmed = MAX_INT;
   for (i = 0; i < _num_players; i++) {
      bool queue_connected = true;
      if (_endpoints[i].IsRunning()) {
         int ignore;
         queue_connected = _endpoints[i].GetPeerConnectStatus(i, &ignore);
      }
      if (!_local_connect_status[i].disconnected) {
         total_min_confirmed = MIN(_local_connect_status[i].last_frame, total_min_confirmed);
      }
      Log("  local endp: connected = %d, last_received = %d, total_min_confirmed = %d.\n", !_local_connect_status[i].disconnected, _local_connect_status[i].last_frame, total_min_confirmed);
      if (!queue_connected && !_local_connect_status[i].disconnected) {
         Log("disconnecting i %d by remote request.\n", i);
         DisconnectPlayerQueue(i, total_min_confirmed);
      }
      Log("  total_min_confirmed = %d.\n", total_min_confirmed);
   }
   return total_min_confirmed;
}

int Peer2PeerBackend::PollNPlayers(int current_frame)
{
   int i, queue, last_received;

   // discard confirmed frames as appropriate
   int total_min_confirmed = MAX_INT;
   for (queue = 0; queue < _num_players; queue++) {
      bool queue_connected = true;
      int queue_min_confirmed = MAX_INT;
      Log("considering queue %d.\n", queue);
      for (i = 0; i < _num_players; i++) {
         // we're going to do a lot of logic here in consideration of endpoint i.
         // keep accumulating the minimum confirmed point for all n*n packets and
         // throw away the rest.
         if (_endpoints[i].IsRunning()) {
            bool connected = _endpoints[i].GetPeerConnectStatus(queue, &last_received);

            queue_connected = queue_connected && connected;
            queue_min_confirmed = MIN(last_received, queue_min_confirmed);
            Log("  endpoint %d: connected = %d, last_received = %d, queue_min_confirmed = %d.\n", i, connected, last_received, queue_min_confirmed);
         } else {
            Log("  endpoint %d: ignoring... not running.\n", i);
         }
      }
      // merge in our local status only if we're still connected!
      if (!_local_connect_status[queue].disconnected) {
         queue_min_confirmed = MIN(_local_connect_status[queue].last_frame, queue_min_confirmed);
      }
      Log("  local endp: connected = %d, last_received = %d, queue_min_confirmed = %d.\n", !_local_connect_status[queue].disconnected, _local_connect_status[queue].last_frame, queue_min_confirmed);

      if (queue_connected) {
         total_min_confirmed = MIN(queue_min_confirmed, total_min_confirmed);
      } else {
         // check to see if this disconnect notification is further back than we've been before.  If
         // so, we need to re-adjust.  This can happen when we detect our own disconnect at frame n
         // and later receive a disconnect notification for frame n-1.
         if (!_local_connect_status[queue].disconnected || _local_connect_status[queue].last_frame > queue_min_confirmed) {
            Log("disconnecting queue %d by remote request.\n", queue);
            DisconnectPlayerQueue(queue, queue_min_confirmed);
         }
      }
      Log("  total_min_confirmed = %d.\n", total_min_confirmed);
   }
   return total_min_confirmed;
}


GGPOErrorCode
Peer2PeerBackend::AddPlayer(GGPOPlayer *player,
                            GGPOPlayerHandle *handle)
{
   if (player->type == GGPO_PLAYERTYPE_SPECTATOR) {
      return AddSpectator(player->u.remote.ip_address, player->u.remote.port);
   }

   int queue = player->player_num - 1;
   if (player->player_num < 1 || player->player_num > _num_players) {
      return GGPO_ERRORCODE_PLAYER_OUT_OF_RANGE;
   }
   *handle = QueueToPlayerHandle(queue);

   if (player->type == GGPO_PLAYERTYPE_REMOTE) {
      AddRemotePlayer(player->u.remote.ip_address, player->u.remote.port, queue);
   }
   return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::AddLocalInput(GGPOPlayerHandle player,
                                void *values,
                                int size)
{
   int queue;
   GameInput input;
   GGPOErrorCode result;

   if (_sync.InRollback()) {
      return GGPO_ERRORCODE_IN_ROLLBACK;
   }
   if (_synchronizing) {
      return GGPO_ERRORCODE_NOT_SYNCHRONIZED;
   }
   
   result = PlayerHandleToQueue(player, &queue);
   if (!GGPO_SUCCEEDED(result)) {
      return result;
   }

   input.init(-1, (char *)values, size);

   // Feed the input for the current frame into the synchronzation layer.
   if (!_sync.AddLocalInput(queue, input)) {
      return GGPO_ERRORCODE_PREDICTION_THRESHOLD;
   }

   if (input.frame != GameInput::NullFrame) { // xxx: <- comment why this is the case
      // Update the local connect status state to indicate that we've got a
      // confirmed local frame for this player.  this must come first so it
      // gets incorporated into the next packet we send.

       // Send checksum for frames old enough to be confirmed (ie older then current - MaxPredictionFrames())

       
      
       _confirmedCheckSumFrame = input.frame - HowFarBackForChecksums();
      
       
       input.checksum = 0; 
       if (_confirmedCheckSumFrame >= 0) {
           char buf[128];
           input.checksum = _pendingCheckSums.at(_confirmedCheckSumFrame);
           _confirmedCheckSums[_confirmedCheckSumFrame] = input.checksum;
           _pendingCheckSums.erase(_confirmedCheckSumFrame);
           sprintf_s<128>(buf, "Frame %d: Send checksum for frame %d, val %d\n", input.frame, _confirmedCheckSumFrame, input.checksum);
           //OutputDebugStringA(buf);
       }
      Log("setting local connect status for local queue %d to %d", queue, input.frame);
      _local_connect_status[queue].last_frame = input.frame;

      // Send the input to all the remote players.
      for (int i = 0; i < _num_players; i++) {
         if (_endpoints[i].IsInitialized()) {
            _endpoints[i].SendInput(input);
         }
      }
   }

   return GGPO_OK;
}


GGPOErrorCode
Peer2PeerBackend::SyncInput(void *values,
                            int size,
                            int *disconnect_flags)
{
   int flags;

   // Wait until we've started to return inputs.
   if (_synchronizing) {
      return GGPO_ERRORCODE_NOT_SYNCHRONIZED;
   }
   flags = _sync.SynchronizeInputs(values, size);
   if (disconnect_flags) {
      *disconnect_flags = flags;
   }
   return GGPO_OK;
}

GGPOErrorCode 
Peer2PeerBackend::CurrentFrame(int& current)
{
    current = _sync.GetFrameCount();
    return GGPO_OK;
}

GGPOErrorCode 
Peer2PeerBackend::PollNetwork()
{
    _poll.Pump(0);
    return GGPO_OK;
}

GGPOErrorCode Peer2PeerBackend::SetManualNetworkPolling(bool value)
{
    _manual_network_polling = value;
    return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::IncrementFrame(uint16_t checksum1)
{
    auto currentFrame = _sync.GetFrameCount();
    _sync.IncrementFrame();
    checksum1 = _sync.GetLastSavedFrame().checksum;
    char buf[256];
    uint16_t cSum = checksum1;
    Log("End of frame (%d)...\n", currentFrame);
    static int maxDif = 0;
    if (_pendingCheckSums.count(currentFrame))
    {
        auto max = _pendingCheckSums.rbegin()->first;
        auto diff = max - currentFrame;
        maxDif = max(maxDif, diff);
        int oldChecksum = _pendingCheckSums[currentFrame];
        _pendingCheckSums[currentFrame] = cSum;
        sprintf_s<256>(buf, "Replace local checksum for frame %d: %d with %d, newest frame is %d, max diff %d\n",
                       currentFrame, oldChecksum, _pendingCheckSums[currentFrame], max, maxDif);
       
        if (currentFrame <= _confirmedCheckSumFrame)
        {
            sprintf_s<256>(buf, "Changing frame %d in a rollback, but we've already sent frame %d\n", currentFrame, _confirmedCheckSumFrame);

            OutputDebugStringA(buf);
            throw std::exception(buf);
        }
        if (diff >= (_sync.MaxPredictionFrames())) {

            sprintf_s<256>(buf, "diff is bigger than max prediction\n");

              OutputDebugStringA(buf);
            throw std::exception(buf);
        }
    }
    else
    {
        sprintf_s<256>(buf, "Added local checksum for frame %d: %d\n", currentFrame, cSum);
       //OutputDebugStringA(buf);
    }

    _pendingCheckSums[currentFrame] = cSum;
  
    DoPoll();
    PollSyncEvents();

    return GGPO_OK;
}


void
Peer2PeerBackend::PollSyncEvents(void)
{
   Sync::Event e;
   while (_sync.GetEvent(e)) {
      OnSyncEvent(e);
   }
   return;
}

void
Peer2PeerBackend::PollUdpProtocolEvents(void)
{
   UdpProtocol::Event evt;
   for (int i = 0; i < _num_players; i++) {
       _endpoints[i].StartPollLoop();
      while (_endpoints[i].GetEvent(evt)) {
         OnUdpProtocolPeerEvent(evt, i);
      }
      _endpoints[i].EndPollLoop();
   }
   for (int i = 0; i < _num_spectators; i++) {
      while (_spectators[i].GetEvent(evt)) {
         OnUdpProtocolSpectatorEvent(evt, i);
      }
   }

   //for (int i = 0; i < _num_players; i++) {
//    _endpoints[i].ApplyToEvents([&](UdpProtocol::Event& e) {
//        OnUdpProtocolPeerEvent(evt, i);
//        });
//}
}

void Peer2PeerBackend::CheckRemoteChecksum(int framenumber, uint16 cs)
{
    if (framenumber <= _sync.MaxPredictionFrames())
        return;
    framenumber; cs;
    //auto frameOfChecksumToSend = framenumber - (_sync.MaxPredictionFrames() + 1);

}

int Peer2PeerBackend::HowFarBackForChecksums()const
{
    return 16;
}/*
uint16 Peer2PeerBackend::GetChecksumForConfirmedFrame(int frameNumber) const
{
  
    
     auto frameOfChecksumToSend = frameNumber - HowFarBackForChecksums();
     if (frameOfChecksumToSend < 0)
         return 0;
    
     if (_checkSums.count(frameOfChecksumToSend) == 0)
     {    
         char s[128];
         sprintf_s<128>(s, "No local checksum found, remote frame is %d, adjusted is %d, most recent we have is %d\n", frameNumber, frameOfChecksumToSend, _checkSums.rbegin()->first);
         OutputDebugStringA(s);
         throw std::exception("s");
     }
     return _checkSums.at(frameOfChecksumToSend);       
}*/
void
Peer2PeerBackend::OnUdpProtocolPeerEvent(UdpProtocol::Event &evt, int queue)
{
   OnUdpProtocolEvent(evt, QueueToPlayerHandle(queue));
   switch (evt.type) {
      case UdpProtocol::Event::Input:
         if (!_local_connect_status[queue].disconnected) {
            int current_remote_frame = _local_connect_status[queue].last_frame;
            int new_remote_frame = evt.u.input.input.frame;
            ASSERT(current_remote_frame == -1 || new_remote_frame == (current_remote_frame + 1));

            _sync.AddRemoteInput(queue, evt.u.input.input);
            // Notify the other endpoints which frame we received from a peer
            Log("setting remote connect status for queue %d to %d\n", queue, evt.u.input.input.frame);
            _local_connect_status[queue].last_frame = evt.u.input.input.frame;

            auto remoteChecksum = evt.u.input.input.checksum;
            int checkSumFrame = new_remote_frame - HowFarBackForChecksums();
            if (checkSumFrame >= _endpoints[queue].RemoteFrameDelay()-1)
                _endpoints[queue]._remoteCheckSumsThisFrame[checkSumFrame] = remoteChecksum;
         //   auto localChecksum = GetChecksumForConfirmedFrame(new_remote_frame);
         //   
           
            if (checkSumFrame %120==0)
            {
                char buf[256];
                sprintf_s<256>(buf, "Received checksum for frame %d, remote cs is %d\n", checkSumFrame, remoteChecksum);
                //OutputDebugStringA(buf);
            }
         }
         break;

   case UdpProtocol::Event::Disconnected:
      DisconnectPlayer(QueueToPlayerHandle(queue));
      break;
   }
}


void
Peer2PeerBackend::OnUdpProtocolSpectatorEvent(UdpProtocol::Event &evt, int queue)
{
   GGPOPlayerHandle handle = QueueToSpectatorHandle(queue);
   OnUdpProtocolEvent(evt, handle);

   GGPOEvent info;

   switch (evt.type) {
   case UdpProtocol::Event::Disconnected:
      _spectators[queue].Disconnect();

      info.code = GGPO_EVENTCODE_DISCONNECTED_FROM_PEER;
      info.u.disconnected.player = handle;
      _callbacks.on_event(_callbacks.context, &info);

      break;
   }
}

void
Peer2PeerBackend::OnUdpProtocolEvent(UdpProtocol::Event &evt, GGPOPlayerHandle handle)
{
   GGPOEvent info;

   switch (evt.type) {
   case UdpProtocol::Event::Connected:
      info.code = GGPO_EVENTCODE_CONNECTED_TO_PEER;
      info.u.connected.player = handle;
      _callbacks.on_event(_callbacks.context, &info);
      break;
   case UdpProtocol::Event::Synchronizing:
      info.code = GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER;
      info.u.synchronizing.player = handle;
      info.u.synchronizing.count = evt.u.synchronizing.count;
      info.u.synchronizing.total = evt.u.synchronizing.total;
      _callbacks.on_event(_callbacks.context, &info);
      break;
   case UdpProtocol::Event::Synchronzied:
      info.code = GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER;
      info.u.synchronized.player = handle;
      _callbacks.on_event(_callbacks.context, &info);

      CheckInitialSync();
      break;

   case UdpProtocol::Event::NetworkInterrupted:
      info.code = GGPO_EVENTCODE_CONNECTION_INTERRUPTED;
      info.u.connection_interrupted.player = handle;
      info.u.connection_interrupted.disconnect_timeout = evt.u.network_interrupted.disconnect_timeout;
      _callbacks.on_event(_callbacks.context, &info);
      break;

   case UdpProtocol::Event::NetworkResumed:
      info.code = GGPO_EVENTCODE_CONNECTION_RESUMED;
      info.u.connection_resumed.player = handle;
      _callbacks.on_event(_callbacks.context, &info);
      break;
   }
}

/*
 * Called only as the result of a local decision to disconnect.  The remote
 * decisions to disconnect are a result of us parsing the peer_connect_settings
 * blob in every endpoint periodically.
 */
GGPOErrorCode
Peer2PeerBackend::DisconnectPlayer(GGPOPlayerHandle player)
{
   int queue;
   GGPOErrorCode result;

   result = PlayerHandleToQueue(player, &queue);
   if (!GGPO_SUCCEEDED(result)) {
      return result;
   }
   
   if (_local_connect_status[queue].disconnected) {
      return GGPO_ERRORCODE_PLAYER_DISCONNECTED;
   }

   if (!_endpoints[queue].IsInitialized()) {
      int current_frame = _sync.GetFrameCount();
      // xxx: we should be tracking who the local player is, but for now assume
      // that if the endpoint is not initalized, this must be the local player.
      Log("Disconnecting local player %d at frame %d by user request.\n", queue, _local_connect_status[queue].last_frame);
      for (int i = 0; i < _num_players; i++) {
         if (_endpoints[i].IsInitialized()) {
            DisconnectPlayerQueue(i, current_frame);
         }
      }
   } else {
      Log("Disconnecting queue %d at frame %d by user request.\n", queue, _local_connect_status[queue].last_frame);
      DisconnectPlayerQueue(queue, _local_connect_status[queue].last_frame);
   }
   return GGPO_OK;
}

void
Peer2PeerBackend::DisconnectPlayerQueue(int queue, int syncto)
{
   GGPOEvent info;
   int framecount = _sync.GetFrameCount();

   _endpoints[queue].Disconnect();

   Log("Changing queue %d local connect status for last frame from %d to %d on disconnect request (current: %d).\n",
       queue, _local_connect_status[queue].last_frame, syncto, framecount);

   _local_connect_status[queue].disconnected = 1;
   _local_connect_status[queue].last_frame = syncto;

   /*if (syncto < framecount) {
      Log("adjusting simulation to account for the fact that %d disconnected @ %d.\n", queue, syncto);
      _sync.AdjustSimulation(syncto);
      Log("finished adjusting simulation.\n");
   }*/

   info.code = GGPO_EVENTCODE_DISCONNECTED_FROM_PEER;
   info.u.disconnected.player = QueueToPlayerHandle(queue);
   _callbacks.on_event(_callbacks.context, &info);

   CheckInitialSync();
}


GGPOErrorCode
Peer2PeerBackend::GetNetworkStats(GGPONetworkStats *stats, GGPOPlayerHandle player)
{
   int queue;
   GGPOErrorCode result;

   result = PlayerHandleToQueue(player, &queue);
   if (!GGPO_SUCCEEDED(result)) {
      return result;
   }

   memset(stats, 0, sizeof *stats);
   _endpoints[queue].GetNetworkStats(stats);

   return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::SetFrameDelay(GGPOPlayerHandle player, int delay) 
{ 
   int queue;
   GGPOErrorCode result;

   result = PlayerHandleToQueue(player, &queue);
   if (!GGPO_SUCCEEDED(result)) {
      return result;
   } _sync.SetFrameDelay(queue, delay);
   
   for (int i = 0; i < _num_players; i++) {
       if (_endpoints[i].IsInitialized()) {
           _endpoints[i].SetFrameDelay(delay);
          
       }
   }
   ;
   return GGPO_OK; 
}

GGPOErrorCode
Peer2PeerBackend::SetDisconnectTimeout(int timeout)
{
   _disconnect_timeout = timeout;
   for (int i = 0; i < _num_players; i++) {
      if (_endpoints[i].IsInitialized()) {
         _endpoints[i].SetDisconnectTimeout(_disconnect_timeout);
      }
   }
   return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::SetDisconnectNotifyStart(int timeout)
{
   _disconnect_notify_start = timeout;
   for (int i = 0; i < _num_players; i++) {
      if (_endpoints[i].IsInitialized()) {
         _endpoints[i].SetDisconnectNotifyStart(_disconnect_notify_start);
      }
   }
   return GGPO_OK;
}

GGPOErrorCode 
Peer2PeerBackend::Chat(const char* text)
{
    if (strlen(text) >= MAX_CHAT_LENGTH)
        return GGPO_CHAT_MESSAGE_TOO_LONG;

    // Send the input to all the remote players.
    for (int i = 0; i < _num_players; i++) {
        if (_endpoints[i].IsInitialized()) {
            _endpoints[i].SendChat(text);
        }
    }
    return GGPO_OK;
}

GGPOErrorCode
Peer2PeerBackend::PlayerHandleToQueue(GGPOPlayerHandle player, int *queue)
{
   int offset = ((int)player - 1);
   if (offset < 0 || offset >= _num_players) {
      return GGPO_ERRORCODE_INVALID_PLAYER_HANDLE;
   }
   *queue = offset;
   return GGPO_OK;
}

 
void
Peer2PeerBackend::OnMsg(sockaddr_in &from, UdpMsg *msg, int len)
{
   for (int i = 0; i < _num_players; i++) {
      if (_endpoints[i].HandlesMsg(from, msg)) {
         _endpoints[i].OnMsg(msg, len);
         return;
      }
   }
   for (int i = 0; i < _num_spectators; i++) {
      if (_spectators[i].HandlesMsg(from, msg)) {
         _spectators[i].OnMsg(msg, len);
         return;
      }
   }
}

void
Peer2PeerBackend::CheckInitialSync()
{
   int i;

   if (_synchronizing) {
      // Check to see if everyone is now synchronized.  If so,
      // go ahead and tell the client that we're ok to accept input.
      for (i = 0; i < _num_players; i++) {
         // xxx: IsInitialized() must go... we're actually using it as a proxy for "represents the local player"
         if (_endpoints[i].IsInitialized() && !_endpoints[i].IsSynchronized() && !_local_connect_status[i].disconnected) {
            return;
         }
      }
      for (i = 0; i < _num_spectators; i++) {
         if (_spectators[i].IsInitialized() && !_spectators[i].IsSynchronized()) {
            return;
         }
      }

      GGPOEvent info;
      info.code = GGPO_EVENTCODE_RUNNING;
      _callbacks.on_event(_callbacks.context, &info);
      _synchronizing = false;
   }
}
