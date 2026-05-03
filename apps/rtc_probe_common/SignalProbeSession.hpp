#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rtc_probe_common {

struct SignalProbeSessionConfig {
    std::string role;
    std::string peerId;
    std::string targetId;
    std::string roomId;
    bool signalMode = false;
    bool isController = false;
    bool startInviteWhenReady = false;
    bool startApplyWhenReady = false;
    bool autoAcceptIncoming = false;
    bool autoRejectIncoming = false;
    int requestTimeoutMs = 0;
    int autoHangupMs = 0;
    std::optional<std::string> initialRequestId;
};

struct SignalProbeSessionCallbacks {
    std::function<void(const std::string& event,
                       const std::string& detail,
                       const std::string& state,
                       const std::string& requestId,
                       const std::string& remoteUserId)> logEvent;
    std::function<void(const nlohmann::json& message)> sendJson;
    std::function<void(const std::string& message)> recordFailure;
    std::function<void()> markDone;
    std::function<void()> onRtcOfferRequested;
    std::function<bool()> shouldFailWhenTargetLeavesDuringSession;
};

class SignalProbeSession {
public:
    SignalProbeSession(SignalProbeSessionConfig config, SignalProbeSessionCallbacks callbacks);

    [[nodiscard]] std::string currentSignalState() const;
    [[nodiscard]] std::string currentRequestId() const;
    [[nodiscard]] bool matchesActiveRequest(const std::string& requestId) const;
    [[nodiscard]] bool messageTargetsLocalUser(const nlohmann::json& message) const;

    void adoptActiveRequestId(const std::string& requestId);
    bool handleRegistrationAndMemberMessage(const nlohmann::json& message);
    void handleSignalModeMessage(const nlohmann::json& message);
    void checkSignalTimers();

private:
    [[nodiscard]] std::string resetStateForRole() const;
    [[nodiscard]] std::string connectingStateForRole() const;
    [[nodiscard]] std::string activeStateForRole() const;
    [[nodiscard]] std::string endingStateForRole() const;
    [[nodiscard]] std::string generateRequestId(const std::string& prefix);

    void logEvent(const std::string& event,
                  const std::string& detail,
                  const std::string& state = "",
                  const std::string& requestId = "",
                  const std::string& remoteUserId = "") const;
    void transitionSignalState(const std::string& nextState,
                               const std::string& reason,
                               const std::string& requestId = "",
                               const std::string& remoteUserId = "");
    void sendBusinessMessage(const std::string& type,
                             const std::string& toUserId,
                             const std::string& requestId,
                             nlohmann::json payload,
                             const std::string& detail);
    void startOutgoingSignalRequest(const std::string& messageType,
                                    const std::string& nextState,
                                    const std::string& detail);
    void maybeStartSignalScenario();
    void updateTargetPresenceFromSnapshot(std::vector<std::string> members);

    SignalProbeSessionConfig config_;
    SignalProbeSessionCallbacks callbacks_;
    mutable std::mutex mutex_;
    bool registered_ = false;
    bool targetPresent_ = false;
    bool signalScenarioStarted_ = false;
    bool waitingJoinParams_ = false;
    bool autoHangupSent_ = false;
    std::optional<std::chrono::steady_clock::time_point> requestDeadline_;
    std::optional<std::chrono::steady_clock::time_point> hangupDeadline_;
    std::optional<std::string> activeRequestId_;
    std::string activeRequestType_;
    std::string signalState_;
    std::vector<std::string> onlineMembers_;
    std::uint64_t requestSequence_ = 0;
};

}  // namespace rtc_probe_common
