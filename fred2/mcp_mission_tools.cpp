#include "stdafx.h"
#include "mcp_mission_tools.h"
#include "mcp_messages.h"
#include "mcp_events.h"
#include "mcp_goals.h"
#include "mcp_waypoints.h"
#include "mcp_cmd_brief.h"
#include "mcp_debrief.h"
#include "mcp_fiction_viewer.h"
#include "mcp_jump_node.h"
#include "mcp_loadout.h"
#include "mcp_mission_info.h"
#include "mcp_reinforcements.h"
#include "mcp_ships.h"
#include "mcp_submodels.h"
#include "mcp_wings.h"
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
#include "missioneditor/common.h"     // target_to_anchor, stuff_special_arrival_anchor_name
#include "missionui/missioncmdbrief.h"
#include "mission/missionbriefcommon.h"
#include "mission/missionparse.h"
#include "ship/anchor_t.h"            // ANCHOR_SPECIAL_ARRIVAL
#include "ship/ship.h"
#include "parse/parselo.h"
#include "parse/sexp.h"
#include "object/waypoint.h"
#include "freddoc.h"
#include "fredrender.h"               // view_pos, view_orient
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

int lookup_ship(const char *name, McpErrorSink &sink)
{
	int idx = ship_name_lookup(name, 1);
	if (idx < 0)
		set_not_found_error(sink, "Ship", name);
	return idx;
}

int lookup_wing(const char *name, McpErrorSink &sink)
{
	int idx = wing_name_lookup(name);
	if (idx < 0)
		set_not_found_error(sink, "Wing", name);
	return idx;
}

// ---------------------------------------------------------------------------
// Enum helpers
// ---------------------------------------------------------------------------

const SCP_vector<const char *> team_enum_values = { "none", "Team 1", "Team 2" };

// Selector contexts (cmd brief, debrief, loadout) reject "none" at runtime,
// so advertising it in the schema enum would invite invalid calls.
const SCP_vector<const char *> team_selector_enum_values = { "Team 1", "Team 2" };

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
// Arrival/departure location enums (shared by ships and wings)
// ---------------------------------------------------------------------------

const SCP_vector<const char *> arrival_location_enum_values =
	SCP_vector<const char *>(std::begin(Arrival_location_names), std::end(Arrival_location_names));

const SCP_vector<const char *> departure_location_enum_values =
	SCP_vector<const char *>(std::begin(Departure_location_names), std::end(Departure_location_names));

// ---------------------------------------------------------------------------
// Anchor encoding (shared by ships and wings)
// ---------------------------------------------------------------------------

bool resolve_target_name_to_anchor(const char *name, anchor_t &out, McpErrorSink &sink)
{
	// "<none>" is the universal clear convention; it cannot collide with the
	// special anchors ("<any friendly>" etc.), which are checked below.
	if (!name || !*name || !stricmp(name, "<none>")) {
		out = anchor_t::invalid();
		return true;
	}

	auto special = get_special_anchor(name);
	if (special.isValid()) {
		out = special;
		return true;
	}

	int ship_idx = ship_name_lookup(name, 1);
	if (ship_idx >= 0) {
		out = target_to_anchor(ship_idx);
		return true;
	}

	sink.set_error("Unknown arrival/departure target: '%s' "
		"(not a known ship name or special anchor like '<any friendly>')", name);
	return false;
}

SCP_string anchor_to_name(anchor_t anchor)
{
	int raw = anchor.value();
	if (raw < 0)
		return "";

	if (raw & ANCHOR_SPECIAL_ARRIVAL) {
		char buf[NAME_LENGTH + 15];
		stuff_special_arrival_anchor_name(buf, raw, false);
		return buf;
	}

	auto entry = ship_registry_get(anchor);
	if (entry)
		return entry->name;

	return "";	// unresolved (e.g. dangling reference)
}

// ---------------------------------------------------------------------------
// SEXP cue replacement (shared by ships and wings)
// ---------------------------------------------------------------------------

