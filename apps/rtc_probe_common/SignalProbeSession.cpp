#include "SignalProbeSession.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <utility>

using json = nlohmann::json;

namespace rtc_probe_common {
namespace {

std::int64_t NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

json BuildSignalEnvelope(const SignalProbeSessionConfig& config,
                         const std::string& type,
                         const std::string& toUserId,
                         const std::string& requestId,
                         json payload = json::object()) {
    return json{
        {"type", type},
        {"room_id", config.roomId},
        {"from_user_id", config.peerId},
        {"to_user_id", toUserId},
        {"request_id", requestId},
        {"payload", std::move(payload)},
        {"ts_ms", NowEpochMs()}
    };
}

bool ContainsValue(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

std::string ExtractMemberId(const json& member) {
    if (member.is_string()) {
        return member.get<std::string>();
    }
    if (!member.is_object()) {
        return "";
    }
    if (member.contains("peer_id")) {
        return member.value("peer_id", "");
    }
    if (member.contains("user_id")) {
        return member.value("user_id", "");
    }
    if (member.contains("member_id")) {
        return member.value("member_id", "");
    }
    return "";
}

std::vector<std::string> ExtractMemberIds(const json& message) {
    std::vector<std::string> members;
    const json* container = nullptr;
    if (message.contains("peers")) {
        container = &message.at("peers");
    } else if (message.contains("members")) {
        container = &message.at("members");
    }

    if (container == nullptr || !container->is_array()) {
        return members;
    }

    for (const auto& member : *container) {
        const std::string memberId = ExtractMemberId(member);
        if (!memberId.empty()) {
            members.push_back(memberId);
        }
    }
    return members;
}

std::string ExtractUserIdFromPayload(const json& message) {
    if (message.contains("payload")) {
        const auto& payload = message.at("payload");
        if (payload.is_object()) {
            if (payload.contains("member")) {
                return ExtractMemberId(payload.at("member"));
            }
            const std::string userId = ExtractMemberId(payload);
            if (!userId.empty()) {
                return userId;
            }
        }
    }

    if (message.contains("member")) {
        return ExtractMemberId(message.at("member"));
    }

    if (message.contains("user_id")) {
        return message.value("user_id", "");
    }
    if (message.contains("peer_id")) {
        return message.value("peer_id", "");
    }
    return message.value("from_user_id", "");
}

std::string ExtractRequestId(const json& message) {
    return message.value("request_id", "");
}

}  // namespace

SignalProbeSession::SignalProbeSession(SignalProbeSessionConfig config, SignalProbeSessionCallbacks callbacks)
    : config_(std::move(config)), callbacks_(std::move(callbacks)) {
    signalState_ = config_.signalMode ? resetStateForRole() : "";
    activeRequestId_ = config_.initialRequestId;
}

std::string SignalProbeSession::currentSignalState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return signalState_;
}

std::string SignalProbeSession::currentRequestId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeRequestId_.value_or("");
}

bool SignalProbeSession::matchesActiveRequest(const std::string& requestId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!activeRequestId_.has_value()) {
        return true;
    }
    if (requestId.empty()) {
        return false;
    }
    return *activeRequestId_ == requestId;
}

bool SignalProbeSession::messageTargetsLocalUser(const json& message) const {
    if (!message.contains("to_user_id")) {
        return true;
    }
    const std::string toUserId = message.value("to_user_id", "");
    return toUserId.empty() || toUserId == config_.peerId || toUserId == "*";
}

