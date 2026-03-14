#ifndef _MCP_REFERENCE_TOOLS_H
#define _MCP_REFERENCE_TOOLS_H

#include <jansson.h>
#include <atomic>

// Flag indicating that game tables have been fully loaded and are safe to query.
// Set by fred_init() after all table parsing is complete.
extern std::atomic<bool> mcp_tables_ready;

// Append reference/discovery tool schemas to the tools array used by tools/list.
void mcp_register_reference_tools(json_t *tools);

// Try to handle a reference tool call.  Returns a json_t* result if the tool
// name matched one of the reference tools, or nullptr if the name is not
// recognized (so the caller can fall through to other handlers).
json_t *mcp_handle_reference_tool(const char *tool_name, json_t *arguments);

#endif // _MCP_REFERENCE_TOOLS_H
