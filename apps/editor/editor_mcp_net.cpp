// Minimal blocking HTTP/1.1 server bound to 127.0.0.1, one connection at a
// time (MCP clients send one request per call). See editor_mcp_net.h for why
// this file must not include raylib headers.

#include "editor_mcp_net.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SockT = SOCKET;
static constexpr SockT kBadSock = INVALID_SOCKET;
static void closeSock(SockT s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
static constexpr SockT kBadSock = -1;
static void closeSock(SockT s) { ::close(s); }
#endif

namespace figoedit {
namespace net {

namespace {

std::atomic<bool> gRunning{false};
SockT gListen = kBadSock;
std::thread gThread;
HttpHandler gHandler;

constexpr size_t kMaxBody = 16 * 1024 * 1024;

const char* statusText(int code) {
    switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

void sendAll(SockT s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        const int n = ::send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

void respond(SockT s, const HttpResponse& r) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + statusText(r.status) +
                      "\r\nContent-Type: " + r.contentType +
                      "\r\nContent-Length: " + std::to_string(r.body.size()) +
                      "\r\nConnection: close\r\n\r\n" + r.body;
    sendAll(s, out);
}

// Case-insensitive header lookup inside the raw header block.
size_t contentLength(const std::string& headers) {
    std::string lower(headers.size(), '\0');
    for (size_t i = 0; i < headers.size(); ++i)
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(headers[i])));
    const size_t pos = lower.find("content-length:");
    if (pos == std::string::npos) return 0;
    return static_cast<size_t>(std::strtoull(headers.c_str() + pos + 15, nullptr, 10));
}

void handleConnection(SockT conn) {
    // A wedged client must not block the accept loop forever.
#ifdef _WIN32
    DWORD timeoutMs = 10000;
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
               sizeof(timeoutMs));
#else
    timeval tv{10, 0};
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    std::string buf;
    size_t headerEnd = std::string::npos;
    char chunk[8192];
    while (headerEnd == std::string::npos) {
        const int n = ::recv(conn, chunk, sizeof(chunk), 0);
        if (n <= 0) return;
        buf.append(chunk, static_cast<size_t>(n));
        headerEnd = buf.find("\r\n\r\n");
        if (buf.size() > kMaxBody) return;
    }
    const std::string head = buf.substr(0, headerEnd);
    const size_t bodyLen = contentLength(head);
    if (bodyLen > kMaxBody) {
        respond(conn, {400, "text/plain", "body too large"});
        return;
    }
    std::string body = buf.substr(headerEnd + 4);
    while (body.size() < bodyLen) {
        const int n = ::recv(conn, chunk, sizeof(chunk), 0);
        if (n <= 0) return;
        body.append(chunk, static_cast<size_t>(n));
    }

    // Request line: METHOD SP PATH SP HTTP/1.1
    std::string method, path;
    {
        const size_t sp1 = head.find(' ');
        const size_t sp2 = sp1 == std::string::npos ? sp1 : head.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) {
            respond(conn, {400, "text/plain", "bad request line"});
            return;
        }
        method = head.substr(0, sp1);
        path = head.substr(sp1 + 1, sp2 - sp1 - 1);
        const size_t q = path.find('?');
        if (q != std::string::npos) path.resize(q);
    }
    respond(conn, gHandler(method, path, body));
}

void serverLoop() {
    while (gRunning) {
        sockaddr_in peer{};
#ifdef _WIN32
        int peerLen = sizeof(peer);
#else
        socklen_t peerLen = sizeof(peer);
#endif
        const SockT conn = ::accept(gListen, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (conn == kBadSock) {
            if (!gRunning) break;  // listen socket closed by serverStop()
            continue;
        }
        handleConnection(conn);
        closeSock(conn);
    }
}

}  // namespace

bool serverStart(int port, HttpHandler handler) {
    if (gRunning) return false;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#else
    // Writing to a socket the client already closed raises SIGPIPE on macOS/BSD
    // (default action: terminate the process). The editor must survive a client
    // that disconnects mid-response, so ignore it process-wide.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    gListen = ::socket(AF_INET, SOCK_STREAM, 0);
    if (gListen == kBadSock) return false;
    int yes = 1;
    setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
               sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // localhost only
    if (::bind(gListen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(gListen, 8) != 0) {
        closeSock(gListen);
        gListen = kBadSock;
        return false;
    }
    gHandler = std::move(handler);
    gRunning = true;
    gThread = std::thread(serverLoop);
    return true;
}

void serverStop() {
    if (!gRunning) return;
    gRunning = false;
    closeSock(gListen);  // unblocks accept()
    gListen = kBadSock;
    if (gThread.joinable()) gThread.join();
    gHandler = nullptr;
}

}  // namespace net
}  // namespace figoedit
