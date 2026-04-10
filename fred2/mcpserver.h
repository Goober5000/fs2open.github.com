#ifndef _MCPSERVER_H
#define _MCPSERVER_H

#include "globalincs/pstypes.h"
#include "mcp_json.h"
#include <jansson.h>
#include <atomic>

// Custom window message for marshaling MCP tool calls to the main MFC thread
#define WM_MCP_TOOL_CALL (WM_USER + 100)

enum class McpToolId {
	LOAD_MISSION,
	SAVE_MISSION,
	NEW_MISSION,
	LOAD_SHIP_MODEL,
	UNLOAD_SHIP_MODEL,
	REBUILD_SEXP_FOREST,
	GET_SEXP_LISTING,
	GET_SERVER_INFO,
	GET_UI_STATUS,
	MISSION_TOOL
};

struct McpToolRequest {
	McpToolId tool;
	char filepath[MAX_PATH_LEN];

	// Structured input arguments for MISSION_TOOL calls (incref'd by caller)
	json_t *input_json;

	// Result fields, filled by main thread handler
	bool success;
	SCP_string result_message;

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
// Set by mcp_server_start() when the HTTP server begins listening.
extern std::atomic<bool> mcp_fred_ready;

// Marshal a tool call to the main MFC thread with a configurable timeout.
// Heap-allocates the request internally. Returns MCP tool result JSON.
json_t *mcp_execute_on_main_thread(McpToolId tool, const char *param);

// Overload for MISSION_TOOL: passes tool_name in filepath (no path normalization)
// and structured JSON arguments via input_json.
json_t *mcp_execute_on_main_thread(McpToolId tool, const char *tool_name, json_t *input_json);

// Called by the UI thread handler after filling results. Signals the
// completion event and handles cleanup if the caller already timed out.
void mcp_signal_completion(McpToolRequest *req);

#endif // _MCPSERVER_H
