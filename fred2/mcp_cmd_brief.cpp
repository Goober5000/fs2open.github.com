#include "stdafx.h"
#include "mcp_cmd_brief.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "mcp_array_utils.h"

#include <jansson.h>
#include <cstring>

#include "globalincs/utility.h"

#include "missionui/missioncmdbrief.h"
#include "parse/parselo.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_cmd_brief(SCP_string &error_msg)
{
	return validate_single_dialog("command briefing stages", "command briefing", error_msg);
}

// ---------------------------------------------------------------------------
// Command briefing tool handlers (run on main thread)
// ---------------------------------------------------------------------------

// Resolve optional "team" parameter to a cmd_brief pointer.
// Returns nullptr and sets error on failure.
static cmd_brief *get_cmd_brief_for_team(json_t *input, McpErrorSink &sink)
{
	auto team_str = get_optional_string(input, "team", sink);
	if (sink.has_error()) return nullptr;
	int team_index = 0;  // default to Team 1

	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", sink))
			return nullptr;
		if (reject_team_none(team_str, "command briefing", sink)) return nullptr;
		team_index = team_index_from_name(team_str);
	}

	return &Cmd_briefs[team_index];
}

static json_t *build_cmd_brief_stage_json(const cmd_brief_stage &stage, int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "text", json_safe_string(stage.text.c_str()));
	if (stage.ani_filename && stricmp(stage.ani_filename, "<default>") != 0)
		set_optional_filename(obj, "animation_filename", stage.ani_filename);
	set_optional_filename(obj, "voice_filename", stage.wave_filename);
	return obj;
}

static void handle_list_cmd_brief_stages(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_cmd_brief, sink)) return;

	auto *cb = get_cmd_brief_for_team(input, sink);
	if (!cb) return;

	json_t *arr = json_array();
	for (int i = 0; i < cb->num_stages; i++)
		json_array_append_new(arr, build_cmd_brief_stage_json(cb->stage[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_cmd_brief_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_cmd_brief, sink)) return;

	auto *cb = get_cmd_brief_for_team(input, sink);
	if (!cb) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, cb->num_stages, "index", sink)) return;

	req->result_json = make_json_tool_result(build_cmd_brief_stage_json(cb->stage[*index - 1], *index - 1));
	req->success = true;
}

static void handle_create_cmd_brief_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_cmd_brief, sink)) return;

	auto *cb = get_cmd_brief_for_team(input, sink);
	if (!cb) return;

	if (cb->num_stages >= CMD_BRIEF_STAGES_MAX) {
		sink.set_error("Cannot add more than %d command briefing stages", CMD_BRIEF_STAGES_MAX);
		return;
	}

	auto text = get_required_string(input, "text", sink, false, MULTITEXT_LENGTH - 1);
	if (!text) return;

	auto ani = get_optional_filename(input, "animation_filename", sink, false, MAX_FILENAME_LEN - 1);
	auto wave = get_optional_filename(input, "voice_filename", sink, false, MAX_FILENAME_LEN - 1);
	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	// Resolve insert position
	int target;
	if (!insert_index.has_value()) {
		target = cb->num_stages;
	} else {
		// Upper bound is num_stages + 1 (append position)
		if (!check_int_range(*insert_index, 1, cb->num_stages + 1, "index", sink))
			return;
		target = *insert_index - 1;
	}

	// Insert slot
	if (!array_insert_slot(cb->stage, cb->num_stages, CMD_BRIEF_STAGES_MAX, target)) {
		sink.set_error("Cannot add more than %d command briefing stages", CMD_BRIEF_STAGES_MAX);
		return;
	}

	// Fill new stage
	cmd_brief_stage &s = cb->stage[target];
	s.text = text;
	strcpy_s(s.ani_filename, (ani && ani[0]) ? ani : "<default>");
	strcpy_s(s.wave_filename, (wave && wave[0]) ? wave : "none");
	s.wave = -1;

	mark_modified("MCP: create cmd brief stage %d", target + 1);

	req->result_json = make_json_tool_result(build_cmd_brief_stage_json(cb->stage[target], target));
	req->success = true;
}

