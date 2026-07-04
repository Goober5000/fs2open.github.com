#ifndef _MCP_WAYPOINTS_H
#define _MCP_WAYPOINTS_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Waypoint and waypoint-list tools (list/get/create/update/delete/move/swap).
extern const McpToolDef mcp_waypoint_tool_defs[];
extern const size_t mcp_waypoint_tool_def_count;

#endif // _MCP_WAYPOINTS_H
