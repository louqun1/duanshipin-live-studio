#include "SignalProbeApp.hpp"
#include "RtcAudioProbe.hpp"
#include "SignalProbeSession.hpp"

#include <rtc/rtc.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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
constexpr char kDefaultRecvAudioWavDir[] = "logs/rtc-audio";
constexpr int kDefaultMediaRunMs = 5000;
constexpr int kDefaultOpusBitrate = 64000;

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
    std::string recvAudioWavPath;
    int timeoutMs = 30000;
    int requestTimeoutMs = 10000;
    int autoHangupMs = 2000;
    int mediaRunMs = kDefaultMediaRunMs;
    int opusBitrate = kDefaultOpusBitrate;
};

struct ProbeRuntime {
    ProbeArgs args;
    std::unique_ptr<rtc_probe_common::RtcAudioProbe> audioProbe;
    std::unique_ptr<rtc_probe_common::SignalProbeSession> signalingSession;
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<std::promise<void>> donePromise;
    std::shared_ptr<std::atomic_bool> doneFlag;
    std::mutex stateMutex;
    bool offerStarted = false;
    bool shutdownRequested = false;
    std::optional<std::string> failureMessage;
};

std::string CurrentRequestId(ProbeRuntime& runtime);
void LogProbeEvent(const ProbeArgs& args,
                   const std::string& event,
                   const std::string& detail,
                   const std::string& state = "",
                   const std::string& requestId = "",
                   const std::string& remoteUserId = "");
void MarkDone(const std::shared_ptr<std::promise<void>>& promise,
              const std::shared_ptr<std::atomic_bool>& doneFlag);
void RecordFailure(ProbeRuntime& runtime, const std::string& message);

std::int64_t NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string GenerateProbeRequestId(const ProbeArgs& args, const std::string& prefix) {
    static std::atomic_uint64_t sequence{0};
    std::ostringstream out;
    out << prefix << "-" << args.peerId << "-to-" << args.targetId
        << "-" << NowEpochMs() << "-" << sequence.fetch_add(1);
    return out.str();
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
    return args.mode == ProbeMode::rtc && args.sendMic;
}

bool ShouldExpectRemoteAudio(const ProbeArgs& args) {
    return args.mode == ProbeMode::rtc && args.expectRemoteAudio;
}

std::string SanitizeFileComponent(std::string value) {
    for (char& ch : value) {
        const bool keep = std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_';
        if (!keep) {
            ch = '_';
        }
    }
    if (value.empty()) {
        return "audio";
    }
    return value;
}

