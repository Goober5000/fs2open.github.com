#include "stdafx.h"
#include "mcp_debrief.h"
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

#include "mission/missionbriefcommon.h"
#include "parse/parselo.h"
#include "parse/sexp.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_debriefing(SCP_string &error_msg)
{
	return validate_single_dialog("debriefing stages", "debriefing", error_msg);
}

// ---------------------------------------------------------------------------
// Debriefing stage tools
// ---------------------------------------------------------------------------

// Resolve optional "team" parameter to a debriefing pointer.
// Defaults to Team 1. Rejects "none".
static debriefing *get_debriefing_for_team(json_t *input, McpErrorSink &sink)
{
	auto team_str = get_optional_string(input, "team", sink);
	if (sink.has_error()) return nullptr;
	int team_index = 0;  // default to Team 1

	if (team_str) {
		if (!check_string_enum(team_str, team_selector_enum_values, "team", sink))
			return nullptr;
		if (reject_team_none(team_str, "debriefing", sink)) return nullptr;
		team_index = team_index_from_name(team_str);
	}

	return &Debriefings[team_index];
}

static json_t *build_debrief_stage_json(const debrief_stage &stage, int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "text", json_safe_string(stage.text.c_str()));
	set_optional_filename(obj, "voice_filename", stage.voice);
	json_object_set_new(obj, "recommendation_text", json_safe_string(stage.recommendation_text.c_str()));
	json_object_set_new(obj, "formula", json_integer(stage.formula));
	return obj;
}

static void handle_list_debriefing_stages(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_debriefing, sink)) return;

	auto *db = get_debriefing_for_team(input, sink);
	if (!db) return;

	json_t *arr = json_array();
	for (int i = 0; i < db->num_stages; i++)
		json_array_append_new(arr, build_debrief_stage_json(db->stages[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_debriefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_debriefing, sink)) return;

	auto *db = get_debriefing_for_team(input, sink);
	if (!db) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, db->num_stages, "index", sink)) return;

	req->result_json = make_json_tool_result(build_debrief_stage_json(db->stages[*index - 1], *index - 1));
	req->success = true;
}

static void handle_create_debriefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_debriefing, sink)) return;

	auto *db = get_debriefing_for_team(input, sink);
	if (!db) return;

	if (db->num_stages >= MAX_DEBRIEF_STAGES) {
		sink.set_error("Cannot add more than %d debriefing stages", MAX_DEBRIEF_STAGES);
		return;
	}

	auto text = get_required_string(input, "text", sink, false, MULTITEXT_LENGTH - 1);
	if (!text) return;

	auto voice = get_optional_filename(input, "voice_filename", sink, false);
	auto rec_text = get_optional_string(input, "recommendation_text", sink, MULTITEXT_LENGTH - 1);
	auto formula = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, sink)) return;
	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	int target;
	if (!insert_index.has_value()) {
		target = db->num_stages;
	} else {
		if (!check_int_range(*insert_index, 1, db->num_stages + 1, "index", sink))
			return;
		target = *insert_index - 1;
	}

	if (!array_insert_slot(db->stages, db->num_stages, MAX_DEBRIEF_STAGES, target)) {
		sink.set_error("Cannot add more than %d debriefing stages", MAX_DEBRIEF_STAGES);
		return;
	}

	debrief_stage &s = db->stages[target];
	s.text = text;
	strcpy_s(s.voice, (voice && voice[0]) ? voice : "none");
	s.recommendation_text = rec_text ? rec_text : "";

	if (formula.has_value()) {
		s.formula = *formula;
	} else {
		s.formula = Locked_sexp_true;
	}

	mcp_sexp_forest_mark_dirty({ s.formula });
	mark_modified("MCP: create debriefing stage %d", target + 1);

	req->result_json = make_json_tool_result(build_debrief_stage_json(db->stages[target], target));
	req->success = true;
}

static void handle_update_debriefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_debriefing, sink)) return;

	auto *db = get_debriefing_for_team(input, sink);
	if (!db) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, db->num_stages, "index", sink)) return;

	auto new_text = get_optional_string(input, "text", sink, MULTITEXT_LENGTH - 1);
	auto new_voice = get_optional_filename(input, "voice_filename", sink, false);
	auto new_rec_text = get_optional_string(input, "recommendation_text", sink, MULTITEXT_LENGTH - 1);
	auto new_formula = get_optional_integer(input, "formula", sink);
	if (sink.has_error()) return;
	if (new_formula.has_value() && !check_sexp_formula(*new_formula, OPR_BOOL, sink)) return;

	debrief_stage &s = db->stages[*index - 1];
	bool changed = false;

	if (new_text && s.text != new_text) {
		s.text = new_text;
		changed = true;
	}
	if (new_voice && strcmp(s.voice, new_voice) != 0) {
		strcpy_s(s.voice, new_voice);
		changed = true;
	}
	if (new_rec_text && s.recommendation_text != new_rec_text) {
		s.recommendation_text = new_rec_text;
		changed = true;
	}
	if (new_formula.has_value() && replace_cue(s.formula, *new_formula))
		changed = true;

	if (changed)
		mark_modified("MCP: update debriefing stage %d", *index);

	req->result_json = make_json_tool_result(build_debrief_stage_json(s, *index - 1));
	req->success = true;
}

