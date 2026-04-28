#include "SignalProbeApp.hpp"

#include <rtc/rtc.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

constexpr char kDefaultSignalingUrl[] = "ws://127.0.0.1:8080/ws";
constexpr char kDefaultRtcRoomId[] = "stage0";
constexpr char kDefaultSignalRoomId[] = "stage1";

enum class ProbeMode {
    rtc,
    signal,
};

enum class SignalAction {
    invite,
    apply,
    accept,
    reject,
    passive,
};

struct ProbeArgs {
    std::string role;
    std::string peerId;
    std::string targetId;
    std::string signalingUrl = kDefaultSignalingUrl;
    std::string roomId = kDefaultRtcRoomId;
    std::optional<std::string> iceServer;
    ProbeMode mode = ProbeMode::rtc;
    SignalAction signalAction = SignalAction::passive;
    bool relayOnly = false;
    int timeoutMs = 30000;
    int requestTimeoutMs = 10000;
    int autoHangupMs = 2000;
};

struct ProbeRuntime {
    ProbeArgs args;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<std::promise<void>> donePromise;
    std::shared_ptr<std::atomic_bool> doneFlag;
    std::mutex stateMutex;
    bool offerStarted = false;
    bool registered = false;
    bool targetPresent = false;
    bool signalScenarioStarted = false;
    bool waitingJoinParams = false;
    bool autoHangupSent = false;
    std::optional<std::chrono::steady_clock::time_point> requestDeadline;
    std::optional<std::chrono::steady_clock::time_point> hangupDeadline;
    std::optional<std::string> activeRequestId;
    std::string activeRequestType;
    std::string signalState;
    std::vector<std::string> onlineMembers;
    std::uint64_t requestSequence = 0;
    std::optional<std::string> failureMessage;
};

std::int64_t NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string ProbeModeToString(ProbeMode mode) {
    return mode == ProbeMode::rtc ? "rtc" : "signal";
}

std::string SignalActionToString(SignalAction action) {
    switch (action) {
    case SignalAction::invite:
        return "invite";
    case SignalAction::apply:
        return "apply";
    case SignalAction::accept:
        return "accept";
    case SignalAction::reject:
        return "reject";
    case SignalAction::passive:
        return "passive";
    }
    return "passive";
}

ProbeMode ParseProbeMode(const std::string& text) {
    const std::string lowered = ToLowerCopy(text);
    if (lowered == "rtc") {
        return ProbeMode::rtc;
    }
    if (lowered == "signal" || lowered == "stage1") {
        return ProbeMode::signal;
    }
    throw std::runtime_error("unknown probe mode: " + text);
}

SignalAction ParseSignalAction(const std::string& text) {
    const std::string lowered = ToLowerCopy(text);
    if (lowered == "invite") {
        return SignalAction::invite;
    }
    if (lowered == "apply") {
        return SignalAction::apply;
    }
    if (lowered == "accept") {
        return SignalAction::accept;
    }
    if (lowered == "reject") {
        return SignalAction::reject;
    }
    if (lowered == "passive") {
        return SignalAction::passive;
    }
    throw std::runtime_error("unknown signal action: " + text);
}

std::string JoinStrings(const std::vector<std::string>& values) {
    if (values.empty()) {
        return "(none)";
    }

    std::ostringstream out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out << ",";
        }
        out << values[index];
    }
    return out.str();
}