bool replace_cue(int &cue_slot, int new_cue)
{
	if (cue_slot == new_cue)
		return false;

	SCP_vector<int> dirty;
	if (cue_slot >= 0 && cue_slot != Locked_sexp_true && cue_slot != Locked_sexp_false) {
		free_sexp2(cue_slot);
		dirty.push_back(cue_slot);
	}
	cue_slot = new_cue;
	dirty.push_back(new_cue);
	mcp_sexp_forest_mark_dirty(dirty);
	return true;
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
// Editor view tool handlers
// ---------------------------------------------------------------------------

static json_t *build_editor_view_json()
{
	json_t *obj = json_object();
	json_object_set_new(obj, "position", build_vec3d_json(view_pos));
	json_object_set_new(obj, "orientation", build_matrix_json(view_orient));
	return obj;
}

static void handle_get_editor_view(json_t * /*input*/, McpToolRequest *req)
{
	req->result_json = make_json_tool_result(build_editor_view_json());
	req->success = true;
}

static void handle_set_editor_view(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);

	auto pos = get_optional_vec3d(input, "position", sink);
	if (sink.has_error()) return;
	auto orient = get_optional_matrix(input, "orientation", sink);
	if (sink.has_error()) return;

	if (pos.has_value())
		view_pos = *pos;
	if (orient.has_value())
		view_orient = *orient;

	// do not mark the mission modified; these are transient values
	// (they are saved in the mission file, but only to let the mission designer
	// pick up where he left off)

	req->result_json = make_json_tool_result(build_editor_view_json());
	req->success = true;
}

// ---------------------------------------------------------------------------
// Known mission tool names (for routing)
// ---------------------------------------------------------------------------

