#ifndef _MCP_MISSION_INFO_H
#define _MCP_MISSION_INFO_H

#include <jansson.h>

struct McpToolRequest;

// Register mission-info tool schemas (currently just update_mission_info).
// Called from mcp_register_mission_tools.
void mcp_register_mission_info_tools(json_t *tools);

// Try to dispatch a mission-info tool call on the main thread.
// Returns true if the tool name matched and was handled; false to let the
// caller fall through to other handlers.  Called from mcp_handle_mission_tool.
bool mcp_handle_mission_info_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

// Returns a JSON object with the same shape as get_mission_info.
// Used by both the GET_MISSION_INFO handler in mainfrm.cpp and by
// update_mission_info to confirm changes.  The caller takes ownership.
json_t *build_mission_info_json();

#endif // _MCP_MISSION_INFO_H
