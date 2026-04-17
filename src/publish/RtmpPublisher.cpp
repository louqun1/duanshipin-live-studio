#include "publish/RtmpPublisher.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace live_studio::publish {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

void cleanupSocketSubsystem();
int closeSocket(SocketHandle socketHandle);

struct RtmpUrlParts {
    std::string host;
    int port = 1935;
    std::string app;
    std::string streamName;
    std::string tcUrl;
};

enum class Amf0Type {
    number,
    boolean,
    string,
    object,
    null_value,
    ecma_array,
};

struct Amf0Value {
    Amf0Type type = Amf0Type::null_value;
    double numberValue = 0.0;
    bool booleanValue = false;
    std::string stringValue;
    std::vector<std::pair<std::string, Amf0Value>> objectProperties;
};

struct ParsedCommand {
    std::string name;
    double transactionId = 0.0;
    std::vector<Amf0Value> values;
};

struct IncomingMessage {
    std::uint32_t chunkStreamId = 0;
    std::uint32_t timestamp = 0;
    std::uint8_t typeId = 0;
    std::uint32_t messageStreamId = 0;
    std::vector<std::uint8_t> payload;
};

struct InboundChunkStreamState {
    bool initialized = false;
    bool inProgress = false;
    bool extendedTimestamp = false;
    std::uint32_t timestamp = 0;
    std::uint32_t timestampDelta = 0;
    std::uint32_t messageLength = 0;
    std::uint8_t typeId = 0;
    std::uint32_t messageStreamId = 0;
    std::vector<std::uint8_t> payload;
    std::size_t bytesRead = 0;
};

}  // namespace

struct PublisherContext {
    SocketHandle socketHandle = kInvalidSocket;
    bool socketSubsystemStarted = false;
    RtmpUrlParts urlParts;
    std::uint32_t streamId = 0;
    std::uint32_t inboundChunkSize = 128;
    std::uint32_t outboundChunkSize = 128;
    bool metadataSent = false;
    std::vector<InboundChunkStreamState> inboundChunkStreams;

    PublisherContext() : inboundChunkStreams(64) {}

    ~PublisherContext() {
        if (socketHandle != kInvalidSocket) {
            closeSocket(socketHandle);
            socketHandle = kInvalidSocket;
        }
        if (socketSubsystemStarted) {
            cleanupSocketSubsystem();
            socketSubsystemStarted = false;
        }
    }
};

namespace {

void appendUint16Be(std::uint16_t value, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>(value & 0xff));
}

void appendUint24Be(std::uint32_t value, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>(value & 0xff));
}

void appendUint32Be(std::uint32_t value, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>(value & 0xff));
}

void appendUint32Le(std::uint32_t value, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(static_cast<std::uint8_t>(value & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    outBytes->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

std::uint16_t readUint16Be(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                      static_cast<std::uint16_t>(data[1]));
}

std::uint32_t readUint24Be(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 16) | (static_cast<std::uint32_t>(data[1]) << 8) |
           static_cast<std::uint32_t>(data[2]);
}

std::uint32_t readUint32Be(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

std::uint32_t readUint32Le(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

void appendAmf0Number(double value, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(0x00);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int shift = 56; shift >= 0; shift -= 8) {
        outBytes->push_back(static_cast<std::uint8_t>((bits >> shift) & 0xff));
    }
}

void appendAmf0Boolean(bool value, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(0x01);
    outBytes->push_back(static_cast<std::uint8_t>(value ? 1 : 0));
}

void appendAmf0String(const std::string& value, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(0x02);
    appendUint16Be(static_cast<std::uint16_t>(value.size()), outBytes);
    outBytes->insert(outBytes->end(), value.begin(), value.end());
}

void appendAmf0Null(std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(0x05);
}

void appendAmf0ObjectPropertyName(const std::string& name, std::vector<std::uint8_t>* outBytes) {
    appendUint16Be(static_cast<std::uint16_t>(name.size()), outBytes);
    outBytes->insert(outBytes->end(), name.begin(), name.end());
}

void appendAmf0ObjectStart(std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(0x03);
}

void appendAmf0ObjectEnd(std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(0x00);
    outBytes->push_back(0x00);
    outBytes->push_back(0x09);
}

void appendAmf0EcmaArrayStart(std::uint32_t propertyCount, std::vector<std::uint8_t>* outBytes) {
    outBytes->push_back(0x08);
    appendUint32Be(propertyCount, outBytes);
}

std::vector<std::string> splitString(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

std::string joinStringParts(const std::vector<std::string>& parts, std::size_t first, std::size_t lastExclusive) {
    std::string joined;
    for (std::size_t i = first; i < lastExclusive; ++i) {
        if (!joined.empty()) {
            joined.push_back('/');
        }
        joined += parts[i];
    }
    return joined;
}

Status parsePositivePort(const std::string& text, int* outPort) {
    if (outPort == nullptr) {
        return Status::InvalidArgument("outPort is null");
    }
    if (text.empty()) {
        return Status::InvalidArgument("rtmp port is empty");
    }

    int parsedValue = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return Status::InvalidArgument("rtmp port must be numeric");
        }
        parsedValue = (parsedValue * 10) + (ch - '0');
        if (parsedValue > 65535) {
            return Status::InvalidArgument("rtmp port exceeds 65535");
        }
    }
    if (parsedValue <= 0) {
        return Status::InvalidArgument("rtmp port must be > 0");
    }

    *outPort = parsedValue;
    return Status::Ok();
}

Status parseRtmpUrl(const std::string& url, RtmpUrlParts* outParts) {
    if (outParts == nullptr) {
        return Status::InvalidArgument("outParts is null");
    }
    if (url.rfind("rtmp://", 0) != 0) {
        return Status::InvalidArgument("rtmp url must start with rtmp://");
    }

    const std::size_t authorityStart = 7;
    const std::size_t pathStart = url.find('/', authorityStart);
    if (pathStart == std::string::npos || pathStart == (url.size() - 1)) {
        return Status::InvalidArgument("rtmp url must include /app/stream");
    }

    const std::string authority = url.substr(authorityStart, pathStart - authorityStart);
    if (authority.empty()) {
        return Status::InvalidArgument("rtmp url host is empty");
    }

    RtmpUrlParts parts;
    parts.port = 1935;

    if (!authority.empty() && authority.front() == '[') {
        const std::size_t closingBracket = authority.find(']');
        if (closingBracket == std::string::npos) {
            return Status::InvalidArgument("rtmp url has an invalid IPv6 host");
        }
        parts.host = authority.substr(1, closingBracket - 1);
        if ((closingBracket + 1) < authority.size()) {
            if (authority[closingBracket + 1] != ':') {
                return Status::InvalidArgument("rtmp url authority is malformed");
            }
            Status portStatus = parsePositivePort(authority.substr(closingBracket + 2), &parts.port);
            if (!portStatus.ok()) {
                return portStatus;
            }
        }
    } else {
        const std::size_t portSeparator = authority.rfind(':');
        if (portSeparator != std::string::npos) {
            parts.host = authority.substr(0, portSeparator);
            Status portStatus = parsePositivePort(authority.substr(portSeparator + 1), &parts.port);
            if (!portStatus.ok()) {
                return portStatus;
            }
        } else {
            parts.host = authority;
        }
    }

    if (parts.host.empty()) {
        return Status::InvalidArgument("rtmp url host is empty");
    }

    const std::string path = url.substr(pathStart + 1);
    const std::vector<std::string> pathParts = splitString(path, '/');
    if (pathParts.size() < 2) {
        return Status::InvalidArgument("rtmp url must include both app and stream name");
    }
    if (pathParts.back().empty()) {
        return Status::InvalidArgument("rtmp url stream name is empty");
    }

    parts.app = joinStringParts(pathParts, 0, pathParts.size() - 1);
    parts.streamName = pathParts.back();

    parts.tcUrl = "rtmp://";
    if (authority.front() == '[') {
        parts.tcUrl += "[" + parts.host + "]";
        if (parts.port != 1935) {
            parts.tcUrl += ":" + std::to_string(parts.port);
        }
    } else {
        parts.tcUrl += parts.host;
        if (parts.port != 1935) {
            parts.tcUrl += ":" + std::to_string(parts.port);
        }
    }
    parts.tcUrl += "/" + parts.app;

    *outParts = std::move(parts);
    return Status::Ok();
}

#if defined(_WIN32)
std::string socketErrorString(int errorCode) {
    char* messageBuffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD size = FormatMessageA(flags,
                                      nullptr,
                                      static_cast<DWORD>(errorCode),
                                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<LPSTR>(&messageBuffer),
                                      0,
                                      nullptr);
    std::string message = size > 0 && messageBuffer != nullptr ? std::string(messageBuffer, size) : "unknown error";
    if (messageBuffer != nullptr) {
        LocalFree(messageBuffer);
    }
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
    }
    return message;
}

