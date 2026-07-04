#ifndef _MCP_EVENTS_H
#define _MCP_EVENTS_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Event tools (list/get/create/update/delete/move/swap).
extern const McpToolDef mcp_event_tool_defs[];
extern const size_t mcp_event_tool_def_count;

#endif // _MCP_EVENTS_H
