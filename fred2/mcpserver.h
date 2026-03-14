#ifndef _MCPSERVER_H
#define _MCPSERVER_H

#include "globalincs/pstypes.h"
#include <jansson.h>

// Custom window message for marshaling MCP tool calls to the main MFC thread
#define WM_MCP_TOOL_CALL (WM_USER + 100)

enum class McpToolId {
	LOAD_MISSION,
	SAVE_MISSION,
	SAVE_MISSION_JSON
};

struct McpToolRequest {
	McpToolId tool;
	char filepath[MAX_PATH_LEN];

	// Result fields, filled by main thread handler
	bool success;
	char result_message[512];
};

void mcp_server_start();
void mcp_server_stop();
bool mcp_server_is_running();

// Build an MCP tool result with a text content item.
// Shared with mcp_reference_tools.cpp.
json_t *make_tool_result(const char *text, bool is_error = false);

#endif // _MCPSERVER_H
