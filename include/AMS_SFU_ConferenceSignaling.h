#ifndef __AMS_SUF_CONFERENCE_SIGNALING_H__
#define __AMS_SUF_CONFERENCE_SIGNALING_H__

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include "AMSSignalingClient.h"
#include <string>
#include <functional>
#include <memory>
#include <unordered_map>

enum class AMSEvent {
    UNKNOWN,
    JOINED_THE_ROOM,
    PUBLISH_STARTED,
    PLAY_STARTED,
    PLAY_FINISHED,
    SUBTRACK_ADDED,
    SUBTRACK_REMOVED
};

class AMS_SFU_ConferenceSignaling {
public:
    AMS_SFU_ConferenceSignaling(const std::string& roomId);
    ~AMS_SFU_ConferenceSignaling();

    // High-level event notification definitions for WebRTCManager alignment
    std::function<void()> onConnected;
    std::function<void()> onDisconnected;
    std::function<void(const std::string& streamId)> onStartOfferer;
    std::function<void(const std::string& streamId, const std::string& type, const std::string& sdp)> onRemoteSDP;
    std::function<void(const std::string& streamId, const std::string& cand, const std::string& mid)> onRemoteCandidate;
    
    // Explicit structural notifications split from original giant notification loop
    std::function<void(bool isOnlyPlayer)> onJoinedRoomNotification;
    std::function<void()> onPublishStartedNotification;
    std::function<void(const std::string& streamId)> onPlayStartedNotification;
    std::function<void(const std::string& streamId)> onPlayFinishedNotification;
    std::function<void(const std::string& trackId)> onSubtrackAddedNotification;
    std::function<void(const std::string& trackId)> onSubtrackRemovedNotification;

    // Control APIs initiated from outside
    bool Connect(const std::string& url, const std::string& publishStreamId);
    void Disconnect();
    bool IsOpen() const { return m_wsOpen; }

    // Wrapped AMS interface proxy calls
    void SendJoinRoom();
    void SendPublish(bool video, bool audio);
    void SendPlayRoom();
    void SendPlayStream(const std::string& trackId);
    void SendTakeCandidate(const std::string& streamId, const rtc::Candidate& c);
    void SendTakeConfiguration(const std::string& streamId, const std::string& type, const std::string& sdp);
    void SendStopPublish();
    void SendStopPlayRoom();
    void SendLeaveRoom();
    void ShutdownClient();

private:
    AMSEvent stringToEvent(const std::string& def);
    void dispatchNotification(const nlohmann::json& msg);

    std::string m_roomId;
    rtc::WebSocket m_ws;
    std::shared_ptr<AMSSignalingClient> m_signaling;
    bool m_wsOpen;
};

#endif
