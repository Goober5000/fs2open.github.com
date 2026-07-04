#ifndef _MCPSERVER_H
#define _MCPSERVER_H

#include "globalincs/pstypes.h"
#include "mcp_json.h"
#include <jansson.h>
#include <atomic>

// Custom window message for marshaling MCP tool calls to the main MFC thread
#define WM_MCP_TOOL_CALL (WM_USER + 100)

enum class McpToolId {
	REBUILD_SEXP_FOREST,
	GET_SEXP_LISTING,
	APP_TOOL,			// legacy marshal id; being replaced by REGISTRY_TOOL
	MISSION_TOOL,		// legacy marshal id; being replaced by REGISTRY_TOOL
	REFERENCE_TOOL,
	REGISTRY_TOOL		// any registry tool with a main_handler (see mcp_tool_registry.h)
};

struct McpToolRequest {
	McpToolId tool;

	// Tool name or entity name, depending on the tool id.  Mission file paths
	// travel via input_json and are normalized by the app-tool handler.
	char filepath[MAX_PATH_LEN];

	// Structured input arguments (incref'd by caller); may be null
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

bool mcp_server_start();
void mcp_server_stop();
bool mcp_server_is_running();

// Flag indicating that FRED2 is fully initialized and MCP tools may run.
// Set by mcp_server_start() when the HTTP server begins listening.
extern std::atomic<bool> mcp_fred_ready;

// Marshal a tool call to the main MFC thread with a configurable timeout.
// Heap-allocates the request internally.  param is copied into req->filepath
// and holds a tool name or entity name, depending on the tool id; input_json,
// if non-null, is incref'd and passed as structured arguments.
// Returns MCP tool result JSON.
json_t *mcp_execute_on_main_thread(McpToolId tool, const char *param, json_t *input_json = nullptr);

// Called by the UI thread handler after filling results. Signals the
// completion event and handles cleanup if the caller already timed out.
void mcp_signal_completion(McpToolRequest *req);

#endif // _MCPSERVER_H
