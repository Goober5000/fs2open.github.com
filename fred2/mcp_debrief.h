#ifndef _MCP_DEBRIEF_H
#define _MCP_DEBRIEF_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Debriefing stage tools (list/get/create/update/delete/move/swap).
extern const McpToolDef mcp_debrief_tool_defs[];
extern const size_t mcp_debrief_tool_def_count;

#endif // _MCP_DEBRIEF_H
