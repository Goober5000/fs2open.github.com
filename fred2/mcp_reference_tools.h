#ifndef _MCP_REFERENCE_TOOLS_H
#define _MCP_REFERENCE_TOOLS_H

#include <jansson.h>

// Append reference/discovery tool schemas to the tools array used by tools/list.
void mcp_register_reference_tools(json_t *tools);

// Warm up lazy-init caches (e.g., scripting API documentation) on a background
// thread so the first tool call doesn't pay the full generation cost.
void mcp_reference_tools_init();

// Try to handle a reference tool call.  Returns a json_t* result if the tool
// name matched one of the reference tools, or nullptr if the name is not
// recognized (so the caller can fall through to other handlers).
json_t *mcp_handle_reference_tool(const char *tool_name, json_t *arguments);

// Release cached model detail results.
// Must be called after all mongoose threads have stopped (i.e., after mg_stop()).
void mcp_reference_tools_cleanup();

#endif // _MCP_REFERENCE_TOOLS_H
