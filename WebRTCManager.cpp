#include "WebRTCManager.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <sstream>
#include <cstdlib>
#include <ctime>

WebRTCManager::WebRTCManager() 
    : m_signalingServerUrl(""), 
      m_roomId(ROOM_ID_STR), 
      m_publishStreamId(""),
      m_isOnlyPlayer(false), 
      m_isCommunicating(false) 
{
    // Initialize libdatachannel logger
    rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel level, const std::string& msg) {
        if (msg.find("juice: Send failed") != std::string::npos || msg.find("STUN binding failed") != std::string::npos) return;
        std::cerr << msg << std::endl;
    });
}

WebRTCManager::~WebRTCManager() {
    Stop2WayCommunication();
}

void WebRTCManager::SetRoomId(const std::string &roomId)
{
    m_roomId = roomId;
}

void WebRTCManager::SetStreamId(const std::string &streamId)
{
    m_publishStreamId = streamId;
}

// API 1: Set Signaling Server URL
void WebRTCManager::SetSignalingServerURL(const string& url) {
    lock_guard<recursive_mutex> lock(m_apiMutex);
    m_signalingServerUrl = url;
    DBG(0, "[API] Signaling Server URL set to: %s\n", m_signalingServerUrl.c_str());
}

// API 2: Start Two-Way Communication (Non-blocking)
bool WebRTCManager::Start2WayCommunication(bool isPlayer) {
    lock_guard<recursive_mutex> lock(m_apiMutex);
    if (m_isCommunicating) {
        DBG(0, "[API] Communication is already running.\n");
        return true;
    }

    if (m_signalingServerUrl.empty() || m_signalingServerUrl == "AMS_WEBSOCKET_URL") {
        DBG(0, "[ERROR] Invalid Signaling Server URL. Please set valid URL first.\n");
        return false;
    }

    m_isOnlyPlayer = isPlayer;
    m_isCommunicating = true;

    DBG(0, "[init] Version 1.0.4 - Starting API Instance\n");
    DBG(0, "[init] URL: %s\n", m_signalingServerUrl.c_str());

    // Setup GStreamer Manager
    m_gstManager = make_unique<GstManager>(720, 480, 30, 1000000);
    if(m_isOnlyPlayer == false){
        m_gstManager->startVideo();
        m_gstManager->startAudio();
    }
    m_gstManager->startAudioPlayer();

    // Setup abstracted context component
    m_signalingContext = make_unique<AMS_SFU_ConferenceSignaling>(m_roomId);

    // Establish decoupled modern state machine bindings using functional lambdas
    initWebRTCStateMachine();

    if (!m_signalingContext->Connect(m_signalingServerUrl, m_publishStreamId)) {
        DBG(0, "[ERROR] WebSocket failed to open via decoupled handler\n");
        cleanupResources();
        m_isCommunicating = false;
        return false;
    }

    return true;
}

// API 3: Stop Two-Way Communication
void WebRTCManager::Stop2WayCommunication() {
    lock_guard<recursive_mutex> lock(m_apiMutex);
    if (!m_isCommunicating) return;

    DBG(0, "[API] Stopping Two-Way Communication...\n");
    cleanupResources();
    m_isCommunicating = false;
    DBG(0, "[API] Stopped successfully\n");
}

// Exposed Chat Tool API for main usage
void WebRTCManager::SendChatMessage(const string& text) {
    lock_guard<recursive_mutex> lock(m_apiMutex);
    if (!m_isCommunicating || !m_signalingContext || !m_signalingContext->IsOpen()) {
        DBG(0, "[API] Cannot send chat message. Not connected.\n");
        return;
    }

    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);

    char dateBuf[64];
    std::strftime(dateBuf, sizeof(dateBuf), "%a %b %d %Y %H:%M:%S GMT%z", &tm);

    json j = {
        {"eventType", "MESSAGE_RECEIVED"},
        {"message", text},
        {"senderId", m_publishStreamId},
        {"name", m_publishStreamId},
        {"date", std::string(dateBuf)}
    };

    string payload = j.dump();

    lock_guard<recursive_mutex> stateLock(m_stateMutex);
    if (localDataChannels.count(m_publishStreamId)) {
        auto dc = localDataChannels[m_publishStreamId];
        if (dc && dc->isOpen()) {
            DBG(0, "[chat] [localDc] [%s] %s\n", dc->label().c_str(), payload.c_str());
            dc->send(payload);
        }
    }
}

