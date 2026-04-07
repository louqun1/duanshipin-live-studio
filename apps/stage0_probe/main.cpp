#include <iostream>

#include "capture/VideoCaptureSource.hpp"
#include "encode/VideoEncoder.hpp"
#include "mux/FlvMuxer.hpp"
#include "publish/RtmpPublisher.hpp"
#include "session/LiveSessionController.hpp"

int main() {
    using live_studio::capture::VideoCaptureSource;
    using live_studio::encode::VideoEncoder;
    using live_studio::mux::FlvMuxer;
    using live_studio::publish::RtmpPublisher;
    using live_studio::session::LiveSessionController;

    VideoCaptureSource capture;
    VideoEncoder encoder;
    FlvMuxer muxer;
    RtmpPublisher publisher;
    LiveSessionController controller;

    std::cout << "duanshipin-live-studio stage 0 probe\n";
    std::cout << "Project status: architecture alignment only\n";
    std::cout << "Modules declared:\n";
    std::cout << "  - " << capture.moduleName() << "\n";
    std::cout << "  - " << encoder.moduleName() << "\n";
    std::cout << "  - " << muxer.moduleName() << "\n";
    std::cout << "  - " << publisher.moduleName() << "\n";
    std::cout << "  - " << controller.moduleName() << "\n";
    std::cout << "No capture/encode/RTMP logic is implemented in stage 0.\n";
    return 0;
}
