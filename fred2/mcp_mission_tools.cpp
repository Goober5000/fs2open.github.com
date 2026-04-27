#include "stdafx.h"
#include "mcp_mission_tools.h"
#include "mcp_messages.h"
#include "mcp_events.h"
#include "mcp_goals.h"
#include "mcp_waypoints.h"
#include "mcp_cmd_brief.h"
#include "mcp_debrief.h"
#include "mcp_fiction_viewer.h"
#include "mcp_mission_info.h"
#include "mcp_sexp.h"
#include "mcpserver.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcp_array_utils.h"
#include "mcp_sexp_forest.h"
#include "mcp_reference_tools.h"

#include <jansson.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "globalincs/utility.h"

#include "gamesnd/eventmusic.h"
#include "mission/missionmessage.h"
#include "mission/missiongoals.h"
#include "missionui/missioncmdbrief.h"
#include "mission/missionbriefcommon.h"
#include "mission/missionparse.h"
#include "ship/ship.h"
#include "parse/parselo.h"
#include "parse/sexp.h"
#include "jumpnode/jumpnode.h"
#include "object/object.h"
#include "object/waypoint.h"
#include "freddoc.h"
#include "fred.h"
#include "mainfrm.h"


// Returns true if deletion should proceed, false if blocked by references.
// Handles the common pattern: check SEXP refs unless force is set, report error if found.
bool check_and_report_sexp_refs(sexp_ref_type ref_type, const char *entity_label,
	const char *name, std::optional<bool> force, McpErrorSink &sink)
{
	if (force.has_value() && *force)
		return true;

	int node;
	auto ref = query_referenced_in_sexp(ref_type, name, node);
	if (ref.second != sexp_src::NONE) {
		SCP_string desc = sexp_src_to_description(ref.first, ref.second);
		sink.set_error("%s '%s' is referenced in %s. Use force=true to delete anyway "
			"(references will be invalidated).", entity_label, name, desc.c_str());
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_jump_nodes(SCP_string &error_msg)
{
	return validate_single_dialog("jump nodes", "jump node", error_msg);
}

// ---------------------------------------------------------------------------
// Enum helpers
// ---------------------------------------------------------------------------

const SCP_vector<const char *> team_enum_values = { "none", "Team 1", "Team 2" };

// ---------------------------------------------------------------------------
// Team helpers
// ---------------------------------------------------------------------------

const char *team_name_from_index(int multi_team)
{
	switch (multi_team) {
		case 0:  return "Team 1";
		case 1:  return "Team 2";
		default: return "none";
	}
}

int team_index_from_name(const char *name)
{
	if (!stricmp(name, "Team 1")) return 0;
	if (!stricmp(name, "Team 2")) return 1;
	return -1;	// invalid or default
}

bool reject_team_none(const char *team_str, const char *entity_name, McpErrorSink &sink)
{
	if (!stricmp(team_str, "none")) {
		sink.set_error("A team of \"none\" is not valid for a %s. Use \"Team 1\" or \"Team 2\".", entity_name);
		return true;
	}
	return false;
}


// ---------------------------------------------------------------------------
// Autosave helper
// ---------------------------------------------------------------------------

void mark_modified(const char *fmt, ...)
{
	set_modified();
	if (FREDDoc_ptr) {
		SCP_string desc;
		va_list args;
		va_start(args, fmt);
		vsprintf(desc, fmt, args);
		va_end(args);
		FREDDoc_ptr->autosave(desc.c_str());
	}
}

// ---------------------------------------------------------------------------
// Generic move/swap handlers
// ---------------------------------------------------------------------------

void handle_generic_move(json_t *input, McpToolRequest *req, const MoveSwapConfig &cfg)
{
	McpErrorSink sink(req);
	if (!validate(cfg.validate_dialog, sink)) return;

	auto from_index = get_required_integer(input, "from_index", sink);
	if (!from_index.has_value() || !check_int_range(*from_index, cfg.min_index(), cfg.max_index(), "from_index", sink)) return;
	auto to_index = get_required_integer(input, "to_index", sink);
	if (!to_index.has_value() || !check_int_range(*to_index, cfg.min_index(), cfg.max_index(), "to_index", sink)) return;

	if (from_index == to_index) {
		json_t *data = json_object();
		json_object_set_new(data, "name", json_safe_string(cfg.get_name(*from_index).c_str()));
		json_object_set_new(data, "index", json_integer(*from_index));
		req->result_json = make_json_tool_result(data);
		req->success = true;
		return;
	}

	cfg.do_move(*from_index, *to_index);

	mark_modified("MCP: move %s %s from %d to %d", cfg.entity_name, cfg.get_name(*to_index).c_str(), *from_index, *to_index);

	json_t *data = json_object();
	json_object_set_new(data, "name", json_safe_string(cfg.get_name(*to_index).c_str()));
	json_object_set_new(data, "index", json_integer(*to_index));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

void handle_generic_swap(json_t *input, McpToolRequest *req, const MoveSwapConfig &cfg)
{
	McpErrorSink sink(req);
	if (!validate(cfg.validate_dialog, sink)) return;

	auto index_a = get_required_integer(input, "index_a", sink);
	if (!index_a.has_value() || !check_int_range(*index_a, cfg.min_index(), cfg.max_index(), "index_a", sink)) return;
	auto index_b = get_required_integer(input, "index_b", sink);
	if (!index_b.has_value() || !check_int_range(*index_b, cfg.min_index(), cfg.max_index(), "index_b", sink)) return;

	if (index_a != index_b) {
		cfg.do_swap(*index_a, *index_b);

		mark_modified("MCP: swap %ss %s and %s", cfg.entity_name, cfg.get_name(*index_b).c_str(), cfg.get_name(*index_a).c_str());
	}

	json_t *data = json_object();
	json_t *a_obj = json_object();
	json_object_set_new(a_obj, "name", json_safe_string(cfg.get_name(*index_a).c_str()));
	json_object_set_new(a_obj, "index", json_integer(*index_a));
	json_t *b_obj = json_object();
	json_object_set_new(b_obj, "name", json_safe_string(cfg.get_name(*index_b).c_str()));
	json_object_set_new(b_obj, "index", json_integer(*index_b));
	json_object_set_new(data, "a", a_obj);
	json_object_set_new(data, "b", b_obj);
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Jump node tools
// ---------------------------------------------------------------------------

static json_t *build_jump_node_json(const CJumpNode &jn, int index, bool include_details = false)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_safe_string(jn.GetName()));
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "position", build_vec3d_json(*jn.GetPosition()));

	if (include_details) {
		if (jn.HasDisplayName())
			json_object_set_new(obj, "display_name", json_safe_string(jn.GetDisplayName()));
		if (jn.IsColored())
			json_object_set_new(obj, "color", build_color_json(jn.GetColor(), true));
		if (jn.IsSpecialModel()) {
			const char *model_filename = model_get(jn.GetModelNumber())->filename;
			set_optional_filename(obj, "model_filename", model_filename);
		}
		json_object_set_new(obj, "hidden", json_boolean(jn.IsHidden()));
		json_object_set_new(obj, "radius", json_real(jn.GetRadius()));
	}

	return obj;
}

static void handle_list_jump_nodes(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	json_t *arr = json_array();
	int index = 0;
	for (const auto &jn : Jump_nodes)
		json_array_append_new(arr, build_jump_node_json(jn, index++));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_jump_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int index = jumpnode_lookup(name);
	if (index >= 0) {
		req->result_json = make_json_tool_result(build_jump_node_json(Jump_nodes[index], index, true));
		req->success = true;
		return;
	}

	set_not_found_error(sink,"Jump node", name);
}

static void handle_create_jump_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true, NAME_LENGTH - 1);
	if (!name) return;

	auto pos = get_required_vec3d(input, "position", sink);
	if (!pos.has_value()) return;

	auto display_name = get_optional_string(input, "display_name", sink, NAME_LENGTH - 1);
	auto color_val    = get_optional_color(input, "color", sink);
	auto model_file   = get_optional_filename(input, "model_filename", sink, true, MAX_FILENAME_LEN - 1);
	auto show_polys   = get_optional_bool(input, "show_polys", sink);
	auto hidden       = get_optional_bool(input, "hidden", sink);
	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	// Validate name against all object types
	if (!check_object_rename("jump node", name, sink)) return;

	// Validate insert index
	int target_index;
	if (!insert_index.has_value()) {
		target_index = (int)Jump_nodes.size();
	} else {
		if (!check_int_range(*insert_index, 1, (int)Jump_nodes.size() + 1, "index", sink))
			return;
		target_index = *insert_index - 1;
	}

	// Construct the jump node
	vec3d position = *pos;
	CJumpNode jnp(&position);
	jnp.SetName(name);

	if (display_name)
		jnp.SetDisplayName(display_name);
	if (color_val.has_value())
		jnp.SetAlphaColor(color_val->red, color_val->green, color_val->blue, color_val->alpha);
	if (model_file)
		jnp.SetModel(model_file, show_polys.has_value() && *show_polys);
	else if (show_polys.has_value() && *show_polys)
		jnp.SetModel(JN_DEFAULT_MODEL, true);
	if (hidden.has_value() && *hidden)
		jnp.SetVisibility(false);

	// Insert
	Jump_nodes.insert(Jump_nodes.begin() + target_index, std::move(jnp));
	obj_merge_created_list();

	Jumpnode_editor_dialog.initialize_data(1);
	mark_modified("MCP: create jump node %s", name);

	req->result_json = make_json_tool_result(build_jump_node_json(Jump_nodes[target_index], target_index, true));
	req->success = true;
}