Status startupSocketSubsystem(bool* outStarted) {
    if (outStarted == nullptr) {
        return Status::InvalidArgument("outStarted is null");
    }
    WSADATA wsaData{};
    const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        return Status::InternalError("WSAStartup failed: " + std::to_string(result));
    }
    *outStarted = true;
    return Status::Ok();
}

void cleanupSocketSubsystem() {
    WSACleanup();
}

int closeSocket(SocketHandle socketHandle) {
    return closesocket(socketHandle);
}
#else
std::string socketErrorString(int errorCode) {
    return std::string(std::strerror(errorCode));
}

Status startupSocketSubsystem(bool* outStarted) {
    if (outStarted == nullptr) {
        return Status::InvalidArgument("outStarted is null");
    }
    *outStarted = false;
    return Status::Ok();
}

void cleanupSocketSubsystem() {}

int closeSocket(SocketHandle socketHandle) {
    return close(socketHandle);
}
#endif

Status setSocketOptions(SocketHandle socketHandle) {
    const int timeoutMs = 5000;
#if defined(_WIN32)
    const char* timeoutValue = reinterpret_cast<const char*>(&timeoutMs);
    const int timeoutSize = sizeof(timeoutMs);
#else
    const timeval timeoutValue = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    const void* timeoutPtr = &timeoutValue;
    const socklen_t timeoutSize = sizeof(timeoutValue);
#endif

#if defined(_WIN32)
    if (setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, timeoutValue, timeoutSize) != 0) {
        return Status::InternalError("setsockopt(SO_RCVTIMEO) failed: " + socketErrorString(WSAGetLastError()));
    }
    if (setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, timeoutValue, timeoutSize) != 0) {
        return Status::InternalError("setsockopt(SO_SNDTIMEO) failed: " + socketErrorString(WSAGetLastError()));
    }
#else
    if (setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, timeoutPtr, timeoutSize) != 0) {
        return Status::InternalError("setsockopt(SO_RCVTIMEO) failed: " + socketErrorString(errno));
    }
    if (setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, timeoutPtr, timeoutSize) != 0) {
        return Status::InternalError("setsockopt(SO_SNDTIMEO) failed: " + socketErrorString(errno));
    }
#endif

    const int noDelay = 1;
    if (setsockopt(socketHandle,
                   IPPROTO_TCP,
                   TCP_NODELAY,
                   reinterpret_cast<const char*>(&noDelay),
                   static_cast<int>(sizeof(noDelay))) != 0) {
#if defined(_WIN32)
        return Status::InternalError("setsockopt(TCP_NODELAY) failed: " + socketErrorString(WSAGetLastError()));
#else
        return Status::InternalError("setsockopt(TCP_NODELAY) failed: " + socketErrorString(errno));
#endif
    }

    return Status::Ok();
}

