#include "stdafx.h"
#include "mcp_waypoints.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "mcp_array_utils.h"
#include "mcp_sexp_forest.h"

#include <jansson.h>
#include <algorithm>
#include <cstring>

#include "globalincs/utility.h"

#include "ai/aigoals.h"
#include "object/object.h"
#include "object/waypoint.h"
#include "parse/parselo.h"
#include "parse/sexp.h"

#include "fred.h"
#include "waypointpathdlg.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_waypoint_lists(SCP_string &error_msg)
{
	return validate_single_dialog("waypoint lists", "waypoint", error_msg);
}

// ---------------------------------------------------------------------------
// Waypoint list tools
// ---------------------------------------------------------------------------

// After any operation that inserts into or removes from a waypoints vector,
// cur_waypoint (a raw pointer into the vector) may be invalidated by reallocation.
// Re-derive it from cur_object_index.
static void refresh_cur_waypoint()
{
	if (cur_waypoint != nullptr && query_valid_object(cur_object_index)
		&& Objects[cur_object_index].type == OBJ_WAYPOINT)
	{
		cur_waypoint = find_waypoint_with_instance(Objects[cur_object_index].instance);
		cur_waypoint_list = cur_waypoint ? cur_waypoint->get_parent_list() : nullptr;
	}
}

static void reindex_waypoint_instances()
{
	for (int li = 0; li < (int)Waypoint_lists.size(); li++) {
		auto &wpts = Waypoint_lists[li].get_waypoints();
		for (int wi = 0; wi < (int)wpts.size(); wi++) {
			Objects[wpts[wi].get_objnum()].instance = calc_waypoint_instance(li, wi);
		}
	}
}

static json_t *build_waypoint_list_json(const waypoint_list &wl, int index, bool include_points = false)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_safe_string(wl.get_name()));
	json_object_set_new(obj, "index", json_integer(index + 1));

	if (include_points) {
		json_t *arr = json_array();
		for (const auto &wpt : wl.get_waypoints())
			json_array_append_new(arr, build_vec3d_json(*wpt.get_pos()));
		json_object_set_new(obj, "points", arr);
	} else {
		json_object_set_new(obj, "waypoint_count", json_integer((int)wl.get_waypoints().size()));
	}

	return obj;
}

static json_t *build_waypoint_json(const char *list_name, int one_based_index, const vec3d &pos)
{
	char wpt_name[NAME_LENGTH];
	waypoint_stuff_name(wpt_name, list_name, one_based_index);

	json_t *obj = json_object();
	json_object_set_new(obj, "list", json_safe_string(list_name));
	json_object_set_new(obj, "index", json_integer(one_based_index));
	json_object_set_new(obj, "name", json_safe_string(wpt_name));
	json_object_set_new(obj, "position", build_vec3d_json(pos));
	return obj;
}

