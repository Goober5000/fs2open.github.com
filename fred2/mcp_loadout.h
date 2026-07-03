#ifndef _MCP_LOADOUT_H
#define _MCP_LOADOUT_H

#include <jansson.h>

struct McpToolRequest;

// Register team loadout tool schemas.
// Called from mcp_register_mission_tools.
void mcp_register_loadout_tools(json_t *tools);

// Try to dispatch a team loadout tool call on the main thread.
// Returns true if the tool name matched and was handled; false to let the
// caller fall through to other handlers.  Called from mcp_handle_mission_tool.
bool mcp_handle_loadout_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

// ---------------------------------------------------------------------------
// Loadout variable-reference helpers (used by the SEXP variable tools)
//
// Team_data stores SEXP variable *names* (ship_list_variables, weaponry_pool_
// variable, and the two count-variable arrays), so variable renames must be
// propagated and deletions guarded, mirroring how FRED's variable dialog
// consults sexp_tree::get_loadout_variable_count before allowing changes.
// All three scan every MAX_TVT_TEAMS team (loadout data exists for both teams
// regardless of Num_teams).  Comparisons are exact (strcmp), matching the
// engine's loadout bookkeeping.
// ---------------------------------------------------------------------------

// Count how many loadout locations reference the variable.
int mcp_count_loadout_variable_refs(const char *var_name);

// Rewrite all loadout references from old_name to new_name.
// Returns the number of locations updated.
int mcp_rename_loadout_variable_refs(const char *old_name, const char *new_name);

// Remove all loadout references to the variable: entries that use it as their
// ship/weapon class are deleted outright; count references are converted to
// the entry's cached literal count.  Returns the number of locations cleared.
int mcp_clear_loadout_variable_refs(const char *var_name);

#endif // _MCP_LOADOUT_H