void WebRTCManager::SendBinaryMessage(const std::vector<std::byte>& data, const std::string& label) {
    lock_guard<recursive_mutex> stateLock(m_stateMutex);
    if (localDataChannels.count(label) && localDataChannels[label]->isOpen()) {
        localDataChannels[label]->send(data);
    }
}

void WebRTCManager::initWebRTCStateMachine() {
    // Setup self connection parameters explicitly aligned to initialization
    setupPublishPeer(m_publishStreamId);

    m_signalingContext->onConnected = [this]() {
        DBG(0, "[WS] connected\n");
        setupPublishPeer(m_publishStreamId);
        m_signalingContext->SendJoinRoom();
    };

    m_signalingContext->onDisconnected = [this]() {
        DBG(0, "[WS] closed\n");
    };

    m_signalingContext->onStartOfferer = [this](const string& streamId){
        DBG(0, "[info] Got 'start', I am offerer for stream %s\n", streamId.c_str());
        startOffer(streamId);
    };

    m_signalingContext->onRemoteSDP = [this](const string& streamId, const string& type, const string& sdp){
        handleRemoteSDP(streamId, type, sdp);
    };

    m_signalingContext->onRemoteCandidate = [this](const string& streamId, const string& cand, const string& mid){
        addRemoteCandidate(streamId, cand, mid);
    };

    // Mapping clean atomic logic bindings from external event loop notifications
    m_signalingContext->onJoinedRoomNotification = [this](bool triggeredByAms) {
        if (m_isOnlyPlayer) {
            DBG(0, "[Role] Player: Publishing with NO media (DataChannel only)\n");
            m_signalingContext->SendPublish(false, false);
        } else {
            DBG(0, "[Role] Publisher: Publishing WITH media\n");
            m_signalingContext->SendPublish(true, true);
        }
    };

    m_signalingContext->onPublishStartedNotification = [this]() {
        if (sendPlay.find(m_roomId) == sendPlay.end()) {
            sendPlay[m_roomId] = true;
            m_signalingContext->SendPlayRoom();
        }
    };

    m_signalingContext->onPlayStartedNotification = [this](const string& streamId) {
        if (!streamId.empty()) {
            played[streamId] = true;
        }
    };

    m_signalingContext->onPlayFinishedNotification = [this](const string& streamId) {
        if (!streamId.empty()) {
            removePeerIfNotExist(streamId);
            if (streamId == m_roomId) {
                sendPlay[m_roomId] = true;
                m_signalingContext->SendPlayRoom();
                DBG(0, "[room] Only one peer in the room, send play room again\n");
            }
        }
    };

    m_signalingContext->onSubtrackAddedNotification = [this](const string& trackId) {
        if (!trackId.empty() && trackId != m_publishStreamId) {
            lock_guard<recursive_mutex> lock(m_stateMutex);

            if (pcs.count(trackId)) {
                if (pcs[trackId]->state() != rtc::PeerConnection::State::Closed) {
                    return;
                }
            }

            if (sendPlay.count(trackId) && sendPlay[trackId]) return;

            sendPlay[trackId] = true;
            DBG(0, "[room] New remote track detected: %s, sending play\n", trackId.c_str());
            m_signalingContext->SendPlayStream(trackId);
        }
    };

    m_signalingContext->onSubtrackRemovedNotification = [this](const string& trackId) {
        if (!trackId.empty()) {
            if (sendPlay.count(trackId)) {
                sendPlay.erase(trackId);
                DBG(0, "[room] Remote track leaved, trackId %s\n", trackId.c_str());
            } else {
                DBG(0, "[room] Remote track leaved, ignore trackId %s\n", trackId.c_str());
            }
        }
    };
}