static const char *mission_tool_names[] = {
	"get_mission_info",
	"update_mission_info",
	"get_custom_wing_names",
	"update_custom_wing_names",
	"list_fiction_viewer_stages",
	"get_fiction_viewer_stage",
	"create_fiction_viewer_stage",
	"update_fiction_viewer_stage",
	"delete_fiction_viewer_stage",
	"move_fiction_viewer_stage",
	"swap_fiction_viewer_stages",
	"list_cmd_brief_stages",
	"get_cmd_brief_stage",
	"create_cmd_brief_stage",
	"update_cmd_brief_stage",
	"delete_cmd_brief_stage",
	"move_cmd_brief_stage",
	"swap_cmd_brief_stages",
	"list_debriefing_stages",
	"get_debriefing_stage",
	"create_debriefing_stage",
	"update_debriefing_stage",
	"delete_debriefing_stage",
	"move_debriefing_stage",
	"swap_debriefing_stages",
	"list_messages",
	"get_message",
	"create_message",
	"update_message",
	"delete_message",
	"move_message",
	"swap_messages",
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
	"list_events",
	"get_event",
	"create_event",
	"update_event",
	"delete_event",
	"move_event",
	"swap_events",
	"list_goals",
	"get_goal",
	"create_goal",
	"update_goal",
	"delete_goal",
	"move_goal",
	"swap_goals",
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
	"list_jump_nodes",
	"get_jump_node",
	"create_jump_node",
	"update_jump_node",
	"delete_jump_node",
	"move_jump_node",
	"swap_jump_nodes",
	"list_ships",
	"get_ship",
	"create_ship",
	"update_ship",
	"delete_ship",
	"move_ship",
	"swap_ships",
	"dock_ships",
	"undock_ships",
	"undock_all_ships",
	"list_docked_group",
	"set_dock_leader",
	"list_ship_weapons",
	"get_ship_weapon_bank",
	"update_ship_weapon_bank",
	"get_max_ammo_for_bank",
	"list_ship_subsystems",
	"get_ship_subsystem",
	"update_ship_subsystem",
	"get_ship_special_explosion",
	"update_ship_special_explosion",
	"get_ship_special_hitpoints",
	"update_ship_special_hitpoints",
	"list_ship_submodels",
	"get_ship_submodel",
	"update_ship_submodel",
	"list_wings",
	"get_wing",
	"form_wing",
	"update_wing",
	"arrange_in_formation",
	"delete_wing",
	"disband_wing",
	"move_wing",
	"swap_wings",
	"get_mission_music",
	"update_mission_music",
	"get_editor_view",
	"set_editor_view",
	"list_reinforcements",
	"get_reinforcement",
	"set_reinforcement",
	"get_team_loadout",
	"update_team_loadout",
	"set_team_loadout_ship",
	"set_team_loadout_weapon",
	nullptr
};

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_mission_tools(json_t *tools)
{
	mcp_register_mission_info_tools(tools);
	mcp_register_fiction_viewer_tools(tools);
	mcp_register_cmd_brief_tools(tools);
	mcp_register_debrief_tools(tools);
	mcp_register_message_tools(tools);
	mcp_register_sexp_tools(tools);
	mcp_register_event_tools(tools);
	mcp_register_goal_tools(tools);
	mcp_register_waypoint_tools(tools);
	mcp_register_jump_node_tools(tools);
	mcp_register_ship_tools(tools);
	mcp_register_submodel_tools(tools);
	mcp_register_wing_tools(tools);
	mcp_register_reinforcement_tools(tools);
	mcp_register_loadout_tools(tools);

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

	// get_editor_view
	register_tool(tools, "get_editor_view",
		"Returns the FRED editor camera's current position and orientation.",
		nullptr);

	// set_editor_view
	{
		json_t *props = json_object();
		add_vec3d_prop(props, "position",
			"New camera world position. Omit to leave position unchanged.");
		add_matrix_prop(props, "orientation",
			"New camera orientation matrix (rvec/uvec/fvec). Omit to leave orientation unchanged.");
		register_tool(tools, "set_editor_view",
			"Update the FRED editor camera's position and/or orientation. "
			"Both parameters are optional; only provided fields are changed. "
			"Returns the full updated view state.",
			props);
	}
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
	if (mcp_handle_mission_info_tool(tool_name, input_json, req)) {
		// handled by mission-info unit
	} else if (mcp_handle_fiction_viewer_tool(tool_name, input_json, req)) {
		// handled by fiction_viewer unit
	} else if (mcp_handle_cmd_brief_tool(tool_name, input_json, req)) {
		// handled by cmd_brief unit
	} else if (mcp_handle_debrief_tool(tool_name, input_json, req)) {
		// handled by debrief unit
	} else if (mcp_handle_message_tool(tool_name, input_json, req)) {
		// handled by messages unit
	} else if (mcp_handle_sexp_tool(tool_name, input_json, req)) {
		// handled by SEXP unit
	} else if (mcp_handle_event_tool(tool_name, input_json, req)) {
		// handled by events unit
	} else if (mcp_handle_goal_tool(tool_name, input_json, req)) {
		// handled by goals unit
	} else if (mcp_handle_waypoint_tool(tool_name, input_json, req)) {
		// handled by waypoints unit
	} else if (mcp_handle_jump_node_tool(tool_name, input_json, req)) {
		// handled by jump_node unit
	} else if (mcp_handle_ship_tool(tool_name, input_json, req)) {
		// handled by ships unit
	} else if (mcp_handle_submodel_tool(tool_name, input_json, req)) {
		// handled by submodels unit
	} else if (mcp_handle_wing_tool(tool_name, input_json, req)) {
		// handled by wings unit
	} else if (mcp_handle_reinforcement_tool(tool_name, input_json, req)) {
		// handled by reinforcements unit
	} else if (mcp_handle_loadout_tool(tool_name, input_json, req)) {
		// handled by team loadout unit
	} else if (strcmp(tool_name, "get_mission_music") == 0) {
		handle_get_mission_music(input_json, req);
	} else if (strcmp(tool_name, "update_mission_music") == 0) {
		handle_update_mission_music(input_json, req);
	} else if (strcmp(tool_name, "get_editor_view") == 0) {
		handle_get_editor_view(input_json, req);
	} else if (strcmp(tool_name, "set_editor_view") == 0) {
		handle_set_editor_view(input_json, req);
	} else {
		McpErrorSink sink(req);
		sink.set_error("Unknown mission tool: %s", tool_name);
	}
}
