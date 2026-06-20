#pragma once
// figoedit MCP server — lets an AI client (Claude Code, etc.) design directly
// in the running editor.
//
// Transport: MCP "Streamable HTTP" on 127.0.0.1:<port>, endpoint /mcp.
// A background thread owns the socket and answers pure-protocol requests
// (initialize / ping / tools/list) inline; tools/call requests are queued and
// executed on the main thread by mcpPump() once per frame, so document
// mutation needs no locking and reuses the editor's undo machinery (the user
// can Ctrl+Z any AI edit).
//
// Connect from Claude Code:
//   claude mcp add --transport http figoedit http://127.0.0.1:9223/mcp

#include <string>

#include "editor.h"

namespace figoedit {

// Start listening (background thread). Returns false if the port is taken.
bool mcpStart(EditorState& ed, int port);
void mcpStop();

// Execute queued tool calls. Call once per frame from the main loop.
void mcpPump(EditorState& ed);

int mcpPort();  // 0 when not running

// Test hook: dispatch one JSON-RPC message synchronously on the calling
// thread (no sockets). Returns the JSON response ("" for notifications).
std::string mcpHandleMessageForTest(EditorState& ed, const std::string& body);

}  // namespace figoedit