bool ContainsValue(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

std::string ResetStateForRole(const ProbeArgs& args) {
    return args.role == "controller" ? "live-only" : "idle";
}

std::string ConnectingStateForRole(const ProbeArgs& args) {
    return args.role == "controller" ? "linkmic-connecting" : "rtc-connecting";
}

std::string ActiveStateForRole(const ProbeArgs& args) {
    return args.role == "controller" ? "linkmic-active" : "rtc-active";
}

std::string EndingStateForRole(const ProbeArgs& args) {
    return args.role == "controller" ? "linkmic-ending" : "ending";
}

std::string CurrentSignalState(ProbeRuntime& runtime) {
    std::lock_guard<std::mutex> lock(runtime.stateMutex);
    return runtime.signalState;
}

std::string CurrentRequestId(ProbeRuntime& runtime) {
    std::lock_guard<std::mutex> lock(runtime.stateMutex);
    return runtime.activeRequestId.value_or("");
}

void LogProbeEvent(const ProbeArgs& args,
                   const std::string& event,
                   const std::string& detail,
                   const std::string& state = "",
                   const std::string& requestId = "",
                   const std::string& remoteUserId = "") {
    std::ostringstream out;
    out << "[" << args.role << "] event=" << event
        << " room_id=" << args.roomId
        << " user_id=" << args.peerId;
    if (!remoteUserId.empty()) {
        out << " peer_id=" << remoteUserId;
    }
    if (!requestId.empty()) {
        out << " request_id=" << requestId;
    }
    if (!state.empty()) {
        out << " state=" << state;
    }
    if (!detail.empty()) {
        out << " detail=" << detail;
    }
    std::cout << out.str() << std::endl;
}

void PrintUsage(const SignalProbeDefaults& defaults) {
    std::cout
        << "Options:\n"
        << "  --peer-id <value>            default: " << defaults.peerId << "\n"
        << "  --target-id <value>          default: " << defaults.targetId << "\n"
        << "  --role <value>               default: " << defaults.role << "\n"
        << "  --mode <rtc|signal>          default: rtc\n"
        << "  --action <value>             signal mode only; invite/apply/accept/reject/passive\n"
        << "  --signaling-url <value>      default: " << kDefaultSignalingUrl << "\n"
        << "  --room-id <value>            default: stage0 for rtc, stage1 for signal\n"
        << "  --ice-server <value>         example: turn://user:pass@127.0.0.1:3478?transport=udp\n"
        << "  --relay-only                 require relay candidates only\n"
        << "  --timeout-ms <value>         overall timeout, default: 30000\n"
        << "  --request-timeout-ms <value> signal mode only, default: 10000\n"
        << "  --auto-hangup-ms <value>     signal mode only; controller sends hangup after active, default: 2000, <0 disables\n";
}

ProbeArgs ParseArgs(int argc, char** argv, const SignalProbeDefaults& defaults) {
    ProbeArgs args;
    args.role = defaults.role;
    args.peerId = defaults.peerId;
    args.targetId = defaults.targetId;

    bool roomIdExplicitlySet = false;
    bool actionExplicitlySet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + flag);
            }
            return argv[++i];
        };

        if (arg == "--peer-id") {
            args.peerId = requireValue(arg);
        } else if (arg == "--target-id") {
            args.targetId = requireValue(arg);
        } else if (arg == "--role") {
            args.role = requireValue(arg);
        } else if (arg == "--mode") {
            args.mode = ParseProbeMode(requireValue(arg));
        } else if (arg == "--action") {
            args.signalAction = ParseSignalAction(requireValue(arg));
            actionExplicitlySet = true;
        } else if (arg == "--signaling-url") {
            args.signalingUrl = requireValue(arg);
        } else if (arg == "--room-id") {
            args.roomId = requireValue(arg);
            roomIdExplicitlySet = true;
        } else if (arg == "--ice-server") {
            args.iceServer = requireValue(arg);
        } else if (arg == "--relay-only") {
            args.relayOnly = true;
        } else if (arg == "--timeout-ms") {
            args.timeoutMs = std::stoi(requireValue(arg));
        } else if (arg == "--request-timeout-ms") {
            args.requestTimeoutMs = std::stoi(requireValue(arg));
        } else if (arg == "--auto-hangup-ms") {
            args.autoHangupMs = std::stoi(requireValue(arg));
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(defaults);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (!roomIdExplicitlySet) {
        args.roomId = args.mode == ProbeMode::rtc ? kDefaultRtcRoomId : kDefaultSignalRoomId;
    }

    if (!actionExplicitlySet && args.mode == ProbeMode::signal) {
        args.signalAction = args.role == "controller" ? SignalAction::invite : SignalAction::accept;
    }

    return args;
}

void MarkDone(const std::shared_ptr<std::promise<void>>& promise,
              const std::shared_ptr<std::atomic_bool>& doneFlag) {
    bool expected = false;
    if (doneFlag->compare_exchange_strong(expected, true)) {
        promise->set_value();
    }
}

void SendJson(const std::shared_ptr<rtc::WebSocket>& ws, const json& message) {
    if (!ws->send(message.dump())) {
        throw std::runtime_error("failed to send WebSocket message");
    }
}

json BuildSignalEnvelope(const ProbeArgs& args,
                         const std::string& type,
                         const std::string& toUserId,
                         const std::string& requestId,
                         json payload = json::object()) {
    json message = {
        {"type", type},
        {"room_id", args.roomId},
        {"from_user_id", args.peerId},
        {"to_user_id", toUserId},
        {"request_id", requestId},
        {"payload", std::move(payload)},
        {"ts_ms", NowEpochMs()}
    };
    return message;
}

void TransitionSignalState(ProbeRuntime& runtime,
                           const std::string& nextState,
                           const std::string& reason,
                           const std::string& requestId = "",
                           const std::string& remoteUserId = "") {
    std::string previousState;
    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        previousState = runtime.signalState;
        runtime.signalState = nextState;
    }

    if (previousState.empty()) {
        LogProbeEvent(runtime.args, "state", reason, nextState, requestId, remoteUserId);
        return;
    }

    if (previousState == nextState) {
        LogProbeEvent(runtime.args, "state", reason, nextState, requestId, remoteUserId);
        return;
    }

    LogProbeEvent(runtime.args,
                  "state",
                  previousState + " -> " + nextState + " (" + reason + ")",
                  nextState,
                  requestId,
                  remoteUserId);
}

void RecordFailure(ProbeRuntime& runtime, const std::string& message) {
    bool firstFailure = false;
    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        if (!runtime.failureMessage.has_value()) {
            runtime.failureMessage = message;
            firstFailure = true;
        }
    }

    if (firstFailure) {
        TransitionSignalState(runtime, "failed", message, CurrentRequestId(runtime), runtime.args.targetId);
    }
    MarkDone(runtime.donePromise, runtime.doneFlag);
}

