#include "stdafx.h"
#include "mcp_goals.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "mcp_array_utils.h"
#include "mcp_sexp.h"
#include "mcp_sexp_forest.h"

#include <jansson.h>
#include <cstring>

#include "globalincs/utility.h"

#include "mission/missiongoals.h"
#include "parse/parselo.h"
#include "parse/sexp.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_goals(SCP_string &error_msg)
{
	return validate_single_dialog("goals", "goal", error_msg);
}

// ---------------------------------------------------------------------------
// Goal tool handlers (run on main thread)
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> goal_type_enum_values = { "Primary", "Secondary", "Bonus" };

static const char *goal_type_name(int type)
{
	switch (type & GOAL_TYPE_MASK) {
		case PRIMARY_GOAL:   return "Primary";
		case SECONDARY_GOAL: return "Secondary";
		case BONUS_GOAL:     return "Bonus";
		default:             return "Unknown";
	}
}

static int goal_type_from_name(const char *name)
{
	if (!stricmp(name, "Primary"))   return PRIMARY_GOAL;
	if (!stricmp(name, "Secondary")) return SECONDARY_GOAL;
	if (!stricmp(name, "Bonus"))     return BONUS_GOAL;
	return -1;
}

static json_t *build_goal_json(const mission_goal &goal, int index, bool include_details = false)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_safe_string(goal.name.c_str()));
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "goal_type", json_string(goal_type_name(goal.type)));
	json_object_set_new(obj, "formula", json_integer(goal.formula));

	if (include_details) {
		json_object_set_new(obj, "is_valid", json_boolean(!(goal.type & INVALID_GOAL)));
		json_object_set_new(obj, "message", json_safe_string(goal.message.c_str()));
		json_object_set_new(obj, "score", json_integer(goal.score));
		json_object_set_new(obj, "team", json_safe_string(team_name_from_index(goal.team)));
		json_object_set_new(obj, "no_music", json_boolean(goal.flags & MGF_NO_MUSIC));
	}

	return obj;
}

static void handle_list_goals(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_goals, sink)) return;

	json_t *arr = json_array();
	for (int i = 0; i < (int)Mission_goals.size(); i++)
		json_array_append_new(arr, build_goal_json(Mission_goals[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_goal(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_goals, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int idx = mission_goal_lookup(name);
	if (idx < 0) {
		set_not_found_error(sink,"Goal", name);
		return;
	}

	req->result_json = make_json_tool_result(build_goal_json(Mission_goals[idx], idx, true));
	req->success = true;
}

static void handle_create_goal(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_goals, sink)) return;

	auto name = get_required_string(input, "name", sink, true, NAME_LENGTH - 1);
	if (!name) return;

	// Check for duplicate name
	if (mission_goal_lookup(name) >= 0) {
		sink.set_error("A goal with name '%s' already exists", name);
		return;
	}

	auto insert_index = get_optional_integer(input, "index", sink);

	// Optional parameters
	auto type_str   = get_optional_string(input, "goal_type", sink);
	auto formula    = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, sink)) return;
	auto message    = get_optional_string(input, "message", sink, MESSAGE_LENGTH - 1);
	auto score      = get_optional_integer(input, "score", sink);
	auto team_str   = get_optional_string(input, "team", sink);
	auto invalid    = get_optional_bool(input, "invalid", sink);
	auto no_music   = get_optional_bool(input, "no_music", sink);
	if (sink.has_error()) return;

	// Resolve type
	int goal_type = PRIMARY_GOAL;
	if (type_str) {
		if (!check_string_enum(type_str, goal_type_enum_values, "goal_type", sink))
			return;
		goal_type = goal_type_from_name(type_str);
	}

	// Resolve team
	int team = 0;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", sink))
			return;
		team = team_index_from_name(team_str);
	}

	// Validate insert index
	int target_index;
	if (!insert_index.has_value()) {
		target_index = (int)Mission_goals.size();
	} else {
		if (!check_int_range(*insert_index, 1, (int)Mission_goals.size() + 1, "index", sink))
			return;
		target_index = *insert_index - 1;
	}

	if (!formula.has_value()) {
		formula = Locked_sexp_true;
	}

	// Construct the goal
	mission_goal goal;
	goal.name = name;
	goal.type = goal_type;
	goal.formula = *formula;
	goal.score = score.value_or(0);
	goal.team = team;
	goal.flags = 0;

	if (message)
		goal.message = message;

	if (invalid.has_value() && *invalid)
		goal.type |= INVALID_GOAL;

	if (no_music.has_value() && *no_music)
		goal.flags |= MGF_NO_MUSIC;

	// Insert
	Mission_goals.insert(Mission_goals.begin() + target_index, goal);
	mcp_sexp_forest_mark_dirty({ *formula });

	mark_modified("MCP: create goal %s", name);

	req->result_json = make_json_tool_result(build_goal_json(Mission_goals[target_index], target_index, true));
	req->success = true;
}