std::string BuildDefaultRecvAudioWavPath(const ProbeArgs& args) {
    const std::string fileName =
        SanitizeFileComponent(args.roomId) + "-" +
        SanitizeFileComponent(args.peerId) + "-from-" +
        SanitizeFileComponent(args.targetId) + "-" +
        std::to_string(NowEpochMs()) + ".wav";
    return (std::filesystem::path(kDefaultRecvAudioWavDir) / fileName).string();
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

std::string CurrentSignalState(ProbeRuntime& runtime) {
    return runtime.signalingSession ? runtime.signalingSession->currentSignalState() : "";
}

std::string CurrentRequestId(ProbeRuntime& runtime) {
    return runtime.signalingSession ? runtime.signalingSession->currentRequestId() : "";
}

void LogProbeEvent(const ProbeArgs& args,
                   const std::string& event,
                   const std::string& detail,
                   const std::string& state,
                   const std::string& requestId,
                   const std::string& remoteUserId) {
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
        << "  --send-mic                   capture microphone and send Opus RTP\n"
        << "  --expect-remote-audio        keep the RTC session alive until the inbound audio probe completes\n"
        << "  --recv-audio-wav <path>      decode inbound Opus and write PCM WAV\n"
        << "  --mic-capture-config <path>  default: " << kDefaultMicCaptureConfigPath << "\n"
        << "  --media-run-ms <value>       send duration after first audio packet, default: " << kDefaultMediaRunMs << "\n"
        << "  --opus-bitrate <value>       default: " << kDefaultOpusBitrate << "\n"
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
        } else if (arg == "--recv-audio-wav") {
            args.recvAudioWavPath = requireValue(arg);
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
    if (args.expectRemoteAudio && args.mode != ProbeMode::rtc) {
        throw std::runtime_error("--expect-remote-audio is only supported in rtc mode");
    }
    if (!args.recvAudioWavPath.empty() && args.mode != ProbeMode::rtc) {
        throw std::runtime_error("--recv-audio-wav is only supported in rtc mode");
    }
    if (!args.recvAudioWavPath.empty()) {
        args.expectRemoteAudio = true;
    }
    if (args.expectRemoteAudio && args.recvAudioWavPath.empty()) {
        args.recvAudioWavPath = BuildDefaultRecvAudioWavPath(args);
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
        LogProbeEvent(runtime.args,
                      "state",
                      message,
                      "failed",
                      CurrentRequestId(runtime),
                      runtime.args.targetId);
    }
    MarkDone(runtime.donePromise, runtime.doneFlag);
}

void CheckRtcMediaTimers(ProbeRuntime& runtime) {
    if (runtime.audioProbe) {
        runtime.audioProbe->checkMediaTimers();
    }
}

void AttachDataChannelHandlers(const std::shared_ptr<rtc::DataChannel>& dc,
                               ProbeRuntime& runtime,
                               const std::string& peerId) {
    if (runtime.audioProbe) {
        runtime.audioProbe->bindDataChannel(dc);
    }

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
            if (runtime.audioProbe) {
                runtime.audioProbe->onDataChannelOpen();
            } else if (!ShouldSendMic(runtime.args) && !ShouldExpectRemoteAudio(runtime.args)) {
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
            const std::string payload = std::get<std::string>(data);
            if (runtime.audioProbe &&
                runtime.audioProbe->handleDataChannelStringMessage(payload, peerId)) {
                return;
            }

            LogProbeEvent(runtime.args,
                          "rtc.data-channel-message",
                          payload,
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
        if (runtime.audioProbe) {
            runtime.audioProbe->onPeerConnectionStateChange(state);
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
        if (runtime.audioProbe) {
            runtime.audioProbe->attachInboundTrackHandlers(track, runtime.args.targetId);
        }
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
    }
    requestId = CurrentRequestId(runtime);
    if (requestId.empty()) {
        requestId = GenerateProbeRequestId(runtime.args, "rtc");
        if (runtime.signalingSession) {
            runtime.signalingSession->adoptActiveRequestId(requestId);
        }
    }

    runtime.pc = CreatePeerConnection(runtime);
    if (runtime.audioProbe) {
        runtime.audioProbe->ensureOutboundReady(runtime.pc);
        runtime.audioProbe->ensureInboundReady(runtime.pc);
    }
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
    if (runtime.signalingSession) {
        runtime.signalingSession->handleRegistrationAndMemberMessage(message);
    }
}

void HandleSignalPhaseMessage(ProbeRuntime& runtime, const json& message) {
    if (runtime.signalingSession) {
        runtime.signalingSession->handleSignalModeMessage(message);
    }
}

void HandleRtcPhaseMessage(ProbeRuntime& runtime, const json& message) {
    const auto type = message.value("type", "");
    const std::string requestId = message.value("request_id", "");

    if (type == "rtc.offer") {
        if (message.value("to_user_id", "") != runtime.args.peerId) {
            return;
        }
        if (message.value("from_user_id", "") != runtime.args.targetId) {
            return;
        }
        if (runtime.signalingSession && !runtime.signalingSession->matchesActiveRequest(requestId)) {
            return;
        }
        if (runtime.signalingSession) {
            runtime.signalingSession->adoptActiveRequestId(requestId);
        }

        if (!runtime.pc) {
            runtime.pc = CreatePeerConnection(runtime);
        }
        if (runtime.audioProbe) {
            runtime.audioProbe->ensureOutboundReady(runtime.pc);
            runtime.audioProbe->ensureInboundReady(runtime.pc);
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
        if (runtime.signalingSession && !runtime.signalingSession->matchesActiveRequest(requestId)) {
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
        if (runtime.signalingSession && !runtime.signalingSession->matchesActiveRequest(requestId)) {
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
    if (runtime.signalingSession &&
        runtime.signalingSession->handleRegistrationAndMemberMessage(message)) {
        return;
    }

    if (runtime.args.mode == ProbeMode::signal) {
        HandleSignalPhaseMessage(runtime, message);
        return;
    }

    HandleRtcPhaseMessage(runtime, message);
}

void CheckSignalTimers(ProbeRuntime& runtime) {
    if (runtime.signalingSession) {
        runtime.signalingSession->checkSignalTimers();
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
    if (!args.recvAudioWavPath.empty()) {
        std::cout << "  recv_audio_wav: " << args.recvAudioWavPath << "\n";
    }
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
    rtc_probe_common::SignalProbeSessionConfig signalingConfig;
    signalingConfig.role = runtime.args.role;
    signalingConfig.peerId = runtime.args.peerId;
    signalingConfig.targetId = runtime.args.targetId;
    signalingConfig.roomId = runtime.args.roomId;
    signalingConfig.signalMode = runtime.args.mode == ProbeMode::signal;
    signalingConfig.isController = runtime.args.role == "controller";
    signalingConfig.startInviteWhenReady =
        runtime.args.mode == ProbeMode::signal && runtime.args.role == "controller" &&
        runtime.args.signalAction == SignalAction::invite;
    signalingConfig.startApplyWhenReady =
        runtime.args.mode == ProbeMode::signal && runtime.args.role != "controller" &&
        runtime.args.signalAction == SignalAction::apply;
    signalingConfig.autoAcceptIncoming =
        runtime.args.mode == ProbeMode::signal && runtime.args.signalAction == SignalAction::accept;
    signalingConfig.autoRejectIncoming =
        runtime.args.mode == ProbeMode::signal && runtime.args.signalAction == SignalAction::reject;
    signalingConfig.requestTimeoutMs = runtime.args.requestTimeoutMs;
    signalingConfig.autoHangupMs = runtime.args.autoHangupMs;
    signalingConfig.initialRequestId = runtime.args.requestId;

    rtc_probe_common::SignalProbeSessionCallbacks signalingCallbacks;
    signalingCallbacks.logEvent =
        [&runtime](const std::string& event,
                   const std::string& detail,
                   const std::string& state,
                   const std::string& requestId,
                   const std::string& remoteUserId) {
            LogProbeEvent(runtime.args, event, detail, state, requestId, remoteUserId);
        };
    signalingCallbacks.sendJson = [&runtime](const json& message) {
        SendJson(runtime.ws, message);
    };
    signalingCallbacks.recordFailure = [&runtime](const std::string& message) {
        RecordFailure(runtime, message);
    };
    signalingCallbacks.markDone = [&runtime]() {
        MarkDone(runtime.donePromise, runtime.doneFlag);
    };
    signalingCallbacks.onRtcOfferRequested = [&runtime]() {
        EnsureOfferStarted(runtime);
    };
    signalingCallbacks.shouldFailWhenTargetLeavesDuringSession = [&runtime]() {
        return !ShouldExpectRemoteAudio(runtime.args) ||
            !runtime.audioProbe ||
            !runtime.audioProbe->hasSatisfiedInboundAudioProbe();
    };
    runtime.signalingSession = std::make_unique<rtc_probe_common::SignalProbeSession>(
        std::move(signalingConfig),
        std::move(signalingCallbacks));
    rtc_probe_common::RtcAudioProbeConfig audioProbeConfig;
    audioProbeConfig.role = runtime.args.role;
    audioProbeConfig.peerId = runtime.args.peerId;
    audioProbeConfig.targetId = runtime.args.targetId;
    audioProbeConfig.roomId = runtime.args.roomId;
    audioProbeConfig.micCaptureConfigPath = runtime.args.micCaptureConfigPath;
    audioProbeConfig.recvAudioWavPath = runtime.args.recvAudioWavPath;
    audioProbeConfig.sendMic = runtime.args.sendMic;
    audioProbeConfig.expectRemoteAudio = runtime.args.expectRemoteAudio;
    audioProbeConfig.mediaRunMs = runtime.args.mediaRunMs;
    audioProbeConfig.opusBitrate = runtime.args.opusBitrate;

    rtc_probe_common::RtcAudioProbeCallbacks audioProbeCallbacks;
    audioProbeCallbacks.currentRequestId = [&runtime]() {
        return CurrentRequestId(runtime);
    };
    audioProbeCallbacks.logEvent =
        [&runtime](const std::string& event, const std::string& detail, const std::string& remoteUserId) {
            LogProbeEvent(runtime.args,
                          event,
                          detail,
                          "",
                          CurrentRequestId(runtime),
                          remoteUserId);
        };
    audioProbeCallbacks.recordFailure = [&runtime](const std::string& message) {
        RecordFailure(runtime, message);
    };
    audioProbeCallbacks.markDone = [&runtime]() {
        MarkDone(runtime.donePromise, runtime.doneFlag);
    };
    audioProbeCallbacks.isDone = [&runtime]() {
        return runtime.doneFlag->load();
    };
    audioProbeCallbacks.isShutdownRequested = [&runtime]() {
        std::lock_guard<std::mutex> lock(runtime.stateMutex);
        return runtime.shutdownRequested;
    };
    runtime.audioProbe =
        std::make_unique<rtc_probe_common::RtcAudioProbe>(std::move(audioProbeConfig), std::move(audioProbeCallbacks));

    std::promise<void> wsReadyPromise;
    auto wsReadyFuture = wsReadyPromise.get_future();
    auto wsReadySettled = std::make_shared<std::atomic_bool>(false);

    runtime.ws->onOpen([&runtime, &wsReadyPromise, wsReadySettled]() {
        try {
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
