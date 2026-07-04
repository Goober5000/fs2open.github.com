#ifndef _MCP_SHIPS_H
#define _MCP_SHIPS_H

#include <jansson.h>
#include "mcp_tool_registry.h"

#include "mission/mission_flags.h"

struct McpToolRequest;

// Ship tools (CRUD, docking, weapons, subsystems, special explosion/hitpoints).
extern const McpToolDef mcp_ship_tool_defs[];
extern const size_t mcp_ship_tool_def_count;

// MCP-policy filter for Parse_object_flags.  See definition for the exclusion list.
// Shared with mcp_reference_tools.cpp so list_ship_flags returns the same surface
// that resolve_ship_flag_name / build_ship_flags_array accept.
bool mcp_ship_flag_excluded(const Mission::Parse_Object_Flags &candidate);

#endif // _MCP_SHIPS_H
