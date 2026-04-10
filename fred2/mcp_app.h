#ifndef _MCP_APP_H
#define _MCP_APP_H

#include <jansson.h>
#include <windows.h>

struct McpToolRequest;

// Append app-level tool schemas to the tools array used by tools/list.
// Covers: get_server_info, get_ui_status, get_timeout, set_timeout,
//         load_mission, save_mission, new_mission.
void mcp_register_app_tools(json_t *tools);

// Try to dispatch an app tool call from the mongoose thread.
// Handles get_timeout/set_timeout directly; marshals the rest to the main
// thread via McpToolId::APP_TOOL.  Returns nullptr if the tool name is not
// an app tool (so the caller can fall through to other handlers).
json_t *mcp_route_app_tool(const char *tool_name, json_t *params);

// Main-thread handler for APP_TOOL calls.
// Called from OnMcpToolCall when req->tool == McpToolId::APP_TOOL.
void mcp_handle_app_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

// Current timeout (in milliseconds) for main-thread tool marshaling.
// Used by mcpserver.cpp's wait logic.
DWORD mcp_get_tool_timeout_ms();

// Returns false (with error_msg) if the specified modeless editor dialog
// (identified by its editor_key in g_editor_info) is currently visible.
// Used by validate_dialog_for_* helpers in the various tool units.
bool validate_single_dialog(const char *items_to_modify, const char *dialog_key, SCP_string &error_msg);

// Returns false (with error_msg) if any editor dialog is open.
// Used by LOAD_MISSION / SAVE_MISSION / NEW_MISSION to prevent data loss.
bool validate_no_dialogs_open(SCP_string &error_msg);

#endif // _MCP_APP_H
