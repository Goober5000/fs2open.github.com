#ifndef _MCP_FICTION_VIEWER_H
#define _MCP_FICTION_VIEWER_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Fiction viewer stage tools (list/get/create/update/delete/move/swap).
extern const McpToolDef mcp_fiction_viewer_tool_defs[];
extern const size_t mcp_fiction_viewer_tool_def_count;

#endif // _MCP_FICTION_VIEWER_H