static void handle_update_jump_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	auto new_name     = get_optional_string(input, "new_name", sink, NAME_LENGTH - 1);
	auto new_pos      = get_optional_vec3d(input, "position", sink);
	auto display_name = get_optional_string(input, "display_name", sink, NAME_LENGTH - 1);
	auto color_val    = get_optional_color(input, "color", sink);
	auto model_file   = get_optional_filename(input, "model_filename", sink, true, MAX_FILENAME_LEN - 1);
	auto show_polys   = get_optional_bool(input, "show_polys", sink);
	auto hidden       = get_optional_bool(input, "hidden", sink);
	if (sink.has_error()) return;

	// Find the jump node
	int index = jumpnode_lookup(name);
	if (index < 0) {
		set_not_found_error(sink,"Jump node", name);
		return;
	}
	auto &jn = Jump_nodes[index];

	bool changed = false;

	// Rename (with SEXP reference update)
	if (new_name && stricmp(jn.GetName(), new_name) != 0) {
		if (!check_object_rename("jump node", new_name, sink, -1, -1, -1, index)) return;
		update_sexp_references(jn.GetName(), new_name, OPF_JUMP_NODE_NAME);
		mcp_sexp_forest_mark_dirty();
		jn.SetName(new_name);
		changed = true;
	}

	// Position
	if (new_pos.has_value()) {
		const vec3d *cur = jn.GetPosition();
		if (!fl_equal(cur->xyz.x, new_pos->xyz.x) || !fl_equal(cur->xyz.y, new_pos->xyz.y) || !fl_equal(cur->xyz.z, new_pos->xyz.z)) {
			Objects[jn.GetSCPObjectNumber()].pos = *new_pos;
			changed = true;
		}
	}

	// Display name
	if (display_name) {
		if (!display_name[0]) {
			// Empty string clears display name
			if (jn.HasDisplayName()) {
				jn.SetDisplayName("");
				changed = true;
			}
		} else if (strcmp(jn.GetDisplayName(), display_name) != 0) {
			jn.SetDisplayName(display_name);
			changed = true;
		}
	}

	// Color
	if (color_val.has_value()) {
		const color &c = jn.GetColor();
		if (c.red != color_val->red || c.green != color_val->green ||
			c.blue != color_val->blue || c.alpha != color_val->alpha) {
			jn.SetAlphaColor(color_val->red, color_val->green, color_val->blue, color_val->alpha);
			changed = true;
		}
	}

	// Model
	if (model_file) {
		jn.SetModel(model_file, show_polys.has_value() && *show_polys);
		changed = true;
	} else if (show_polys.has_value()) {
		// Reload current model with new show_polys setting
		if (jn.IsSpecialModel()) {
			const char *cur_model = model_get(jn.GetModelNumber())->filename;
			jn.SetModel(cur_model, *show_polys);
		} else {
			jn.SetModel(JN_DEFAULT_MODEL, *show_polys);
		}
		changed = true;
	}

	// Hidden
	if (hidden.has_value() && jn.IsHidden() != *hidden) {
		jn.SetVisibility(!*hidden);
		changed = true;
	}

	if (changed) {
		Jumpnode_editor_dialog.initialize_data(1);
		mark_modified("MCP: update jump node %s", jn.GetName());
	}

	req->result_json = make_json_tool_result(build_jump_node_json(jn, index, true));
	req->success = true;
}

