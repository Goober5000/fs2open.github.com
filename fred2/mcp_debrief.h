#ifndef _MCP_DEBRIEF_H
#define _MCP_DEBRIEF_H

#include <jansson.h>

struct McpToolRequest;

// Register debriefing tool schemas.
// Called from mcp_register_mission_tools.
void mcp_register_debrief_tools(json_t *tools);

// Try to dispatch a debriefing tool call on the main thread.
// Returns true if the tool name matched and was handled; false to let the
// caller fall through to other handlers.  Called from mcp_handle_mission_tool.
bool mcp_handle_debrief_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

#endif // _MCP_DEBRIEF_H