Status sendAll(SocketHandle socketHandle, const std::uint8_t* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const int result =
            send(socketHandle,
                 reinterpret_cast<const char*>(data + sent),
                 static_cast<int>(std::min<std::size_t>(size - sent, static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                 0);
        if (result <= 0) {
#if defined(_WIN32)
            return Status::InternalError("socket send failed: " + socketErrorString(WSAGetLastError()));
#else
            return Status::InternalError("socket send failed: " + socketErrorString(errno));
#endif
        }
        sent += static_cast<std::size_t>(result);
    }
    return Status::Ok();
}

Status recvAll(SocketHandle socketHandle, std::uint8_t* outData, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        const int result =
            recv(socketHandle,
                 reinterpret_cast<char*>(outData + received),
                 static_cast<int>(std::min<std::size_t>(size - received, static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                 0);
        if (result <= 0) {
#if defined(_WIN32)
            return Status::InternalError("socket recv failed: " + socketErrorString(WSAGetLastError()));
#else
            return Status::InternalError("socket recv failed: " + socketErrorString(errno));
#endif
        }
        received += static_cast<std::size_t>(result);
    }
    return Status::Ok();
}

Status connectTcpSocket(const RtmpUrlParts& urlParts, SocketHandle* outSocket) {
    if (outSocket == nullptr) {
        return Status::InvalidArgument("outSocket is null");
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addressList = nullptr;
    const std::string portText = std::to_string(urlParts.port);
    const int lookupResult = getaddrinfo(urlParts.host.c_str(), portText.c_str(), &hints, &addressList);
    if (lookupResult != 0) {
#if defined(_WIN32)
        return Status::InternalError("getaddrinfo failed: " + std::to_string(lookupResult));
#else
        return Status::InternalError("getaddrinfo failed: " + std::string(gai_strerror(lookupResult)));
#endif
    }

    SocketHandle connectedSocket = kInvalidSocket;
    for (addrinfo* item = addressList; item != nullptr; item = item->ai_next) {
        SocketHandle candidate = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (candidate == kInvalidSocket) {
            continue;
        }

        Status optionStatus = setSocketOptions(candidate);
        if (!optionStatus.ok()) {
            closeSocket(candidate);
            freeaddrinfo(addressList);
            return optionStatus;
        }

        if (::connect(candidate, item->ai_addr, static_cast<int>(item->ai_addrlen)) == 0) {
            connectedSocket = candidate;
            break;
        }

        closeSocket(candidate);
    }

    freeaddrinfo(addressList);

    if (connectedSocket == kInvalidSocket) {
#if defined(_WIN32)
        return Status::InternalError("tcp connect failed: " + socketErrorString(WSAGetLastError()));
#else
        return Status::InternalError("tcp connect failed: " + socketErrorString(errno));
#endif
    }

    *outSocket = connectedSocket;
    return Status::Ok();
}

Status performHandshake(SocketHandle socketHandle) {
    std::vector<std::uint8_t> c0c1(1537, 0);
    c0c1[0] = 3;

    const std::uint32_t timestampSeconds = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    c0c1[1] = static_cast<std::uint8_t>((timestampSeconds >> 24) & 0xff);
    c0c1[2] = static_cast<std::uint8_t>((timestampSeconds >> 16) & 0xff);
    c0c1[3] = static_cast<std::uint8_t>((timestampSeconds >> 8) & 0xff);
    c0c1[4] = static_cast<std::uint8_t>(timestampSeconds & 0xff);

    std::mt19937 rng(timestampSeconds ^ 0x73524553u);
    for (std::size_t i = 9; i < c0c1.size(); ++i) {
        c0c1[i] = static_cast<std::uint8_t>(rng() & 0xff);
    }

    Status status = sendAll(socketHandle, c0c1.data(), c0c1.size());
    if (!status.ok()) {
        return status;
    }

    std::array<std::uint8_t, 1537> s0s1{};
    status = recvAll(socketHandle, s0s1.data(), s0s1.size());
    if (!status.ok()) {
        return status;
    }
    if (s0s1[0] != 3) {
        return Status::InternalError("rtmp handshake failed: server did not reply with RTMP version 3");
    }

    std::array<std::uint8_t, 1536> s2{};
    status = recvAll(socketHandle, s2.data(), s2.size());
    if (!status.ok()) {
        return status;
    }

    status = sendAll(socketHandle, s0s1.data() + 1, 1536);
    if (!status.ok()) {
        return status;
    }

    return Status::Ok();
}

std::vector<std::uint8_t> buildConnectCommandPayload(const RtmpUrlParts& urlParts) {
    std::vector<std::uint8_t> payload;
    appendAmf0String("connect", &payload);
    appendAmf0Number(1.0, &payload);

    appendAmf0ObjectStart(&payload);
    appendAmf0ObjectPropertyName("app", &payload);
    appendAmf0String(urlParts.app, &payload);
    appendAmf0ObjectPropertyName("type", &payload);
    appendAmf0String("nonprivate", &payload);
    appendAmf0ObjectPropertyName("flashVer", &payload);
    appendAmf0String("FMLE/3.0 (compatible; stage1_video_trace)", &payload);
    appendAmf0ObjectPropertyName("tcUrl", &payload);
    appendAmf0String(urlParts.tcUrl, &payload);
    appendAmf0ObjectPropertyName("fpad", &payload);
    appendAmf0Boolean(false, &payload);
    appendAmf0ObjectPropertyName("capabilities", &payload);
    appendAmf0Number(15.0, &payload);
    appendAmf0ObjectPropertyName("audioCodecs", &payload);
    appendAmf0Number(0.0, &payload);
    appendAmf0ObjectPropertyName("videoCodecs", &payload);
    appendAmf0Number(252.0, &payload);
    appendAmf0ObjectPropertyName("videoFunction", &payload);
    appendAmf0Number(1.0, &payload);
    appendAmf0ObjectPropertyName("objectEncoding", &payload);
    appendAmf0Number(0.0, &payload);
    appendAmf0ObjectEnd(&payload);

    return payload;
}

std::vector<std::uint8_t> buildCreateStreamCommandPayload() {
    std::vector<std::uint8_t> payload;
    appendAmf0String("createStream", &payload);
    appendAmf0Number(2.0, &payload);
    appendAmf0Null(&payload);
    return payload;
}

std::vector<std::uint8_t> buildPublishCommandPayload(const std::string& streamName) {
    std::vector<std::uint8_t> payload;
    appendAmf0String("publish", &payload);
    appendAmf0Number(3.0, &payload);
    appendAmf0Null(&payload);
    appendAmf0String(streamName, &payload);
    appendAmf0String("live", &payload);
    return payload;
}

std::vector<std::uint8_t> buildSetChunkSizePayload(std::uint32_t chunkSize) {
    std::vector<std::uint8_t> payload;
    appendUint32Be(chunkSize, &payload);
    return payload;
}

std::vector<std::uint8_t> buildOnMetaDataPayload(int width, int height, int fps) {
    std::vector<std::uint8_t> payload;
    appendAmf0String("@setDataFrame", &payload);
    appendAmf0String("onMetaData", &payload);

    appendAmf0EcmaArrayStart(6, &payload);
    appendAmf0ObjectPropertyName("duration", &payload);
    appendAmf0Number(0.0, &payload);
    appendAmf0ObjectPropertyName("width", &payload);
    appendAmf0Number(static_cast<double>(width), &payload);
    appendAmf0ObjectPropertyName("height", &payload);
    appendAmf0Number(static_cast<double>(height), &payload);
    appendAmf0ObjectPropertyName("framerate", &payload);
    appendAmf0Number(static_cast<double>(fps), &payload);
    appendAmf0ObjectPropertyName("videocodecid", &payload);
    appendAmf0Number(7.0, &payload);
    appendAmf0ObjectPropertyName("encoder", &payload);
    appendAmf0String("duanshipin-live-studio/stage1_video_trace", &payload);
    appendAmf0ObjectEnd(&payload);

    return payload;
}

Status parseAmf0Value(const std::vector<std::uint8_t>& bytes, std::size_t* inOutCursor, Amf0Value* outValue) {
    if (inOutCursor == nullptr || outValue == nullptr) {
        return Status::InvalidArgument("AMF0 parse outputs are null");
    }
    if (*inOutCursor >= bytes.size()) {
        return Status::InvalidArgument("AMF0 payload ended before value marker");
    }

    const std::uint8_t marker = bytes[*inOutCursor];
    ++(*inOutCursor);

    switch (marker) {
    case 0x00: {
        if ((*inOutCursor + 8) > bytes.size()) {
            return Status::InvalidArgument("AMF0 number is truncated");
        }
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            bits = (bits << 8) | bytes[*inOutCursor + static_cast<std::size_t>(i)];
        }
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        outValue->type = Amf0Type::number;
        outValue->numberValue = value;
        *inOutCursor += 8;
        return Status::Ok();
    }
    case 0x01:
        if (*inOutCursor >= bytes.size()) {
            return Status::InvalidArgument("AMF0 boolean is truncated");
        }
        outValue->type = Amf0Type::boolean;
        outValue->booleanValue = bytes[*inOutCursor] != 0;
        ++(*inOutCursor);
        return Status::Ok();
    case 0x02: {
        if ((*inOutCursor + 2) > bytes.size()) {
            return Status::InvalidArgument("AMF0 string length is truncated");
        }
        const std::size_t length = readUint16Be(bytes.data() + *inOutCursor);
        *inOutCursor += 2;
        if ((*inOutCursor + length) > bytes.size()) {
            return Status::InvalidArgument("AMF0 string payload is truncated");
        }
        outValue->type = Amf0Type::string;
        outValue->stringValue.assign(reinterpret_cast<const char*>(bytes.data() + *inOutCursor), length);
        *inOutCursor += length;
        return Status::Ok();
    }
    case 0x03:
    case 0x08: {
        outValue->type = marker == 0x03 ? Amf0Type::object : Amf0Type::ecma_array;
        if (marker == 0x08) {
            if ((*inOutCursor + 4) > bytes.size()) {
                return Status::InvalidArgument("AMF0 ECMA array count is truncated");
            }
            *inOutCursor += 4;
        }

        while (true) {
            if ((*inOutCursor + 3) > bytes.size()) {
                return Status::InvalidArgument("AMF0 object ended unexpectedly");
            }
            const std::uint16_t nameLength = readUint16Be(bytes.data() + *inOutCursor);
            if (nameLength == 0 && bytes[*inOutCursor + 2] == 0x09) {
                *inOutCursor += 3;
                return Status::Ok();
            }

            *inOutCursor += 2;
            if ((*inOutCursor + nameLength) > bytes.size()) {
                return Status::InvalidArgument("AMF0 object property name is truncated");
            }

            std::string propertyName(reinterpret_cast<const char*>(bytes.data() + *inOutCursor), nameLength);
            *inOutCursor += nameLength;

            Amf0Value propertyValue;
            Status status = parseAmf0Value(bytes, inOutCursor, &propertyValue);
            if (!status.ok()) {
                return status;
            }
            outValue->objectProperties.emplace_back(std::move(propertyName), std::move(propertyValue));
        }
    }
    case 0x05:
        outValue->type = Amf0Type::null_value;
        return Status::Ok();
    default:
        return Status::InvalidArgument("unsupported AMF0 marker: " + std::to_string(marker));
    }
}

Status parseAmf0Sequence(const std::vector<std::uint8_t>& bytes, std::vector<Amf0Value>* outValues) {
    if (outValues == nullptr) {
        return Status::InvalidArgument("outValues is null");
    }
    outValues->clear();

    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        Amf0Value value;
        Status status = parseAmf0Value(bytes, &cursor, &value);
        if (!status.ok()) {
            return status;
        }
        outValues->push_back(std::move(value));
    }
    return Status::Ok();
}

Status parseCommandMessage(const std::vector<std::uint8_t>& payload, ParsedCommand* outCommand) {
    if (outCommand == nullptr) {
        return Status::InvalidArgument("outCommand is null");
    }

    std::vector<Amf0Value> values;
    Status status = parseAmf0Sequence(payload, &values);
    if (!status.ok()) {
        return status;
    }
    if (values.size() < 2 || values[0].type != Amf0Type::string || values[1].type != Amf0Type::number) {
        return Status::InvalidArgument("AMF0 command payload does not start with string + number");
    }

    outCommand->name = values[0].stringValue;
    outCommand->transactionId = values[1].numberValue;
    outCommand->values.assign(values.begin() + 2, values.end());
    return Status::Ok();
}

const Amf0Value* findObjectProperty(const Amf0Value& value, const char* name) {
    for (const auto& entry : value.objectProperties) {
        if (entry.first == name) {
            return &entry.second;
        }
    }
    return nullptr;
}

bool transactionMatches(double actualValue, int expectedValue) {
    return std::fabs(actualValue - static_cast<double>(expectedValue)) < 0.0001;
}

Status encodeChunkBasicHeader(std::uint8_t fmt, std::uint32_t chunkStreamId, std::vector<std::uint8_t>* outBytes) {
    if (outBytes == nullptr) {
        return Status::InvalidArgument("outBytes is null");
    }
    if (fmt > 3) {
        return Status::InvalidArgument("RTMP chunk fmt must be in [0,3]");
    }

    if (chunkStreamId >= 2 && chunkStreamId <= 63) {
        outBytes->push_back(static_cast<std::uint8_t>((fmt << 6) | chunkStreamId));
        return Status::Ok();
    }
    if (chunkStreamId >= 64 && chunkStreamId <= 319) {
        outBytes->push_back(static_cast<std::uint8_t>(fmt << 6));
        outBytes->push_back(static_cast<std::uint8_t>(chunkStreamId - 64));
        return Status::Ok();
    }
    if (chunkStreamId >= 64 && chunkStreamId <= 65599) {
        const std::uint32_t encoded = chunkStreamId - 64;
        outBytes->push_back(static_cast<std::uint8_t>((fmt << 6) | 1));
        outBytes->push_back(static_cast<std::uint8_t>(encoded & 0xff));
        outBytes->push_back(static_cast<std::uint8_t>((encoded >> 8) & 0xff));
        return Status::Ok();
    }
    return Status::InvalidArgument("RTMP chunk stream id exceeds supported range");
}

Status sendRtmpMessage(PublisherContext* context,
                       std::uint32_t chunkStreamId,
                       std::uint32_t timestamp,
                       std::uint8_t typeId,
                       std::uint32_t messageStreamId,
                       const std::vector<std::uint8_t>& payload) {
    if (context == nullptr) {
        return Status::InvalidArgument("publisher context is null");
    }
    if (context->socketHandle == kInvalidSocket) {
        return Status::InvalidArgument("publisher socket is not connected");
    }
    if (payload.empty()) {
        return Status::InvalidArgument("RTMP payload is empty");
    }
    if (payload.size() > 0x00ffffffu) {
        return Status::InvalidArgument("RTMP payload exceeds 24-bit header limit");
    }
    if (context->outboundChunkSize == 0) {
        return Status::InternalError("RTMP outbound chunk size is zero");
    }

    const bool useExtendedTimestamp = timestamp >= 0x00ffffffu;
    const std::uint32_t headerTimestamp = useExtendedTimestamp ? 0x00ffffffu : timestamp;

    std::vector<std::uint8_t> packetBytes;
    packetBytes.reserve(payload.size() + 32);

    Status status = encodeChunkBasicHeader(0, chunkStreamId, &packetBytes);
    if (!status.ok()) {
        return status;
    }
    appendUint24Be(headerTimestamp, &packetBytes);
    appendUint24Be(static_cast<std::uint32_t>(payload.size()), &packetBytes);
    packetBytes.push_back(typeId);
    appendUint32Le(messageStreamId, &packetBytes);
    if (useExtendedTimestamp) {
        appendUint32Be(timestamp, &packetBytes);
    }

    std::size_t payloadOffset = 0;
    const std::size_t firstChunkSize = std::min<std::size_t>(context->outboundChunkSize, payload.size());
    packetBytes.insert(packetBytes.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(firstChunkSize));
    payloadOffset += firstChunkSize;

    while (payloadOffset < payload.size()) {
        status = encodeChunkBasicHeader(3, chunkStreamId, &packetBytes);
        if (!status.ok()) {
            return status;
        }
        if (useExtendedTimestamp) {
            appendUint32Be(timestamp, &packetBytes);
        }

        const std::size_t chunkSize = std::min<std::size_t>(context->outboundChunkSize, payload.size() - payloadOffset);
        packetBytes.insert(packetBytes.end(),
                           payload.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
                           payload.begin() + static_cast<std::ptrdiff_t>(payloadOffset + chunkSize));
        payloadOffset += chunkSize;
    }

    return sendAll(context->socketHandle, packetBytes.data(), packetBytes.size());
}

InboundChunkStreamState& ensureInboundChunkStream(PublisherContext* context, std::uint32_t chunkStreamId) {
    if (chunkStreamId >= context->inboundChunkStreams.size()) {
        context->inboundChunkStreams.resize(static_cast<std::size_t>(chunkStreamId) + 1);
    }
    return context->inboundChunkStreams[chunkStreamId];
}

Status readChunkBasicHeader(PublisherContext* context,
                            std::uint8_t* outFmt,
                            std::uint32_t* outChunkStreamId) {
    if (context == nullptr || outFmt == nullptr || outChunkStreamId == nullptr) {
        return Status::InvalidArgument("chunk basic header outputs are null");
    }

    std::uint8_t firstByte = 0;
    Status status = recvAll(context->socketHandle, &firstByte, 1);
    if (!status.ok()) {
        return status;
    }

    *outFmt = static_cast<std::uint8_t>(firstByte >> 6);
    const std::uint8_t csidMarker = firstByte & 0x3f;
    if (csidMarker >= 2) {
        *outChunkStreamId = csidMarker;
        return Status::Ok();
    }

    if (csidMarker == 0) {
        std::uint8_t extension = 0;
        status = recvAll(context->socketHandle, &extension, 1);
        if (!status.ok()) {
            return status;
        }
        *outChunkStreamId = 64 + extension;
        return Status::Ok();
    }

    std::array<std::uint8_t, 2> extension{};
    status = recvAll(context->socketHandle, extension.data(), extension.size());
    if (!status.ok()) {
        return status;
    }
    *outChunkStreamId = 64 + static_cast<std::uint32_t>(extension[0]) + (static_cast<std::uint32_t>(extension[1]) << 8);
    return Status::Ok();
}

Status readChunkedMessage(PublisherContext* context, IncomingMessage* outMessage) {
    if (context == nullptr || outMessage == nullptr) {
        return Status::InvalidArgument("chunked message outputs are null");
    }

    while (true) {
        std::uint8_t fmt = 0;
        std::uint32_t chunkStreamId = 0;
        Status status = readChunkBasicHeader(context, &fmt, &chunkStreamId);
        if (!status.ok()) {
            return status;
        }

        InboundChunkStreamState& state = ensureInboundChunkStream(context, chunkStreamId);

        std::uint32_t extendedTimestampValue = 0;
        bool consumeExtendedTimestamp = false;

        if (fmt == 0) {
            std::array<std::uint8_t, 11> header{};
            status = recvAll(context->socketHandle, header.data(), header.size());
            if (!status.ok()) {
                return status;
            }

            const std::uint32_t rawTimestamp = readUint24Be(header.data());
            state.messageLength = readUint24Be(header.data() + 3);
            state.typeId = header[6];
            state.messageStreamId = readUint32Le(header.data() + 7);
            state.extendedTimestamp = rawTimestamp == 0x00ffffffu;
            if (state.extendedTimestamp) {
                std::array<std::uint8_t, 4> extendedBytes{};
                status = recvAll(context->socketHandle, extendedBytes.data(), extendedBytes.size());
                if (!status.ok()) {
                    return status;
                }
                state.timestamp = readUint32Be(extendedBytes.data());
            } else {
                state.timestamp = rawTimestamp;
            }
            state.timestampDelta = 0;
            state.payload.clear();
            state.payload.reserve(state.messageLength);
            state.bytesRead = 0;
            state.inProgress = true;
            state.initialized = true;
        } else if (fmt == 1) {
            if (!state.initialized || state.inProgress) {
                return Status::InternalError("received RTMP fmt=1 without a completed previous header");
            }

            std::array<std::uint8_t, 7> header{};
            status = recvAll(context->socketHandle, header.data(), header.size());
            if (!status.ok()) {
                return status;
            }

            const std::uint32_t rawTimestampDelta = readUint24Be(header.data());
            state.messageLength = readUint24Be(header.data() + 3);
            state.typeId = header[6];
            state.extendedTimestamp = rawTimestampDelta == 0x00ffffffu;
            if (state.extendedTimestamp) {
                std::array<std::uint8_t, 4> extendedBytes{};
                status = recvAll(context->socketHandle, extendedBytes.data(), extendedBytes.size());
                if (!status.ok()) {
                    return status;
                }
                state.timestampDelta = readUint32Be(extendedBytes.data());
            } else {
                state.timestampDelta = rawTimestampDelta;
            }
            state.timestamp += state.timestampDelta;
            state.payload.clear();
            state.payload.reserve(state.messageLength);
            state.bytesRead = 0;
            state.inProgress = true;
        } else if (fmt == 2) {
            if (!state.initialized || state.inProgress) {
                return Status::InternalError("received RTMP fmt=2 without a completed previous header");
            }

            std::array<std::uint8_t, 3> header{};
            status = recvAll(context->socketHandle, header.data(), header.size());
            if (!status.ok()) {
                return status;
            }

            const std::uint32_t rawTimestampDelta = readUint24Be(header.data());
            state.extendedTimestamp = rawTimestampDelta == 0x00ffffffu;
            if (state.extendedTimestamp) {
                std::array<std::uint8_t, 4> extendedBytes{};
                status = recvAll(context->socketHandle, extendedBytes.data(), extendedBytes.size());
                if (!status.ok()) {
                    return status;
                }
                state.timestampDelta = readUint32Be(extendedBytes.data());
            } else {
                state.timestampDelta = rawTimestampDelta;
            }
            state.timestamp += state.timestampDelta;
            state.payload.clear();
            state.payload.reserve(state.messageLength);
            state.bytesRead = 0;
            state.inProgress = true;
        } else if (fmt == 3) {
            if (!state.initialized) {
                return Status::InternalError("received RTMP fmt=3 without any previous header");
            }

            if (state.extendedTimestamp) {
                std::array<std::uint8_t, 4> extendedBytes{};
                status = recvAll(context->socketHandle, extendedBytes.data(), extendedBytes.size());
                if (!status.ok()) {
                    return status;
                }
                extendedTimestampValue = readUint32Be(extendedBytes.data());
                consumeExtendedTimestamp = true;
            }

            if (!state.inProgress) {
                state.payload.clear();
                state.payload.reserve(state.messageLength);
                state.bytesRead = 0;
                state.inProgress = true;

                if (state.timestampDelta != 0) {
                    state.timestamp += consumeExtendedTimestamp ? extendedTimestampValue : state.timestampDelta;
                    if (consumeExtendedTimestamp) {
                        state.timestampDelta = extendedTimestampValue;
                    }
                } else if (consumeExtendedTimestamp) {
                    state.timestamp = extendedTimestampValue;
                }
            }
        } else {
            return Status::InternalError("unsupported RTMP chunk header format");
        }

        if (state.messageLength == 0) {
            outMessage->chunkStreamId = chunkStreamId;
            outMessage->timestamp = state.timestamp;
            outMessage->typeId = state.typeId;
            outMessage->messageStreamId = state.messageStreamId;
            outMessage->payload.clear();
            state.inProgress = false;
            return Status::Ok();
        }

        const std::size_t remaining = state.messageLength - state.bytesRead;
        const std::size_t chunkDataSize =
            std::min<std::size_t>(static_cast<std::size_t>(context->inboundChunkSize), remaining);
        std::vector<std::uint8_t> chunkData(chunkDataSize);
        status = recvAll(context->socketHandle, chunkData.data(), chunkData.size());
        if (!status.ok()) {
            return status;
        }
        state.payload.insert(state.payload.end(), chunkData.begin(), chunkData.end());
        state.bytesRead += chunkDataSize;

        if (state.bytesRead == state.messageLength) {
            outMessage->chunkStreamId = chunkStreamId;
            outMessage->timestamp = state.timestamp;
            outMessage->typeId = state.typeId;
            outMessage->messageStreamId = state.messageStreamId;
            outMessage->payload = state.payload;
            state.inProgress = false;
            return Status::Ok();
        }
    }
}

Status processProtocolControlMessage(PublisherContext* context, const IncomingMessage& message) {
    if (context == nullptr) {
        return Status::InvalidArgument("publisher context is null");
    }

    if (message.typeId == 1) {
        if (message.payload.size() != 4) {
            return Status::InvalidArgument("Set Chunk Size payload must be 4 bytes");
        }
        const std::uint32_t chunkSize = readUint32Be(message.payload.data()) & 0x7fffffffu;
        if (chunkSize == 0) {
            return Status::InvalidArgument("server announced an invalid RTMP chunk size");
        }
        context->inboundChunkSize = chunkSize;
    }

    return Status::Ok();
}

Status waitForConnectResult(PublisherContext* context) {
    while (true) {
        IncomingMessage message;
        Status status = readChunkedMessage(context, &message);
        if (!status.ok()) {
            return status;
        }

        status = processProtocolControlMessage(context, message);
        if (!status.ok()) {
            return status;
        }

        if (message.typeId != 20) {
            continue;
        }

        ParsedCommand command;
        status = parseCommandMessage(message.payload, &command);
        if (!status.ok()) {
            return status;
        }

        if (command.name == "_result" && transactionMatches(command.transactionId, 1)) {
            return Status::Ok();
        }
        if (command.name == "_error" && transactionMatches(command.transactionId, 1)) {
            return Status::InternalError("rtmp connect command failed");
        }
    }
}

Status waitForCreateStreamResult(PublisherContext* context, std::uint32_t* outStreamId) {
    if (outStreamId == nullptr) {
        return Status::InvalidArgument("outStreamId is null");
    }

    while (true) {
        IncomingMessage message;
        Status status = readChunkedMessage(context, &message);
        if (!status.ok()) {
            return status;
        }

        status = processProtocolControlMessage(context, message);
        if (!status.ok()) {
            return status;
        }

        if (message.typeId != 20) {
            continue;
        }

        ParsedCommand command;
        status = parseCommandMessage(message.payload, &command);
        if (!status.ok()) {
            return status;
        }

        if (command.name == "_result" && transactionMatches(command.transactionId, 2)) {
            if (command.values.size() < 2 || command.values[1].type != Amf0Type::number) {
                return Status::InternalError("createStream response did not include a numeric stream id");
            }
            const double streamIdValue = command.values[1].numberValue;
            if (streamIdValue <= 0.0 || streamIdValue > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
                return Status::InternalError("createStream returned an invalid stream id");
            }
            *outStreamId = static_cast<std::uint32_t>(streamIdValue);
            return Status::Ok();
        }
        if (command.name == "_error" && transactionMatches(command.transactionId, 2)) {
            return Status::InternalError("rtmp createStream command failed");
        }
    }
}

Status waitForPublishStart(PublisherContext* context) {
    while (true) {
        IncomingMessage message;
        Status status = readChunkedMessage(context, &message);
        if (!status.ok()) {
            return status;
        }

        status = processProtocolControlMessage(context, message);
        if (!status.ok()) {
            return status;
        }

        if (message.typeId != 20) {
            continue;
        }

        ParsedCommand command;
        status = parseCommandMessage(message.payload, &command);
        if (!status.ok()) {
            return status;
        }

        if (command.name == "onStatus") {
            if (command.values.size() < 2) {
                continue;
            }
            const Amf0Value& infoObject = command.values[1];
            if (infoObject.type != Amf0Type::object && infoObject.type != Amf0Type::ecma_array) {
                continue;
            }

            const Amf0Value* codeValue = findObjectProperty(infoObject, "code");
            if (codeValue != nullptr && codeValue->type == Amf0Type::string) {
                if (codeValue->stringValue == "NetStream.Publish.Start") {
                    return Status::Ok();
                }
                return Status::InternalError("rtmp publish was rejected: " + codeValue->stringValue);
            }
        }

        if (command.name == "_error" && transactionMatches(command.transactionId, 3)) {
            return Status::InternalError("rtmp publish command failed");
        }
    }
}

Status sendSetChunkSize(PublisherContext* context, std::uint32_t chunkSize) {
    const std::vector<std::uint8_t> payload = buildSetChunkSizePayload(chunkSize);
    Status status = sendRtmpMessage(context, 2, 0, 1, 0, payload);
    if (!status.ok()) {
        return status;
    }
    context->outboundChunkSize = chunkSize;
    return Status::Ok();
}

Status sendOnMetaData(PublisherContext* context, int width, int height, int fps) {
    if (width <= 0 || height <= 0 || fps <= 0) {
        return Status::InvalidArgument("metadata dimensions and fps must be positive");
    }
    const std::vector<std::uint8_t> payload = buildOnMetaDataPayload(width, height, fps);
    Status status = sendRtmpMessage(context, 5, 0, 18, context->streamId, payload);
    if (!status.ok()) {
        return status;
    }
    context->metadataSent = true;
    return Status::Ok();
}

}  // namespace