static void handle_update_cmd_brief_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_cmd_brief, sink)) return;

	auto *cb = get_cmd_brief_for_team(input, sink);
	if (!cb) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, cb->num_stages, "index", sink)) return;

	auto new_text = get_optional_string(input, "text", sink, MULTITEXT_LENGTH - 1);
	auto new_ani  = get_optional_filename(input, "animation_filename", sink, false, MAX_FILENAME_LEN - 1);
	auto new_wave = get_optional_filename(input, "voice_filename", sink, false, MAX_FILENAME_LEN - 1);
	if (sink.has_error()) return;

	cmd_brief_stage &s = cb->stage[*index - 1];
	bool changed = false;

	if (new_text && strcmp(s.text.c_str(), new_text) != 0) {
		s.text = new_text;
		changed = true;
	}
	if (new_ani) {
		const char *effective_ani = new_ani[0] ? new_ani : "<default>";
		if (strcmp(s.ani_filename, effective_ani) != 0) {
			strcpy_s(s.ani_filename, effective_ani);
			changed = true;
		}
	}
	if (new_wave) {
		const char *effective_wave = new_wave[0] ? new_wave : "none";
		if (strcmp(s.wave_filename, effective_wave) != 0) {
			strcpy_s(s.wave_filename, effective_wave);
			changed = true;
		}
	}

	if (changed) {
		mark_modified("MCP: update cmd brief stage %d", *index);
	}

	req->result_json = make_json_tool_result(build_cmd_brief_stage_json(s, *index - 1));
	req->success = true;
}

static void handle_delete_cmd_brief_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_cmd_brief, sink)) return;

	auto *cb = get_cmd_brief_for_team(input, sink);
	if (!cb) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, cb->num_stages, "index", sink)) return;

	array_remove_slot(cb->stage, cb->num_stages, *index - 1);

	mark_modified("MCP: delete cmd brief stage %d", *index);

	sprintf(req->result_message,
		"Deleted command briefing stage %d", *index);
	req->success = true;
}

static MoveSwapConfig make_cmd_brief_move_swap_config(cmd_brief *cb)
{
	MoveSwapConfig cfg;
	cfg.entity_name = "cmd brief stage";
	cfg.count = cb->num_stages;
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_cmd_brief;
	cfg.get_name = [cb](int index) {
		SCP_string name;
		sprintf(name, "cmd brief stage %d", index);
		return name;
	};
	cfg.do_move = [cb](int from, int to) {
		array_move_element(cb->stage, cb->num_stages, from - 1, to - 1);
	};
	cfg.do_swap = [cb](int a, int b) {
		std::swap(cb->stage[a - 1], cb->stage[b - 1]);
	};
	return cfg;
}

static void handle_move_cmd_brief_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto *cb = get_cmd_brief_for_team(input, sink);
	if (!cb) return;

	auto cfg = make_cmd_brief_move_swap_config(cb);
	handle_generic_move(input, req, cfg);
}

