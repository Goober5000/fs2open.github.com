#ifndef _MCP_CMD_BRIEF_H
#define _MCP_CMD_BRIEF_H

#include <jansson.h>

struct McpToolRequest;

// Register command briefing tool schemas.
// Called from mcp_register_mission_tools.
void mcp_register_cmd_brief_tools(json_t *tools);

// Try to dispatch a command briefing tool call on the main thread.
// Returns true if the tool name matched and was handled; false to let the
// caller fall through to other handlers.  Called from mcp_handle_mission_tool.
bool mcp_handle_cmd_brief_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

#endif // _MCP_CMD_BRIEF_H