std::string GenerateRequestId(ProbeRuntime& runtime, const std::string& prefix) {
    std::uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        sequence = ++runtime.requestSequence;
    }

    std::ostringstream out;
    out << prefix << "-" << runtime.args.peerId << "-to-" << runtime.args.targetId
        << "-" << NowEpochMs() << "-" << sequence;
    return out.str();
}

void SendBusinessMessage(ProbeRuntime& runtime,
                         const std::string& type,
                         const std::string& toUserId,
                         const std::string& requestId,
                         json payload,
                         const std::string& detail) {
    LogProbeEvent(runtime.args,
                  "send." + type,
                  detail,
                  CurrentSignalState(runtime),
                  requestId,
                  toUserId);
    SendJson(runtime.ws, BuildSignalEnvelope(runtime.args, type, toUserId, requestId, std::move(payload)));
}

void StartOutgoingSignalRequest(ProbeRuntime& runtime,
                                const std::string& messageType,
                                const std::string& nextState,
                                const std::string& detail) {
    const std::string requestId = GenerateRequestId(runtime, "stage1");
    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        runtime.activeRequestId = requestId;
        runtime.activeRequestType = messageType;
        runtime.signalScenarioStarted = true;
        runtime.waitingJoinParams = false;
        runtime.autoHangupSent = false;
        runtime.requestDeadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(runtime.args.requestTimeoutMs);
        runtime.hangupDeadline.reset();
    }

    TransitionSignalState(runtime, nextState, detail, requestId, runtime.args.targetId);
    SendBusinessMessage(runtime,
                        messageType,
                        runtime.args.targetId,
                        requestId,
                        {
                            {"initiator_role", runtime.args.role},
                            {"probe_mode", "signal"}
                        },
                        detail);
}

bool MessageTargetsLocalUser(const json& message, const std::string& localUserId) {
    if (!message.contains("to_user_id")) {
        return true;
    }
    const std::string toUserId = message.value("to_user_id", "");
    return toUserId.empty() || toUserId == localUserId || toUserId == "*";
}

bool ActiveRequestMatches(ProbeRuntime& runtime, const std::string& requestId) {
    std::lock_guard<std::mutex> lock(runtime.stateMutex);
    if (!runtime.activeRequestId.has_value() || requestId.empty()) {
        return true;
    }
    return *runtime.activeRequestId == requestId;
}

std::string ExtractRequestId(const json& message) {
    return message.value("request_id", "");
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

void UpdateTargetPresenceFromSnapshot(ProbeRuntime& runtime, std::vector<std::string> members) {
    bool targetPresent = ContainsValue(members, runtime.args.targetId);
    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        runtime.onlineMembers = std::move(members);
        runtime.targetPresent = targetPresent;
    }
}

void MaybeStartSignalScenario(ProbeRuntime& runtime) {
    if (runtime.args.mode != ProbeMode::signal) {
        return;
    }

    const bool controllerShouldInvite =
        runtime.args.role == "controller" && runtime.args.signalAction == SignalAction::invite;
    const bool participantShouldApply =
        runtime.args.role != "controller" && runtime.args.signalAction == SignalAction::apply;
    if (!controllerShouldInvite && !participantShouldApply) {
        return;
    }

    bool shouldStart = false;
    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        if (!runtime.registered || !runtime.targetPresent || runtime.signalScenarioStarted
            || runtime.activeRequestId.has_value()) {
            return;
        }
        shouldStart = true;
    }

    if (!shouldStart) {
        return;
    }

    if (controllerShouldInvite) {
        StartOutgoingSignalRequest(runtime,
                                   "linkmic.invite",
                                   "host-inviting",
                                   "target is online, sending linkmic.invite");
        return;
    }

    StartOutgoingSignalRequest(runtime,
                               "linkmic.apply",
                               "waiting-host",
                               "controller is online, sending linkmic.apply");
}

void AttachDataChannelHandlers(const std::shared_ptr<rtc::DataChannel>& dc,
                               ProbeRuntime& runtime,
                               const std::string& peerId) {
    dc->onOpen([wdc = std::weak_ptr<rtc::DataChannel>(dc), &runtime, peerId]() {
        try {
            LogProbeEvent(runtime.args,
                          "rtc.data-channel-open",
                          "opened label=stage0 and sending a test payload",
                          "",
                          CurrentRequestId(runtime),
                          peerId);
            if (auto locked = wdc.lock()) {
                locked->send("hello-from-" + runtime.args.role + "-" + runtime.args.peerId);
            }
            MarkDone(runtime.donePromise, runtime.doneFlag);
        } catch (const std::exception& exception) {
            RecordFailure(runtime, std::string("data channel open handler failed: ") + exception.what());
        }
    });

    dc->onClosed([&runtime, peerId]() {
        LogProbeEvent(runtime.args,
                      "rtc.data-channel-closed",
                      "remote data channel closed",
                      "",
                      CurrentRequestId(runtime),
                      peerId);
    });

    dc->onMessage([&runtime, peerId](rtc::message_variant data) {
        if (std::holds_alternative<std::string>(data)) {
            LogProbeEvent(runtime.args,
                          "rtc.data-channel-message",
                          std::get<std::string>(data),
                          "",
                          CurrentRequestId(runtime),
                          peerId);
        }
    });
}

