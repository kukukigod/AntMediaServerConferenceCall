#pragma once

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <functional>
#include <string>

class AMSSignalingClient {
public:
    AMSSignalingClient(rtc::WebSocket* ws,
                       const std::string& selfStreamId,
                       const std::string& roomId);

    ~AMSSignalingClient();

    // ---- Outgoing commands ----
    void sendPublish(bool video, bool audio);
	void sendJoinRoom();
    void sendPlayRoom();
    void sendPlayStream(const std::string& targetStreamId);
    void sendGetRoomInfo();
    void sendTakeConfiguration(const std::string& streamId,
                               const std::string& type,
                               const std::string& sdp);
    void sendTakeCandidate(const std::string& streamId,
                           const rtc::Candidate& cand);
	void sendStopPublish();
	void sendStopPlayRoom();
	void sendLeaveRoom();
    // ---- Incoming message entry ----
    void handleMessage(const nlohmann::json& msg);

    // ---- Callbacks to upper layer (WebRTCManager) ----
    std::function<void(const std::string& streamId)> onStartOfferer;
    std::function<void(const std::string& streamId,
                       const std::string& type,
                       const std::string& sdp)> onRemoteSDP;
    std::function<void(const std::string& streamId,
                       const std::string& cand,
                       const std::string& mid,
                       int label)> onRemoteCandidate;
    std::function<void(const nlohmann::json& msg)> onNotification;
	
	void shutdown(); 

private:
    AMSSignalingClient(const AMSSignalingClient&) = delete;
    AMSSignalingClient& operator=(const AMSSignalingClient&) = delete;

private:
    rtc::WebSocket* ws_;

    std::string selfStreamId_;
    std::string roomId_;

    int candLabel_ = 0;

private:
    void sendJson(const nlohmann::json& j);
};
