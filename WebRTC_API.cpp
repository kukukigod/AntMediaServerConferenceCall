#include "WebRTC_API.h"
#include "WebRTCManager.h"
#include <memory>

// Unique global/static instance managed internally
static std::unique_ptr<WebRTCManager> g_webrtcManager = nullptr;

static WebRTCManager* GetInstance()
{
    if (!g_webrtcManager) {
        g_webrtcManager = std::make_unique<WebRTCManager>();
    }
    return g_webrtcManager.get();
}

void WebRTC_Init(void)
{
    if (!g_webrtcManager) {
        g_webrtcManager = std::make_unique<WebRTCManager>();
    }
}

void WebRTC_SetSignalingServerURL(const std::string& url)
{
    GetInstance()->SetSignalingServerURL(url);
}

bool WebRTC_Start2WayCommunication(bool isPlayer)
{
    return GetInstance()->Start2WayCommunication(isPlayer);
}

void WebRTC_Stop2WayCommunication()
{
    if (g_webrtcManager) {
        g_webrtcManager->Stop2WayCommunication();
    }
}

void WebRTC_SendChatMessage(const std::string& text)
{
    if (g_webrtcManager) {
        g_webrtcManager->SendChatMessage(text);
    }
}

void WebRTC_SetRoomId(const std::string& roomId)
{
    if (g_webrtcManager) {
        g_webrtcManager->SetRoomId(roomId);
    }
}

void WebRTC_SetStreamId(const std::string& streamId)
{
    if (g_webrtcManager) {
        g_webrtcManager->SetStreamId(streamId);
    }
}