std::shared_ptr<rtc::PeerConnection> CreatePeerConnection(ProbeRuntime& runtime) {
    rtc::Configuration config;
    if (runtime.args.iceServer.has_value()) {
        config.iceServers.emplace_back(*runtime.args.iceServer);
    }
    if (runtime.args.relayOnly) {
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;
    }

    auto pc = std::make_shared<rtc::PeerConnection>(config);

    pc->onLocalDescription([&runtime](rtc::Description description) {
        try {
            const std::string requestId = CurrentRequestId(runtime);
            const std::string messageType =
                description.type() == rtc::Description::Type::Offer ? "rtc.offer" : "rtc.answer";

            LogProbeEvent(runtime.args,
                          "send." + messageType,
                          "forwarding local SDP to signaling service",
                          "",
                          requestId,
                          runtime.args.targetId);

            SendJson(runtime.ws,
                     BuildSignalEnvelope(runtime.args,
                                         messageType,
                                         runtime.args.targetId,
                                         requestId,
                                         {
                                             {"description_type", description.typeString()},
                                             {"sdp", std::string(description)}
                                         }));
        } catch (const std::exception& exception) {
            RecordFailure(runtime, std::string("failed to forward local description: ") + exception.what());
        }
    });

    pc->onLocalCandidate([&runtime](rtc::Candidate candidate) {
        try {
            const std::string requestId = CurrentRequestId(runtime);
            LogProbeEvent(runtime.args,
                          "send.rtc.ice-candidate",
                          "forwarding local ICE candidate",
                          "",
                          requestId,
                          runtime.args.targetId);

            SendJson(runtime.ws,
                     BuildSignalEnvelope(runtime.args,
                                         "rtc.ice-candidate",
                                         runtime.args.targetId,
                                         requestId,
                                         {
                                             {"candidate", std::string(candidate)},
                                             {"mid", candidate.mid()}
                                         }));
        } catch (const std::exception& exception) {
            RecordFailure(runtime, std::string("failed to forward ICE candidate: ") + exception.what());
        }
    });

    pc->onStateChange([&runtime](rtc::PeerConnection::State state) {
        std::ostringstream detail;
        detail << state;
        LogProbeEvent(runtime.args,
                      "rtc.peer-connection-state",
                      detail.str(),
                      "",
                      CurrentRequestId(runtime),
                      runtime.args.targetId);
        if (state == rtc::PeerConnection::State::Failed) {
            RecordFailure(runtime, "PeerConnection entered failed state");
        }
    });

    pc->onGatheringStateChange([&runtime](rtc::PeerConnection::GatheringState state) {
        std::ostringstream detail;
        detail << state;
        LogProbeEvent(runtime.args,
                      "rtc.gathering-state",
                      detail.str(),
                      "",
                      CurrentRequestId(runtime),
                      runtime.args.targetId);
    });

    pc->onSignalingStateChange([&runtime](rtc::PeerConnection::SignalingState state) {
        std::ostringstream detail;
        detail << state;
        LogProbeEvent(runtime.args,
                      "rtc.signaling-state",
                      detail.str(),
                      "",
                      CurrentRequestId(runtime),
                      runtime.args.targetId);
    });

    pc->onDataChannel([&runtime](std::shared_ptr<rtc::DataChannel> dc) {
        LogProbeEvent(runtime.args,
                      "rtc.incoming-data-channel",
                      "received label=" + dc->label(),
                      "",
                      CurrentRequestId(runtime),
                      runtime.args.targetId);
        runtime.dc = dc;
        AttachDataChannelHandlers(dc, runtime, runtime.args.targetId);
    });

    return pc;
}

void EnsureOfferStarted(ProbeRuntime& runtime) {
    const std::string requestId = GenerateRequestId(runtime, "rtc");
    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        if (runtime.offerStarted || runtime.args.role != "controller") {
            return;
        }
        runtime.offerStarted = true;
        runtime.activeRequestId = requestId;
    }

    runtime.pc = CreatePeerConnection(runtime);
    runtime.dc = runtime.pc->createDataChannel("stage0");
    AttachDataChannelHandlers(runtime.dc, runtime, runtime.args.targetId);

    LogProbeEvent(runtime.args,
                  "rtc.offer-start",
                  "creating offer for target peer",
                  "",
                  CurrentRequestId(runtime),
                  runtime.args.targetId);
}

