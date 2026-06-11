#include "AMS_SFU_ConferenceSignaling.h"
#include "Log.h" // For DBG interface standard out

using json = nlohmann::json;
using namespace std;

AMS_SFU_ConferenceSignaling::AMS_SFU_ConferenceSignaling(const string& roomId)
    : m_roomId(roomId), m_wsOpen(false) {
}

AMS_SFU_ConferenceSignaling::~AMS_SFU_ConferenceSignaling() {
    Disconnect();
}

AMSEvent AMS_SFU_ConferenceSignaling::stringToEvent(const string& def) {
    static const unordered_map<string, AMSEvent> eventMap = {
        {"joinedTheRoom",   AMSEvent::JOINED_THE_ROOM},
        {"publish_started", AMSEvent::PUBLISH_STARTED},
        {"play_started",    AMSEvent::PLAY_STARTED},
        {"play_finished",   AMSEvent::PLAY_FINISHED},
        {"subtrackAdded",   AMSEvent::SUBTRACK_ADDED},
        {"subtrackRemoved", AMSEvent::SUBTRACK_REMOVED}
    };
    auto it = eventMap.find(def);
    return (it != eventMap.end()) ? it->second : AMSEvent::UNKNOWN;
}

bool AMS_SFU_ConferenceSignaling::Connect(const string& url, const string& publishStreamId) {
    m_wsOpen = false;

    // Instantiate signaling implementation client wrapper
    m_signaling = make_shared<AMSSignalingClient>(&m_ws, publishStreamId, m_roomId);

    // Bind state signaling client callback internals
    m_signaling->onStartOfferer = [this](const string& streamId) {
        if (onStartOfferer) onStartOfferer(streamId);
    };

    m_signaling->onRemoteSDP = [this](const string& streamId, const string& type, const string& sdp) {
        if (onRemoteSDP) onRemoteSDP(streamId, type, sdp);
    };

    m_signaling->onRemoteCandidate = [this](const string& streamId, const string& cand, const string& mid, int label) {
        if (onRemoteCandidate) onRemoteCandidate(streamId, cand, mid);
    };

    m_signaling->onNotification = [this](const nlohmann::json& msg) {
        dispatchNotification(msg);
    };

    // Bind underlying raw WebSocket handler hooks
    m_ws.onOpen([this]() {
        m_wsOpen = true;
        if (onConnected) onConnected();
    });

    m_ws.onMessage([this](variant<vector<std::byte>, string> msg) {
        if (holds_alternative<string>(msg)) {
            try {
                if (m_signaling) {
                    m_signaling->handleMessage(json::parse(get<string>(msg)));
                }
            } catch (...) {
                // Gracefully suppress parsing anomalies
            }
        }
    });

    m_ws.onClosed([this]() {
        m_wsOpen = false;
        if (onDisconnected) onDisconnected();
    });

    try {
        m_ws.open(url);
    } catch (const std::exception& e) {
        return false;
    }

    return true;
}

void AMS_SFU_ConferenceSignaling::Disconnect() {
    m_ws.onMessage(nullptr);
    m_ws.onOpen(nullptr);
    m_ws.onClosed(nullptr);

    if (m_wsOpen) {
        try { m_ws.close(); } catch (...) {}
    }
    m_wsOpen = false;
}

void AMS_SFU_ConferenceSignaling::dispatchNotification(const nlohmann::json& msg) {
    string def = msg.value("definition", "");
    AMSEvent event = stringToEvent(def);

    string streamId = msg.contains("streamId") ? msg["streamId"].get<string>() : "";
    string trackId  = msg.contains("trackId")  ? msg["trackId"].get<string>()  : "";

    switch (event) {
        case AMSEvent::JOINED_THE_ROOM:
            if (onJoinedRoomNotification) {
                // The room metadata evaluation can be passed out based on message payload or structural triggers
                onJoinedRoomNotification(false); 
            }
            break;

        case AMSEvent::PUBLISH_STARTED:
            if (onPublishStartedNotification) onPublishStartedNotification();
            break;

        case AMSEvent::PLAY_STARTED:
            if (onPlayStartedNotification) onPlayStartedNotification(streamId);
            break;

        case AMSEvent::PLAY_FINISHED:
            if (onPlayFinishedNotification) onPlayFinishedNotification(streamId);
            break;

        case AMSEvent::SUBTRACK_ADDED:
            if (onSubtrackAddedNotification) onSubtrackAddedNotification(trackId);
            break;

        case AMSEvent::SUBTRACK_REMOVED:
            if (onSubtrackRemovedNotification) onSubtrackRemovedNotification(trackId);
            break;

        default:
            if (!def.empty()) {
                DBG(0, "[warning] Unknown AMS event definition: %s\n", def.c_str());
            }
            break;
    }
}

void AMS_SFU_ConferenceSignaling::SendJoinRoom() {
    if (m_signaling) m_signaling->sendJoinRoom();
}

void AMS_SFU_ConferenceSignaling::SendPublish(bool video, bool audio) {
    if (m_signaling) m_signaling->sendPublish(video, audio);
}

void AMS_SFU_ConferenceSignaling::SendPlayRoom() {
    if (m_signaling) m_signaling->sendPlayRoom();
}

void AMS_SFU_ConferenceSignaling::SendPlayStream(const string& trackId) {
    if (m_signaling) m_signaling->sendPlayStream(trackId);
}

void AMS_SFU_ConferenceSignaling::SendTakeCandidate(const string& streamId, const rtc::Candidate& c) {
    if (m_signaling) m_signaling->sendTakeCandidate(streamId, c);
}

void AMS_SFU_ConferenceSignaling::SendTakeConfiguration(const string& streamId, const string& type, const string& sdp) {
    if (m_signaling) m_signaling->sendTakeConfiguration(streamId, type, sdp);
}

void AMS_SFU_ConferenceSignaling::SendStopPublish() {
    if (m_signaling) m_signaling->sendStopPublish();
}

void AMS_SFU_ConferenceSignaling::SendStopPlayRoom() {
    if (m_signaling) m_signaling->sendStopPlayRoom();
}

void AMS_SFU_ConferenceSignaling::SendLeaveRoom() {
    if (m_signaling) m_signaling->sendLeaveRoom();
}

void AMS_SFU_ConferenceSignaling::ShutdownClient() {
    if (m_signaling) {
        m_signaling->shutdown();
        m_signaling.reset();
    }
}