static void handle_list_waypoint_lists(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_waypoint_lists, sink)) return;

	json_t *arr = json_array();
	for (int i = 0; i < (int)Waypoint_lists.size(); i++)
		json_array_append_new(arr, build_waypoint_list_json(Waypoint_lists[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_waypoint_list(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_waypoint_lists, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int index = find_matching_waypoint_list_index(name);
	if (index >= 0) {
		req->result_json = make_json_tool_result(build_waypoint_list_json(Waypoint_lists[index], index, true));
		req->success = true;
		return;
	}

	set_not_found_error(sink,"Waypoint list", name);
}

// Helper to rename SEXP and AI goal references for an individual waypoint.
// old_list_name and new_list_name can be the same (for reordering within one list)
// or different (for list rename).
static void rename_waypoint_sexp_refs(const char *old_list_name, int old_1based,
	const char *new_list_name, int new_1based)
{
	char old_name[NAME_LENGTH];
	char new_name[NAME_LENGTH];
	waypoint_stuff_name(old_name, old_list_name, old_1based);
	waypoint_stuff_name(new_name, new_list_name, new_1based);
	update_sexp_references(old_name, new_name);
	ai_update_goal_references(sexp_ref_type::WAYPOINT, old_name, new_name);
	mcp_sexp_forest_mark_dirty();
}

// Convenience overload for renaming within the same list.
static void rename_waypoint_sexp_refs(const char *list_name, int old_1based, int new_1based)
{
	rename_waypoint_sexp_refs(list_name, old_1based, list_name, new_1based);
}

// Helper to invalidate SEXP and AI goal references for an individual waypoint
// by wrapping the name in angle brackets (e.g. "Path:1" -> "<Path:1>").
static void invalidate_waypoint_sexp_refs(const char *list_name, int one_based)
{
	char wpt_name[NAME_LENGTH];
	char buf[NAME_LENGTH + 4];
	waypoint_stuff_name(wpt_name, list_name, one_based);
	snprintf(buf, sizeof(buf), "<%s>", wpt_name);
	update_sexp_references(wpt_name, buf);
	ai_update_goal_references(sexp_ref_type::WAYPOINT, wpt_name, buf);
	mcp_sexp_forest_mark_dirty();
}

static void rename_waypoint_sexp_refs_to_temp(const char *list_name, int one_based, char *temp_buf, size_t buf_size)
{
	char name[NAME_LENGTH];
	waypoint_stuff_name(name, list_name, one_based);
	snprintf(temp_buf, buf_size, "<temp_wpt_%d>", one_based);
	update_sexp_references(name, temp_buf);
	ai_update_goal_references(sexp_ref_type::WAYPOINT, name, temp_buf);
	mcp_sexp_forest_mark_dirty();
}

static void rename_waypoint_sexp_refs_from_temp(const char *temp_name, const char *list_name, int new_1based)
{
	char new_name[NAME_LENGTH];
	waypoint_stuff_name(new_name, list_name, new_1based);
	update_sexp_references(temp_name, new_name);
	ai_update_goal_references(sexp_ref_type::WAYPOINT, temp_name, new_name);
	mcp_sexp_forest_mark_dirty();
}

static void handle_create_waypoint_list(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_waypoint_lists, sink)) return;

	auto name = get_required_string(input, "name", sink, true, NAME_LENGTH - 1);
	if (!name) return;

	auto points_opt = get_required_vec3d_array(input, "points", sink, 1);
	if (!points_opt.has_value()) return;
	auto &points = *points_opt;

	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	// Validate name against all object types
	if (!check_object_rename("waypoint path", name, sink)) return;

	// Validate insert index
	int target_index;
	if (!insert_index.has_value()) {
		target_index = (int)Waypoint_lists.size();
	} else {
		if (!check_int_range(*insert_index, 1, (int)Waypoint_lists.size() + 1, "index", sink))
			return;
		target_index = *insert_index - 1;
	}

	// Create the waypoint list (appends to end but does NOT create game objects)
	waypoint_add_list(name, points);
	int list_index = (int)Waypoint_lists.size() - 1;

	// Create game objects for each waypoint in the new list
	{
		int wi = 0;
		for (auto &wpt : Waypoint_lists[list_index].get_waypoints()) {
			waypoint_create_game_object(&wpt, list_index, wi);
			wi++;
		}
	}
	obj_merge_created_list();

	// Move to requested position if not at end
	if (target_index != list_index) {
		array_move_element(Waypoint_lists, list_index, target_index);
		reindex_waypoint_instances();
	}

	refresh_cur_waypoint();
	mark_modified("MCP: create waypoint list %s", name);

	req->result_json = make_json_tool_result(build_waypoint_list_json(Waypoint_lists[target_index], target_index, true));
	req->success = true;
}

static void handle_update_waypoint_list(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_waypoint_lists, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	auto new_name = get_optional_string(input, "new_name", sink, NAME_LENGTH - 1);
	if (sink.has_error()) return;

	// Find the waypoint list
	int index = find_matching_waypoint_list_index(name);
	if (index < 0) {
		set_not_found_error(sink,"Waypoint list", name);
		return;
	}
	auto &wl = Waypoint_lists[index];

	bool changed = false;

	// Rename (with SEXP and AI goal reference updates)
	if (new_name && stricmp(wl.get_name(), new_name) != 0) {
		if (!check_object_rename("waypoint path", new_name, sink, -1, -1, index)) return;

		const char *old_name = wl.get_name();

		// Update SEXP references for the list name
		update_sexp_references(old_name, new_name);
		ai_update_goal_references(sexp_ref_type::WAYPOINT_PATH, old_name, new_name);
		mcp_sexp_forest_mark_dirty();

		// Update SEXP references for each individual waypoint name
		for (int wpt_idx = 0; wpt_idx < (int)wl.get_waypoints().size(); wpt_idx++)
			rename_waypoint_sexp_refs(old_name, wpt_idx + 1, new_name, wpt_idx + 1);

		wl.set_name(new_name);
		changed = true;
	}

	if (changed) {
		Waypoint_editor_dialog.initialize_data(1);
		mark_modified("MCP: update waypoint list %s", wl.get_name());
	}

	req->result_json = make_json_tool_result(build_waypoint_list_json(wl, index, true));
	req->success = true;
}

static void handle_delete_waypoint_list(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_waypoint_lists, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	// Find the waypoint list
	int index = find_matching_waypoint_list_index(name);
	if (index < 0) {
		set_not_found_error(sink,"Waypoint list", name);
		return;
	}

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	// Check for SEXP references unless force is set
	if (!force.has_value() || !*force) {
		// Check the list name
		int node;
		auto ref = query_referenced_in_sexp(sexp_ref_type::WAYPOINT_PATH, name, node);
		if (ref.second != sexp_src::NONE) {
			SCP_string desc = sexp_src_to_description(ref.first, ref.second);
			sink.set_error("Waypoint list '%s' is referenced in %s. Use force=true to delete anyway "
				"(references will be invalidated).", name, desc.c_str());
			return;
		}

		// Check individual waypoint names
		auto &wpts = Waypoint_lists[index].get_waypoints();
		for (int wpt_idx = 0; wpt_idx < (int)wpts.size(); wpt_idx++) {
			char wpt_name[NAME_LENGTH];
			waypoint_stuff_name(wpt_name, name, wpt_idx + 1);
			ref = query_referenced_in_sexp(sexp_ref_type::WAYPOINT, wpt_name, node);
			if (ref.second != sexp_src::NONE) {
				SCP_string desc = sexp_src_to_description(ref.first, ref.second);
				sink.set_error("Waypoint '%s' is referenced in %s. Use force=true to delete anyway "
					"(references will be invalidated).", wpt_name, desc.c_str());
				return;
			}
		}
	}

	// Invalidate SEXP and AI goal references for the list name
	char buf[NAME_LENGTH + 4];
	snprintf(buf, sizeof(buf), "<%s>", name);
	update_sexp_references(name, buf);
	ai_update_goal_references(sexp_ref_type::WAYPOINT_PATH, name, buf);
	mcp_sexp_forest_mark_dirty();

	// Invalidate references for each individual waypoint name
	for (int wpt_idx = 0; wpt_idx < (int)Waypoint_lists[index].get_waypoints().size(); wpt_idx++)
		invalidate_waypoint_sexp_refs(name, wpt_idx + 1);

	// Remove all waypoints (waypoint_remove skips obj_delete in FRED, so we must do it).
	// When the last waypoint is removed, waypoint_remove also erases the list from
	// Waypoint_lists, so we must not hold a reference across that call.
	while (index < (int)Waypoint_lists.size() && !stricmp(Waypoint_lists[index].get_name(), name)) {
		auto &wpts = Waypoint_lists[index].get_waypoints();
		if (wpts.empty())
			break;
		int objnum = wpts.back().get_objnum();
		unmark_object(objnum);
		waypoint_remove(&wpts.back());
		obj_delete(objnum);
	}

	refresh_cur_waypoint();
	mark_modified("MCP: delete waypoint list %s", name);

	sprintf(req->result_message,
		"Deleted waypoint list: %s", name);
	req->success = true;
}

// Move/swap config for waypoint lists
static MoveSwapConfig make_waypoint_list_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "waypoint list";
	cfg.count = (int)Waypoint_lists.size();
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_waypoint_lists;
	cfg.get_name = [](int i) {
		return Waypoint_lists[i - 1].get_name();
	};
	cfg.do_move = [](int from, int to) {
		array_move_element(Waypoint_lists, from - 1, to - 1);
		reindex_waypoint_instances();
	};
	cfg.do_swap = [](int a, int b) {
		std::swap(Waypoint_lists[a - 1], Waypoint_lists[b - 1]);
		reindex_waypoint_instances();
	};
	return cfg;
}