void SignalProbeSession::adoptActiveRequestId(const std::string& requestId) {
    if (requestId.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    activeRequestId_ = requestId;
}

bool SignalProbeSession::handleRegistrationAndMemberMessage(const json& message) {
    const std::string type = message.value("type", "");
    if (type == "probe.registered") {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            registered_ = true;
        }
        logEvent("probe.registered",
                 "registration acknowledged by signaling service",
                 config_.signalMode ? currentSignalState() : "",
                 currentRequestId(),
                 config_.targetId);
        maybeStartSignalScenario();
        return true;
    }

    if (type == "room.member-list") {
        const auto members = ExtractMemberIds(message);
        updateTargetPresenceFromSnapshot(members);

        std::ostringstream detail;
        detail << "online_count=" << members.size() << " members=";
        if (members.empty()) {
            detail << "(none)";
        } else {
            for (std::size_t index = 0; index < members.size(); ++index) {
                if (index != 0) {
                    detail << ",";
                }
                detail << members[index];
            }
        }
        logEvent("room.member-list",
                 detail.str(),
                 config_.signalMode ? currentSignalState() : "",
                 "",
                 config_.targetId);

        if (!config_.signalMode && config_.isController && ContainsValue(members, config_.targetId) &&
            callbacks_.onRtcOfferRequested) {
            callbacks_.onRtcOfferRequested();
            return true;
        }

        maybeStartSignalScenario();
        return true;
    }

    if (type == "room.member-join" || type == "room.member-leave") {
        const std::string memberId = ExtractUserIdFromPayload(message);
        if (memberId.empty()) {
            return true;
        }

        bool targetLeftDuringSession = false;
        bool targetLeaveShouldFail = false;
        std::vector<std::string> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = onlineMembers_;
            if (type == "room.member-join") {
                if (!ContainsValue(snapshot, memberId)) {
                    snapshot.push_back(memberId);
                }
            } else {
                snapshot.erase(std::remove(snapshot.begin(), snapshot.end(), memberId), snapshot.end());
                const bool sessionStillActive =
                    activeRequestId_.has_value() && signalState_ != resetStateForRole();
                if (memberId == config_.targetId && sessionStillActive) {
                    targetLeftDuringSession = true;
                    targetLeaveShouldFail = callbacks_.shouldFailWhenTargetLeavesDuringSession
                        ? callbacks_.shouldFailWhenTargetLeavesDuringSession()
                        : true;
                }
            }
            onlineMembers_ = snapshot;
            targetPresent_ = ContainsValue(snapshot, config_.targetId);
        }

        std::ostringstream detail;
        detail << "member=" << memberId << " online_members=";
        if (snapshot.empty()) {
            detail << "(none)";
        } else {
            for (std::size_t index = 0; index < snapshot.size(); ++index) {
                if (index != 0) {
                    detail << ",";
                }
                detail << snapshot[index];
            }
        }
        logEvent(type,
                 detail.str(),
                 config_.signalMode ? currentSignalState() : "",
                 "",
                 memberId);

        if (targetLeftDuringSession && targetLeaveShouldFail) {
            if (callbacks_.recordFailure) {
                callbacks_.recordFailure("target user left the room before the scenario completed");
            }
            return true;
        }

        maybeStartSignalScenario();
        return true;
    }

    return false;
}

