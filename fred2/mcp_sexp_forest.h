#ifndef _MCP_SEXP_FOREST_H
#define _MCP_SEXP_FOREST_H

#include "globalincs/pstypes.h"

// ---------------------------------------------------------------------------
// Cached headless sexp_tree representing every live SEXP tree in the mission.
//
// The forest is rebuilt on the main thread the first time it is needed, and
// again whenever SEXP-editing state becomes dirty.  A mutex protects the
// forest so that mongoose threads can read it safely while the main thread
// may rebuild it.
// ---------------------------------------------------------------------------

// Mark the entire forest as needing a full rebuild.  Safe to call from any thread.
void mcp_sexp_forest_mark_dirty();

// Mark specific root nodes dirty (partial rebuild).
// roots must be valid Sexp_nodes[] root indices saved after the most recent save_tree() call.
// Safe to call from any thread.
void mcp_sexp_forest_mark_dirty(SCP_vector<int> roots);

// Rebuild the forest from the current Sexp_nodes[] array.
// MUST be called on the main thread only.
void mcp_sexp_forest_rebuild();

// Release all resources owned by the forest.
// MUST be called after all mongoose threads have stopped (i.e., after mg_stop()).
void mcp_sexp_forest_cleanup();

// Returns true if the forest needs a rebuild before use.
bool mcp_sexp_forest_is_dirty();

// Call get_listing_opf on the cached forest under its mutex.
// parent_node is a Sexp_nodes[] index (-1 for no context).
// Returns a heap-allocated sexp_list_item chain; caller must free with ->destroy().
// The caller must ensure the forest is not dirty before calling this
// (i.e., marshal REBUILD_SEXP_FOREST to the main thread if dirty).
class sexp_list_item;
sexp_list_item *mcp_sexp_forest_get_listing(int opf, int parent_node, int arg_index);

#endif // _MCP_SEXP_FOREST_H