static void handle_update_goal(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_goals, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int idx = mission_goal_lookup(name);
	if (idx < 0) {
		set_not_found_error(sink,"Goal", name);
		return;
	}

	mission_goal &goal = Mission_goals[idx];

	// Validate new_name
	auto new_name = get_optional_string(input, "new_name", sink, NAME_LENGTH - 1);
	if (new_name) {
		if (!check_general_rename(new_name, goal.name.c_str(),
			[](const char *n) { return mission_goal_lookup(n) >= 0; },
			"Goal", sink)) return;
	}

	// Extract optional fields
	auto type_str   = get_optional_string(input, "goal_type", sink);
	auto formula    = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, sink)) return;
	auto message    = get_optional_string(input, "message", sink, MESSAGE_LENGTH - 1);
	auto score      = get_optional_integer(input, "score", sink);
	auto team_str   = get_optional_string(input, "team", sink);
	auto invalid    = get_optional_bool(input, "invalid", sink);
	auto no_music   = get_optional_bool(input, "no_music", sink);
	if (sink.has_error()) return;

	// Validate type
	int new_type = -1;
	if (type_str) {
		if (!check_string_enum(type_str, goal_type_enum_values, "goal_type", sink))
			return;
		new_type = goal_type_from_name(type_str);
	}

	// Validate team
	std::optional<int> new_team = std::nullopt;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", sink))
			return;
		new_team = team_index_from_name(team_str);
	}

	bool changed = false;

	if (new_type >= 0) {
		// Preserve INVALID_GOAL bit, replace type bits
		int updated = (goal.type & INVALID_GOAL) | new_type;
		if (goal.type != updated) {
			goal.type = updated;
			changed = true;
		}
	}
	if (formula.has_value() && goal.formula != *formula) {
		if (goal.formula >= 0)
			free_sexp2(goal.formula);
		goal.formula = *formula;
		changed = true;
		mcp_sexp_forest_mark_dirty({ *formula });
	}
	if (message && strcmp(goal.message.c_str(), message) != 0) {
		goal.message = message;
		changed = true;
	}
	if (score.has_value() && goal.score != *score) {
		goal.score = *score;
		changed = true;
	}
	if (new_team.has_value() && goal.team != *new_team) {
		goal.team = *new_team;
		changed = true;
	}
	if (invalid.has_value()) {
		bool currently_invalid = (goal.type & INVALID_GOAL) != 0;
		if (*invalid != currently_invalid) {
			if (*invalid)
				goal.type |= INVALID_GOAL;
			else
				goal.type &= ~INVALID_GOAL;
			changed = true;
		}
	}
	if (no_music.has_value()) {
		bool currently_no_music = (goal.flags & MGF_NO_MUSIC) != 0;
		if (*no_music != currently_no_music) {
			if (*no_music)
				goal.flags |= MGF_NO_MUSIC;
			else
				goal.flags &= ~MGF_NO_MUSIC;
			changed = true;
		}
	}

	// Rename last — updates SEXP references
	if (new_name && stricmp(goal.name.c_str(), new_name) != 0) {
		update_sexp_references(goal.name.c_str(), new_name, OPF_GOAL_NAME);
		mcp_sexp_forest_mark_dirty();
		goal.name = new_name;
		changed = true;
	}

	if (changed)
		mark_modified("MCP: update goal %s", goal.name.c_str());

	req->result_json = make_json_tool_result(build_goal_json(goal, idx, true));
	req->success = true;
}

static void handle_delete_goal(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_goals, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int idx = mission_goal_lookup(name);
	if (idx < 0) {
		set_not_found_error(sink,"Goal", name);
		return;
	}

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	if (!check_and_report_sexp_refs(sexp_ref_type::NON_OBJECT, "Goal", Mission_goals[idx].name.c_str(), force, sink))
		return;

	// Invalidate SEXP references
	SCP_string buf = SCP_string("<") + Mission_goals[idx].name + ">";
	update_sexp_references(Mission_goals[idx].name.c_str(), buf.c_str(), OPF_GOAL_NAME);
	mcp_sexp_forest_mark_dirty();

	// Free the SEXP formula
	if (Mission_goals[idx].formula >= 0)
		free_sexp2(Mission_goals[idx].formula);

	Mission_goals.erase(Mission_goals.begin() + idx);

	mark_modified("MCP: delete goal %s", name);

	sprintf(req->result_message, "Deleted goal: %s", name);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Move/swap config and handlers
// ---------------------------------------------------------------------------

static MoveSwapConfig make_goal_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "goal";
	cfg.count = (int)Mission_goals.size();
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_goals;
	cfg.get_name = [](int i) {
		return Mission_goals[i - 1].name;
	};
	cfg.do_move = [](int from, int to) {
		array_move_element(Mission_goals, from - 1, to - 1);
	};
	cfg.do_swap = [](int a, int b) {
		std::swap(Mission_goals[a - 1], Mission_goals[b - 1]);
	};
	return cfg;
}