void SignalProbeSession::handleSignalModeMessage(const json& message) {
    const std::string type = message.value("type", "");
    if (type == "signal.error") {
        throw std::runtime_error(message.value("message", "signaling error"));
    }

    if (!messageTargetsLocalUser(message)) {
        return;
    }

    const std::string requestId = ExtractRequestId(message);
    const std::string remoteUserId = message.value("from_user_id", "");
    const bool echoedFromLocalUser = remoteUserId == config_.peerId;
    if (!echoedFromLocalUser && !remoteUserId.empty() && remoteUserId != config_.targetId) {
        return;
    }

    if (type == "linkmic.apply") {
        if (echoedFromLocalUser || !config_.isController) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeRequestId_ = requestId;
            activeRequestType_ = type;
            signalScenarioStarted_ = true;
            requestDeadline_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.requestTimeoutMs);
        }
        transitionSignalState("guest-applying", "received linkmic.apply", requestId, remoteUserId);

        if (config_.autoAcceptIncoming) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                waitingJoinParams_ = true;
                requestDeadline_ =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.requestTimeoutMs);
            }
            transitionSignalState(connectingStateForRole(),
                                  "accepting guest apply and waiting for rtc.join-params",
                                  requestId,
                                  remoteUserId);
            sendBusinessMessage("linkmic.accept",
                                remoteUserId,
                                requestId,
                                {
                                    {"accepted_from", "linkmic.apply"},
                                    {"decision", "accept"}
                                },
                                "accepted incoming guest apply");
            return;
        }

        if (config_.autoRejectIncoming) {
            sendBusinessMessage("linkmic.reject",
                                remoteUserId,
                                requestId,
                                {
                                    {"rejected_from", "linkmic.apply"},
                                    {"decision", "reject"}
                                },
                                "rejected incoming guest apply");
            transitionSignalState(resetStateForRole(), "rejected guest apply", requestId, remoteUserId);
            if (callbacks_.markDone) {
                callbacks_.markDone();
            }
        }
        return;
    }

    if (type == "linkmic.invite") {
        if (echoedFromLocalUser || config_.isController) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeRequestId_ = requestId;
            activeRequestType_ = type;
            signalScenarioStarted_ = true;
            requestDeadline_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.requestTimeoutMs);
        }
        transitionSignalState("guest-invited", "received linkmic.invite", requestId, remoteUserId);

        if (config_.autoAcceptIncoming) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                waitingJoinParams_ = true;
                requestDeadline_ =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.requestTimeoutMs);
            }
            transitionSignalState(connectingStateForRole(),
                                  "accepting host invite and waiting for rtc.join-params",
                                  requestId,
                                  remoteUserId);
            sendBusinessMessage("linkmic.accept",
                                remoteUserId,
                                requestId,
                                {
                                    {"accepted_from", "linkmic.invite"},
                                    {"decision", "accept"}
                                },
                                "accepted incoming host invite");
            return;
        }

        if (config_.autoRejectIncoming) {
            sendBusinessMessage("linkmic.reject",
                                remoteUserId,
                                requestId,
                                {
                                    {"rejected_from", "linkmic.invite"},
                                    {"decision", "reject"}
                                },
                                "rejected incoming host invite");
            transitionSignalState(resetStateForRole(), "rejected host invite", requestId, remoteUserId);
            if (callbacks_.markDone) {
                callbacks_.markDone();
            }
        }
        return;
    }

    if (type == "linkmic.accept") {
        if (echoedFromLocalUser || !matchesActiveRequest(requestId)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            activeRequestId_ = requestId;
            waitingJoinParams_ = true;
            requestDeadline_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.requestTimeoutMs);
        }
        transitionSignalState(connectingStateForRole(),
                              "received linkmic.accept, waiting for rtc.join-params",
                              requestId,
                              remoteUserId);
        return;
    }

    if (type == "rtc.join-params" || type == "linkmic.connected") {
        if (echoedFromLocalUser || !matchesActiveRequest(requestId)) {
            return;
        }

        const std::string payloadText = message.contains("payload") ? message.at("payload").dump() : "{}";
        bool shouldScheduleAutoHangup = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waitingJoinParams_ = false;
            requestDeadline_.reset();
            activeRequestId_ = requestId.empty() ? activeRequestId_ : std::optional<std::string>(requestId);
            if (config_.isController && config_.autoHangupMs >= 0 && !autoHangupSent_ && !hangupDeadline_.has_value()) {
                hangupDeadline_ =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.autoHangupMs);
                shouldScheduleAutoHangup = true;
            }
        }
        transitionSignalState(activeStateForRole(), "received " + type, requestId, remoteUserId);
        logEvent("recv." + type, payloadText, currentSignalState(), requestId, remoteUserId);

        if (config_.autoHangupMs < 0) {
            if (callbacks_.markDone) {
                callbacks_.markDone();
            }
            return;
        }

        if (shouldScheduleAutoHangup) {
            logEvent("timer.auto-hangup",
                     "will send linkmic.hangup after " + std::to_string(config_.autoHangupMs) + " ms",
                     currentSignalState(),
                     requestId,
                     remoteUserId);
        }
        return;
    }

    if (type == "linkmic.reject" || type == "linkmic.cancel") {
        if (echoedFromLocalUser || !matchesActiveRequest(requestId)) {
            return;
        }
        transitionSignalState(resetStateForRole(), "received " + type, requestId, remoteUserId);
        if (callbacks_.markDone) {
            callbacks_.markDone();
        }
        return;
    }

    if (type == "linkmic.hangup" || type == "linkmic.kick") {
        if (echoedFromLocalUser || !matchesActiveRequest(requestId)) {
            return;
        }
        transitionSignalState(resetStateForRole(), "received " + type, requestId, remoteUserId);
        if (callbacks_.markDone) {
            callbacks_.markDone();
        }
        return;
    }

    if (type == "linkmic.state-sync") {
        if (!matchesActiveRequest(requestId)) {
            return;
        }
        const std::string syncedState = message.contains("payload") && message.at("payload").is_object()
            ? message.at("payload").value("state", "")
            : message.value("state", "");

        if (!syncedState.empty()) {
            transitionSignalState(syncedState, "received linkmic.state-sync", requestId, remoteUserId);
            if (syncedState == resetStateForRole() && callbacks_.markDone) {
                callbacks_.markDone();
            }
            return;
        }

        logEvent("recv.linkmic.state-sync",
                 message.dump(),
                 currentSignalState(),
                 requestId,
                 remoteUserId);
    }
}

