#ifndef _MCP_SUBMODELS_H
#define _MCP_SUBMODELS_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Submodel-instance tools (list/get/update).
extern const McpToolDef mcp_submodel_tool_defs[];
extern const size_t mcp_submodel_tool_def_count;

#endif // _MCP_SUBMODELS_H
