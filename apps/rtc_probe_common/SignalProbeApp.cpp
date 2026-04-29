#include "SignalProbeApp.hpp"

#include "capture/LiveDeviceCaptureSource.hpp"
#include "encode/OpusAudioEncoder.hpp"

#include <rtc/rtc.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/rtppacketizer.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
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
constexpr char kDefaultMicCaptureConfigPath[] = "apps/rtc_probe/default_mic_capture.txt";
constexpr int kDefaultMediaRunMs = 5000;
constexpr int kDefaultOpusBitrate = 64000;
constexpr int kDefaultOpusPayloadType = 111;

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
    std::string micCaptureConfigPath = kDefaultMicCaptureConfigPath;
    std::optional<std::string> iceServer;
    std::optional<std::string> requestId;
    ProbeMode mode = ProbeMode::rtc;
    SignalAction signalAction = SignalAction::passive;
    bool relayOnly = false;
    bool sendMic = false;
    bool expectRemoteAudio = false;
    int timeoutMs = 30000;
    int requestTimeoutMs = 10000;
    int autoHangupMs = 2000;
    int mediaRunMs = kDefaultMediaRunMs;
    int opusBitrate = kDefaultOpusBitrate;
};

struct ProbeOutboundAudioState {
    std::shared_ptr<rtc::Track> track;
    std::thread worker;
    std::atomic_bool stopRequested{false};
    std::atomic_bool trackOpen{false};
    std::atomic_bool firstPacketSent{false};
    std::atomic_int capturedFrames{0};
    std::atomic_int sentPackets{0};
    std::atomic_int droppedFramesBeforeTrackOpen{0};
    std::atomic_llong sentSamples{0};
    std::optional<std::chrono::steady_clock::time_point> completionDeadline;
    live_studio::capture::LiveDeviceCaptureSource captureSource;
    live_studio::encode::OpusAudioEncoder encoder;
    int payloadType = kDefaultOpusPayloadType;
    bool initialized = false;
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
    bool shutdownRequested = false;
    bool incomingAudioTrackOpened = false;
    std::optional<std::chrono::steady_clock::time_point> requestDeadline;
    std::optional<std::chrono::steady_clock::time_point> hangupDeadline;
    std::optional<std::string> activeRequestId;
    std::string activeRequestType;
    std::string signalState;
    std::vector<std::string> onlineMembers;
    std::uint64_t requestSequence = 0;
    std::optional<std::string> failureMessage;
    ProbeOutboundAudioState outboundAudio;
    std::shared_ptr<rtc::Track> inboundAudioTrack;
    std::atomic_bool inboundAudioFirstPacketLogged{false};
    std::atomic_int inboundAudioPackets{0};

    ~ProbeRuntime() {
        outboundAudio.stopRequested.store(true);
        outboundAudio.captureSource.stop();
        if (outboundAudio.worker.joinable()) {
            outboundAudio.worker.join();
        }
    }
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

bool ShouldSendMic(const ProbeArgs& args) {
    return args.mode == ProbeMode::rtc && args.role == "controller" && args.sendMic;
}

bool ShouldExpectRemoteAudio(const ProbeArgs& args) {
    return args.mode == ProbeMode::rtc && args.expectRemoteAudio;
}

void ThrowIfStatusNotOk(const live_studio::Status& status, const std::string& context) {
    if (!status.ok()) {
        throw std::runtime_error(context + ": " + status.message);
    }
}

std::uint32_t StableSsrcForArgs(const ProbeArgs& args) {
    const std::string seed = args.peerId + "|" + args.targetId + "|" + args.roomId + "|audio";
    const auto hashed = static_cast<std::uint32_t>(std::hash<std::string>{}(seed));
    return hashed == 0 ? 1u : hashed;
}

std::string BuildOpusProfile(int channels, int bitrate) {
    const bool stereo = channels > 1;
    return "minptime=10;maxaveragebitrate=" + std::to_string(std::max(6000, bitrate)) +
           ";stereo=" + (stereo ? "1" : "0") +
           ";sprop-stereo=" + (stereo ? "1" : "0") +
           ";useinbandfec=1";
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

std::string NormalizeParticipantRole(const std::string& role) {
    const std::string lowered = ToLowerCopy(role);
    if (lowered == "controller" || lowered == "participant") {
        return lowered;
    }
    throw std::runtime_error("unsupported participantRole: " + role);
}

const json* FindJsonField(const json& object, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        const auto it = object.find(name);
        if (it != object.end()) {
            return &(*it);
        }
    }
    return nullptr;
}

std::string JsonValueToString(const json& value, const std::string& fieldName) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    throw std::runtime_error(fieldName + " must be a string or integer");
}

