#ifndef _MCP_LOADOUT_H
#define _MCP_LOADOUT_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Team loadout tools (get/update/set-ship/set-weapon).
extern const McpToolDef mcp_loadout_tool_defs[];
extern const size_t mcp_loadout_tool_def_count;

// ---------------------------------------------------------------------------
// Loadout variable-reference helpers (used by the SEXP variable tools)
//
// Team_data stores SEXP variable *names* (loadout_entry::class_variable and
// ::count_variable, in both the ship and weapon pools), so variable renames
// must be propagated and deletions guarded, mirroring how FRED's variable
// dialog consults sexp_tree::get_loadout_variable_count before allowing
// changes.  Comparisons are exact, matching the engine's loadout bookkeeping.
//
// All three scan every MAX_TVT_TEAMS team, deliberately unlike the loadout
// tools, which only address the teams the mission has.  Team_data[1] keeps its
// entries after a team-versus-team mission is switched to another type, so
// ignoring them here would let a variable deletion leave dangling references
// that resurface if the designer switches back.
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