void HandleRegistrationAndMemberMessages(ProbeRuntime& runtime, const json& message) {
    const std::string type = message.value("type", "");
    if (type == "probe.registered") {
        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            runtime.registered = true;
        }
        LogProbeEvent(runtime.args,
                      "probe.registered",
                      "registration acknowledged by signaling service",
                      runtime.args.mode == ProbeMode::signal ? CurrentSignalState(runtime) : "",
                      "",
                      runtime.args.targetId);
        MaybeStartSignalScenario(runtime);
        return;
    }

    if (type == "room.member-list") {
        const auto members = ExtractMemberIds(message);
        UpdateTargetPresenceFromSnapshot(runtime, members);

        std::ostringstream detail;
        detail << "online_count=" << members.size() << " members=" << JoinStrings(members);
        LogProbeEvent(runtime.args,
                      "room.member-list",
                      detail.str(),
                      runtime.args.mode == ProbeMode::signal ? CurrentSignalState(runtime) : "",
                      "",
                      runtime.args.targetId);

        if (runtime.args.mode == ProbeMode::rtc && runtime.args.role == "controller"
            && ContainsValue(members, runtime.args.targetId)) {
            EnsureOfferStarted(runtime);
            return;
        }

        MaybeStartSignalScenario(runtime);
        return;
    }

    if (type == "room.member-join" || type == "room.member-leave") {
        const std::string memberId = ExtractUserIdFromPayload(message);
        if (memberId.empty()) {
            return;
        }

        bool targetLeftDuringSession = false;
        std::vector<std::string> snapshot;
        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            snapshot = runtime.onlineMembers;
            if (type == "room.member-join") {
                if (!ContainsValue(snapshot, memberId)) {
                    snapshot.push_back(memberId);
                }
            } else {
                snapshot.erase(std::remove(snapshot.begin(), snapshot.end(), memberId), snapshot.end());
                if (memberId == runtime.args.targetId && runtime.activeRequestId.has_value()) {
                    targetLeftDuringSession = true;
                }
            }
            runtime.onlineMembers = snapshot;
            runtime.targetPresent = ContainsValue(snapshot, runtime.args.targetId);
        }

        LogProbeEvent(runtime.args,
                      type,
                      "member=" + memberId + " online_members=" + JoinStrings(snapshot),
                      runtime.args.mode == ProbeMode::signal ? CurrentSignalState(runtime) : "",
                      "",
                      memberId);

        if (targetLeftDuringSession) {
            RecordFailure(runtime, "target user left the room before the scenario completed");
            return;
        }

        MaybeStartSignalScenario(runtime);
    }
}