void SignalProbeSession::checkSignalTimers() {
    std::optional<std::string> timeoutMessage;
    std::optional<std::string> hangupRequestId;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();

        if (requestDeadline_.has_value() && now >= *requestDeadline_) {
            if (waitingJoinParams_) {
                timeoutMessage = "timed out waiting for rtc.join-params";
            } else if (!activeRequestType_.empty()) {
                timeoutMessage = "timed out waiting for " + activeRequestType_ + " response";
            } else {
                timeoutMessage = "timed out waiting for signal phase progress";
            }
            requestDeadline_.reset();
        } else if (hangupDeadline_.has_value() && now >= *hangupDeadline_ &&
                   !autoHangupSent_ && activeRequestId_.has_value()) {
            autoHangupSent_ = true;
            hangupDeadline_.reset();
            hangupRequestId = activeRequestId_;
        }
    }

    if (timeoutMessage.has_value()) {
        if (callbacks_.recordFailure) {
            callbacks_.recordFailure(*timeoutMessage);
        }
        return;
    }

    if (hangupRequestId.has_value()) {
        transitionSignalState(endingStateForRole(),
                              "auto hangup timer reached",
                              *hangupRequestId,
                              config_.targetId);
        sendBusinessMessage("linkmic.hangup",
                            config_.targetId,
                            *hangupRequestId,
                            {
                                {"reason", "auto-hangup"},
                                {"probe_mode", "signal"}
                            },
                            "controller requested session teardown");
        if (callbacks_.markDone) {
            callbacks_.markDone();
        }
    }
}

std::string SignalProbeSession::resetStateForRole() const {
    return config_.isController ? "live-only" : "idle";
}

std::string SignalProbeSession::connectingStateForRole() const {
    return config_.isController ? "linkmic-connecting" : "rtc-connecting";
}

std::string SignalProbeSession::activeStateForRole() const {
    return config_.isController ? "linkmic-active" : "rtc-active";
}

std::string SignalProbeSession::endingStateForRole() const {
    return config_.isController ? "linkmic-ending" : "ending";
}

std::string SignalProbeSession::generateRequestId(const std::string& prefix) {
    std::uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sequence = ++requestSequence_;
    }

    std::ostringstream out;
    out << prefix << "-" << config_.peerId << "-to-" << config_.targetId
        << "-" << NowEpochMs() << "-" << sequence;
    return out.str();
}

void SignalProbeSession::logEvent(const std::string& event,
                                  const std::string& detail,
                                  const std::string& state,
                                  const std::string& requestId,
                                  const std::string& remoteUserId) const {
    if (callbacks_.logEvent) {
        callbacks_.logEvent(event, detail, state, requestId, remoteUserId);
    }
}

void SignalProbeSession::transitionSignalState(const std::string& nextState,
                                               const std::string& reason,
                                               const std::string& requestId,
                                               const std::string& remoteUserId) {
    std::string previousState;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previousState = signalState_;
        signalState_ = nextState;
    }

    if (previousState.empty() || previousState == nextState) {
        logEvent("state", reason, nextState, requestId, remoteUserId);
        return;
    }

    logEvent("state",
             previousState + " -> " + nextState + " (" + reason + ")",
             nextState,
             requestId,
             remoteUserId);
}

void SignalProbeSession::sendBusinessMessage(const std::string& type,
                                             const std::string& toUserId,
                                             const std::string& requestId,
                                             json payload,
                                             const std::string& detail) {
    logEvent("send." + type, detail, currentSignalState(), requestId, toUserId);
    if (callbacks_.sendJson) {
        callbacks_.sendJson(
            BuildSignalEnvelope(config_, type, toUserId, requestId, std::move(payload)));
    }
}

void SignalProbeSession::startOutgoingSignalRequest(const std::string& messageType,
                                                    const std::string& nextState,
                                                    const std::string& detail) {
    const std::string requestId = generateRequestId("stage1");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeRequestId_ = requestId;
        activeRequestType_ = messageType;
        signalScenarioStarted_ = true;
        waitingJoinParams_ = false;
        autoHangupSent_ = false;
        requestDeadline_ =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.requestTimeoutMs);
        hangupDeadline_.reset();
    }

    transitionSignalState(nextState, detail, requestId, config_.targetId);
    sendBusinessMessage(messageType,
                        config_.targetId,
                        requestId,
                        {
                            {"initiator_role", config_.role},
                            {"probe_mode", "signal"}
                        },
                        detail);
}

void SignalProbeSession::maybeStartSignalScenario() {
    if (!config_.signalMode || (!config_.startInviteWhenReady && !config_.startApplyWhenReady)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!registered_ || !targetPresent_ || signalScenarioStarted_ || activeRequestId_.has_value()) {
            return;
        }
    }

    if (config_.startInviteWhenReady) {
        startOutgoingSignalRequest("linkmic.invite",
                                   "host-inviting",
                                   "target is online, sending linkmic.invite");
        return;
    }

    startOutgoingSignalRequest("linkmic.apply",
                               "waiting-host",
                               "controller is online, sending linkmic.apply");
}

void SignalProbeSession::updateTargetPresenceFromSnapshot(std::vector<std::string> members) {
    const bool targetPresent = ContainsValue(members, config_.targetId);
    std::lock_guard<std::mutex> lock(mutex_);
    onlineMembers_ = std::move(members);
    targetPresent_ = targetPresent;
}

}  // namespace rtc_probe_common
