#ifndef _MCP_TOOL_REGISTRY_H
#define _MCP_TOOL_REGISTRY_H

#include <jansson.h>
#include <cstddef>

struct McpToolRequest;

// ---------------------------------------------------------------------------
// Table-driven MCP tool registry.
//
// Each tool is described by exactly one McpToolDef entry in its area's table.
// Registration (tools/list), routing membership, and dispatch all derive from
// these tables, so a tool cannot be registered without being routable or
// dispatchable (and vice versa).
//
// THREADING CONTRACT: mcp_tool_registry_init() must run on the main thread
// before mg_start() launches any mongoose workers.  The lookup map is never
// mutated afterwards, so mcp_find_tool() is lock-free and safe from any
// thread.
// ---------------------------------------------------------------------------

struct McpToolDef {
	const char *name;

	// Appends this tool's schema to the tools/list array.
	void (*register_fn)(json_t *tools);

	// Exactly one of the two handlers is non-null.
	// worker_handler runs directly on the mongoose worker thread and returns
	// an MCP tool result.  main_handler is marshaled to the main MFC thread
	// via McpToolId::REGISTRY_TOOL.
	json_t *(*worker_handler)(json_t *arguments);
	void (*main_handler)(json_t *input, McpToolRequest *req);

	// True only for tools that must work before mcp_fred_ready
	// (get_timeout / set_timeout).
	bool works_before_ready;
};

struct McpToolArea {
	const char *area_name;		// diagnostics only
	const McpToolDef *tools;
	size_t count;
};

// Build the name->def lookup and validate the tables (duplicate names,
// exactly-one-handler; in debug builds, also that each register_fn registers
// exactly the tool named by its table entry).  Idempotent.
void mcp_tool_registry_init();

// Name lookup; returns nullptr for unknown names.
const McpToolDef *mcp_find_tool(const char *name);

// Append every tool's schema in canonical (client-visible) order.
void mcp_register_all_tools(json_t *tools);

#endif // _MCP_TOOL_REGISTRY_H
