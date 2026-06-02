#ifndef _MCP_SHIPS_H
#define _MCP_SHIPS_H

#include <jansson.h>

#include "mission/mission_flags.h"

struct McpToolRequest;

// Register ship tool schemas.
// Called from mcp_register_mission_tools.
void mcp_register_ship_tools(json_t *tools);

// Try to dispatch a ship tool call on the main thread.
// Returns true if the tool name matched and was handled; false to let the
// caller fall through to other handlers.  Called from mcp_handle_mission_tool.
bool mcp_handle_ship_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

// MCP-policy filter for Parse_object_flags.  See definition for the exclusion list.
// Shared with mcp_reference_tools.cpp so list_ship_flags returns the same surface
// that resolve_ship_flag_name / build_ship_flags_array accept.
bool mcp_ship_flag_excluded(const Mission::Parse_Object_Flags &candidate);

#endif // _MCP_SHIPS_H
