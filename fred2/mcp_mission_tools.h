#ifndef _MCP_MISSION_TOOLS_H
#define _MCP_MISSION_TOOLS_H

#include <jansson.h>

struct McpToolRequest;

// Append mission CRUD tool schemas to the tools array used by tools/list.
void mcp_register_mission_tools(json_t *tools);

// Try to route a mission tool call.  Returns a json_t* result if the tool
// name matched one of the mission tools (marshaled to main thread), or
// nullptr if the name is not recognized.
json_t *mcp_route_mission_tool(const char *tool_name, json_t *arguments);

// Main-thread handler for MISSION_TOOL calls.
// Called from OnMcpToolCall when req->tool == McpToolId::MISSION_TOOL.
void mcp_handle_mission_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

#endif // _MCP_MISSION_TOOLS_H
