#ifndef _MCP_MISSION_TOOLS_H
#define _MCP_MISSION_TOOLS_H

#include <jansson.h>
#include "globalincs/vmallocator.h"

struct McpToolRequest;

// Sets the mission as modified and autosaves it.
void mark_modified(const char *fmt, ...);

// Append mission CRUD tool schemas to the tools array used by tools/list.
void mcp_register_mission_tools(json_t *tools);

// Try to route a mission tool call.  Returns a json_t* result if the tool
// name matched one of the mission tools (marshaled to main thread), or
// nullptr if the name is not recognized.
json_t *mcp_route_mission_tool(const char *tool_name, json_t *arguments);

// Main-thread handler for MISSION_TOOL calls.
// Called from OnMcpToolCall when req->tool == McpToolId::MISSION_TOOL.
void mcp_handle_mission_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

// Returns false (with error_msg) if any editor dialog is open.
// Used by LOAD_MISSION / SAVE_MISSION / NEW_MISSION to prevent data loss.
bool validate_no_dialogs_open(SCP_string &error_msg);

// Returns false (with error_msg) if the specified modeless editor dialog
// (identified by its editor_key in g_editor_info) is currently visible.
// Used by validate_dialog_for_* helpers in the various tool units.
bool validate_single_dialog(const char *items_to_modify, const char *dialog_key, SCP_string &error_msg);

#endif // _MCP_MISSION_TOOLS_H
