#include "AMSSignalingClient.h"
#include "Logger.h"
#include <iostream>

using namespace rtc;
using json = nlohmann::json;
using namespace std;

// ---------------- Constructor / Destructor ----------------

AMSSignalingClient::AMSSignalingClient(rtc::WebSocket* ws,
                                       const std::string& selfStreamId,
                                       const std::string& roomId)
    : ws_(ws),
      selfStreamId_(selfStreamId),
      roomId_(roomId)
{
}

AMSSignalingClient::~AMSSignalingClient()
{
}

// ---------------- Internal helper ----------------

void AMSSignalingClient::sendJson(const json& j)
{
    if (!ws_) return;

    std::string msg = j.dump();
    ws_->send(msg);
	if(j["command"] == "takeConfiguration")
		logWithTime("[Peer => AMS] takeConfiguration");
	else
		logWithTime("[Peer => AMS] " + msg);
}

// ---------------- Outgoing signaling commands ----------------

void AMSSignalingClient::sendPublish(bool video, bool audio)
{
    json publish = {
        {"command", "publish"},
        {"streamId", selfStreamId_},
        {"streamName", selfStreamId_},
        {"mainTrack", roomId_},
        {"metaData", {}},
        {"subscriberCode", "client1"},
        {"subscriberId", "client1"},
        {"video", video}, // Use the parameter
        {"audio", audio}, // Use the parameter
        {"role", ""}
    };

    sendJson(publish);
}

void AMSSignalingClient::sendJoinRoom()
{
    json join = {
        {"command", "joinRoom"},
        {"streamId", selfStreamId_},
        {"room", roomId_},
		{"mode", "amcu"}
    };

    sendJson(join);
}

// Play whole room
void AMSSignalingClient::sendPlayRoom()
{
    json play = {
        {"command", "play"},
        {"room", roomId_},
        {"streamId", roomId_}
    };

    sendJson(play);
}

// Play specific stream
void AMSSignalingClient::sendPlayStream(const std::string& targetStreamId)
{
    json play = {
        {"command", "play"},
        {"room", roomId_},
        {"streamId", targetStreamId}
    };

    sendJson(play);
}

void AMSSignalingClient::sendGetRoomInfo()
{
    json getRoomInfo = {
        {"command", "getRoomInfo"},
        {"room", roomId_},
		{"streamId", roomId_}
    };

    sendJson(getRoomInfo);
}

void AMSSignalingClient::sendTakeConfiguration(const std::string& streamId,
                                               const std::string& type,
                                               const std::string& sdp)
{
    json msg = {
        {"command", "takeConfiguration"},
        {"streamId", streamId},
        {"type", type},
        {"sdp", sdp}
    };

    sendJson(msg);
}

void AMSSignalingClient::sendTakeCandidate(const std::string& streamId,
                                           const rtc::Candidate& cand)
{
    json candMsg = {
        {"command", "takeCandidate"},
        {"streamId", streamId},
        {"candidate", cand.candidate()},
        {"label", candLabel_++}
    };

    if (!cand.mid().empty())
        candMsg["id"] = cand.mid();

    sendJson(candMsg);
}

void AMSSignalingClient::sendStopPublish()
{
    json play = {
        {"command", "stop"},
        {"streamId", selfStreamId_}
    };

    sendJson(play);
}

void AMSSignalingClient::sendStopPlayRoom()
{
    json play = {
        {"command", "stop"},
        {"streamId", roomId_}
    };

    sendJson(play);
}

void AMSSignalingClient::sendLeaveRoom()
{
    json leaveRoom = {
        {"command", "leaveFromRoom"},
        {"room", roomId_}
    };

    sendJson(leaveRoom);
}

// ---------------- Incoming message handler ----------------

void AMSSignalingClient::handleMessage(const json& msg)
{
    if (!msg.contains("command"))
        return;

	std::string command = msg.value("command", "");
	std::string streamId, def;
	if (msg.contains("definition")) {
		def = msg["definition"];
		if (def == "bitrateMeasurement") {
			return;
		}
	}
		
	if (msg.contains("streamId"))
		streamId = msg.value("streamId", "");
	if (command == "takeConfiguration" || command == "takeCandidate")
		logWithTime("[AMS => Peer] command = " + command + ", streamId = " + streamId);
	else
		logWithTime("[AMS => Peer] " + msg.dump());
	
    if (command == "start") {
        if (onStartOfferer)
            onStartOfferer(streamId);
    }
    else if (command == "takeConfiguration") {
        std::string type = msg.value("type", "");
        std::string sdp  = msg.value("sdp", "");

        if (onRemoteSDP)
            onRemoteSDP(streamId, type, sdp);
    }
    else if (command == "takeCandidate") {
        std::string cand = msg.value("candidate", "");
        std::string mid  = msg.value("id", "");
        int label        = msg.value("label", 0);

        if (onRemoteCandidate)
            onRemoteCandidate(streamId, cand, mid, label);
    }
    else if (command == "notification") {
        if (onNotification)
            onNotification(msg);
    }
}

void AMSSignalingClient::shutdown()
{
    onStartOfferer = nullptr;
    onRemoteSDP = nullptr;
    onRemoteCandidate = nullptr;
    onNotification = nullptr;

    logWithTime("[AMSSignalingClient] shutdown complete, callbacks detached");
}