void HandleSignalPhaseMessage(ProbeRuntime& runtime, const json& message) {
    const std::string type = message.value("type", "");
    if (type == "probe.registered" || type == "room.member-list" || type == "room.member-join"
        || type == "room.member-leave") {
        HandleRegistrationAndMemberMessages(runtime, message);
        return;
    }

    if (type == "signal.error") {
        throw std::runtime_error(message.value("message", "signaling error"));
    }

    if (!MessageTargetsLocalUser(message, runtime.args.peerId)) {
        return;
    }

    const std::string requestId = ExtractRequestId(message);
    const std::string remoteUserId = message.value("from_user_id", "");
    const bool echoedFromLocalUser = remoteUserId == runtime.args.peerId;
    if (!echoedFromLocalUser && !remoteUserId.empty() && remoteUserId != runtime.args.targetId) {
        return;
    }

    if (type == "linkmic.apply") {
        if (echoedFromLocalUser || runtime.args.role != "controller") {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            runtime.activeRequestId = requestId;
            runtime.activeRequestType = type;
            runtime.signalScenarioStarted = true;
            runtime.requestDeadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(runtime.args.requestTimeoutMs);
        }
        TransitionSignalState(runtime, "guest-applying", "received linkmic.apply", requestId, remoteUserId);

        if (runtime.args.signalAction == SignalAction::accept) {
            {
                std::lock_guard<std::mutex> lock(runtime.stateMutex);
                runtime.waitingJoinParams = true;
                runtime.requestDeadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(runtime.args.requestTimeoutMs);
            }
            TransitionSignalState(runtime,
                                  ConnectingStateForRole(runtime.args),
                                  "accepting guest apply and waiting for rtc.join-params",
                                  requestId,
                                  remoteUserId);
            SendBusinessMessage(runtime,
                                "linkmic.accept",
                                remoteUserId,
                                requestId,
                                {
                                    {"accepted_from", "linkmic.apply"},
                                    {"decision", "accept"}
                                },
                                "accepted incoming guest apply");
            return;
        }

        if (runtime.args.signalAction == SignalAction::reject) {
            SendBusinessMessage(runtime,
                                "linkmic.reject",
                                remoteUserId,
                                requestId,
                                {
                                    {"rejected_from", "linkmic.apply"},
                                    {"decision", "reject"}
                                },
                                "rejected incoming guest apply");
            TransitionSignalState(runtime,
                                  ResetStateForRole(runtime.args),
                                  "rejected guest apply",
                                  requestId,
                                  remoteUserId);
            MarkDone(runtime.donePromise, runtime.doneFlag);
        }
        return;
    }

    if (type == "linkmic.invite") {
        if (echoedFromLocalUser || runtime.args.role == "controller") {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            runtime.activeRequestId = requestId;
            runtime.activeRequestType = type;
            runtime.signalScenarioStarted = true;
            runtime.requestDeadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(runtime.args.requestTimeoutMs);
        }
        TransitionSignalState(runtime, "guest-invited", "received linkmic.invite", requestId, remoteUserId);

        if (runtime.args.signalAction == SignalAction::accept) {
            {
                std::lock_guard<std::mutex> lock(runtime.stateMutex);
                runtime.waitingJoinParams = true;
                runtime.requestDeadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(runtime.args.requestTimeoutMs);
            }
            TransitionSignalState(runtime,
                                  ConnectingStateForRole(runtime.args),
                                  "accepting host invite and waiting for rtc.join-params",
                                  requestId,
                                  remoteUserId);
            SendBusinessMessage(runtime,
                                "linkmic.accept",
                                remoteUserId,
                                requestId,
                                {
                                    {"accepted_from", "linkmic.invite"},
                                    {"decision", "accept"}
                                },
                                "accepted incoming host invite");
            return;
        }

        if (runtime.args.signalAction == SignalAction::reject) {
            SendBusinessMessage(runtime,
                                "linkmic.reject",
                                remoteUserId,
                                requestId,
                                {
                                    {"rejected_from", "linkmic.invite"},
                                    {"decision", "reject"}
                                },
                                "rejected incoming host invite");
            TransitionSignalState(runtime,
                                  ResetStateForRole(runtime.args),
                                  "rejected host invite",
                                  requestId,
                                  remoteUserId);
            MarkDone(runtime.donePromise, runtime.doneFlag);
        }
        return;
    }

    if (type == "linkmic.accept") {
        if (echoedFromLocalUser) {
            return;
        }
        if (!ActiveRequestMatches(runtime, requestId)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            runtime.activeRequestId = requestId;
            runtime.waitingJoinParams = true;
            runtime.requestDeadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(runtime.args.requestTimeoutMs);
        }
        TransitionSignalState(runtime,
                              ConnectingStateForRole(runtime.args),
                              "received linkmic.accept, waiting for rtc.join-params",
                              requestId,
                              remoteUserId);
        return;
    }

    if (type == "rtc.join-params" || type == "linkmic.connected") {
        if (echoedFromLocalUser) {
            return;
        }
        if (!ActiveRequestMatches(runtime, requestId)) {
            return;
        }

        const std::string payloadText = message.contains("payload") ? message.at("payload").dump() : "{}";
        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            runtime.waitingJoinParams = false;
            runtime.requestDeadline.reset();
            runtime.activeRequestId = requestId.empty() ? runtime.activeRequestId : std::optional<std::string>(requestId);
        }
        TransitionSignalState(runtime,
                              ActiveStateForRole(runtime.args),
                              "received " + type,
                              requestId,
                              remoteUserId);
        LogProbeEvent(runtime.args,
                      "recv." + type,
                      payloadText,
                      CurrentSignalState(runtime),
                      requestId,
                      remoteUserId);

        if (runtime.args.autoHangupMs < 0) {
            MarkDone(runtime.donePromise, runtime.doneFlag);
            return;
        }

        if (runtime.args.role == "controller") {
            {
                std::lock_guard<std::mutex> lock(runtime.stateMutex);
                runtime.hangupDeadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(runtime.args.autoHangupMs);
            }
            LogProbeEvent(runtime.args,
                          "timer.auto-hangup",
                          "will send linkmic.hangup after " + std::to_string(runtime.args.autoHangupMs) + " ms",
                          CurrentSignalState(runtime),
                          requestId,
                          remoteUserId);
        }
        return;
    }

    if (type == "linkmic.reject" || type == "linkmic.cancel") {
        if (echoedFromLocalUser) {
            return;
        }
        if (!ActiveRequestMatches(runtime, requestId)) {
            return;
        }
        TransitionSignalState(runtime,
                              ResetStateForRole(runtime.args),
                              "received " + type,
                              requestId,
                              remoteUserId);
        MarkDone(runtime.donePromise, runtime.doneFlag);
        return;
    }

    if (type == "linkmic.hangup" || type == "linkmic.kick") {
        if (echoedFromLocalUser) {
            return;
        }
        if (!ActiveRequestMatches(runtime, requestId)) {
            return;
        }
        TransitionSignalState(runtime,
                              ResetStateForRole(runtime.args),
                              "received " + type,
                              requestId,
                              remoteUserId);
        MarkDone(runtime.donePromise, runtime.doneFlag);
        return;
    }

    if (type == "linkmic.state-sync") {
        if (!ActiveRequestMatches(runtime, requestId)) {
            return;
        }
        const std::string syncedState = message.contains("payload") && message.at("payload").is_object()
            ? message.at("payload").value("state", "")
            : message.value("state", "");

        if (!syncedState.empty()) {
            TransitionSignalState(runtime,
                                  syncedState,
                                  "received linkmic.state-sync",
                                  requestId,
                                  remoteUserId);
            if (syncedState == ResetStateForRole(runtime.args)) {
                MarkDone(runtime.donePromise, runtime.doneFlag);
            }
            return;
        }

        LogProbeEvent(runtime.args,
                      "recv.linkmic.state-sync",
                      message.dump(),
                      CurrentSignalState(runtime),
                      requestId,
                      remoteUserId);
    }
}

