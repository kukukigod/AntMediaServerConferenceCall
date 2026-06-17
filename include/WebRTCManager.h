#ifndef _WEBRTC_MANAGER_H_
#define _WEBRTC_MANAGER_H_

#include <rtc/rtc.hpp>
#include <json.hpp>
#include "sdptransform.hpp"
#include "GstManager.h"
#include "AMS_SFU_ConferenceSignaling.h"
#include "Log.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>

using namespace rtc;
using json = nlohmann::json;
using namespace std;

#define ROOM_ID_STR "room1"

class WebRTCManager {
public:
    // Constructor and Destructor
    WebRTCManager();
    ~WebRTCManager();

    // API 1: Set Signaling Server URL
    void SetSignalingServerURL(const string& url);

    // API 2: Start Two-Way Communication (Non-blocking)
    bool Start2WayCommunication(bool isPlayer);

    // API 3: Stop Two-Way Communication
    void Stop2WayCommunication();

    // Exposed Chat Tool API for main usage
    void SendChatMessage(const string& text);

    // Send binary custom payloads via data channels
    void SendBinaryMessage(const std::vector<std::byte>& data, const std::string& label = ROOM_ID_STR);

    void SetRoomId(const string &roomId);

    void SetStreamId(const string &streamId);

private:
    // Core WebRTC State Machine and Peer Setup
    void initWebRTCStateMachine();
    void setupPublishPeer(const string& streamId);
    shared_ptr<PeerConnection> createPeerIfNotExist(const string& streamId, bool needDC);
    void removePeerIfNotExist(const string& streamId);
    
    // Media and Data Channel Handling
    void addRemoteTrack(shared_ptr<Track> track, const string &streamId);
    void prepareRecvTracksFromSDP(shared_ptr<PeerConnection> pc, const string &sdp);
    void setupLocalTracks(shared_ptr<PeerConnection> pc, const string &streamId);
    void setupDataChannel(shared_ptr<PeerConnection> pc, const string &label);
    void handleDataChannelText(const std::string& label, const std::string& text);
    
    // SDP & ICE Candidate Signaling Processing
    void handleRemoteSDP(const string &streamId, const string &type, const string &sdp);
    void addRemoteCandidate(const string &streamId, const string &cand, const string &mid);
    void startOffer(const string &streamId);
    
    // Resource Management
    void cleanupResources();

private:
    // API Configurations
    string m_signalingServerUrl;
    string m_roomId;
    string m_publishStreamId;
    bool m_isOnlyPlayer;
    atomic_bool m_isCommunicating;

    // Concurrency Controls
    recursive_mutex m_apiMutex;   // Protects API public entry boundaries
    recursive_mutex m_stateMutex; // Protects inner WebRTC tracking structures

    // External Subsystems
    uint32_t placeholder_val;
    unique_ptr<GstManager> m_gstManager;
    unique_ptr<AMS_SFU_ConferenceSignaling> m_signalingContext;

    // WebRTC Object Trackers
    unordered_map<string, shared_ptr<PeerConnection>> pcs;
    unordered_map<string, bool> isPublisher;
    unordered_map<string, bool> sendPlay;
    unordered_map<string, bool> played;
    unordered_map<string, vector<shared_ptr<Track>>> remoteTracks;
    unordered_map<string, vector<Candidate>> pendingCandidates;
    unordered_map<string, string> midTypeMap;
    unordered_map<string, shared_ptr<DataChannel>> localDataChannels;
    unordered_map<string, shared_ptr<DataChannel>> remoteDataChannels;
    unordered_map<string, bool> isStreamTalking;
};

#endif // WEBRTC_MANAGER_H
