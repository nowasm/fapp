#pragma once
// Minimal localhost HTTP/1.1 server for the MCP endpoint. Lives in its own
// translation unit: winsock pulls in windows.h, whose CloseWindow/Rectangle/
// ShowCursor declarations clash with raylib's, so nothing here may include
// editor.h or raylib.h.

#include <functional>
#include <string>

namespace figoedit {
namespace net {

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
};

// Called on the server thread for every request.
using HttpHandler = std::function<HttpResponse(
    const std::string& method, const std::string& path, const std::string& body)>;

// Listen on 127.0.0.1:<port> (background thread). False if the port is taken.
bool serverStart(int port, HttpHandler handler);
void serverStop();  // closes the socket and joins the thread

}  // namespace net
}  // namespace figoedit
