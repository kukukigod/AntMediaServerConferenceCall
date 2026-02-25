#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include "sdptransform.hpp"
#include "GstManager.h"
#include "AMSSignalingClient.h"
#include "Logger.h"

#include <iostream>
#include <iomanip>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <csignal>
#include <chrono>
#include <unordered_map>
#include <sstream>
#include <mutex>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

using namespace rtc;
using json = nlohmann::json;
using namespace std;

#define AMS_WS    "AMS_WEBSOCKET_URL" //Please modify it
#define ROOM_ID_STR   "room1"  // Renamed to avoid confusion with roomId variable

#define WIDTH     720
#define HEIGHT    480
#define FPS       30
#define BITRATE   1000000

// 1. Move Enum outside for better scope access
enum class AMSEvent {
    UNKNOWN,
    JOINED_THE_ROOM,
    PUBLISH_STARTED,
    PLAY_STARTED,
    PLAY_FINISHED,
    SUBTRACK_ADDED,
    SUBTRACK_REMOVED
};

static atomic_bool g_running(true);
static atomic_bool g_ws_open(false);

static unique_ptr<GstManager> gstManager;
static string g_publishStreamId;
static shared_ptr<AMSSignalingClient> g_signaling;
static WebSocket g_ws;

static void signalHandler(int sig) {
    logWithTime("[signal] exiting");
    g_running = false;
}

class WebRTCManager {
    public:
        WebRTCManager(const string &roomId, bool isPlayer)
            : roomId(roomId), isOnlyPlayer(isPlayer) {}