void HandleRtcPhaseMessage(ProbeRuntime& runtime, const json& message) {
    const auto type = message.value("type", "");
    if (type == "probe.registered" || type == "room.member-list" || type == "room.member-join"
        || type == "room.member-leave") {
        HandleRegistrationAndMemberMessages(runtime, message);
        return;
    }

    if (type == "rtc.offer") {
        if (message.value("to_user_id", "") != runtime.args.peerId) {
            return;
        }
        if (message.value("from_user_id", "") != runtime.args.targetId) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            runtime.activeRequestId = ExtractRequestId(message);
        }

        if (!runtime.pc) {
            runtime.pc = CreatePeerConnection(runtime);
        }

        const auto payload = message.at("payload");
        const auto sdp = payload.at("sdp").get<std::string>();
        const auto descriptionType = payload.at("description_type").get<std::string>();
        LogProbeEvent(runtime.args,
                      "recv.rtc.offer",
                      "applying remote offer",
                      "",
                      CurrentRequestId(runtime),
                      message.value("from_user_id", ""));
        runtime.pc->setRemoteDescription(rtc::Description(sdp, descriptionType));
        return;
    }

    if (type == "rtc.answer") {
        if (!runtime.pc || message.value("to_user_id", "") != runtime.args.peerId) {
            return;
        }
        if (message.value("from_user_id", "") != runtime.args.targetId) {
            return;
        }

        const auto payload = message.at("payload");
        LogProbeEvent(runtime.args,
                      "recv.rtc.answer",
                      "applying remote answer",
                      "",
                      CurrentRequestId(runtime),
                      message.value("from_user_id", ""));
        runtime.pc->setRemoteDescription(
            rtc::Description(payload.at("sdp").get<std::string>(),
                             payload.at("description_type").get<std::string>()));
        return;
    }

    if (type == "rtc.ice-candidate") {
        if (!runtime.pc || message.value("to_user_id", "") != runtime.args.peerId) {
            return;
        }
        if (message.value("from_user_id", "") != runtime.args.targetId) {
            return;
        }

        const auto payload = message.at("payload");
        LogProbeEvent(runtime.args,
                      "recv.rtc.ice-candidate",
                      "applying remote ICE candidate",
                      "",
                      CurrentRequestId(runtime),
                      message.value("from_user_id", ""));
        runtime.pc->addRemoteCandidate(
            rtc::Candidate(payload.at("candidate").get<std::string>(),
                           payload.at("mid").get<std::string>()));
        return;
    }

    if (type == "signal.error") {
        throw std::runtime_error(message.value("message", "signaling error"));
    }
}

void HandleSignalingMessage(ProbeRuntime& runtime, const json& message) {
    if (runtime.args.mode == ProbeMode::signal) {
        HandleSignalPhaseMessage(runtime, message);
        return;
    }

    HandleRtcPhaseMessage(runtime, message);
}

void CheckSignalTimers(ProbeRuntime& runtime) {
    std::optional<std::string> timeoutMessage;
    std::optional<std::string> timeoutRequestId;
    std::optional<std::string> hangupRequestId;

    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        const auto now = std::chrono::steady_clock::now();

        if (runtime.requestDeadline.has_value() && now >= *runtime.requestDeadline) {
            timeoutRequestId = runtime.activeRequestId;
            if (runtime.waitingJoinParams) {
                timeoutMessage = "timed out waiting for rtc.join-params";
            } else if (!runtime.activeRequestType.empty()) {
                timeoutMessage = "timed out waiting for " + runtime.activeRequestType + " response";
            } else {
                timeoutMessage = "timed out waiting for signal phase progress";
            }
            runtime.requestDeadline.reset();
        } else if (runtime.hangupDeadline.has_value() && now >= *runtime.hangupDeadline
                   && !runtime.autoHangupSent && runtime.activeRequestId.has_value()) {
            runtime.autoHangupSent = true;
            runtime.hangupDeadline.reset();
            hangupRequestId = runtime.activeRequestId;
        }
    }

    if (timeoutMessage.has_value()) {
        RecordFailure(runtime, *timeoutMessage);
        return;
    }

    if (hangupRequestId.has_value()) {
        TransitionSignalState(runtime,
                              EndingStateForRole(runtime.args),
                              "auto hangup timer reached",
                              *hangupRequestId,
                              runtime.args.targetId);
        SendBusinessMessage(runtime,
                            "linkmic.hangup",
                            runtime.args.targetId,
                            *hangupRequestId,
                            {
                                {"reason", "auto-hangup"},
                                {"probe_mode", "signal"}
                            },
                            "controller requested session teardown");
        MarkDone(runtime.donePromise, runtime.doneFlag);
    }
}

void PrintBanner(const ProbeArgs& args) {
    std::cout << "signal_probe\n";
    std::cout << "  role: " << args.role << "\n";
    std::cout << "  peer_id: " << args.peerId << "\n";
    std::cout << "  target_id: " << args.targetId << "\n";
    std::cout << "  mode: " << ProbeModeToString(args.mode) << "\n";
    if (args.mode == ProbeMode::signal) {
        std::cout << "  action: " << SignalActionToString(args.signalAction) << "\n";
        std::cout << "  request_timeout_ms: " << args.requestTimeoutMs << "\n";
        std::cout << "  auto_hangup_ms: " << args.autoHangupMs << "\n";
    }
    std::cout << "  signaling_url: " << args.signalingUrl << "\n";
    if (args.iceServer.has_value()) {
        std::cout << "  ice_server: " << *args.iceServer << "\n";
    }
    std::cout << "  relay_only: " << (args.relayOnly ? "true" : "false") << "\n";
    std::cout << "  room_id: " << args.roomId << "\n";
    std::cout << "  timeout_ms: " << args.timeoutMs << std::endl;
}

}  // namespace