static void handle_delete_jump_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	// Find the jump node
	int index = jumpnode_lookup(name);
	if (index < 0 ) {
		set_not_found_error(sink,"Jump node", name);
		return;
	}

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	if (!check_and_report_sexp_refs(sexp_ref_type::NON_OBJECT, "Jump node", name, force, sink))
		return;

	// Invalidate SEXP references
	char buf[NAME_LENGTH + 4];
	snprintf(buf, sizeof(buf), "<%s>", name);
	update_sexp_references(name, buf, OPF_JUMP_NODE_NAME);
	mcp_sexp_forest_mark_dirty();

	// Must follow the same pattern as management.cpp delete_object() to avoid
	// orphaning the OBJ_JUMP_NODE object slot.  The CJumpNode move-assignment
	// operator does not call obj_delete on the overwritten m_objnum, so a
	// naive vector::erase on a non-last element silently leaks the object.
	int objnum = Jump_nodes[index].GetSCPObjectNumber();
	Objects[objnum].type = OBJ_NONE;          // fool destructor into skipping obj_delete
	Jump_nodes.erase(Jump_nodes.begin() + index);
	Objects[objnum].type = OBJ_JUMP_NODE;     // restore for obj_delete
	unmark_object(objnum);
	obj_delete(objnum);                        // free the object slot

	Jumpnode_editor_dialog.initialize_data(1);
	mark_modified("MCP: delete jump node %s", name);

	sprintf(req->result_message,
		"Deleted jump node: %s", name);
	req->success = true;
}

