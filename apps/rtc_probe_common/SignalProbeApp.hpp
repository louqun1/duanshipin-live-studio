#pragma once

#include <string>

struct SignalProbeDefaults {
    std::string role;
    std::string peerId;
    std::string targetId;
};

int RunSignalProbeApp(int argc, char** argv, const SignalProbeDefaults& defaults);