static void handle_swap_cmd_brief_stages(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto *cb = get_cmd_brief_for_team(input, sink);
	if (!cb) return;

	auto cfg = make_cmd_brief_move_swap_config(cb);
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_cmd_brief_tools(json_t *tools)
{
	// -----------------------------------------------------------------------
	// Command briefing stage tools
	// -----------------------------------------------------------------------

	static const char *cmd_brief_team_desc =
		"Which team's command briefing to operate on (\"Team 1\" or \"Team 2\"). "
		"Defaults to \"Team 1\". \"none\" is not valid for command briefings.";

	// list_cmd_brief_stages
	{
		json_t *props = json_object();
		add_string_enum_prop(props, "team", cmd_brief_team_desc, team_enum_values);
		register_tool(tools, "list_cmd_brief_stages",
			"List all command briefing stages. Returns each stage's index, text, "
			"animation filename, and wave filename.",
			props);
	}

	// get_cmd_brief_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "index", "1-based index of the stage to retrieve");
		add_string_enum_prop(props, "team", cmd_brief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "get_cmd_brief_stage",
			"Get full details of a command briefing stage by index.",
			props, req);
	}

	// create_cmd_brief_stage
	{
		json_t *props = json_object();
		add_string_prop(props, "text", "The text displayed during this stage");
		add_string_prop(props, "animation_filename",
			"Animation filename (ani/eff/png). Defaults to \"<default>\".");
		add_string_prop(props, "voice_filename",
			"Voice audio filename (wav/ogg). Defaults to \"none\".");
		add_integer_prop(props, "index",
			"Position to insert the stage (1 = first). If omitted, appends to the end.");
		add_string_enum_prop(props, "team", cmd_brief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("text"));
		register_tool(tools, "create_cmd_brief_stage",
			"Create a new command briefing stage. Command briefings are narrated "
			"slideshows shown before the main briefing, with text, animation, and "
			"optional voice per stage. Maximum " SCP_TOKEN_TO_STR(CMD_BRIEF_STAGES_MAX) " stages.",
			props, req);
	}

	// update_cmd_brief_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "index", "1-based index of the stage to update");
		add_string_prop(props, "text", "New text for this stage");
		add_string_prop(props, "animation_filename", "New animation filename (ani/eff/png) (empty string to reset to default)");
		add_string_prop(props, "voice_filename", "New voice audio filename (wav/ogg) (empty string to clear)");
		add_string_enum_prop(props, "team", cmd_brief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "update_cmd_brief_stage",
			"Update properties of an existing command briefing stage. Only specified "
			"fields are changed; omitted fields are left unchanged.",
			props, req);
	}

	// delete_cmd_brief_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "index", "1-based index of the stage to delete");
		add_string_enum_prop(props, "team", cmd_brief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "delete_cmd_brief_stage",
			"Delete a command briefing stage. Remaining stages are shifted down.",
			props, req);
	}

	// move_cmd_brief_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"Current 1-based index of the stage");
		add_integer_prop(props, "to_index",
			"Target 1-based index to move the stage to");
		add_string_enum_prop(props, "team", cmd_brief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_cmd_brief_stage",
			"Move a command briefing stage from one position to another. "
			"Indices are 1-based.",
			props, req);
	}

	// swap_cmd_brief_stages
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first stage");
		add_integer_prop(props, "index_b",
			"1-based index of the second stage");
		add_string_enum_prop(props, "team", cmd_brief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_cmd_brief_stages",
			"Swap two command briefing stages at the given positions. "
			"Indices are 1-based.",
			props, req);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_cmd_brief_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_cmd_brief_stages") == 0) {
		handle_list_cmd_brief_stages(input_json, req);
	} else if (strcmp(tool_name, "get_cmd_brief_stage") == 0) {
		handle_get_cmd_brief_stage(input_json, req);
	} else if (strcmp(tool_name, "create_cmd_brief_stage") == 0) {
		handle_create_cmd_brief_stage(input_json, req);
	} else if (strcmp(tool_name, "update_cmd_brief_stage") == 0) {
		handle_update_cmd_brief_stage(input_json, req);
	} else if (strcmp(tool_name, "delete_cmd_brief_stage") == 0) {
		handle_delete_cmd_brief_stage(input_json, req);
	} else if (strcmp(tool_name, "move_cmd_brief_stage") == 0) {
		handle_move_cmd_brief_stage(input_json, req);
	} else if (strcmp(tool_name, "swap_cmd_brief_stages") == 0) {
		handle_swap_cmd_brief_stages(input_json, req);
	} else {
		return false;
	}
	return true;
}