void WebRTCManager::setupPublishPeer(const string& streamId){
    lock_guard<recursive_mutex> lock(m_stateMutex);
    if(pcs.count(streamId)) return;

    auto pc = createPeerIfNotExist(streamId, true);

    m_publishStreamId = streamId;
    isPublisher[streamId] = true;

    if (!m_isOnlyPlayer)
        setupLocalTracks(pc, streamId);

    if(!localDataChannels.count(streamId)){
        auto dc = pc->createDataChannel(streamId);
        localDataChannels[streamId] = dc;

        dc->onOpen([streamId](){
            DBG(0, "[DataChannel][Local][%s] opened\n", streamId.c_str());
        });

        dc->onClosed([streamId](){
            DBG(0, "[DataChannel][Local][%s] closed\n", streamId.c_str());
        });

        dc->onError([streamId](const string& e){
            DBG(0, "[DataChannel][Local][%s] error: %s\n", streamId.c_str(), e.c_str());
        });

        dc->onMessage([this, streamId](variant<vector<std::byte>, string> msg){
            if(holds_alternative<string>(msg)){
                handleDataChannelText(streamId, get<string>(msg));
            }
        });
    }
}

shared_ptr<PeerConnection> WebRTCManager::createPeerIfNotExist(const string& streamId, bool needDC){
    if(pcs.count(streamId)) return pcs[streamId];

    DBG(0, "[pc] create PeerConnection for stream %s\n", streamId.c_str());

    Configuration config;
    config.disableAutoNegotiation = true;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");

    auto pc = make_shared<PeerConnection>(config);
    pcs[streamId] = pc;
    isPublisher[streamId] = false;

    if(needDC) setupDataChannel(pc, streamId);

    pc->onTrack([this, streamId](shared_ptr<Track> track){ addRemoteTrack(track, streamId); });
    pc->onLocalCandidate([this, streamId](Candidate c){ if (m_signalingContext) m_signalingContext->SendTakeCandidate(streamId, c); });
    pc->onLocalDescription([this, streamId](Description desc){
        if (m_signalingContext) {
            m_signalingContext->SendTakeConfiguration(
                streamId,
                desc.type() == Description::Type::Offer ? "offer" : "answer",
                string(desc)
            );
        }
    });

    pc->onIceStateChange([streamId](PeerConnection::IceState state) {
        DBG(0, "[ICE][%s] State: %d\n", streamId.c_str(), static_cast<int>(state));
    });

    return pc;
}

void WebRTCManager::removePeerIfNotExist(const string& streamId){
    lock_guard<recursive_mutex> lock(m_stateMutex);
    if(localDataChannels.count(streamId)){
        if(localDataChannels[streamId]->isOpen()) localDataChannels[streamId]->close();
        localDataChannels.erase(streamId);
    }
    if(remoteDataChannels.count(streamId)){
        if(remoteDataChannels[streamId]->isOpen()) remoteDataChannels[streamId]->close();
        remoteDataChannels.erase(streamId);
    }
    if(pcs.count(streamId)){
        pcs[streamId]->close();
        pcs.erase(streamId);
    }
    played.erase(streamId);
    sendPlay.erase(streamId);
}