// Move/swap config for jump nodes
static MoveSwapConfig make_jump_node_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "jump node";
	cfg.count = (int)Jump_nodes.size();
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_jump_nodes;
	cfg.get_name = [](int i) {
		return Jump_nodes[i - 1].GetName();
	};
	cfg.do_move = [](int from, int to) {
		array_move_element(Jump_nodes, from - 1, to - 1);
	};
	cfg.do_swap = [](int a, int b) {
		std::swap(Jump_nodes[a - 1], Jump_nodes[b - 1]);
	};
	return cfg;
}

static void handle_move_jump_node(json_t *input, McpToolRequest *req)
{
	auto cfg = make_jump_node_move_swap_config();
	handle_generic_move(input, req, cfg);
}

static void handle_swap_jump_nodes(json_t *input, McpToolRequest *req)
{
	auto cfg = make_jump_node_move_swap_config();
	handle_generic_swap(input, req, cfg);
}


// ---------------------------------------------------------------------------
// Mission music tool handlers
// ---------------------------------------------------------------------------

static json_t *build_mission_music_json()
{
	json_t *m = json_object();

	// Mission menu music
	struct { const char *key; int score; } scores[] = {
		{ "briefing_music",           SCORE_BRIEFING           },
		{ "debriefing_success_music", SCORE_DEBRIEFING_SUCCESS },
		{ "debriefing_average_music", SCORE_DEBRIEFING_AVERAGE },
		{ "debriefing_failure_music", SCORE_DEBRIEFING_FAILURE },
		{ "fiction_viewer_music",     SCORE_FICTION_VIEWER     },
	};
	for (auto &s : scores) {
		int idx = Mission_music[s.score];
		json_object_set_new(m, s.key, json_safe_string(Spooled_music.in_bounds(idx) ? Spooled_music[idx].name : "None"));
	}

	// Event music
	json_object_set_new(m, "event_music", json_safe_string(Soundtracks.in_bounds(Current_soundtrack_num) ? Soundtracks[Current_soundtrack_num].name : "None"));

	// Substitute music
	struct { const char *key; const char *ref; } substitutes[] = {
		{ "substitute_briefing_music", The_mission.substitute_briefing_music_name },
		{ "substitute_event_music",    The_mission.substitute_event_music_name    },
	};
	for (auto &s : substitutes) {
		if (s.ref[0] && stricmp(s.ref, "None") != 0)
			json_object_set_new(m, s.key, json_safe_string(s.ref));
		else
			json_object_set_new(m, s.key, json_string("None"));
	}

	return m;
}

