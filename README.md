AntMediaServerConferenceCall

A high-performance C++ WebRTC client tailored for Ant Media Server (AMS). This project implements the full AMS signaling protocol and integrates with libdatachannel and GStreamer to provide robust multi-party conference call capabilities.  
🚀 Key Features

    Hybrid Role Support: Join a conference as a Publisher (bi-directional media) or Player-only (subscribe-only mode).

    Automated Signaling: Full implementation of AMS WebSocket signaling, including joinedTheRoom, subtrackAdded, and dynamic track management.

    Hardware-Ready Media Pipeline: Leverages GStreamer for RTP payloading/depayloading and media playback, optimized for embedded platforms like QCS610.

    Real-time Interaction: Integrated JSON-based chat functionality via WebRTC DataChannel.

    Audio Activity Control: Supports iamtalking / iammute signaling logic to optimize downstream audio processing in multi-peer environments.

🛠 Tech Stack

    libdatachannel: Core WebRTC stack handling ICE, DTLS, and DataChannels.

    GStreamer: Media processing pipeline for RTP handling and audio/video playback.

    nlohmann/json: Modern JSON parsing for signaling messages and chat data.

    sdptransform: SDP parsing and session negotiation.

🏗 Build & Execution
Build

The project is designed to be compiled directly using g++ without the need for complex build systems like CMake. Ensure GStreamer and OpenSSL development headers are installed on your system.
Bash

g++ -std=c++17 main.cpp AMSSignalingClient.cpp GstManager.cpp Logger.cpp \
    -I./include -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 \
    -L./lib/qcs610 -ldatachannel -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 \
    -lpthread -lcrypto -lssl \
    -o AntMediaServerConferenceCall

  
Export AMS_WS_URL  

export AMS_WS_URL="ws://your-server-ip:5080/ConferenceCall/websocket"  

Execution  
1. Publisher Mode (Default)
Broadcasts local media (H.264/Opus) to the room.
Bash

./AntMediaServerConferenceCall

2. Player-only Mode
Subscribes to all remote tracks without publishing local media.
Bash

./AntMediaServerConferenceCall --player

⚙️ System Architecture
Media Flow

    Egress (Outgoing): Media captured/encoded via GStreamer → RTP Payloading → libdatachannel track transmission.

    Ingress (Incoming): libdatachannel receives RTP packets → GstManager::pushAudioFrame → GStreamer decoding and playback.

Console Commands

During runtime, use the console to interact:

    Message: Type any string and press Enter to broadcast a chat message via DataChannel.

    quit: Gracefully leave the conference and terminate all connections.