void WebRTCManager::addRemoteTrack(shared_ptr<Track> track, const string &streamId) {
    lock_guard<recursive_mutex> lock(m_stateMutex);
    remoteTracks[streamId].push_back(track);
    track->setMediaHandler(make_shared<RtcpReceivingSession>());

    string kind = midTypeMap.count(track->mid()) ? midTypeMap[track->mid()] : track->description().type();

    DBG(0, "[SFU] Remote track added for stream %s mid=%s type=%s\n", streamId.c_str(), track->mid().c_str(), kind.c_str());

    track->onMessage([this, kind, streamId](std::variant<std::vector<std::byte>, std::string> msg){
        if (kind == "audio" && holds_alternative<std::vector<std::byte>>(msg)){
            auto &data = get<std::vector<std::byte>>(msg);
            DBG(1, "Get audio data, len = %ld\n", data.size());
            bool playStream = false;
            lock_guard<recursive_mutex> lock(m_stateMutex);
            if(isStreamTalking.count(streamId))
                playStream = isStreamTalking[streamId];
                if(m_gstManager) m_gstManager->pushAudioFrame(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        }
    });
}

void WebRTCManager::prepareRecvTracksFromSDP(shared_ptr<PeerConnection> pc, const string &sdp){
    auto sdpJson = sdptransform::parse(sdp);
    if(!sdpJson.contains("media")) return;

    for(auto &media : sdpJson["media"]){
        string type = media.value("type","");
        string dir  = media.value("direction","sendrecv");
        if((dir != "sendrecv" && dir != "sendonly") || (type != "audio" && type != "video")) continue;

        int payloadType = -1;
        if(media.contains("rtp")){
            for(auto &rtp : media["rtp"]){
                string codec = rtp.value("codec","");
                int pt = rtp.value("payload",-1);
                if(type == "video" && codec=="H264"){ payloadType=pt; break;}
                if(type == "audio" && codec=="opus"){ payloadType=pt; break;}
            }
        }
        if(payloadType < 0) continue;

        string trackName = "remote-" + type + "-" + to_string(payloadType);
        if(type=="video"){
            Description::Video recv(trackName, Description::Direction::RecvOnly);
            recv.addH264Codec(payloadType);
            auto track = pc->addTrack(recv);
            midTypeMap[track->mid()] = "video";
        } else {
            Description::Audio recv(trackName, Description::Direction::RecvOnly);
            recv.addOpusCodec(payloadType);
            auto track = pc->addTrack(recv);
            midTypeMap[track->mid()] = "audio";
        }
    }
}

void WebRTCManager::setupLocalTracks(shared_ptr<PeerConnection> pc, const string &streamId){
    // Video Track Setup
    Description::Video videoDesc("video-stream", Description::Direction::SendOnly);
    videoDesc.addH264Codec(96);
    videoDesc.addSSRC(SSRC(42),"video-stream","stream","video-stream");
    auto localVideoTrack = pc->addTrack(videoDesc);

    localVideoTrack->onOpen([this, localVideoTrack](){
        if(m_gstManager) {
            m_gstManager->setOnVideoRTPFrame([localVideoTrack](const vector<uint8_t>& rtpPayload){
                if(localVideoTrack->isOpen())
                    localVideoTrack->send(reinterpret_cast<const std::byte*>(rtpPayload.data()), rtpPayload.size());
            });
            m_gstManager->startVideo();
        }
    });

    // Audio Track Setup
    Description::Audio audioDesc("audio-stream", Description::Direction::SendOnly);
    audioDesc.addOpusCodec(111);
    audioDesc.addSSRC(SSRC(43),"audio-stream","stream","audio-stream");
    auto localAudioTrack = pc->addTrack(audioDesc);

    localAudioTrack->onOpen([this, localAudioTrack](){
        if(m_gstManager) {
            m_gstManager->setOnAudioRTPFrame([localAudioTrack](const vector<uint8_t>& rtpPayload){
                if(localAudioTrack->isOpen())
                    localAudioTrack->send(reinterpret_cast<const std::byte*>(rtpPayload.data()), rtpPayload.size());
            });
            m_gstManager->startAudio();
        }
    });
}

void WebRTCManager::setupDataChannel(shared_ptr<PeerConnection> pc, const string &label){
    pc->onDataChannel([this](shared_ptr<DataChannel> remoteDc){
        lock_guard<recursive_mutex> lock(m_stateMutex);
        auto label = remoteDc->label();
        DBG(0, "[DataChannel][Remote][%s] detected\n", label.c_str());
        if(remoteDataChannels.count(label)) return;
        remoteDataChannels[label] = remoteDc;

        remoteDc->onOpen([label](){ DBG(0, "[DataChannel][Remote][%s] opened\n", label.c_str()); });
        remoteDc->onClosed([label](){ DBG(0, "[DataChannel][Remote][%s] closed\n", label.c_str()); });
        remoteDc->onMessage([this, label](const variant<vector<std::byte>, string>& msg){
            if (holds_alternative<string>(msg)) {
                handleDataChannelText(label, get<string>(msg));
            }
        });
    });
}

void WebRTCManager::handleDataChannelText(const std::string& label, const std::string& text) {
    json j;
    try { j = json::parse(text); } catch (...) { return; }
    if (!j.contains("eventType")) return;
    if (j["eventType"].get<std::string>() == "AUDIO_TRACK_ASSIGNMENT") return;

    DBG(0, "[Chat] [%s] %s\n", label.c_str(), j["eventType"].get<std::string>().c_str());

    if (!j.contains("senderId") || !j.contains("message")) return;

    string senderId = j["senderId"].get<string>();
    string msgText = j["message"].get<string>();

    lock_guard<recursive_mutex> lock(m_stateMutex);
    if (msgText == "iamtalking") {
        isStreamTalking[senderId] = true;
        DBG(0, "[AudioControl] Start playing audio from: %s\n", senderId.c_str());
    }
    else if (msgText == "iammute") {
        isStreamTalking[senderId] = false;
        DBG(0, "[AudioControl] Stop playing audio from: %s\n", senderId.c_str());
    }

    DBG(0, "[Chat] = %s\n", j.dump().c_str());
}

void WebRTCManager::handleRemoteSDP(const string &streamId, const string &type, const string &sdp){
    lock_guard<recursive_mutex> lock(m_stateMutex);
    bool needDC = (streamId == m_roomId);
    auto pc = createPeerIfNotExist(streamId, needDC);
    if(type=="offer") prepareRecvTracksFromSDP(pc, sdp);
    Description remoteDesc(sdp, type=="offer"?Description::Type::Offer:Description::Type::Answer);
    pc->setRemoteDescription(remoteDesc);
    if(pendingCandidates.count(streamId)){
        for(auto &c : pendingCandidates[streamId]) pc->addRemoteCandidate(c);
        pendingCandidates.erase(streamId);
    }
    if(type=="offer") pc->setLocalDescription(Description::Type::Answer);
}

void WebRTCManager::addRemoteCandidate(const string &streamId, const string &cand, const string &mid){
    if(cand.empty()) return;
    lock_guard<recursive_mutex> lock(m_stateMutex);
    if(!pcs.count(streamId)){ pendingCandidates[streamId].emplace_back(cand, mid); return;}
    pcs[streamId]->addRemoteCandidate(Candidate(cand, mid));
}

void WebRTCManager::startOffer(const string &streamId){
    lock_guard<recursive_mutex> lock(m_stateMutex);
    if(pcs.count(streamId)) pcs[streamId]->setLocalDescription(Description::Type::Offer);
}

void WebRTCManager::cleanupResources() {
    if (m_signalingContext && !m_publishStreamId.empty() && isPublisher[m_publishStreamId]){
        try {
            m_signalingContext->SendStopPublish();
            m_signalingContext->SendStopPlayRoom();
            m_signalingContext->SendLeaveRoom();
        } catch(...) {}
    }
    if (m_gstManager) m_gstManager->stopAudioPlayer();
    m_gstManager.reset();

    DBG(0, "[cleanup] closing all PeerConnections\n");

    lock_guard<recursive_mutex> lock(m_stateMutex);
    for (auto &[id, pc] : pcs) {
        pc->onTrack(nullptr);
        pc->onLocalCandidate(nullptr);
        pc->onLocalDescription(nullptr);
        pc->close();
    }

    pcs.clear();
    localDataChannels.clear();
    remoteDataChannels.clear();
    remoteTracks.clear();
    played.clear();
    sendPlay.clear();
    pendingCandidates.clear();
    midTypeMap.clear();
    isStreamTalking.clear();
    isPublisher.clear();

    if (m_signalingContext) { 
        m_signalingContext->Disconnect();
        m_signalingContext->ShutdownClient(); 
        m_signalingContext.reset(); 
    }
}