static void handle_move_waypoint_list(json_t *input, McpToolRequest *req)
{
	auto cfg = make_waypoint_list_move_swap_config();
	handle_generic_move(input, req, cfg);
}

static void handle_swap_waypoint_lists(json_t *input, McpToolRequest *req)
{
	auto cfg = make_waypoint_list_move_swap_config();
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Individual waypoint tools
// ---------------------------------------------------------------------------

static void do_waypoint_move(int li, int from, int to);

static void handle_create_waypoint(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_waypoint_lists, sink)) return;

	auto list = get_required_string(input, "list", sink, true);
	if (!list) return;

	auto pos = get_required_vec3d(input, "position", sink);
	if (!pos.has_value()) return;

	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	// Find the waypoint list
	int li = find_matching_waypoint_list_index(list);
	if (li < 0) {
		set_not_found_error(sink,"Waypoint list", list);
		return;
	}

	int wpt_count = (int)Waypoint_lists[li].get_waypoints().size();

	// Validate insert index (1-based; default appends to end)
	int target_index;
	if (!insert_index.has_value()) {
		target_index = wpt_count;
	} else {
		if (!check_int_range(*insert_index, 1, wpt_count + 1, "index", sink))
			return;
		target_index = *insert_index - 1;
	}

	// Always append to end so the new game object has the highest object number,
	// preserving object creation order in the editor's listing.
	vec3d position = *pos;
	int objnum;
	if (wpt_count == 0) {
		objnum = waypoint_add(&position, calc_waypoint_instance(li, 0), true);
	} else {
		objnum = waypoint_add(&position, calc_waypoint_instance(li, wpt_count - 1));
	}

	if (objnum < 0) {
		sink.set_error("Failed to create waypoint in list '%s'", list);
		return;
	}

	obj_merge_created_list();

	// If the target is not the end, shift positions so the new waypoint
	// appears at the desired index.  do_waypoint_move handles SEXP ref updates.
	if (target_index < wpt_count)
		do_waypoint_move(li, wpt_count, target_index);

	refresh_cur_waypoint();

	int one_based = target_index + 1;
	mark_modified("MCP: create waypoint %s:%d", Waypoint_lists[li].get_name(), one_based);

	req->result_json = make_json_tool_result(build_waypoint_json(Waypoint_lists[li].get_name(), one_based, position));
	req->success = true;
}

