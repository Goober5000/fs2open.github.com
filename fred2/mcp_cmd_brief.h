#ifndef _MCP_CMD_BRIEF_H
#define _MCP_CMD_BRIEF_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Command briefing stage tools (list/get/create/update/delete/move/swap).
extern const McpToolDef mcp_cmd_brief_tool_defs[];
extern const size_t mcp_cmd_brief_tool_def_count;

#endif // _MCP_CMD_BRIEF_H
