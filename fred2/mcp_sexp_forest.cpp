#include "stdafx.h"
#include "mcp_json.h"
#include "mcp_sexp_forest.h"
#include "sexp_tree.h"
#include "parse/sexp.h"

#include <jansson.h>

// ---------------------------------------------------------------------------
// Forest state
// ---------------------------------------------------------------------------
// THREADING CONTRACT: everything in this file is main-thread-only.  Editor
// dialogs mark roots dirty from the main thread, and MCP tool handlers run on
// the main thread via the marshaled tool-call path (OnMcpToolCall), which is
// also the only way worker threads can obtain forest data.  This is enforced
// with assertions rather than locks: a mutex here could not provide real
// thread safety anyway, because load_branch and get_listing_opf read the
// unprotected Sexp_nodes[] array and main-thread-owned globals (Ships[],
// Wings[], Messages[], Mission_events[], etc.).

// The single cached headless sexp_tree that holds every live SEXP in the
// mission.
// NOTE: The reason we need to keep a sexp_tree here is so that we can use it
// query for argument values.  Once get_opf_* functions are separated from
// sexp_tree, this whole cache setup may no longer be needed.
// Constructed on first use (Meyers singleton) rather than at static-init time:
// sexp_tree derives from CTreeCtrl and its constructor logs via mprintf, both
// of which are only safe once MFC and FRED's logging are up.
static sexp_tree &get_sexp_forest()
{
	static sexp_tree forest(true /* headless */);
	return forest;
}

// Full-dirty flag.  Initialized true so the forest is built on first use.
static bool g_sexp_forest_dirty = true;

// Partial-dirty state: set of root Sexp_nodes[] indices to reload.
static SCP_unordered_set<int> g_dirty_roots;

// All public entry points must run on the main thread; see the threading
// contract above.
static void forest_assert_main_thread(const char *func)
{
	Assertion(GetCurrentThreadId() == AfxGetApp()->m_nThreadID,
		"%s must be called on the main thread!  Worker threads must go through the marshaled tool-call path.", func);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void mcp_sexp_forest_mark_dirty()
{
	forest_assert_main_thread("mcp_sexp_forest_mark_dirty");

	g_sexp_forest_dirty = true;
	// full rebuild supersedes any pending partial set
	g_dirty_roots.clear();
}

void mcp_sexp_forest_mark_dirty(const SCP_vector<int> &roots)
{
	forest_assert_main_thread("mcp_sexp_forest_mark_dirty");

	if (g_sexp_forest_dirty)
		return;  // full rebuild already pending; partial set is redundant

	for (int r : roots) {
		// skip out-of-range indexes, special root nodes, and unused nodes
		if ((r < 0) || (r >= Num_sexp_nodes) || (r == Locked_sexp_true) || (r == Locked_sexp_false) || (Sexp_nodes[r].type == SEXP_NOT_USED))
			continue;
		g_dirty_roots.insert(r);
	}
}

void mcp_sexp_forest_unmark_dirty(const SCP_vector<int> &roots)
{
	forest_assert_main_thread("mcp_sexp_forest_unmark_dirty");

	if (g_sexp_forest_dirty)
		return;  // full rebuild already pending; partial set is redundant

	for (int r : roots) {
		// skip out-of-range indexes, special root nodes, and unused nodes
		if ((r < 0) || (r >= Num_sexp_nodes) || (r == Locked_sexp_true) || (r == Locked_sexp_false) || (Sexp_nodes[r].type == SEXP_NOT_USED))
			continue;
		g_dirty_roots.erase(r);
	}
}

void mcp_sexp_forest_rebuild()
{
	forest_assert_main_thread("mcp_sexp_forest_rebuild");

	if (g_sexp_forest_dirty) {
		// --- Full rebuild ---

		g_dirty_roots.clear();

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

		get_sexp_forest().clear_tree();

		// Pass 2: load each root subtree into the forest
		for (int i = 0; i < Num_sexp_nodes; i++) {
			if (Sexp_nodes[i].type == SEXP_NOT_USED)
				continue;
			if (referenced.count(i) == 0)
				get_sexp_forest().load_branch(i, -1);
		}

		g_sexp_forest_dirty = false;

	} else if (!g_dirty_roots.empty()) {
		// --- Partial rebuild ---

		// Snapshot and clear the dirty set first, so any roots marked while we
		// load (however unlikely) are kept for the next rebuild rather than lost.
		SCP_vector<int> roots(g_dirty_roots.begin(), g_dirty_roots.end());
		g_dirty_roots.clear();

		// load_branch in headless mode writes directly to tree_nodes[r] by Sexp_nodes index,
		// overwriting any prior content.  No need to remove previous stale branches.
		for (int r : roots) {
			if (Sexp_nodes[r].type != SEXP_NOT_USED)
				get_sexp_forest().load_branch(r, -1);
		}
	}
}

void mcp_sexp_forest_cleanup()
{
	forest_assert_main_thread("mcp_sexp_forest_cleanup");

	get_sexp_forest().clear_tree();
}

// ---------------------------------------------------------------------------
// Combined rebuild + listing
// ---------------------------------------------------------------------------
// Rebuilds the forest if dirty, then calls get_listing_opf and converts
// the result to a JSON array of strings.

json_t *mcp_sexp_forest_get_listing_on_main_thread(int opf, int parent_node, int arg_index)
{
	forest_assert_main_thread("mcp_sexp_forest_get_listing_on_main_thread");

	// Rebuild if needed — this is a no-op when the forest is already clean.
	mcp_sexp_forest_rebuild();

	sexp_list_item *list = get_sexp_forest().get_listing_opf(opf, parent_node, arg_index);

	json_t *values = json_array();
	for (sexp_list_item *item = list; item != nullptr; item = item->next)
		json_array_append_new(values, json_safe_string(item->text.c_str()));

	if (list)
		list->destroy();

	return values;
}