static void handle_update_waypoint(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_waypoint_lists, sink)) return;

	auto list = get_required_string(input, "list", sink, true);
	if (!list) return;

	auto wpt_index = get_required_integer(input, "index", sink);
	if (!wpt_index.has_value()) return;

	auto new_pos = get_optional_vec3d(input, "position", sink);
	if (sink.has_error()) return;

	// Find the waypoint list
	int li = find_matching_waypoint_list_index(list);
	if (li < 0) {
		set_not_found_error(sink,"Waypoint list", list);
		return;
	}

	auto &wpts = Waypoint_lists[li].get_waypoints();
	if (!check_int_range(*wpt_index, 1, (int)wpts.size(), "index", sink))
		return;

	auto &wpt = wpts[*wpt_index - 1];
	bool changed = false;

	// Update position
	if (new_pos.has_value()) {
		const vec3d *cur = wpt.get_pos();
		if (!fl_equal(cur->xyz.x, new_pos->xyz.x) || !fl_equal(cur->xyz.y, new_pos->xyz.y) || !fl_equal(cur->xyz.z, new_pos->xyz.z)) {
			vec3d position = *new_pos;
			wpt.set_pos(&position);
			changed = true;
		}
	}

	if (changed)
		mark_modified("MCP: update waypoint %s:%d", Waypoint_lists[li].get_name(), *wpt_index);

	req->result_json = make_json_tool_result(build_waypoint_json(Waypoint_lists[li].get_name(), *wpt_index, *wpt.get_pos()));
	req->success = true;
}

