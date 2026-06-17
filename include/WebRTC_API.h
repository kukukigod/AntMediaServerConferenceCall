#ifndef WEBRTC_API_H
#define WEBRTC_API_H

#include <string>
#include <vector>
#include <cstddef>

// API declarations for external program invocation
void WebRTC_Init(void);
void WebRTC_SetSignalingServerURL(const std::string& url);
bool WebRTC_Start2WayCommunication(bool isPlayer);
void WebRTC_Stop2WayCommunication();
void WebRTC_SendChatMessage(const std::string& text);
void WebRTC_SetRoomId(const std::string& roomId);
void WebRTC_SetStreamId(const std::string& streamId);

#endif // WEBRTC_API_H
