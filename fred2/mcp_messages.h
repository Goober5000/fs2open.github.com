#ifndef _MCP_MESSAGES_H
#define _MCP_MESSAGES_H

#include <jansson.h>

struct McpToolRequest;

// Register message tool schemas.
// Called from mcp_register_mission_tools.
void mcp_register_message_tools(json_t *tools);

// Try to dispatch a message tool call on the main thread.
// Returns true if the tool name matched and was handled; false to let the
// caller fall through to other handlers.  Called from mcp_handle_mission_tool.
bool mcp_handle_message_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

#endif // _MCP_MESSAGES_H
