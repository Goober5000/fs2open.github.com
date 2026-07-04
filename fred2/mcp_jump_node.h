#ifndef _MCP_JUMP_NODE_H
#define _MCP_JUMP_NODE_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Jump node tools (list/get/create/update/delete/move/swap).
extern const McpToolDef mcp_jump_node_tool_defs[];
extern const size_t mcp_jump_node_tool_def_count;

#endif // _MCP_JUMP_NODE_H