static void handle_get_mission_music(json_t * /*input*/, McpToolRequest *req)
{
	req->result_json = make_json_tool_result(build_mission_music_json());
	req->success = true;
}

static void handle_update_mission_music(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);

	auto resolve_index = [&](const char *param, bool is_soundtrack)->std::optional<int> {
		auto str = get_optional_string(input, param, sink);
		if (!str) return std::nullopt;
		if (!str[0] || !stricmp(str, "none") || !stricmp(str, "<none>")) return -1;
		return check_lookup(str, is_soundtrack ? event_music_get_soundtrack_index : static_cast<int(*)(const char*)>(event_music_get_spooled_music_index), param, sink);
	};

	auto new_briefing           = resolve_index("briefing_music", false);
	auto new_debriefing_success = resolve_index("debriefing_success_music", false);
	auto new_debriefing_average = resolve_index("debriefing_average_music", false);
	auto new_debriefing_failure = resolve_index("debriefing_failure_music", false);
	auto new_fiction            = resolve_index("fiction_viewer_music", false);
	auto new_event              = resolve_index("event_music", true);
	auto new_sub_briefing       = get_optional_string(input, "substitute_briefing_music", sink, NAME_LENGTH - 1);
	auto new_sub_event          = get_optional_string(input, "substitute_event_music", sink, NAME_LENGTH - 1);
	if (sink.has_error()) return;

	bool changed = false;

	if (new_briefing.has_value() && Mission_music[SCORE_BRIEFING] != *new_briefing) {
		Mission_music[SCORE_BRIEFING] = *new_briefing;
		changed = true;
	}
	if (new_debriefing_success.has_value() && Mission_music[SCORE_DEBRIEFING_SUCCESS] != *new_debriefing_success) {
		Mission_music[SCORE_DEBRIEFING_SUCCESS] = *new_debriefing_success;
		changed = true;
	}
	if (new_debriefing_average.has_value() && Mission_music[SCORE_DEBRIEFING_AVERAGE] != *new_debriefing_average) {
		Mission_music[SCORE_DEBRIEFING_AVERAGE] = *new_debriefing_average;
		changed = true;
	}
	if (new_debriefing_failure.has_value() && Mission_music[SCORE_DEBRIEFING_FAILURE] != *new_debriefing_failure) {
		Mission_music[SCORE_DEBRIEFING_FAILURE] = *new_debriefing_failure;
		changed = true;
	}
	if (new_fiction.has_value() && Mission_music[SCORE_FICTION_VIEWER] != *new_fiction) {
		Mission_music[SCORE_FICTION_VIEWER] = *new_fiction;
		changed = true;
	}
	if (new_event.has_value() && Current_soundtrack_num != *new_event) {
		Current_soundtrack_num = *new_event;
		changed = true;
	}
	if (new_sub_briefing && strcmp(The_mission.substitute_briefing_music_name, new_sub_briefing) != 0) {
		strcpy_s(The_mission.substitute_briefing_music_name, new_sub_briefing);
		changed = true;
	}
	if (new_sub_event && strcmp(The_mission.substitute_event_music_name, new_sub_event) != 0) {
		strcpy_s(The_mission.substitute_event_music_name, new_sub_event);
		changed = true;
	}

	if (changed)
		mark_modified("MCP: update mission music");

	req->result_json = make_json_tool_result(build_mission_music_json());
	req->success = true;
}