static void handle_delete_debriefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_debriefing, sink)) return;

	auto *db = get_debriefing_for_team(input, sink);
	if (!db) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, db->num_stages, "index", sink)) return;

	free_cue(db->stages[*index - 1].formula);

	array_remove_slot(db->stages, db->num_stages, *index - 1);

	mark_modified("MCP: delete debriefing stage %d", *index);
	sprintf(req->result_message,
		"Deleted debriefing stage %d", *index);
	req->success = true;
}

static MoveSwapConfig make_debriefing_move_swap_config(debriefing *db)
{
	MoveSwapConfig cfg;
	cfg.entity_name = "debriefing stage";
	cfg.count = db->num_stages;
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_debriefing;
	cfg.get_name = [](int index) {
		SCP_string name;
		sprintf(name, "debriefing stage %d", index);
		return name;
	};
	cfg.do_move = [db](int from, int to) {
		array_move_element(db->stages, db->num_stages, from - 1, to - 1);
	};
	cfg.do_swap = [db](int a, int b) {
		std::swap(db->stages[a - 1], db->stages[b - 1]);
	};
	return cfg;
}

static void handle_move_debriefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto *db = get_debriefing_for_team(input, sink);
	if (!db) return;

	auto cfg = make_debriefing_move_swap_config(db);
	handle_generic_move(input, req, cfg);
}

static void handle_swap_debriefing_stages(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto *db = get_debriefing_for_team(input, sink);
	if (!db) return;

	auto cfg = make_debriefing_move_swap_config(db);
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Debriefing background tools
// ---------------------------------------------------------------------------

static json_t *build_debriefing_background_json(const debriefing &db)
{
	json_t *obj = json_object();
	set_optional_filename(obj, "background_640", db.background[GR_640]);
	set_optional_filename(obj, "background_1024", db.background[GR_1024]);
	return obj;
}

static void handle_get_debriefing_background(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_debriefing, sink)) return;

	auto *db = get_debriefing_for_team(input, sink);
	if (!db) return;

	req->result_json = make_json_tool_result(build_debriefing_background_json(*db));
	req->success = true;
}

static void handle_update_debriefing_background(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_debriefing, sink)) return;

	auto *db = get_debriefing_for_team(input, sink);
	if (!db) return;

	auto bg640  = get_optional_filename(input, "background_640", sink, false);
	auto bg1024 = get_optional_filename(input, "background_1024", sink, false);
	if (sink.has_error()) return;

	bool changed = false;
	if (bg640 && strcmp(db->background[GR_640], bg640) != 0) {
		strcpy_s(db->background[GR_640], bg640);
		changed = true;
	}
	if (bg1024 && strcmp(db->background[GR_1024], bg1024) != 0) {
		strcpy_s(db->background[GR_1024], bg1024);
		changed = true;
	}

	if (changed)
		mark_modified("MCP: update debriefing background");

	req->result_json = make_json_tool_result(build_debriefing_background_json(*db));
	req->success = true;
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

static const char *debrief_team_desc =
	"Which team's debriefing to operate on (\"Team 1\" or \"Team 2\"). "
	"Defaults to \"Team 1\". \"none\" is not valid for debriefings.";

static void register_list_debriefing_stages(json_t *tools)
{
	json_t *props = json_object();
	add_string_enum_prop(props, "team", debrief_team_desc, team_selector_enum_values);
	register_tool(tools, "list_debriefing_stages",
		"List all debriefing stages for a team. Returns each stage's index, "
		"text, voice, recommendation text, and SEXP formula root node.",
		props);
}

static void register_get_debriefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index",
		"1-based index of the stage to retrieve");
	add_string_enum_prop(props, "team", debrief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "get_debriefing_stage",
		"Get full details of a debriefing stage by index.",
		props, req);
}