std::string ExtractRequiredJsonString(const json& object,
                                      std::initializer_list<const char*> names,
                                      const std::string& fieldName) {
    const json* value = FindJsonField(object, names);
    if (value == nullptr) {
        throw std::runtime_error(fieldName + " is required in rtcJoinParams");
    }

    const std::string extracted = JsonValueToString(*value, fieldName);
    if (extracted.empty()) {
        throw std::runtime_error(fieldName + " must not be empty in rtcJoinParams");
    }
    return extracted;
}

std::optional<std::string> ExtractOptionalJsonString(const json& object,
                                                     std::initializer_list<const char*> names,
                                                     const std::string& fieldName) {
    const json* value = FindJsonField(object, names);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    return JsonValueToString(*value, fieldName);
}

std::string LoadTextFile(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open rtcJoinParams file: " + path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

const json& ResolveRtcJoinParamsObject(const json& root) {
    const json* joinParams = FindJsonField(root, {"rtcJoinParams", "rtc_join_params"});
    if (joinParams != nullptr) {
        if (!joinParams->is_object()) {
            throw std::runtime_error("rtcJoinParams must be a JSON object");
        }
        return *joinParams;
    }

    if (!root.is_object()) {
        throw std::runtime_error("rtcJoinParams payload must be a JSON object");
    }
    return root;
}

void ApplyRtcJoinParams(ProbeArgs& args, const json& root) {
    const json& joinParams = ResolveRtcJoinParamsObject(root);
    const auto transport = ExtractOptionalJsonString(joinParams, {"transport"}, "transport");
    if (transport.has_value() && ToLowerCopy(*transport) != "libdatachannel") {
        throw std::runtime_error("unsupported rtcJoinParams transport: " + *transport);
    }

    const std::string roomName =
        ExtractRequiredJsonString(joinParams, {"roomName", "room_name"}, "roomName");
    const std::string participantRole = NormalizeParticipantRole(
        ExtractRequiredJsonString(joinParams, {"participantRole", "participant_role"}, "participantRole"));
    const std::string signalingUrl =
        ExtractRequiredJsonString(joinParams, {"signalingUrl", "signaling_url"}, "signalingUrl");
    const std::string requestId =
        ExtractRequiredJsonString(joinParams, {"requestId", "request_id"}, "requestId");
    const std::string controllerUserId = ExtractRequiredJsonString(
        joinParams, {"controllerUserId", "controller_user_id"}, "controllerUserId");
    const std::string participantUserId = ExtractRequiredJsonString(
        joinParams, {"participantUserId", "participant_user_id"}, "participantUserId");
    if (controllerUserId == participantUserId) {
        throw std::runtime_error("rtcJoinParams controllerUserId and participantUserId must differ");
    }

    args.mode = ProbeMode::rtc;
    args.role = participantRole;
    args.roomId = roomName;
    args.signalingUrl = signalingUrl;
    args.requestId = requestId;
    if (participantRole == "controller") {
        args.peerId = controllerUserId;
        args.targetId = participantUserId;
    } else {
        args.peerId = participantUserId;
        args.targetId = controllerUserId;
    }
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
        << "  --request-id <value>         bind RTC/WebSocket registration to an accepted linkmic request\n"
        << "  --rtc-join-params-json <v>   inline JSON from rtcJoinParams or a full /linkmic/respond response\n"
        << "  --rtc-join-params-file <v>   path to a JSON file containing rtcJoinParams or /linkmic/respond response\n"
        << "  --ice-server <value>         example: turn://user:pass@127.0.0.1:3478?transport=udp\n"
        << "  --relay-only                 require relay candidates only\n"
        << "  --send-mic                   controller-side only; capture microphone and send Opus RTP\n"
        << "  --expect-remote-audio        keep the RTC session alive until the inbound audio track closes\n"
        << "  --mic-capture-config <path>  default: " << kDefaultMicCaptureConfigPath << "\n"
        << "  --media-run-ms <value>       send duration after first audio packet, default: " << kDefaultMediaRunMs << "\n"
        << "  --opus-bitrate <value>       controller-side only, default: " << kDefaultOpusBitrate << "\n"
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
    std::optional<std::string> rtcJoinParamsJson;
    std::optional<std::string> rtcJoinParamsFile;

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
        } else if (arg == "--request-id") {
            args.requestId = requireValue(arg);
        } else if (arg == "--rtc-join-params-json") {
            rtcJoinParamsJson = requireValue(arg);
        } else if (arg == "--rtc-join-params-file") {
            rtcJoinParamsFile = requireValue(arg);
        } else if (arg == "--ice-server") {
            args.iceServer = requireValue(arg);
        } else if (arg == "--relay-only") {
            args.relayOnly = true;
        } else if (arg == "--send-mic") {
            args.sendMic = true;
        } else if (arg == "--expect-remote-audio") {
            args.expectRemoteAudio = true;
        } else if (arg == "--mic-capture-config") {
            args.micCaptureConfigPath = requireValue(arg);
        } else if (arg == "--media-run-ms") {
            args.mediaRunMs = std::stoi(requireValue(arg));
        } else if (arg == "--opus-bitrate") {
            args.opusBitrate = std::stoi(requireValue(arg));
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

    if (rtcJoinParamsJson.has_value() && rtcJoinParamsFile.has_value()) {
        throw std::runtime_error("use only one of --rtc-join-params-json or --rtc-join-params-file");
    }

    if (!roomIdExplicitlySet) {
        args.roomId = args.mode == ProbeMode::rtc ? kDefaultRtcRoomId : kDefaultSignalRoomId;
    }

    if (!actionExplicitlySet && args.mode == ProbeMode::signal) {
        args.signalAction = args.role == "controller" ? SignalAction::invite : SignalAction::accept;
    }

    if (rtcJoinParamsFile.has_value()) {
        rtcJoinParamsJson = LoadTextFile(*rtcJoinParamsFile);
    }
    if (rtcJoinParamsJson.has_value()) {
        ApplyRtcJoinParams(args, json::parse(*rtcJoinParamsJson));
    }

    if (args.mediaRunMs < 0) {
        throw std::runtime_error("--media-run-ms must be >= 0");
    }
    if (args.opusBitrate <= 0) {
        throw std::runtime_error("--opus-bitrate must be positive");
    }
    if (args.sendMic && args.mode != ProbeMode::rtc) {
        throw std::runtime_error("--send-mic is only supported in rtc mode");
    }
    if (args.sendMic && args.role != "controller") {
        throw std::runtime_error("--send-mic is currently supported on the controller side only");
    }
    if (args.expectRemoteAudio && args.mode != ProbeMode::rtc) {
        throw std::runtime_error("--expect-remote-audio is only supported in rtc mode");
    }

    return args;
}

void MarkDone(const std::shared_ptr<std::promise<void>>& promise,
              const std::shared_ptr<std::atomic_bool>& doneFlag) {
    bool expected = false;
    if (doneFlag->compare_exchange_strong(expected, true)) {
        try {
            promise->set_value();
        } catch (const std::future_error&) {
        }
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
    if (!runtime.activeRequestId.has_value()) {
        return true;
    }
    if (requestId.empty()) {
        return false;
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

void AttachInboundTrackHandlers(const std::shared_ptr<rtc::Track>& track,
                                ProbeRuntime& runtime,
                                const std::string& peerId) {
    const std::string mid = track->mid();
    const std::string kind = track->description().type();

    track->onOpen([&runtime, peerId, mid, kind]() {
        LogProbeEvent(runtime.args,
                      "rtc.track-open",
                      "opened mid=" + mid + " kind=" + kind,
                      "",
                      CurrentRequestId(runtime),
                      peerId);
        if (kind == "audio") {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            runtime.incomingAudioTrackOpened = true;
        }
    });

    track->onClosed([&runtime, peerId, mid, kind]() {
        bool shutdownRequested = false;
        bool shouldMarkDone = false;
        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            shutdownRequested = runtime.shutdownRequested;
            shouldMarkDone = !shutdownRequested && ShouldExpectRemoteAudio(runtime.args) &&
                runtime.incomingAudioTrackOpened && kind == "audio";
        }

        LogProbeEvent(runtime.args,
                      "rtc.track-closed",
                      "closed mid=" + mid + " kind=" + kind,
                      "",
                      CurrentRequestId(runtime),
                      peerId);

        if (shouldMarkDone) {
            MarkDone(runtime.donePromise, runtime.doneFlag);
        }
    });

    track->onError([&runtime, peerId, mid](const std::string& error) {
        RecordFailure(runtime, "track " + mid + " error: " + error + " peer=" + peerId);
    });
}

void EnsureInboundAudioReady(ProbeRuntime& runtime) {
    if (!ShouldExpectRemoteAudio(runtime.args) || runtime.args.role == "controller") {
        return;
    }
    if (runtime.inboundAudioTrack) {
        return;
    }
    if (!runtime.pc) {
        throw std::runtime_error("cannot initialize inbound audio before PeerConnection exists");
    }

    rtc::Description::Audio audioDescription("audio", rtc::Description::Direction::RecvOnly);
    audioDescription.addOpusCodec(kDefaultOpusPayloadType);

    runtime.inboundAudioTrack = runtime.pc->addTrack(audioDescription);
    auto receivingSession = std::make_shared<rtc::RtcpReceivingSession>();
    runtime.inboundAudioTrack->setMediaHandler(receivingSession);
    runtime.inboundAudioTrack->onMessage(
        [&runtime](rtc::binary message) {
            const int packets = runtime.inboundAudioPackets.fetch_add(1) + 1;
            {
                std::lock_guard<std::mutex> lock(runtime.stateMutex);
                runtime.incomingAudioTrackOpened = true;
            }
            bool expected = false;
            if (runtime.inboundAudioFirstPacketLogged.compare_exchange_strong(expected, true)) {
                LogProbeEvent(runtime.args,
                              "rtc.audio-packet-recv",
                              "first inbound RTP packet size=" + std::to_string(message.size()),
                              "",
                              CurrentRequestId(runtime),
                              runtime.args.targetId);
            } else if (packets % 100 == 0) {
                LogProbeEvent(runtime.args,
                              "rtc.audio-packet-recv",
                              "packets=" + std::to_string(packets),
                              "",
                              CurrentRequestId(runtime),
                              runtime.args.targetId);
            }
        },
        nullptr);
    AttachInboundTrackHandlers(runtime.inboundAudioTrack, runtime, runtime.args.targetId);

    LogProbeEvent(runtime.args,
                  "rtc.audio-track-recv-ready",
                  "prepared recvonly Opus audio track before answer",
                  "",
                  CurrentRequestId(runtime),
                  runtime.args.targetId);
}

void AttachOutboundAudioTrackHandlers(const std::shared_ptr<rtc::Track>& track,
                                      ProbeRuntime& runtime,
                                      const std::string& peerId) {
    const std::string mid = track->mid();

    track->onOpen([&runtime, peerId, mid]() {
        runtime.outboundAudio.trackOpen.store(true);
        LogProbeEvent(runtime.args,
                      "rtc.audio-track-open",
                      "opened mid=" + mid,
                      "",
                      CurrentRequestId(runtime),
                      peerId);
    });

    track->onClosed([&runtime, peerId, mid]() {
        runtime.outboundAudio.trackOpen.store(false);

        bool shutdownRequested = false;
        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            shutdownRequested = runtime.shutdownRequested;
        }

        LogProbeEvent(runtime.args,
                      "rtc.audio-track-closed",
                      "closed mid=" + mid,
                      "",
                      CurrentRequestId(runtime),
                      peerId);

        if (!shutdownRequested && ShouldSendMic(runtime.args) && !runtime.doneFlag->load()) {
            RecordFailure(runtime, "outbound audio track closed before the media run completed");
        }
    });

    track->onError([&runtime, peerId, mid](const std::string& error) {
        RecordFailure(runtime, "outbound audio track " + mid + " error: " + error + " peer=" + peerId);
    });
}

void OutboundAudioWorkerMain(ProbeRuntime* runtime) {
    auto& state = runtime->outboundAudio;
    try {
        while (!state.stopRequested.load() && !runtime->doneFlag->load()) {
            live_studio::capture::CaptureEvent event;
            const live_studio::Status readStatus = state.captureSource.readNext(&event);
            if (!readStatus.ok()) {
                if (!state.stopRequested.load() && !runtime->doneFlag->load()) {
                    RecordFailure(*runtime, "microphone capture failed: " + readStatus.message);
                }
                break;
            }

            if (event.kind != live_studio::capture::CaptureEventKind::audio) {
                continue;
            }

            state.capturedFrames.fetch_add(1);
            if (!state.trackOpen.load()) {
                state.droppedFramesBeforeTrackOpen.fetch_add(1);
                continue;
            }

            std::vector<live_studio::encode::EncodedOpusFrame> encodedFrames;
            const live_studio::Status encodeStatus = state.encoder.encodeFrame(event.audioFrame, &encodedFrames);
            if (!encodeStatus.ok()) {
                RecordFailure(*runtime, "Opus encode failed: " + encodeStatus.message);
                break;
            }

            for (const auto& encodedFrame : encodedFrames) {
                if (state.stopRequested.load() || runtime->doneFlag->load()) {
                    break;
                }

                try {
                    runtime->outboundAudio.track->sendFrame(
                        reinterpret_cast<const rtc::byte*>(encodedFrame.payload.data()),
                        encodedFrame.payload.size(),
                        std::chrono::duration<double>(
                            static_cast<double>(encodedFrame.ptsSamples) /
                            static_cast<double>(std::max(1, encodedFrame.sampleRate))));
                } catch (const std::exception& exception) {
                    RecordFailure(*runtime, std::string("failed to send Opus frame: ") + exception.what());
                    break;
                }

                const int sentPackets = state.sentPackets.fetch_add(1) + 1;
                state.sentSamples.fetch_add(encodedFrame.samplesPerChannel);

                bool firstPacket = false;
                bool expected = false;
                if (state.firstPacketSent.compare_exchange_strong(expected, true)) {
                    firstPacket = true;
                    {
                        std::lock_guard<std::mutex> lock(runtime->stateMutex);
                        state.completionDeadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(runtime->args.mediaRunMs);
                    }
                    LogProbeEvent(runtime->args,
                                  "rtc.audio-send-start",
                                  "first packet sent " + encodedFrame.payloadSummary +
                                      " run_ms=" + std::to_string(runtime->args.mediaRunMs),
                                  "",
                                  CurrentRequestId(*runtime),
                                  runtime->args.targetId);
                }

                if (firstPacket || sentPackets % 100 == 0) {
                    LogProbeEvent(runtime->args,
                                  "rtc.audio-packet-sent",
                                  encodedFrame.payloadSummary + " packets=" + std::to_string(sentPackets),
                                  "",
                                  CurrentRequestId(*runtime),
                                  runtime->args.targetId);
                }
            }
        }
    } catch (const std::exception& exception) {
        if (!state.stopRequested.load() && !runtime->doneFlag->load()) {
            RecordFailure(*runtime, std::string("outbound audio worker failed: ") + exception.what());
        }
    }
}

void EnsureOutboundAudioReady(ProbeRuntime& runtime) {
    if (!ShouldSendMic(runtime.args)) {
        return;
    }
    if (runtime.outboundAudio.initialized) {
        return;
    }
    if (!runtime.pc) {
        throw std::runtime_error("cannot initialize outbound audio before PeerConnection exists");
    }

    auto& state = runtime.outboundAudio;
    ThrowIfStatusNotOk(state.captureSource.configureDeviceSource(runtime.args.micCaptureConfigPath),
                       "failed to load mic capture config");
    ThrowIfStatusNotOk(state.captureSource.start(), "failed to start microphone capture");
    if (!state.captureSource.hasAudio()) {
        throw std::runtime_error("mic capture config did not enable audio_device");
    }

    const auto& captureConfig = state.captureSource.config();
    ThrowIfStatusNotOk(state.encoder.configure(captureConfig.sampleRate, captureConfig.channels, runtime.args.opusBitrate),
                       "failed to configure Opus encoder");

    const std::uint32_t ssrc = StableSsrcForArgs(runtime.args);
    const std::string cname = runtime.args.peerId + "-audio";
    const std::string msid = runtime.args.peerId + "-stream";

    rtc::Description::Audio audioDescription("audio");
    audioDescription.addOpusCodec(state.payloadType, BuildOpusProfile(captureConfig.channels, runtime.args.opusBitrate));
    audioDescription.addSSRC(ssrc, cname, msid, cname);

    state.track = runtime.pc->addTrack(audioDescription);
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc,
                                                                   cname,
                                                                   static_cast<std::uint8_t>(state.payloadType),
                                                                   48000);
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
    auto senderReport = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(senderReport);
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    state.track->setMediaHandler(packetizer);

    AttachOutboundAudioTrackHandlers(state.track, runtime, runtime.args.targetId);

    state.worker = std::thread(OutboundAudioWorkerMain, &runtime);
    state.initialized = true;

    LogProbeEvent(runtime.args,
                  "rtc.audio-track-added",
                  "configured " + captureConfig.resolvedAudioDeviceName + " " +
                      std::to_string(captureConfig.sampleRate) + "Hz/" +
                      std::to_string(captureConfig.channels) + "ch bitrate=" +
                      std::to_string(runtime.args.opusBitrate),
                  "",
                  CurrentRequestId(runtime),
                  runtime.args.targetId);
}

void CheckRtcMediaTimers(ProbeRuntime& runtime) {
    if (!ShouldSendMic(runtime.args)) {
        return;
    }

    bool shouldComplete = false;
    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        shouldComplete = runtime.outboundAudio.completionDeadline.has_value() &&
            std::chrono::steady_clock::now() >= *runtime.outboundAudio.completionDeadline;
        if (shouldComplete) {
            runtime.outboundAudio.completionDeadline.reset();
        }
    }

    if (!shouldComplete) {
        return;
    }

    LogProbeEvent(runtime.args,
                  "rtc.media-run-complete",
                  "sent_packets=" + std::to_string(runtime.outboundAudio.sentPackets.load()) +
                      " captured_frames=" + std::to_string(runtime.outboundAudio.capturedFrames.load()) +
                      " dropped_before_track_open=" +
                      std::to_string(runtime.outboundAudio.droppedFramesBeforeTrackOpen.load()),
                  "",
                  CurrentRequestId(runtime),
                  runtime.args.targetId);
    MarkDone(runtime.donePromise, runtime.doneFlag);
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
            if (!ShouldSendMic(runtime.args) && !ShouldExpectRemoteAudio(runtime.args)) {
                MarkDone(runtime.donePromise, runtime.doneFlag);
            }
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
        if (ShouldSendMic(runtime.args)) {
            if (state == rtc::PeerConnection::State::Connected) {
                runtime.outboundAudio.trackOpen.store(true);
                LogProbeEvent(runtime.args,
                              "rtc.audio-send-ready",
                              "peer connection connected, outbound audio may flow",
                              "",
                              CurrentRequestId(runtime),
                              runtime.args.targetId);
            } else if (state == rtc::PeerConnection::State::Disconnected ||
                       state == rtc::PeerConnection::State::Failed ||
                       state == rtc::PeerConnection::State::Closed) {
                runtime.outboundAudio.trackOpen.store(false);
            }
        }
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

    pc->onTrack([&runtime](std::shared_ptr<rtc::Track> track) {
        LogProbeEvent(runtime.args,
                      "rtc.incoming-track",
                      "received mid=" + track->mid() + " kind=" + track->description().type(),
                      "",
                      CurrentRequestId(runtime),
                      runtime.args.targetId);
        AttachInboundTrackHandlers(track, runtime, runtime.args.targetId);
    });

    return pc;
}

