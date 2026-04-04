#include "stdafx.h"
#include "mcp_sexp_forest.h"
#include "sexp_tree.h"
#include "parse/sexp.h"

#include <jansson.h>
#include <mutex>
#include <atomic>

// ---------------------------------------------------------------------------
// Forest state
// ---------------------------------------------------------------------------

// The single cached headless sexp_tree that holds every live SEXP in the
// mission.  Access is protected by g_sexp_forest_mutex.
// NOTE: The reason we need to keep a sexp_tree here is so that we can use it
// query for argument values.  Once get_opf_* functions are separated from
// sexp_tree, this whole cache setup may no longer be needed.
static sexp_tree g_sexp_forest(true /* headless */);
static std::mutex g_sexp_forest_mutex;

// Full-dirty flag.  Initialized true so the forest is built on first use.
static std::atomic<bool> g_sexp_forest_dirty(true);

// Partial-dirty state: set of root Sexp_nodes[] indices to reload.
// Protected by g_dirty_roots_mutex; g_dirty_roots_nonempty is an atomic
// summary flag readable from any thread without holding the mutex.
static std::atomic<bool>      g_dirty_roots_nonempty(false);
static std::mutex             g_dirty_roots_mutex;
static SCP_unordered_set<int> g_dirty_roots;

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void mcp_sexp_forest_mark_dirty()
{
	g_sexp_forest_dirty.store(true);
	// full rebuild supersedes any pending partial set
	std::lock_guard<std::mutex> lock(g_dirty_roots_mutex);
	g_dirty_roots.clear();
	g_dirty_roots_nonempty.store(false);
}

void mcp_sexp_forest_mark_dirty(const SCP_vector<int> &roots)
{
	if (g_sexp_forest_dirty.load())
		return;  // full rebuild already pending; partial set is redundant
	std::lock_guard<std::mutex> lock(g_dirty_roots_mutex);
	for (int r : roots) {
		// skip special root nodes
		if (r < 0 || r == Locked_sexp_true || r == Locked_sexp_false)
			continue;
		g_dirty_roots.insert(r);
	}
	g_dirty_roots_nonempty.store(!g_dirty_roots.empty());
}

void mcp_sexp_forest_rebuild()
{
	if (g_sexp_forest_dirty.load()) {
		// --- Full rebuild ---

		// Clear partial state before taking the forest mutex (avoids holding two mutexes).
		// Any mark_dirty(roots) call that races here will see g_sexp_forest_dirty==true and no-op.
		{
			std::lock_guard<std::mutex> dlock(g_dirty_roots_mutex);
			g_dirty_roots.clear();
			g_dirty_roots_nonempty.store(false);
		}

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

	} else if (g_dirty_roots_nonempty.load()) {
		// --- Partial rebuild ---

		SCP_vector<int> roots;
		{
			std::lock_guard<std::mutex> dlock(g_dirty_roots_mutex);
			roots.assign(g_dirty_roots.begin(), g_dirty_roots.end());
			g_dirty_roots.clear();
			g_dirty_roots_nonempty.store(false);
		}

		// load_branch in headless mode writes directly to tree_nodes[r] by Sexp_nodes index,
		// overwriting any prior content.  No need to remove previous stale branches.
		std::lock_guard<std::mutex> lock(g_sexp_forest_mutex);
		for (int r : roots) {
			if (Sexp_nodes[r].type != SEXP_NOT_USED)
				g_sexp_forest.load_branch(r, -1);
		}
	}
}

void mcp_sexp_forest_cleanup()
{
	std::lock_guard<std::mutex> lock(g_sexp_forest_mutex);
	g_sexp_forest.clear_tree();
}

// ---------------------------------------------------------------------------
// Combined rebuild + listing (main thread only)
// ---------------------------------------------------------------------------
// Rebuilds the forest if dirty, then calls get_listing_opf and converts
// the result to a JSON array of strings.  This must run on the main thread
// because the get_listing_opf_* helpers read main-thread-owned globals
// (Ships[], Wings[], Messages[], Mission_events[], etc.).

json_t *mcp_sexp_forest_get_listing_on_main_thread(int opf, int parent_node, int arg_index)
{
	// Rebuild if needed — this is a no-op when the forest is already clean.
	mcp_sexp_forest_rebuild();

	sexp_list_item *list;
	{
		std::lock_guard<std::mutex> lock(g_sexp_forest_mutex);
		list = g_sexp_forest.get_listing_opf(opf, parent_node, arg_index);
	}

	json_t *values = json_array();
	for (sexp_list_item *item = list; item != nullptr; item = item->next)
		json_array_append_new(values, json_string(item->text.c_str()));

	if (list)
		list->destroy();

	return values;
}