static void handle_move_goal(json_t *input, McpToolRequest *req)
{
	auto cfg = make_goal_move_swap_config();
	handle_generic_move(input, req, cfg);
}

static void handle_swap_goals(json_t *input, McpToolRequest *req)
{
	auto cfg = make_goal_move_swap_config();
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_goal_tools(json_t *tools)
{
	// list_goals
	register_tool(tools, "list_goals",
		"List all mission goals. Returns each goal's name, index, type "
		"(Primary/Secondary/Bonus), and SEXP formula root node.",
		json_object());

	// get_goal
	register_tool_with_required_string(tools, "get_goal",
		"Get full details of a mission goal by name, including type, message, "
		"score, team, validity, no_music flag, and SEXP formula root node.",
		"name", "Name of the goal to retrieve");

	// create_goal
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Unique name for the goal");
		add_string_enum_prop(props, "goal_type",
			"Goal type (default: \"Primary\")",
			goal_type_enum_values);
		add_integer_prop(props, "formula", "Root node of the SEXP formula used for this goal");
		add_string_prop(props, "message", "Brief description of the goal objective");
		add_integer_prop(props, "score", "Score awarded when goal is completed");
		add_string_enum_prop(props, "team",
			"Multiplayer team assignment (default: \"Team 1\")",
			team_enum_values);
		add_bool_prop(props, "invalid",
			"If true, the goal is marked as invalid (not evaluated during a mission). "
			"Note that goals can be validated and invalidated during a mission.");
		add_bool_prop(props, "no_music",
			"If true, no event music plays when goal is achieved");
		add_integer_prop(props, "index",
			"Position to insert the goal (1 = first). If omitted, appends to the end.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "create_goal",
			"Create a new mission goal. Mission goals are objectives for the player to "
			"accomplish during a mission.",
			props, req);
	}

	// update_goal
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the existing goal to update");
		add_string_prop(props, "new_name", "New name for the goal");
		add_string_enum_prop(props, "goal_type",
			"Goal type",
			goal_type_enum_values);
		add_integer_prop(props, "formula", "Root node of the SEXP formula used for this goal");
		add_string_prop(props, "message",
			"Brief description of the goal objective");
		add_integer_prop(props, "score", "Score awarded when goal is completed");
		add_string_enum_prop(props, "team",
			"Multiplayer team assignment",
			team_enum_values);
		add_bool_prop(props, "invalid",
			"If true, the goal is marked as invalid (not evaluated during a mission). "
			"Note that goals can be validated and invalidated during a mission.");
		add_bool_prop(props, "no_music",
			"If true, no event music plays when goal is achieved");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_goal",
			"Update properties of an existing mission goal. Only specified fields are "
			"changed; omitted fields are left unchanged. Renaming automatically updates "
			"all SEXP references to the goal.",
			props, req);
	}

	// delete_goal
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the goal to delete");
		add_bool_prop(props, "force",
			"If true, delete even if the goal is referenced in SEXPs (references "
			"will be invalidated). Default: false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "delete_goal",
			"Delete a mission goal. Frees its SEXP formula. "
			"SEXP references to the deleted goal are invalidated (wrapped in angle brackets).",
			props, req);
	}

	// move_goal
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"Current 1-based index of the goal");
		add_integer_prop(props, "to_index",
			"Target 1-based index to move the goal to");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_goal",
			"Move a mission goal from one position to another. "
			"Indices are 1-based.",
			props, req);
	}

	// swap_goals
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first goal");
		add_integer_prop(props, "index_b",
			"1-based index of the second goal");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_goals",
			"Swap two mission goals at the given positions. "
			"Indices are 1-based.",
			props, req);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_goal_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_goals") == 0) {
		handle_list_goals(input_json, req);
	} else if (strcmp(tool_name, "get_goal") == 0) {
		handle_get_goal(input_json, req);
	} else if (strcmp(tool_name, "create_goal") == 0) {
		handle_create_goal(input_json, req);
	} else if (strcmp(tool_name, "update_goal") == 0) {
		handle_update_goal(input_json, req);
	} else if (strcmp(tool_name, "delete_goal") == 0) {
		handle_delete_goal(input_json, req);
	} else if (strcmp(tool_name, "move_goal") == 0) {
		handle_move_goal(input_json, req);
	} else if (strcmp(tool_name, "swap_goals") == 0) {
		handle_swap_goals(input_json, req);
	} else {
		return false;
	}
	return true;
}
