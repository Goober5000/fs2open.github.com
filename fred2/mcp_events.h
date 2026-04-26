#ifndef _MCP_EVENTS_H
#define _MCP_EVENTS_H

#include <jansson.h>

struct McpToolRequest;

// Register event tool schemas.
// Called from mcp_register_mission_tools.
void mcp_register_event_tools(json_t *tools);

// Try to dispatch an event tool call on the main thread.
// Returns true if the tool name matched and was handled; false to let the
// caller fall through to other handlers.  Called from mcp_handle_mission_tool.
bool mcp_handle_event_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

#endif // _MCP_EVENTS_H
