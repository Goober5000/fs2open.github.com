#ifndef _MCPSERVER_H
#define _MCPSERVER_H

#include "globalincs/pstypes.h"
#include <jansson.h>
#include <atomic>

// Custom window message for marshaling MCP tool calls to the main MFC thread
#define WM_MCP_TOOL_CALL (WM_USER + 100)

enum class McpToolId {
	LOAD_MISSION,
	SAVE_MISSION,
	NEW_MISSION,
	LOAD_SHIP_MODEL,
	GET_MISSION_INFO,
	GET_UI_STATUS
};

struct McpToolRequest {
	McpToolId tool;
	char filepath[MAX_PATH_LEN];

	// Result fields, filled by main thread handler
	bool success;
	char result_message[512];

	// Optional structured result (caller must json_decref if non-null)
	json_t *result_json;

	// Async completion support (used by PostMessage + WaitForSingleObject path)
	HANDLE completion_event;
	std::atomic<int> refcount;  // init to 2: one for caller, one for handler
};

void mcp_server_start();
void mcp_server_stop();
bool mcp_server_is_running();

// Flag indicating that FRED2 is fully initialized and MCP tools may run.
// Set by fred_init() after all startup is complete.
extern std::atomic<bool> mcp_fred_ready;

// Build an MCP tool result with a text content item.
// Shared with mcp_reference_tools.cpp.
json_t *make_tool_result(const char *text, bool is_error = false);

// Marshal a tool call to the main MFC thread with a 30-second timeout.
// Heap-allocates the request internally. Returns MCP tool result JSON.
json_t *mcp_execute_on_main_thread(McpToolId tool, const char *param);

// Called by the UI thread handler after filling results. Signals the
// completion event and handles cleanup if the caller already timed out.
void mcp_signal_completion(McpToolRequest *req);

#endif // _MCPSERVER_H
