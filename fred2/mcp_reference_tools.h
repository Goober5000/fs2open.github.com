#ifndef _MCP_REFERENCE_TOOLS_H
#define _MCP_REFERENCE_TOOLS_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Reference/discovery tools (static game data, SEXP metadata, scripting docs).
extern const McpToolDef mcp_reference_tool_defs[];
extern const size_t mcp_reference_tool_def_count;

// Warm up lazy-init caches (e.g., scripting API documentation) on a background
// thread so the first tool call doesn't pay the full generation cost.
void mcp_reference_tools_init();

// Internal marshal target for the four tools whose worker handlers post
// REFERENCE_TOOL to the main thread (get_ship_class_model_details,
// list_ship_class_dockpoints, list_missions, get_root_paths); this is not
// registry dispatch.  Called from OnMcpToolCall when
// req->tool == McpToolId::REFERENCE_TOOL.
void mcp_handle_reference_tool_on_main_thread(const char *tool_name, json_t *input_json, McpToolRequest *req);

// Release cached model detail results.
// Must be called after all mongoose threads have stopped (i.e., after mg_stop()).
void mcp_reference_tools_cleanup();

#endif // _MCP_REFERENCE_TOOLS_H