void EnsureOfferStarted(ProbeRuntime& runtime) {
    std::string requestId;
    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        if (runtime.offerStarted || runtime.args.role != "controller") {
            return;
        }
        runtime.offerStarted = true;
        if (runtime.activeRequestId.has_value() && !runtime.activeRequestId->empty()) {
            requestId = *runtime.activeRequestId;
        } else {
            requestId = GenerateRequestId(runtime, "rtc");
            runtime.activeRequestId = requestId;
        }
    }

    runtime.pc = CreatePeerConnection(runtime);
    EnsureOutboundAudioReady(runtime);
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
                      CurrentRequestId(runtime),
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
        bool targetLeaveShouldFail = false;
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
                    targetLeaveShouldFail =
                        !ShouldExpectRemoteAudio(runtime.args) || !runtime.incomingAudioTrackOpened;
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

        if (targetLeftDuringSession && targetLeaveShouldFail) {
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
        bool shouldScheduleAutoHangup = false;
        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            runtime.waitingJoinParams = false;
            runtime.requestDeadline.reset();
            runtime.activeRequestId = requestId.empty() ? runtime.activeRequestId : std::optional<std::string>(requestId);
            if (runtime.args.role == "controller"
                && runtime.args.autoHangupMs >= 0
                && !runtime.autoHangupSent
                && !runtime.hangupDeadline.has_value()) {
                runtime.hangupDeadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(runtime.args.autoHangupMs);
                shouldScheduleAutoHangup = true;
            }
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

        if (shouldScheduleAutoHangup) {
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
    const std::string requestId = ExtractRequestId(message);
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
        if (!ActiveRequestMatches(runtime, requestId)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            runtime.activeRequestId = requestId.empty() ? runtime.activeRequestId : std::optional<std::string>(requestId);
        }

        if (!runtime.pc) {
            runtime.pc = CreatePeerConnection(runtime);
            EnsureInboundAudioReady(runtime);
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
        if (!ActiveRequestMatches(runtime, requestId)) {
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
        if (!ActiveRequestMatches(runtime, requestId)) {
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
    if (args.requestId.has_value()) {
        std::cout << "  request_id: " << *args.requestId << "\n";
    }
    if (args.iceServer.has_value()) {
        std::cout << "  ice_server: " << *args.iceServer << "\n";
    }
    std::cout << "  relay_only: " << (args.relayOnly ? "true" : "false") << "\n";
    std::cout << "  send_mic: " << (args.sendMic ? "true" : "false") << "\n";
    std::cout << "  expect_remote_audio: " << (args.expectRemoteAudio ? "true" : "false") << "\n";
    if (args.sendMic) {
        std::cout << "  mic_capture_config: " << args.micCaptureConfigPath << "\n";
        std::cout << "  media_run_ms: " << args.mediaRunMs << "\n";
        std::cout << "  opus_bitrate: " << args.opusBitrate << "\n";
    }
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
    runtime.activeRequestId = runtime.args.requestId;

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
            if (runtime.args.requestId.has_value()) {
                registerMessage["request_id"] = *runtime.args.requestId;
            }

            LogProbeEvent(runtime.args,
                          "send.probe.register",
                          "registering probe with signaling service",
                          runtime.args.mode == ProbeMode::signal ? CurrentSignalState(runtime) : "",
                          CurrentRequestId(runtime),
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
        bool shutdownRequested = false;
        {
            std::lock_guard<std::mutex> lock(runtime.stateMutex);
            shutdownRequested = runtime.shutdownRequested;
        }

        LogProbeEvent(runtime.args,
                      "signaling.closed",
                      "WebSocket closed",
                      runtime.args.mode == ProbeMode::signal ? CurrentSignalState(runtime) : "",
                      CurrentRequestId(runtime),
                      runtime.args.targetId);
        if (!shutdownRequested && !runtime.doneFlag->load()) {
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
                                         : (ShouldSendMic(runtime.args) || ShouldExpectRemoteAudio(runtime.args))
                                             ? "timed out waiting for RTC media scenario"
                                             : "timed out waiting for RTC connection");
        }

        if (runtime.args.mode == ProbeMode::signal) {
            CheckSignalTimers(runtime);
        } else {
            CheckRtcMediaTimers(runtime);
        }
    }

    {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        if (runtime.failureMessage.has_value()) {
            throw std::runtime_error(*runtime.failureMessage);
        }
        runtime.shutdownRequested = true;
    }

    runtime.outboundAudio.stopRequested.store(true);
    runtime.outboundAudio.captureSource.stop();
    if (runtime.outboundAudio.worker.joinable()) {
        runtime.outboundAudio.worker.join();
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
