#include "stdafx.h"
#include "mcp_sexp_forest.h"
#include "sexp_tree.h"
#include "parse/sexp.h"

#include <mutex>
#include <atomic>

// ---------------------------------------------------------------------------
// Forest state
// ---------------------------------------------------------------------------

// The single cached headless sexp_tree that holds every live SEXP in the
// mission.  Access is protected by g_sexp_forest_mutex.
static sexp_tree g_sexp_forest(true /* headless */);
static std::mutex g_sexp_forest_mutex;

// Dirty flag.  Initialized true so the forest is built on first use.
static std::atomic<bool> g_sexp_forest_dirty(true);

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void mcp_sexp_forest_mark_dirty()
{
	g_sexp_forest_dirty.store(true);
}

void mcp_sexp_forest_rebuild()
{
	// Collect all Sexp_nodes[] indices that are referenced as .first or .rest
	// by some other live node.  Any live node NOT in this set is a tree root.
	SCP_unordered_set<int> referenced;
	referenced.reserve(static_cast<size_t>(Num_sexp_nodes));

	// Pass 1: gather all child/sibling references
	for (int i = 0; i < Num_sexp_nodes; i++) {
		if (Sexp_nodes[i].type == SEXP_NOT_USED)
			continue;
		if (Sexp_nodes[i].first >= 0)
			referenced.insert(Sexp_nodes[i].first);
		if (Sexp_nodes[i].rest >= 0)
			referenced.insert(Sexp_nodes[i].rest);
	}

	// Rebuild the forest under the mutex so mongoose threads block during the swap.
	std::lock_guard<std::mutex> lock(g_sexp_forest_mutex);

	g_sexp_forest.clear_tree();

	// Pass 2: load each root subtree into the forest
	for (int i = 0; i < Num_sexp_nodes; i++) {
		if (Sexp_nodes[i].type == SEXP_NOT_USED)
			continue;
		if (referenced.count(i) == 0)
			g_sexp_forest.load_branch(i, -1);
	}

	g_sexp_forest_dirty.store(false);
}

void mcp_sexp_forest_cleanup()
{
	std::lock_guard<std::mutex> lock(g_sexp_forest_mutex);
	g_sexp_forest.clear_tree();
}

// ---------------------------------------------------------------------------
// Accessor used by MCP tools (mongoose thread)
// ---------------------------------------------------------------------------
// Defined here as a friend-style accessor so mcp_reference_tools.cpp can
// call get_listing_opf on the forest without exposing the sexp_tree globally.
// Returns a heap-allocated sexp_list_item list; caller must free it.
// Acquires g_sexp_forest_mutex for the duration of the call.
//
// NOTE: if dirty, the caller must have already marshaled a rebuild to the
// main thread before calling this function.

sexp_list_item *mcp_sexp_forest_get_listing(int opf, int parent_node, int arg_index)
{
	std::lock_guard<std::mutex> lock(g_sexp_forest_mutex);
	return g_sexp_forest.get_listing_opf(opf, parent_node, arg_index);
}

bool mcp_sexp_forest_is_dirty()
{
	return g_sexp_forest_dirty.load();
}