RtmpPublisher::RtmpPublisher() = default;

RtmpPublisher::~RtmpPublisher() = default;

Status RtmpPublisher::configureVideoStream(int width, int height, int fps) {
    if (width <= 0 || height <= 0 || fps <= 0) {
        return Status::InvalidArgument("video stream width/height/fps must be positive");
    }

    videoWidth_ = width;
    videoHeight_ = height;
    videoFps_ = fps;
    return Status::Ok();
}

Status RtmpPublisher::connect(const std::string& url) {
    if (url.empty()) {
        return Status::InvalidArgument("rtmp url is empty");
    }
    if (state_ != RtmpConnectionState::idle) {
        return Status::InvalidArgument("publisher must be idle before connect");
    }

    state_ = RtmpConnectionState::connecting;

    RtmpUrlParts urlParts;
    Status status = parseRtmpUrl(url, &urlParts);
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    std::unique_ptr<PublisherContext> nextContext = std::make_unique<PublisherContext>();
    nextContext->urlParts = urlParts;

    status = startupSocketSubsystem(&nextContext->socketSubsystemStarted);
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    status = connectTcpSocket(urlParts, &nextContext->socketHandle);
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    status = performHandshake(nextContext->socketHandle);
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    status = sendRtmpMessage(nextContext.get(), 3, 0, 20, 0, buildConnectCommandPayload(urlParts));
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    status = waitForConnectResult(nextContext.get());
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    status = sendRtmpMessage(nextContext.get(), 3, 0, 20, 0, buildCreateStreamCommandPayload());
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    status = waitForCreateStreamResult(nextContext.get(), &nextContext->streamId);
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    status = sendRtmpMessage(nextContext.get(), 3, 0, 20, nextContext->streamId, buildPublishCommandPayload(urlParts.streamName));
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    status = waitForPublishStart(nextContext.get());
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    status = sendSetChunkSize(nextContext.get(), 4096);
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    if (videoWidth_ > 0 && videoHeight_ > 0 && videoFps_ > 0) {
        status = sendOnMetaData(nextContext.get(), videoWidth_, videoHeight_, videoFps_);
        if (!status.ok()) {
            state_ = RtmpConnectionState::failed;
            return status;
        }
    }

    context_ = std::move(nextContext);
    url_ = url;
    state_ = RtmpConnectionState::publishing;
    return Status::Ok();
}