// ---------------------------------------------------------------------------
// Known mission tool names (for routing)
// ---------------------------------------------------------------------------

static const char *mission_tool_names[] = {
	"list_messages",
	"get_message",
	"create_message",
	"update_message",
	"delete_message",
	"move_message",
	"swap_messages",
	"list_events",
	"get_event",
	"create_event",
	"update_event",
	"delete_event",
	"move_event",
	"swap_events",
	"list_cmd_brief_stages",
	"get_cmd_brief_stage",
	"create_cmd_brief_stage",
	"update_cmd_brief_stage",
	"delete_cmd_brief_stage",
	"move_cmd_brief_stage",
	"swap_cmd_brief_stages",
	"list_goals",
	"get_goal",
	"create_goal",
	"update_goal",
	"delete_goal",
	"move_goal",
	"swap_goals",
	"list_fiction_viewer_stages",
	"get_fiction_viewer_stage",
	"create_fiction_viewer_stage",
	"update_fiction_viewer_stage",
	"delete_fiction_viewer_stage",
	"move_fiction_viewer_stage",
	"swap_fiction_viewer_stages",
	"list_debriefing_stages",
	"get_debriefing_stage",
	"create_debriefing_stage",
	"update_debriefing_stage",
	"delete_debriefing_stage",
	"move_debriefing_stage",
	"swap_debriefing_stages",
	"list_jump_nodes",
	"get_jump_node",
	"create_jump_node",
	"update_jump_node",
	"delete_jump_node",
	"move_jump_node",
	"swap_jump_nodes",
	"list_waypoint_lists",
	"get_waypoint_list",
	"create_waypoint_list",
	"update_waypoint_list",
	"delete_waypoint_list",
	"move_waypoint_list",
	"swap_waypoint_lists",
	"create_waypoint",
	"update_waypoint",
	"delete_waypoint",
	"move_waypoint",
	"swap_waypoints",
	"sexp_to_text",
	"get_sexp_node",
	"get_sexp_formula_info",
	"walk_sexp_tree",
	"find_sexp_text",
	"text_to_sexp",
	"detach_sexp_node",
	"attach_sexp_node",
	"move_sexp_node",
	"swap_sexp_nodes",
	"create_sexp_node",
	"update_sexp_node",
	"list_sexp_variables",
	"get_sexp_variable",
	"create_sexp_variable",
	"update_sexp_variable",
	"delete_sexp_variable",
	"get_mission_info",
	"update_mission_info",
	"get_mission_music",
	"update_mission_music",
	nullptr
};

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_mission_tools(json_t *tools)
{
	mcp_register_message_tools(tools);
	mcp_register_event_tools(tools);
	mcp_register_cmd_brief_tools(tools);
	mcp_register_goal_tools(tools);
	mcp_register_fiction_viewer_tools(tools);
	mcp_register_debrief_tools(tools);

	// -----------------------------------------------------------------------
	// Jump node tools
	// -----------------------------------------------------------------------

	// list_jump_nodes
	register_tool(tools, "list_jump_nodes",
		"List all jump nodes in the mission. Returns each node's name, "
		"index, and position.",
		json_object());

	// get_jump_node
	register_tool_with_required_string(tools, "get_jump_node",
		"Get full details of a jump node by name, including position, "
		"display name, color, model file, hidden state, and radius.",
		"name", "Name of the jump node to retrieve");

	// create_jump_node
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Unique name for the jump node");
		add_vec3d_prop(props, "position", "World position of the jump node");
		add_string_prop(props, "display_name",
			"Display name shown to the player (if different from name)");
		add_color_prop(props, "color",
			"Custom RGBA display color. If omitted, defaults to green (0,255,0,255).");
		add_string_prop(props, "model_filename",
			"Model filename (POF). Defaults to \"" JN_DEFAULT_MODEL "\".");
		add_bool_prop(props, "show_polys",
			"If true, render as solid model instead of wireframe. Default false.");
		add_bool_prop(props, "hidden",
			"If true, the jump node is hidden from rendering. Default false.");
		add_integer_prop(props, "index",
			"Position to insert the jump node (1 = first). If omitted, appends to the end.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("position"));
		register_tool(tools, "create_jump_node",
			"Create a new jump node at a given position. Jump nodes are subspace "
			"navigation points that ships can depart through.",
			props, req);
	}

	// update_jump_node
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the jump node to update");
		add_string_prop(props, "new_name", "New name for the jump node");
		add_vec3d_prop(props, "position", "New world position");
		add_string_prop(props, "display_name",
			"New display name (empty string to clear)");
		add_color_prop(props, "color", "New RGBA display color");
		add_string_prop(props, "model_filename",
			"New model filename (POF)");
		add_bool_prop(props, "show_polys",
			"If true, render as solid model instead of wireframe");
		add_bool_prop(props, "hidden",
			"If true, the jump node is hidden from rendering");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_jump_node",
			"Update properties of an existing jump node. Only specified fields "
			"are changed; omitted fields are left unchanged. Renaming updates "
			"SEXP references automatically.",
			props, req);
	}

	// delete_jump_node
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the jump node to delete");
		add_bool_prop(props, "force",
			"If true, delete even if the jump node is referenced in SEXPs "
			"(references will be invalidated). Default false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "delete_jump_node",
			"Delete a jump node from the mission. Fails if the node is "
			"referenced in SEXPs unless force=true.",
			props, req);
	}

	// move_jump_node
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"1-based index of the jump node to move");
		add_integer_prop(props, "to_index",
			"1-based target index");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_jump_node",
			"Move a jump node from one list position to another. "
			"Indices are 1-based.",
			props, req);
	}

	// swap_jump_nodes
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first jump node");
		add_integer_prop(props, "index_b",
			"1-based index of the second jump node");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_jump_nodes",
			"Swap two jump nodes at the given positions. "
			"Indices are 1-based.",
			props, req);
	}

	mcp_register_waypoint_tools(tools);

	// get_mission_music
	register_tool(tools, "get_mission_music",
		"Returns all mission music assignments: event music, briefing music, debriefing music "
		"(success/average/failure), fiction viewer music, and the substitute event/briefing music "
		"fields used for FS1 compatibility. Each field is a music name or \"None\" if unset.",
		nullptr);

	// update_mission_music
	{
		json_t *props = json_object();
		add_string_prop(props, "event_music",
			"Event music soundtrack name. Use list_soundtracks to see valid names. Pass an empty string or \"None\" to clear.");
		add_string_prop(props, "substitute_event_music",
			"Alternate event music soundtrack used at mission load if available (FS1 compatibility). Pass an empty string or \"None\" to clear.");
		add_string_prop(props, "briefing_music",
			"Briefing music name. Use list_menu_music to see valid names. Pass an empty string or \"None\" to clear.");
		add_string_prop(props, "substitute_briefing_music",
			"Alternate briefing music used at mission load if available. Pass an empty string or \"None\" to clear.");
		add_string_prop(props, "debriefing_success_music",
			"Music for the success debriefing outcome. Use list_menu_music to see valid names. Pass an empty string or \"None\" to clear.");
		add_string_prop(props, "debriefing_average_music",
			"Music for the average debriefing outcome. Pass an empty string or \"None\" to clear.");
		add_string_prop(props, "debriefing_failure_music",
			"Music for the failure debriefing outcome. Pass an empty string or \"None\" to clear.");
		add_string_prop(props, "fiction_viewer_music",
			"Music played during the fiction viewer for this mission. Pass an empty string or \"None\" to clear.");
		register_tool(tools, "update_mission_music",
			"Update one or more mission music assignments. All parameters are optional; "
			"only provided fields are changed. Returns the full updated music state.",
			props);
	}

	mcp_register_sexp_tools(tools);

	mcp_register_mission_info_tools(tools);
}