static void handle_delete_waypoint(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_waypoint_lists, sink)) return;

	auto list = get_required_string(input, "list", sink, true);
	if (!list) return;

	auto wpt_index = get_required_integer(input, "index", sink);
	if (!wpt_index.has_value()) return;

	// Find the waypoint list
	int li = find_matching_waypoint_list_index(list);
	if (li < 0) {
		set_not_found_error(sink,"Waypoint list", list);
		return;
	}

	auto &wpts = Waypoint_lists[li].get_waypoints();
	if (!check_int_range(*wpt_index, 1, (int)wpts.size(), "index", sink))
		return;

	int count = (int)wpts.size();
	int deleted_index = *wpt_index - 1;

	// Construct waypoint name for reference checking
	char wpt_name[NAME_LENGTH];
	waypoint_stuff_name(wpt_name, list, *wpt_index);

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	// Check for SEXP references unless force is set
	if (!force.has_value() || !*force) {
		int node;

		// If this is the last waypoint, check for list references too
		if (count == 1) {
			auto ref = query_referenced_in_sexp(sexp_ref_type::WAYPOINT_PATH, list, node);
			if (ref.second != sexp_src::NONE) {
				SCP_string desc = sexp_src_to_description(ref.first, ref.second);
				sink.set_error("Waypoint list '%s' is referenced in %s (this is the last waypoint, so "
					"deleting it would remove the list). Use force=true to delete anyway "
					"(references will be invalidated).", list, desc.c_str());
				return;
			}
		}

		// Check the individual waypoint
		auto ref = query_referenced_in_sexp(sexp_ref_type::WAYPOINT, wpt_name, node);
		if (ref.second != sexp_src::NONE) {
			SCP_string desc = sexp_src_to_description(ref.first, ref.second);
			sink.set_error("Waypoint '%s' is referenced in %s. Use force=true to delete anyway "
				"(references will be invalidated).", wpt_name, desc.c_str());
			return;
		}
	}

	// Invalidate SEXP and AI goal references for this waypoint
	invalidate_waypoint_sexp_refs(list, deleted_index + 1);

	// If last waypoint, also invalidate list references
	if (count == 1) {
		char buf[NAME_LENGTH + 4];
		snprintf(buf, sizeof(buf), "<%s>", list);
		update_sexp_references(list, buf);
		ai_update_goal_references(sexp_ref_type::WAYPOINT_PATH, list, buf);
		mcp_sexp_forest_mark_dirty();
	}

	// Save info before removal (waypoint_remove may erase the list)
	char list_name[NAME_LENGTH];
	strcpy_s(list_name, Waypoint_lists[li].get_name());
	int objnum = wpts[deleted_index].get_objnum();

	// Remove the waypoint from data structures (waypoint_remove skips obj_delete in FRED)
	unmark_object(objnum);
	waypoint_remove(&wpts[deleted_index]);
	obj_delete(objnum);
	refresh_cur_waypoint();

	// Update SEXP and AI goal references for waypoints that shifted down
	for (int i = deleted_index; i < count - 1; i++)
		rename_waypoint_sexp_refs(list_name, i + 2, i + 1);
	mark_modified("MCP: delete waypoint %s", wpt_name);

	sprintf(req->result_message,
		"Deleted waypoint: %s", wpt_name);
	req->success = true;
}

