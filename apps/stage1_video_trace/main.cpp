#include <cstdlib>
#include <iostream>
#include <string>

#include "common/TraceLog.hpp"
#include "session/LiveSessionController.hpp"

int main(int argc, char** argv) {
    std::string rtmpUrl = "rtmp://127.0.0.1/live/stage1-trace";
    int frameCount = 6;

    if (argc >= 2 && argv[1] != nullptr) {
        rtmpUrl = argv[1];
    }
    if (argc >= 3 && argv[2] != nullptr) {
        frameCount = std::atoi(argv[2]);
    }

    live_studio::TraceLog trace;
    live_studio::session::LiveSessionController controller;
    const live_studio::Status status = controller.runStage1VideoTrace(rtmpUrl, frameCount, &trace);

    std::cout << "duanshipin-live-studio stage 1 video trace\n";
    std::cout << "Controller: " << controller.moduleName() << "\n";
    std::cout << "Trace output:\n";
    trace.print(std::cout);

    if (!status.ok()) {
        std::cerr << "stage 1 trace failed: " << status.message << "\n";
        return 1;
    }

    std::cout << "Current milestone status: trace only.\n";
    std::cout << "TODO(user): replace synthetic capture/encode/mux/publish with real stage 1 video-only live logic.\n";
    return 0;
}
