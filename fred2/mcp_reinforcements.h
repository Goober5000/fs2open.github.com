#ifndef _MCP_REINFORCEMENTS_H
#define _MCP_REINFORCEMENTS_H

#include <jansson.h>

struct McpToolRequest;

// Register reinforcement tool schemas.
// Called from mcp_register_mission_tools.
void mcp_register_reinforcement_tools(json_t *tools);

// Try to dispatch a reinforcement tool call on the main thread.
// Returns true if the tool name matched and was handled; false to let the
// caller fall through to other handlers.  Called from mcp_handle_mission_tool.
bool mcp_handle_reinforcement_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

#endif // _MCP_REINFORCEMENTS_H
