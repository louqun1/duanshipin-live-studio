#include "../rtc_probe_common/SignalProbeApp.hpp"

int main(int argc, char** argv) {
    return RunSignalProbeApp(argc, argv, SignalProbeDefaults{
        .role = "participant",
        .peerId = "guest-probe",
        .targetId = "controller-probe"
    });
}
