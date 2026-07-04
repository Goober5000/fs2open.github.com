#ifndef _MCP_WINGS_H
#define _MCP_WINGS_H

#include <jansson.h>
#include "mcp_tool_registry.h"

#include "ship/ship_flags.h"

struct McpToolRequest;

// Wing tools (list/get/form/update/arrange/delete/disband/move/swap).
extern const McpToolDef mcp_wing_tool_defs[];
extern const size_t mcp_wing_tool_def_count;

// MCP-policy filter for Parse_wing_flags.  See definition for the exclusion list.
// Shared with mcp_reference_tools.cpp so list_wing_flags returns the same surface
// that find_wing_flag_by_name / build_wing_flags_array accept.
bool mcp_wing_flag_excluded(const Ship::Wing_Flags &candidate);

#endif // _MCP_WINGS_H
