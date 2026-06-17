#include <iostream>
#include <atomic>
#include <thread>
#include <functional>
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include "WebRTC_API.h"
#include "Log.h"

using namespace std;

// Application global running status indicator
static atomic_bool g_appRunning(true);

// Native System Signal Termination Hook
static void nativeSignalHandler(int sig) {
    DBG(0, "[signal] native system exiting via signal: %d\n", sig);
    g_appRunning = false;
}

// Non-blocking Console Input Monitor worker thread initialization
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

// Main execution process entry
int main(int argc, char* argv[])
{
    const char* env_url = std::getenv("AMS_WS_URL");
    string streamId;
    string ams_url = (env_url != nullptr) ? string(env_url) : "AMS_WEBSOCKET_URL";

    if (ams_url == "AMS_WEBSOCKET_URL") {
        DBG(0, "[ERROR] No WebSocket URL provided. Set AMS_WS_URL environment variable.\n");
        return -1;
    }

    bool isPlayer = (argc > 1 && string(argv[1]) == "--player");
    signal(SIGINT, nativeSignalHandler);
    signal(SIGTERM, nativeSignalHandler);

    WebRTC_Init();
    WebRTC_SetSignalingServerURL(ams_url);
    WebRTC_SetRoomId("room1");
    srand((unsigned)time(nullptr));
    string prefix = isPlayer ? "player_" : "publisher_";
#if PLATFORM_NUM == 0x86
    streamId = "Ubuntu_" + prefix + to_string(rand());
#else
    streamId = "Device_" + prefix + to_string(rand());
#endif
    WebRTC_SetStreamId(streamId);


    if (WebRTC_Start2WayCommunication(isPlayer) == false) {
        DBG(0, "[ERROR] WebRTCManager failed initialization.\n");
        return -1;
    }

    std::thread consoleThread = startConsoleInputThread(g_appRunning, [&](const std::string& line){
        if (line == "quit") {
            g_appRunning = false;
            return;
        }
        WebRTC_SendChatMessage(line);
    });

    while (g_appRunning) {
        this_thread::sleep_for(100ms);
    }

    // 6. Safe join sequence for the concurrent background system worker context
    if (consoleThread.joinable()) {
        consoleThread.join();
    }

    // 7. Teardown systems explicitly using API 3
    WebRTC_Stop2WayCommunication();

    DBG(0, "[main] exited\n");
    return 0;
}