// ---------------------------------------------------------------------------
// Routing (called on mongoose thread — marshals to main thread)
// ---------------------------------------------------------------------------

json_t *mcp_route_mission_tool(const char *tool_name, json_t *arguments)
{
	for (const char **p = mission_tool_names; *p; p++) {
		if (strcmp(tool_name, *p) == 0)
			return mcp_execute_on_main_thread(McpToolId::MISSION_TOOL, tool_name, arguments);
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

void mcp_handle_mission_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (mcp_handle_message_tool(tool_name, input_json, req)) {
		// handled by messages unit
	} else if (mcp_handle_event_tool(tool_name, input_json, req)) {
		// handled by events unit
	} else if (mcp_handle_cmd_brief_tool(tool_name, input_json, req)) {
		// handled by cmd_brief unit
	} else if (mcp_handle_goal_tool(tool_name, input_json, req)) {
		// handled by goals unit
	} else if (mcp_handle_fiction_viewer_tool(tool_name, input_json, req)) {
		// handled by fiction_viewer unit
	} else if (mcp_handle_debrief_tool(tool_name, input_json, req)) {
		// handled by debrief unit
	} else if (strcmp(tool_name, "list_jump_nodes") == 0) {
		handle_list_jump_nodes(input_json, req);
	} else if (strcmp(tool_name, "get_jump_node") == 0) {
		handle_get_jump_node(input_json, req);
	} else if (strcmp(tool_name, "create_jump_node") == 0) {
		handle_create_jump_node(input_json, req);
	} else if (strcmp(tool_name, "update_jump_node") == 0) {
		handle_update_jump_node(input_json, req);
	} else if (strcmp(tool_name, "delete_jump_node") == 0) {
		handle_delete_jump_node(input_json, req);
	} else if (strcmp(tool_name, "move_jump_node") == 0) {
		handle_move_jump_node(input_json, req);
	} else if (strcmp(tool_name, "swap_jump_nodes") == 0) {
		handle_swap_jump_nodes(input_json, req);
	} else if (mcp_handle_waypoint_tool(tool_name, input_json, req)) {
		// handled by waypoints unit
	} else if (strcmp(tool_name, "get_mission_music") == 0) {
		handle_get_mission_music(input_json, req);
	} else if (strcmp(tool_name, "update_mission_music") == 0) {
		handle_update_mission_music(input_json, req);
	} else if (mcp_handle_sexp_tool(tool_name, input_json, req)) {
		// handled by SEXP unit
	} else if (mcp_handle_mission_info_tool(tool_name, input_json, req)) {
		// handled by mission-info unit
	} else {
		McpErrorSink sink(req);
		sink.set_error("Unknown mission tool: %s", tool_name);
	}
}