// Shift waypoint positions within a list (positions move, objects stay in place).
// Uses 0-based indices internally.
static void do_waypoint_move(int li, int from, int to)
{
	auto &wl = Waypoint_lists[li];
	auto list_name = wl.get_name();
	auto &wpts = wl.get_waypoints();

	int lo = std::min(from, to);
	int hi = std::max(from, to);

	// Step 1: Rename all affected SEXP refs to temporary names
	SCP_vector<SCP_string> temp_names(hi - lo + 1);
	for (int i = lo; i <= hi; i++) {
		char temp[NAME_LENGTH + 16];
		rename_waypoint_sexp_refs_to_temp(list_name, i + 1, temp, sizeof(temp));
		temp_names[i - lo] = temp;
	}

	// Step 2: Shift positions (waypoint objects stay in place)
	vec3d saved_pos = *wpts[from].get_pos();
	if (from < to) {
		for (int i = from; i < to; i++)
			wpts[i].set_pos(wpts[i + 1].get_pos());
	} else {
		for (int i = from; i > to; i--)
			wpts[i].set_pos(wpts[i - 1].get_pos());
	}
	wpts[to].set_pos(&saved_pos);

	// Step 3: Rename from temp names to final (shifted) names.
	// temp_names[i - lo] holds the temp name for what was originally at 0-based index i.
	// Map each original index to its new index after the move:
	//   - The element at 'from' moved to 'to'
	//   - If from < to: elements at from+1..to shifted down by 1
	//   - If from > to: elements at to..from-1 shifted up by 1
	for (int i = lo; i <= hi; i++) {
		int new_index;
		if (i == from)
			new_index = to;
		else if (from < to)
			new_index = i - 1;  // shifted down
		else
			new_index = i + 1;  // shifted up
		rename_waypoint_sexp_refs_from_temp(temp_names[i - lo].c_str(), list_name, new_index + 1);
	}
}

// Swap two waypoint positions within a list (positions move, objects stay in place).
// Uses 0-based indices internally.
static void do_waypoint_swap(int li, int a, int b)
{
	auto &wl = Waypoint_lists[li];
	auto list_name = wl.get_name();
	auto &wpts = wl.get_waypoints();

	// Step 1: Rename both SEXP refs to temp names
	char temp_a[NAME_LENGTH + 16];
	char temp_b[NAME_LENGTH + 16];
	rename_waypoint_sexp_refs_to_temp(list_name, a + 1, temp_a, sizeof(temp_a));
	rename_waypoint_sexp_refs_to_temp(list_name, b + 1, temp_b, sizeof(temp_b));

	// Step 2: Swap positions (waypoint objects stay in place)
	vec3d pos_a = *wpts[a].get_pos();
	vec3d pos_b = *wpts[b].get_pos();
	wpts[a].set_pos(&pos_b);
	wpts[b].set_pos(&pos_a);

	// Step 3: Rename from temp to final (swapped positions)
	rename_waypoint_sexp_refs_from_temp(temp_a, list_name, b + 1);
	rename_waypoint_sexp_refs_from_temp(temp_b, list_name, a + 1);
}

static MoveSwapConfig make_waypoint_move_swap_config(int waypoint_list_index)
{
	MoveSwapConfig cfg;
	cfg.entity_name = "waypoint";
	cfg.count = (int)Waypoint_lists[waypoint_list_index].get_waypoints().size();
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_waypoint_lists;

	// Capture list index for use in lambdas
	int li = waypoint_list_index;

	cfg.get_name = [li](int i) {
		SCP_string name_buf;
		waypoint_stuff_name(name_buf, Waypoint_lists[li].get_name(), i);
		return name_buf;
	};

	// Lambdas receive 1-based indices (matching one_based = true).
	// Convert to 0-based for internal array access.
	cfg.do_move = [li](int from, int to) {
		do_waypoint_move(li, from - 1, to - 1);
	};

	cfg.do_swap = [li](int a, int b) {
		do_waypoint_swap(li, a - 1, b - 1);
	};

	return cfg;
}

