#ifndef _MCP_REINFORCEMENTS_H
#define _MCP_REINFORCEMENTS_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Reinforcement tools (list/get/set).
extern const McpToolDef mcp_reinforcement_tool_defs[];
extern const size_t mcp_reinforcement_tool_def_count;

#endif // _MCP_REINFORCEMENTS_H
