#ifndef _MCP_WINGS_H
#define _MCP_WINGS_H

#include <jansson.h>

#include "ship/ship_flags.h"

struct McpToolRequest;

// Register wing tool schemas.
// Called from mcp_register_mission_tools.
void mcp_register_wing_tools(json_t *tools);

// Try to dispatch a wing tool call on the main thread.
// Returns true if the tool name matched and was handled; false to let the
// caller fall through to other handlers.  Called from mcp_handle_mission_tool.
bool mcp_handle_wing_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

// MCP-policy filter for Parse_wing_flags.  See definition for the exclusion list.
// Shared with mcp_reference_tools.cpp so list_wing_flags returns the same surface
// that find_wing_flag_by_name / build_wing_flags_array accept.
bool mcp_wing_flag_excluded(const Ship::Wing_Flags &candidate);

#endif // _MCP_WINGS_H