static void handle_move_waypoint(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto list = get_required_string(input, "list", sink, true);
	if (!list) return;

	int li = find_matching_waypoint_list_index(list);
	if (li < 0) {
		set_not_found_error(sink,"Waypoint list", list);
		return;
	}

	auto cfg = make_waypoint_move_swap_config(li);
	handle_generic_move(input, req, cfg);
}

static void handle_swap_waypoints(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto list = get_required_string(input, "list", sink, true);
	if (!list) return;

	int li = find_matching_waypoint_list_index(list);
	if (li < 0) {
		set_not_found_error(sink,"Waypoint list", list);
		return;
	}

	auto cfg = make_waypoint_move_swap_config(li);
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_waypoint_tools(json_t *tools)
{
	// -----------------------------------------------------------------------
	// Waypoint list tools
	// -----------------------------------------------------------------------

	// list_waypoint_lists
	register_tool(tools, "list_waypoint_lists",
		"List all waypoint lists in the mission. Returns each list's name, "
		"index, and waypoint count.",
		json_object());

	// get_waypoint_list
	register_tool_with_required_string(tools, "get_waypoint_list",
		"Get full details of a waypoint list by name, including all "
		"waypoint positions.",
		"name", "Name of the waypoint list to retrieve");

	// create_waypoint_list
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Unique name for the waypoint list");
		add_vec3d_array_prop(props, "points",
			"Array of 3D positions ({x, y, z} objects) for the waypoints in this list. "
			"At least one point is required.");
		add_integer_prop(props, "index",
			"Position to insert the waypoint list (1 = first). If omitted, appends to the end.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("points"));
		register_tool(tools, "create_waypoint_list",
			"Create a new waypoint list with the given positions. Waypoint lists define "
			"flight paths that ships can follow via ai-waypoints SEXPs.",
			props, req);
	}

	// update_waypoint_list
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the waypoint list to update");
		add_string_prop(props, "new_name", "New name for the waypoint list");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_waypoint_list",
			"Rename a waypoint list. SEXP and AI goal references are updated "
			"automatically, including individual waypoint names (e.g. Path:1 becomes NewPath:1).",
			props, req);
	}

	// delete_waypoint_list
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the waypoint list to delete");
		add_bool_prop(props, "force",
			"If true, delete even if the waypoint list or its waypoints are referenced in SEXPs "
			"(references will be invalidated). Default false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "delete_waypoint_list",
			"Delete a waypoint list and all its waypoints from the mission. Fails if the list "
			"or any of its waypoints are referenced in SEXPs unless force=true.",
			props, req);
	}

	// move_waypoint_list
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"1-based index of the waypoint list to move");
		add_integer_prop(props, "to_index",
			"1-based target index");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_waypoint_list",
			"Move a waypoint list from one position to another. "
			"Indices are 1-based.",
			props, req);
	}

	// swap_waypoint_lists
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first waypoint list");
		add_integer_prop(props, "index_b",
			"1-based index of the second waypoint list");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_waypoint_lists",
			"Swap two waypoint lists at the given positions. "
			"Indices are 1-based.",
			props, req);
	}

	// -----------------------------------------------------------------------
	// Individual waypoint tools
	// -----------------------------------------------------------------------

	// create_waypoint
	{
		json_t *props = json_object();
		add_string_prop(props, "list", "Name of the waypoint list to add to");
		add_vec3d_prop(props, "position", "World position of the new waypoint");
		add_integer_prop(props, "index",
			"1-based position to insert within the list. If omitted, appends to the end.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("list"));
		json_array_append_new(req, json_string("position"));
		register_tool(tools, "create_waypoint",
			"Add a new waypoint to an existing waypoint list at a given position.",
			props, req);
	}

	// update_waypoint
	{
		json_t *props = json_object();
		add_string_prop(props, "list", "Name of the waypoint list");
		add_integer_prop(props, "index", "1-based index of the waypoint to update");
		add_vec3d_prop(props, "position", "New world position for the waypoint");
		json_t *req = json_array();
		json_array_append_new(req, json_string("list"));
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "update_waypoint",
			"Update the position of an individual waypoint. Only specified fields "
			"are changed; omitted fields are left unchanged.",
			props, req);
	}

	// delete_waypoint
	{
		json_t *props = json_object();
		add_string_prop(props, "list", "Name of the waypoint list");
		add_integer_prop(props, "index", "1-based index of the waypoint to delete");
		add_bool_prop(props, "force",
			"If true, delete even if the waypoint is referenced in SEXPs "
			"(references will be invalidated). Default false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("list"));
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "delete_waypoint",
			"Delete a waypoint from a list. If this is the last waypoint, the entire "
			"list is removed. Fails if the waypoint is referenced in SEXPs unless force=true. "
			"SEXP references for subsequent waypoints are automatically renumbered.",
			props, req);
	}

	// move_waypoint
	{
		json_t *props = json_object();
		add_string_prop(props, "list", "Name of the waypoint list");
		add_integer_prop(props, "from_index",
			"1-based index of the waypoint to move");
		add_integer_prop(props, "to_index",
			"1-based target index");
		json_t *req = json_array();
		json_array_append_new(req, json_string("list"));
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_waypoint",
			"Move a waypoint from one position to another within the same list. "
			"SEXP references are updated to follow the waypoints. Indices are 1-based.",
			props, req);
	}

	// swap_waypoints
	{
		json_t *props = json_object();
		add_string_prop(props, "list", "Name of the waypoint list");
		add_integer_prop(props, "index_a",
			"1-based index of the first waypoint");
		add_integer_prop(props, "index_b",
			"1-based index of the second waypoint");
		json_t *req = json_array();
		json_array_append_new(req, json_string("list"));
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_waypoints",
			"Swap two waypoints within the same list. "
			"SEXP references are updated to follow the waypoints. Indices are 1-based.",
			props, req);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_waypoint_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_waypoint_lists") == 0) {
		handle_list_waypoint_lists(input_json, req);
	} else if (strcmp(tool_name, "get_waypoint_list") == 0) {
		handle_get_waypoint_list(input_json, req);
	} else if (strcmp(tool_name, "create_waypoint_list") == 0) {
		handle_create_waypoint_list(input_json, req);
	} else if (strcmp(tool_name, "update_waypoint_list") == 0) {
		handle_update_waypoint_list(input_json, req);
	} else if (strcmp(tool_name, "delete_waypoint_list") == 0) {
		handle_delete_waypoint_list(input_json, req);
	} else if (strcmp(tool_name, "move_waypoint_list") == 0) {
		handle_move_waypoint_list(input_json, req);
	} else if (strcmp(tool_name, "swap_waypoint_lists") == 0) {
		handle_swap_waypoint_lists(input_json, req);
	} else if (strcmp(tool_name, "create_waypoint") == 0) {
		handle_create_waypoint(input_json, req);
	} else if (strcmp(tool_name, "update_waypoint") == 0) {
		handle_update_waypoint(input_json, req);
	} else if (strcmp(tool_name, "delete_waypoint") == 0) {
		handle_delete_waypoint(input_json, req);
	} else if (strcmp(tool_name, "move_waypoint") == 0) {
		handle_move_waypoint(input_json, req);
	} else if (strcmp(tool_name, "swap_waypoints") == 0) {
		handle_swap_waypoints(input_json, req);
	} else {
		return false;
	}
	return true;
}
