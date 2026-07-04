#ifndef _MCP_MISSION_INFO_H
#define _MCP_MISSION_INFO_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Mission-info tools (get/update mission info, get/update custom wing names).
extern const McpToolDef mcp_mission_info_tool_defs[];
extern const size_t mcp_mission_info_tool_def_count;

#endif // _MCP_MISSION_INFO_H