        // 2. Helper to convert string definition to enum (Class member or static)
        AMSEvent stringToEvent(const std::string& def) {
            static const std::unordered_map<std::string, AMSEvent> eventMap = {
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

        void init() {
            gstManager = make_unique<GstManager>(WIDTH, HEIGHT, FPS, BITRATE);
            gstManager->startAudioPlayer();

            // Both roles setup the "Self" PeerConnection for DataChannel/Signaling
            setupPublishPeer(g_publishStreamId);
            g_signaling->onStartOfferer = [this](const string& streamId){
                logWithTime("[info] Got 'start', I am offerer for stream " + streamId);
                startOffer(streamId);
            };

            g_signaling->onRemoteSDP = [this](const string& streamId, const string& type, const string& sdp){
                handleRemoteSDP(streamId, type, sdp);
            };

            g_signaling->onRemoteCandidate = [this](const string& streamId, const string& cand, const string& mid, int label){
                addRemoteCandidate(streamId, cand, mid);
            };

            // 3. Updated onNotification handler (Clean State Machine)
            g_signaling->onNotification = [this](const nlohmann::json& msg) {
                std::string def = msg.value("definition", "");
                AMSEvent event = stringToEvent(def);

                // Pre-extract tokens to avoid redundant lookups and switch scope issues
                std::string streamId = msg.contains("streamId") ? msg["streamId"].get<std::string>() : "";
                std::string trackId  = msg.contains("trackId")  ? msg["trackId"].get<std::string>()  : "";

                switch (event) {
                    case AMSEvent::JOINED_THE_ROOM:
                        if (isOnlyPlayer) {
                            logWithTime("[Role] Player: Publishing with NO media (DataChannel only)");
                            g_signaling->sendPublish(false, false);
                        } else {
                            logWithTime("[Role] Publisher: Publishing WITH media");
                            g_signaling->sendPublish(true, true);
                        }
                        break;

                    case AMSEvent::PUBLISH_STARTED:
                        // Both roles will play the room to see others
                        if (sendPlay.find(roomId) == sendPlay.end()) {
                            sendPlay[roomId] = true;
                            g_signaling->sendPlayRoom();
                        }
                        break;

                    case AMSEvent::PLAY_STARTED:
                        if (!streamId.empty()) {
                            played[streamId] = true;
                        }
                        break;

                    case AMSEvent::PLAY_FINISHED:
                        if (!streamId.empty()) {
                            removePeerIfNotExist(streamId);
                            if (streamId == roomId) {
                                // AMS will send play_finished when there is only one peer in the room.
                                sendPlay[roomId] = true;
                                g_signaling->sendPlayRoom();
                                logWithTime("[room] Only one peer in the room, send play room again");
                            }
                        }
                        break;

                    case AMSEvent::SUBTRACK_ADDED:
                        if (!trackId.empty() && trackId != g_publishStreamId) {
                            lock_guard<recursive_mutex> lock(m_mutex);

                            // Check if PC exists and is NOT closed
                            if (pcs.count(trackId)) {
                                if (pcs[trackId]->state() != rtc::PeerConnection::State::Closed) {
                                    // Already has an active connection for this track, skip
                                    return;
                                }
                            }

                            if (sendPlay.count(trackId) && sendPlay[trackId]) return;

                            sendPlay[trackId] = true;
                            logWithTime("[room] New remote track detected: " + trackId + ", sending play");
                            g_signaling->sendPlayStream(trackId);
                        }
                        break;

                    case AMSEvent::SUBTRACK_REMOVED:
                        if (!trackId.empty()) {
                            if (sendPlay.count(trackId)) {
                                sendPlay.erase(trackId);
                                logWithTime("[room] Remote track leaved, trackId " + trackId);
                            } else {
                                logWithTime("[room] Remote track leaved, ignore trackId " + trackId);
                            }
                        }
                        break;

                    default:
                        if (!def.empty()) {
                            logWithTime("[warning] Unknown AMS event definition: " + def);
                        }
                        break;
                }
            };
        }

        // ---------------- Publish ----------------
        void setupPublishPeer(const string& streamId){
            lock_guard<recursive_mutex> lock(m_mutex);
            if(pcs.count(streamId)) return;

            auto pc = createPeerIfNotExist(streamId, true);

            publishStreamId = streamId;
            isPublisher[streamId] = true;

            if (!isOnlyPlayer)
                setupLocalTracks(pc, streamId);

            if(!localDataChannels.count(streamId)){
                auto dc = pc->createDataChannel(streamId);
                localDataChannels[streamId] = dc;

                dc->onOpen([this, streamId](){
                        logWithTime("[DataChannel][Local][" + streamId + "] opened");
                        });

                dc->onClosed([this, streamId](){
                        logWithTime("[DataChannel][Local][" + streamId + "] closed");
                        });

                dc->onError([this, streamId](const string& e){
                        logWithTime("[DataChannel][Local][" + streamId + "] error: " + e);
                        });

                dc->onMessage([this, streamId](variant<vector<std::byte>, string> msg){
                        if(holds_alternative<string>(msg)){
                        handleDataChannelText(streamId, get<string>(msg));
                        }
                        });
            }
        }

        // ---------------- Send Chat Message ----------------
        void sendChatMessage(const string& text){
            std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtime_r(&now, &tm);

            char dateBuf[64];
            std::strftime(dateBuf, sizeof(dateBuf), "%a %b %d %Y %H:%M:%S GMT%z", &tm);

            json j = {
                {"eventType", "MESSAGE_RECEIVED"},
                {"message", text},
                {"senderId", publishStreamId},
                {"name", publishStreamId},
                {"date", std::string(dateBuf)}
            };

            string payload = j.dump();

            lock_guard<recursive_mutex> lock(m_mutex);
            if(localDataChannels.count(publishStreamId)){
                auto dc = localDataChannels[publishStreamId];
                if(dc && dc->isOpen()){
                    logWithTime("[chat] [localDc] [" + dc->label() + "] " + payload);
                    dc->send(payload);
                }
            }
        }

        void sendBinaryMessage(const std::vector<std::byte>& data, const std::string& label=ROOM_ID_STR)
        {
            lock_guard<recursive_mutex> lock(m_mutex);
            if(localDataChannels.count(label) && localDataChannels[label]->isOpen()){
                localDataChannels[label]->send(data);
            }
        }

        void cleanup() {
            if(!publishStreamId.empty() && isPublisher[publishStreamId]){
                try {
                    g_signaling->sendStopPublish();
                    g_signaling->sendStopPlayRoom();
                    g_signaling->sendLeaveRoom();
                } catch(...) {}
            }
            if (gstManager) gstManager->stopAudioPlayer();
            gstManager.reset();

            logWithTime("[cleanup] closing all PeerConnections");

            lock_guard<recursive_mutex> lock(m_mutex);
            for (auto &[id, pc] : pcs) {
                // Pre-clear callbacks to prevent deadlocks during destruction
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
        }

    private:
        string roomId;
        string publishStreamId;
        bool isOnlyPlayer;
        recursive_mutex m_mutex;
        unordered_map<string, shared_ptr<PeerConnection>> pcs;
        unordered_map<string, bool> isPublisher;
        unordered_map<string, bool> sendPlay, played;
        unordered_map<string, vector<shared_ptr<Track>>> remoteTracks;
        unordered_map<string, vector<Candidate>> pendingCandidates;
        unordered_map<string, string> midTypeMap;
        unordered_map<string, shared_ptr<DataChannel>> localDataChannels;
        unordered_map<string, shared_ptr<DataChannel>> remoteDataChannels;
        unordered_map<string, bool> isStreamTalking;

        shared_ptr<PeerConnection> createPeerIfNotExist(const string& streamId, bool needDC){
            if(pcs.count(streamId)) return pcs[streamId];

            logWithTime("[pc] create PeerConnection for stream " + streamId);

            Configuration config;
            config.disableAutoNegotiation = true;
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");

            auto pc = make_shared<PeerConnection>(config);
            pcs[streamId] = pc;
            isPublisher[streamId] = false;

            if(needDC) setupDataChannel(pc, streamId);

            pc->onTrack([this, streamId](shared_ptr<Track> track){ addRemoteTrack(track, streamId); });
            pc->onLocalCandidate([streamId](Candidate c){ g_signaling->sendTakeCandidate(streamId, c); });
            pc->onLocalDescription([streamId](Description desc){
                    g_signaling->sendTakeConfiguration(
                            streamId,
                            desc.type() == Description::Type::Offer ? "offer" : "answer",
                            string(desc)
                            );
                    });

            pc->onIceStateChange([streamId](PeerConnection::IceState state) {
                    logWithTime("[ICE][" + streamId + "] State: " + to_string(static_cast<int>(state)));
                    });

            return pc;
        }

        void removePeerIfNotExist(const string& streamId){
            lock_guard<recursive_mutex> lock(m_mutex);
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

        void addRemoteTrack(shared_ptr<Track> track, const string &streamId) {
            lock_guard<recursive_mutex> lock(m_mutex);
            remoteTracks[streamId].push_back(track);
            track->setMediaHandler(make_shared<RtcpReceivingSession>());

            string kind = midTypeMap.count(track->mid()) ? midTypeMap[track->mid()] : track->description().type();

            logWithTime("[SFU] Remote track added for stream " + streamId + " mid=" + track->mid() + " type=" + kind);

            track->onMessage([this, kind, streamId](std::variant<std::vector<std::byte>, std::string> msg){
                    if (kind == "audio" && holds_alternative<std::vector<std::byte>>(msg)){
                    auto &data = get<std::vector<std::byte>>(msg);

                    bool playStream = false;
                    lock_guard<recursive_mutex> lock(m_mutex);
                    if(isStreamTalking.count(streamId))
                    playStream = isStreamTalking[streamId];
#if PLATFORM_NUM == 0x86
                    if(gstManager) gstManager->pushAudioFrame(reinterpret_cast<const uint8_t*>(data.data()), data.size());
#else
                    if(playStream == true) gstManager->pushAudioFrame(reinterpret_cast<const uint8_t*>(data.data()), data.size());
#endif
                    }
                    });
        }

        void prepareRecvTracksFromSDP(shared_ptr<PeerConnection> pc, const string &sdp){
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

        // ---------------- Local Tracks Setup ----------------
        void setupLocalTracks(shared_ptr<PeerConnection> pc, const string &streamId){
            // Video Track Setup
            Description::Video videoDesc("video-stream", Description::Direction::SendOnly);
            videoDesc.addH264Codec(96);
            videoDesc.addSSRC(SSRC(42),"video-stream","stream","video-stream");
            auto localVideoTrack = pc->addTrack(videoDesc);

            localVideoTrack->onOpen([this, localVideoTrack](){
                    // Updated to use specific RTP frame callback for WebRTC
                    gstManager->setOnVideoRTPFrame([localVideoTrack](const vector<uint8_t>& rtpPayload){
                            if(localVideoTrack->isOpen())
                            localVideoTrack->send(reinterpret_cast<const std::byte*>(rtpPayload.data()), rtpPayload.size());
                            });
                    gstManager->startVideo();
                    });

            // Audio Track Setup
            Description::Audio audioDesc("audio-stream", Description::Direction::SendOnly);
            audioDesc.addOpusCodec(111);
            audioDesc.addSSRC(SSRC(43),"audio-stream","stream","audio-stream");
            auto localAudioTrack = pc->addTrack(audioDesc);

            localAudioTrack->onOpen([this, localAudioTrack](){
                    // Updated to use specific RTP frame callback for WebRTC (Opus)
                    gstManager->setOnAudioRTPFrame([localAudioTrack](const vector<uint8_t>& rtpPayload){
                            if(localAudioTrack->isOpen())
                            localAudioTrack->send(reinterpret_cast<const std::byte*>(rtpPayload.data()), rtpPayload.size());
                            });
                    gstManager->startAudio();
                    });
        }

        void setupDataChannel(shared_ptr<PeerConnection> pc, const string &label){
            pc->onDataChannel([this](shared_ptr<DataChannel> remoteDc){
                    lock_guard<recursive_mutex> lock(m_mutex);
                    auto label = remoteDc->label();
                    logWithTime("[DataChannel][Remote][" + label + "] detected");
                    if(remoteDataChannels.count(label)) return;
                    remoteDataChannels[label] = remoteDc;

                    remoteDc->onOpen([label](){ logWithTime("[DataChannel][Remote][" + label + "] opened"); });
                    remoteDc->onClosed([label](){ logWithTime("[DataChannel][Remote][" + label + "] closed"); });
                    remoteDc->onMessage([this, label](const variant<vector<std::byte>, string>& msg){
                            if (holds_alternative<string>(msg)) {
                            handleDataChannelText(label, get<string>(msg));
                            }
                            });
                    });
        }

        void handleDataChannelText(const std::string& label, const std::string& text)
        {
            json j;
            try { j = json::parse(text); } catch (...) { return; }
            if (!j.contains("eventType")) return;

            if (j["eventType"].get<std::string>() == "AUDIO_TRACK_ASSIGNMENT") return;

            logWithTime("[Chat] [" + label + "] " + j["eventType"].get<std::string>());

            if (!j.contains("senderId") || !j.contains("message")) return;

            string senderId = j["senderId"].get<string>();
            string msgText = j["message"].get<string>();

            lock_guard<recursive_mutex> lock(m_mutex);
            if (msgText == "iamtalking") {
                isStreamTalking[senderId] = true;
                logWithTime("[AudioControl] Start playing audio from: " + senderId);
            }
            else if (msgText == "iammute") {
                isStreamTalking[senderId] = false;
                logWithTime("[AudioControl] Stop playing audio from: " + senderId);
            }
            // ------------------

            logWithTime("[Chat] = " + j.dump());
        }

        void handleRemoteSDP(const string &streamId, const string &type, const string &sdp){
            lock_guard<recursive_mutex> lock(m_mutex);
            bool needDC = (streamId == roomId);
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

        void addRemoteCandidate(const string &streamId, const string &cand, const string &mid){
            if(cand.empty()) return;
            lock_guard<recursive_mutex> lock(m_mutex);
            if(!pcs.count(streamId)){ pendingCandidates[streamId].emplace_back(cand, mid); return;}
            pcs[streamId]->addRemoteCandidate(Candidate(cand, mid));
        }

        void startOffer(const string &streamId){
            lock_guard<recursive_mutex> lock(m_mutex);
            if(pcs.count(streamId)) pcs[streamId]->setLocalDescription(Description::Type::Offer);
        }
};

// ... consoleInputThread implementation remains same ...
std::thread startConsoleInputThread(std::atomic_bool& running, std::function<void(const std::string&)> onLine)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    return std::thread([&running, onLine]() {
            char buf[512];
            std::string lineBuffer;
            struct pollfd pfd{};

            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;

            while (running) {
            if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
            if (n > 0) {
            buf[n] = '\0';
            lineBuffer.append(buf);
            size_t pos;
            while ((pos = lineBuffer.find('\n')) != std::string::npos) {
            std::string line = lineBuffer.substr(0, pos);
            lineBuffer.erase(0, pos + 1);
            if (!line.empty()) onLine(line);
            }
            }
            }
            }
    });
}

int main(int argc, char* argv[])
{
    // Environment Variable Support
    const char* env_url = std::getenv("AMS_WS_URL");
    string ams_url = (env_url != nullptr) ? string(env_url) : AMS_WS;

    // Safety check for placeholder string
    if (ams_url == "AMS_WEBSOCKET_URL") {
        logWithTime("[ERROR] No WebSocket URL provided. Set AMS_WS_URL environment variable.");
        return -1;
    }

    bool isPlayer = (argc > 1 && string(argv[1]) == "--player");
    string prefix = isPlayer ? "player_" : "publisher_";
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    rtc::InitLogger(rtc::LogLevel::Warning, [](rtc::LogLevel level, const std::string& msg) {
            if (msg.find("juice: Send failed") != std::string::npos || msg.find("STUN binding failed") != std::string::npos) return;
            std::cerr << msg << std::endl;
            });

    srand((unsigned)time(nullptr));
#if PLATFORM_NUM == 0x86
    g_publishStreamId = "Ubuntu_" + prefix + to_string(rand());
#else
    g_publishStreamId = "BC04_" + prefix + to_string(rand());
#endif

    logWithTime("[init] Version 1.0.4 - Starting");
    logWithTime("[init] URL: " + ams_url);

    WebRTCManager manager(ROOM_ID_STR, isPlayer);

    try {
        g_ws.open(ams_url);
    } catch (const std::exception& e) {
        logWithTime("[ERROR] WebSocket failed to open: " + string(e.what()));
        return -1;
    }

    g_signaling = make_shared<AMSSignalingClient>(&g_ws, g_publishStreamId, ROOM_ID_STR);

    manager.init();

    g_ws.onOpen([&](){
            logWithTime("[WS] connected");
            g_ws_open = true;
            manager.setupPublishPeer(g_publishStreamId);
            g_signaling->sendJoinRoom();
            });

    g_ws.onMessage([&](variant<vector<std::byte>, string> msg){
            if (holds_alternative<string>(msg)) {
                try { 
                    if (g_signaling) g_signaling->handleMessage(json::parse(get<string>(msg))); 
                } catch(...) {}
            }
            });

    g_ws.onClosed([&](){
            logWithTime("[WS] closed");
            g_running = false;
            g_ws_open = false;
            });

    std::thread consoleThread = startConsoleInputThread(g_running, [&](const std::string& line){
            if (line == "quit") { g_running = false; return; }
            if (g_ws_open) manager.sendChatMessage(line);
            });

    while (g_running) this_thread::sleep_for(100ms);
    if (consoleThread.joinable()) consoleThread.join();

    manager.cleanup();
    g_ws.onMessage(nullptr);
    g_ws.onOpen(nullptr);
    g_ws.onClosed(nullptr);
    if (g_signaling) { g_signaling->shutdown(); g_signaling.reset(); }
    if (g_ws_open) {
        try { g_ws.close(); } catch(...) {}
    }

    logWithTime("[main] exited");
    return 0;
}
