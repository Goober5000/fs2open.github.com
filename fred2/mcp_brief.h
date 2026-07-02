#ifndef _MCP_BRIEF_H
#define _MCP_BRIEF_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Briefing stage tools (list/get/create/update/delete/move/swap).
extern const McpToolDef mcp_brief_tool_defs[];
extern const size_t mcp_brief_tool_def_count;

#endif // _MCP_BRIEF_H