Status RtmpPublisher::mapVideoTagToMessage(const media::FlvVideoTagPlan& tag, media::RtmpMessagePlan* outMessage) const {
    if (outMessage == nullptr) {
        return Status::InvalidArgument("outMessage is null");
    }
    if (tag.timestampMs < 0) {
        return Status::InvalidArgument("tag timestamp must be >= 0");
    }
    if (tag.payloadBytes.empty()) {
        return Status::InvalidArgument("tag payload bytes are empty");
    }

    outMessage->chunkStreamId = 6;
    outMessage->messageStreamId =
        context_ != nullptr && context_->streamId > 0 ? static_cast<int>(context_->streamId) : 1;
    outMessage->typeId = 9;
    outMessage->frameIndex = tag.frameIndex;
    outMessage->timestampMs = tag.timestampMs;
    outMessage->payloadSummary = (tag.sequenceHeader ? "rtmp-video-sequence-header(" : "rtmp-video-tag(") +
                                 tag.payloadSummary + ")";
    outMessage->payloadBytes = tag.payloadBytes;
    return Status::Ok();
}

Status RtmpPublisher::publishVideoTag(const media::RtmpMessagePlan& message) {
    if (state_ != RtmpConnectionState::publishing) {
        return Status::InvalidArgument("publisher is not in publishing state");
    }
    if (context_ == nullptr || context_->socketHandle == kInvalidSocket || context_->streamId == 0) {
        return Status::InvalidArgument("publisher transport is not connected");
    }
    if (message.payloadBytes.empty()) {
        return Status::InvalidArgument("message payload bytes are empty");
    }
    if (message.timestampMs < 0) {
        return Status::InvalidArgument("message timestamp must be >= 0");
    }

    const std::uint32_t messageStreamId =
        message.messageStreamId > 0 ? static_cast<std::uint32_t>(message.messageStreamId) : context_->streamId;
    Status status = sendRtmpMessage(context_.get(),
                                    static_cast<std::uint32_t>(message.chunkStreamId),
                                    static_cast<std::uint32_t>(message.timestampMs),
                                    static_cast<std::uint8_t>(message.typeId),
                                    messageStreamId,
                                    message.payloadBytes);
    if (!status.ok()) {
        state_ = RtmpConnectionState::failed;
        return status;
    }

    lastPublishedSummary_ = message.payloadSummary;
    return Status::Ok();
}

void RtmpPublisher::disconnect() noexcept {
    context_.reset();
    state_ = RtmpConnectionState::idle;
    url_.clear();
    lastPublishedSummary_.clear();
}

}  // namespace live_studio::publish