static void register_create_debriefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_string_prop(props, "text", "The debriefing text displayed during this stage");
	add_string_prop(props, "voice_filename",
		"Voice audio filename (wav/ogg). Defaults to empty (no voice).");
	add_string_prop(props, "recommendation_text",
		"Recommendation text displayed during this stage. Defaults to empty.");
	add_integer_prop(props, "formula", "Root node of the SEXP formula used for this stage. "
		"Defaults to true (stage always shown).");
	add_integer_prop(props, "index",
		"Position to insert the stage (1 = first). If omitted, appends to the end.");
	add_string_enum_prop(props, "team", debrief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("text"));
	register_tool(tools, "create_debriefing_stage",
		"Create a new debriefing stage. Debriefing stages are shown after a mission "
		"completes; each stage has a SEXP formula controlling whether it is displayed. "
		"Maximum " SCP_TOKEN_TO_STR(MAX_DEBRIEF_STAGES) " stages per team.",
		props, req);
}

static void register_update_debriefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index",
		"1-based index of the stage to update");
	add_string_prop(props, "text", "New debriefing text for this stage");
	add_string_prop(props, "voice_filename",
		"New voice audio filename. Empty string clears the voice.");
	add_string_prop(props, "recommendation_text",
		"New recommendation text. Empty string clears the recommendation.");
	add_integer_prop(props, "formula", "Root node of the SEXP formula used for this stage.");
	add_string_enum_prop(props, "team", debrief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "update_debriefing_stage",
		"Update properties of an existing debriefing stage. Only specified "
		"fields are changed; omitted fields are left unchanged.",
		props, req);
}

static void register_delete_debriefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index",
		"1-based index of the stage to delete");
	add_string_enum_prop(props, "team", debrief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "delete_debriefing_stage",
		"Delete a debriefing stage. Frees its SEXP formula. "
		"Remaining stages are shifted down.",
		props, req);
}

static void register_move_debriefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "from_index",
		"Current 1-based index of the stage");
	add_integer_prop(props, "to_index",
		"Target 1-based index to move the stage to");
	add_string_enum_prop(props, "team", debrief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("from_index"));
	json_array_append_new(req, json_string("to_index"));
	register_tool(tools, "move_debriefing_stage",
		"Move a debriefing stage from one position to another. "
		"Indices are 1-based.",
		props, req);
}

static void register_swap_debriefing_stages(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index_a",
		"1-based index of the first stage");
	add_integer_prop(props, "index_b",
		"1-based index of the second stage");
	add_string_enum_prop(props, "team", debrief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index_a"));
	json_array_append_new(req, json_string("index_b"));
	register_tool(tools, "swap_debriefing_stages",
		"Swap two debriefing stages at the given positions. "
		"Indices are 1-based.",
		props, req);
}

static void register_get_debriefing_background(json_t *tools)
{
	json_t *props = json_object();
	add_string_enum_prop(props, "team", debrief_team_desc, team_selector_enum_values);
	register_tool(tools, "get_debriefing_background",
		"Get the custom background images for a team's debriefing screen. "
		"The 640 variant is used at resolutions below 1024x768; the 1024 variant "
		"otherwise. Fields are omitted when no custom background is set (the "
		"standard background is used).",
		props);
}

static void register_update_debriefing_background(json_t *tools)
{
	json_t *props = json_object();
	add_string_prop(props, "background_640",
		"Debriefing screen background, 640x480 variant");
	add_string_prop(props, "background_1024",
		"Debriefing screen background, 1024x768 variant");
	add_string_enum_prop(props, "team", debrief_team_desc, team_selector_enum_values);
	register_tool(tools, "update_debriefing_background",
		"Update the custom background images for a team's debriefing screen. "
		"Only specified fields are changed; pass an empty string to clear a "
		"field back to the standard background.",
		props);
}

// ---------------------------------------------------------------------------
// Tool table
// ---------------------------------------------------------------------------

const McpToolDef mcp_debrief_tool_defs[] = {
	{ "list_debriefing_stages",  register_list_debriefing_stages,  nullptr, handle_list_debriefing_stages,  false },
	{ "get_debriefing_stage",    register_get_debriefing_stage,    nullptr, handle_get_debriefing_stage,    false },
	{ "create_debriefing_stage", register_create_debriefing_stage, nullptr, handle_create_debriefing_stage, false },
	{ "update_debriefing_stage", register_update_debriefing_stage, nullptr, handle_update_debriefing_stage, false },
	{ "delete_debriefing_stage", register_delete_debriefing_stage, nullptr, handle_delete_debriefing_stage, false },
	{ "move_debriefing_stage",   register_move_debriefing_stage,   nullptr, handle_move_debriefing_stage,   false },
	{ "swap_debriefing_stages",  register_swap_debriefing_stages,  nullptr, handle_swap_debriefing_stages,  false },
	{ "get_debriefing_background",    register_get_debriefing_background,    nullptr, handle_get_debriefing_background,    false },
	{ "update_debriefing_background", register_update_debriefing_background, nullptr, handle_update_debriefing_background, false },
};
const size_t mcp_debrief_tool_def_count = sizeof(mcp_debrief_tool_defs) / sizeof(mcp_debrief_tool_defs[0]);