int RunSignalProbeApp(int argc, char** argv, const SignalProbeDefaults& defaults) try {
    ProbeArgs args = ParseArgs(argc, argv, defaults);
    PrintBanner(args);

    rtc::InitLogger(rtc::LogLevel::Warning);

    ProbeRuntime runtime;
    runtime.args = std::move(args);
    runtime.ws = std::make_shared<rtc::WebSocket>();
    runtime.donePromise = std::make_shared<std::promise<void>>();
    runtime.doneFlag = std::make_shared<std::atomic_bool>(false);
    runtime.signalState = runtime.args.mode == ProbeMode::signal ? ResetStateForRole(runtime.args) : "";

    std::promise<void> wsReadyPromise;
    auto wsReadyFuture = wsReadyPromise.get_future();
    auto wsReadySettled = std::make_shared<std::atomic_bool>(false);

    runtime.ws->onOpen([&runtime, &wsReadyPromise, wsReadySettled]() {
        try {
            {
                std::lock_guard<std::mutex> lock(runtime.stateMutex);
                runtime.registered = false;
            }

            json registerMessage = {
                {"type", "probe.register"},
                {"room_id", runtime.args.roomId},
                {"peer_id", runtime.args.peerId},
                {"role", runtime.args.role},
                {"mode", ProbeModeToString(runtime.args.mode)}
            };

            LogProbeEvent(runtime.args,
                          "send.probe.register",
                          "registering probe with signaling service",
                          runtime.args.mode == ProbeMode::signal ? CurrentSignalState(runtime) : "",
                          "",
                          runtime.args.targetId);
            SendJson(runtime.ws, registerMessage);

            bool expected = false;
            if (wsReadySettled->compare_exchange_strong(expected, true)) {
                wsReadyPromise.set_value();
            }
        } catch (const std::exception& exception) {
            RecordFailure(runtime, std::string("failed during WebSocket open handler: ") + exception.what());
            bool expected = false;
            if (wsReadySettled->compare_exchange_strong(expected, true)) {
                wsReadyPromise.set_exception(std::make_exception_ptr(std::runtime_error(exception.what())));
            }
        }
    });

    runtime.ws->onError([&runtime, &wsReadyPromise, wsReadySettled](const std::string& error) {
        LogProbeEvent(runtime.args,
                      "signaling.error",
                      error,
                      runtime.args.mode == ProbeMode::signal ? CurrentSignalState(runtime) : "",
                      CurrentRequestId(runtime),
                      runtime.args.targetId);
        bool expected = false;
        if (wsReadySettled->compare_exchange_strong(expected, true)) {
            wsReadyPromise.set_exception(std::make_exception_ptr(std::runtime_error(error)));
        }
    });

    runtime.ws->onClosed([&runtime]() {
        LogProbeEvent(runtime.args,
                      "signaling.closed",
                      "WebSocket closed",
                      runtime.args.mode == ProbeMode::signal ? CurrentSignalState(runtime) : "",
                      CurrentRequestId(runtime),
                      runtime.args.targetId);
        if (!runtime.doneFlag->load()) {
            RecordFailure(runtime, "signaling socket closed before scenario completed");
        }
    });

    runtime.ws->onMessage([&runtime](rtc::message_variant data) {
        if (!std::holds_alternative<std::string>(data)) {
            return;
        }

        try {
            const json message = json::parse(std::get<std::string>(data));
            HandleSignalingMessage(runtime, message);
        } catch (const std::exception& exception) {
            RecordFailure(runtime, std::string("failed to process signaling message: ") + exception.what());
        }
    });

    runtime.ws->open(runtime.args.signalingUrl);
    wsReadyFuture.get();

    auto doneFuture = runtime.donePromise->get_future();
    const auto overallDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(runtime.args.timeoutMs);

    while (doneFuture.wait_for(100ms) != std::future_status::ready) {
        if (std::chrono::steady_clock::now() >= overallDeadline) {
            throw std::runtime_error(runtime.args.mode == ProbeMode::signal
                                         ? "timed out waiting for signal phase scenario"
                                         : "timed out waiting for RTC connection");
        }

        if (runtime.args.mode == ProbeMode::signal) {
            CheckSignalTimers(runtime);
        }
    }

    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        if (runtime.failureMessage.has_value()) {
            throw std::runtime_error(*runtime.failureMessage);
        }
    }

    std::this_thread::sleep_for(500ms);
    if (runtime.dc) {
        runtime.dc->close();
    }
    if (runtime.pc) {
        runtime.pc->close();
    }
    runtime.ws->close();
    return 0;
} catch (const std::exception& ex) {
    std::cerr << "signal_probe failed: " << ex.what() << std::endl;
    return 1;
}
