#ifndef _MCP_SEXP_FOREST_H
#define _MCP_SEXP_FOREST_H

#include "globalincs/pstypes.h"

// ---------------------------------------------------------------------------
// Cached headless sexp_tree representing every live SEXP tree in the mission.
//
// The forest is rebuilt the first time it is needed, and again whenever
// SEXP-editing state becomes dirty.
//
// THREADING CONTRACT: every function here is main-thread-only (enforced by
// assertions).  Worker threads must obtain forest data exclusively through
// the marshaled tool-call path (OnMcpToolCall), which runs the handler on
// the main thread.  Locks could not make these functions worker-safe anyway,
// because the forest reads the unprotected Sexp_nodes[] array and
// main-thread-owned globals (Ships[], Wings[], Messages[], etc.).
// ---------------------------------------------------------------------------

// Mark the entire forest as needing a full rebuild.
void mcp_sexp_forest_mark_dirty();

// Mark specific root nodes dirty (partial rebuild).
// roots must be valid Sexp_nodes[] root indices saved after the most recent save_tree() call.
void mcp_sexp_forest_mark_dirty(const SCP_vector<int> &roots);

// Remove root nodes from the partial-rebuild set.  Call when a node that was
// previously marked dirty as a root is being absorbed into another tree
// (e.g., during attach_sexp_node) so that partial rebuild doesn't re-stamp
// it as a free-standing root.  No-op if root is not currently in the dirty set.
void mcp_sexp_forest_unmark_dirty(const SCP_vector<int> &roots);

// Rebuild the forest from the current Sexp_nodes[] array.
void mcp_sexp_forest_rebuild();

// Release all resources owned by the forest.
// Call after all mongoose threads have stopped (i.e., after mg_stop()).
void mcp_sexp_forest_cleanup();

// Rebuild the forest if dirty, then call get_listing_opf and return the
// results as a JSON array of strings.  This is the preferred entry point
// for tool handlers (the opf helpers read main-thread-owned globals like
// Ships[], Wings[], Messages[], etc.).
struct json_t;
json_t *mcp_sexp_forest_get_listing_on_main_thread(int opf, int parent_node, int arg_index);

#endif // _MCP_SEXP_FOREST_H
