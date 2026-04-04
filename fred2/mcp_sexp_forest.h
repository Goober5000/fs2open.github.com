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
void mcp_sexp_forest_mark_dirty(const SCP_vector<int> &roots);

// Rebuild the forest from the current Sexp_nodes[] array.
// MUST be called on the main thread only.
void mcp_sexp_forest_rebuild();

// Release all resources owned by the forest.
// MUST be called after all mongoose threads have stopped (i.e., after mg_stop()).
void mcp_sexp_forest_cleanup();

// Rebuild the forest if dirty, then call get_listing_opf and return the
// results as a JSON array of strings.  MUST be called on the main thread.
// This is the preferred entry point — it avoids the thread-safety issues
// of calling get_listing_opf on a worker thread (the opf helpers read
// main-thread-owned globals like Ships[], Wings[], Messages[], etc.).
struct json_t;
json_t *mcp_sexp_forest_get_listing_on_main_thread(int opf, int parent_node, int arg_index);

#endif // _MCP_SEXP_FOREST_H
