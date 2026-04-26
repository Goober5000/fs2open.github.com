#include "stdafx.h"
#include "mcp_mission_tools.h"
#include "mcp_messages.h"
#include "mcp_events.h"
#include "mcp_goals.h"
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
#include "missionui/fictionviewer.h"
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

static bool validate_dialog_for_cmd_brief(SCP_string &error_msg)
{
	return validate_single_dialog("command briefing stages", "command briefing", error_msg);
}

static bool validate_dialog_for_fiction(SCP_string &error_msg)
{
	return validate_single_dialog("fiction viewer stages", "fiction viewer", error_msg);
}

static bool validate_dialog_for_debriefing(SCP_string &error_msg)
{
	return validate_single_dialog("debriefing stages", "debriefing", error_msg);
}

static bool validate_dialog_for_jump_nodes(SCP_string &error_msg)
{
	return validate_single_dialog("jump nodes", "jump node", error_msg);
}

static bool validate_dialog_for_waypoint_lists(SCP_string &error_msg)
{
	return validate_single_dialog("waypoint lists", "waypoint", error_msg);
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

static bool reject_team_none(const char *team_str, const char *entity_name, McpErrorSink &sink)
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
		json_object_set_new(data, "name", json_string(cfg.get_name(*from_index).c_str()));
		json_object_set_new(data, "index", json_integer(*from_index));
		req->result_json = make_json_tool_result(data);
		req->success = true;
		return;
	}

	cfg.do_move(*from_index, *to_index);

	mark_modified("MCP: move %s %s from %d to %d", cfg.entity_name, cfg.get_name(*to_index).c_str(), *from_index, *to_index);

	json_t *data = json_object();
	json_object_set_new(data, "name", json_string(cfg.get_name(*to_index).c_str()));
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
	json_object_set_new(a_obj, "name", json_string(cfg.get_name(*index_a).c_str()));
	json_object_set_new(a_obj, "index", json_integer(*index_a));
	json_t *b_obj = json_object();
	json_object_set_new(b_obj, "name", json_string(cfg.get_name(*index_b).c_str()));
	json_object_set_new(b_obj, "index", json_integer(*index_b));
	json_object_set_new(data, "a", a_obj);
	json_object_set_new(data, "b", b_obj);
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Entity-specific move/swap configs
// ---------------------------------------------------------------------------

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
	json_object_set_new(obj, "text", json_string(stage.text.c_str()));
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

	auto text = get_required_string(input, "text", sink, false);
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

	auto new_text = get_optional_string(input, "text", sink);
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
// Fiction Viewer stage tools
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> fiction_ui_name_values = { "FS2", "WCS" };

static json_t *build_fiction_viewer_stage_json(const fiction_viewer_stage &stage, int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "index", json_integer(index + 1));
	set_optional_filename(obj, "story_filename", stage.story_filename);
	set_optional_filename(obj, "font_filename", stage.font_filename);
	set_optional_filename(obj, "voice_filename", stage.voice_filename);
	set_optional_string(obj, "ui_name", stage.ui_name, true);
	set_optional_filename(obj, "background_640", stage.background[0]);
	set_optional_filename(obj, "background_1024", stage.background[1]);
	json_object_set_new(obj, "formula", json_integer(stage.formula));
	return obj;
}

static void handle_list_fiction_viewer_stages(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_fiction, sink)) return;

	json_t *arr = json_array();
	for (int i = 0; i < (int)Fiction_viewer_stages.size(); i++)
		json_array_append_new(arr, build_fiction_viewer_stage_json(Fiction_viewer_stages[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_fiction_viewer_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_fiction, sink)) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, (int)Fiction_viewer_stages.size(), "index", sink)) return;

	req->result_json = make_json_tool_result(build_fiction_viewer_stage_json(Fiction_viewer_stages[*index - 1], *index - 1));
	req->success = true;
}

static void handle_create_fiction_viewer_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_fiction, sink)) return;

	auto story = get_required_filename(input, "story_filename", sink, true, MAX_FILENAME_LEN - 1);
	if (!story) return;

	auto font  = get_optional_filename(input, "font_filename", sink, false, MAX_FILENAME_LEN - 1);
	auto voice = get_optional_filename(input, "voice_filename", sink, false, MAX_FILENAME_LEN - 1);
	auto ui    = get_optional_string(input, "ui_name", sink);
	auto bg640 = get_optional_filename(input, "background_640", sink, false, MAX_FILENAME_LEN - 1);
	auto bg1024 = get_optional_filename(input, "background_1024", sink, false, MAX_FILENAME_LEN - 1);
	auto formula = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, sink)) return;
	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	if (ui && !check_string_enum(ui, fiction_ui_name_values, "ui_name", sink)) return;

	int target;
	if (!insert_index.has_value()) {
		target = (int)Fiction_viewer_stages.size();
	} else {
		if (!check_int_range(*insert_index, 1, (int)Fiction_viewer_stages.size() + 1, "index", sink))
			return;
		target = *insert_index - 1;
	}

	fiction_viewer_stage stage;
	memset(&stage, 0, sizeof(fiction_viewer_stage));
	strcpy_s(stage.story_filename, story);
	if (font && font[0])  strcpy_s(stage.font_filename, font);
	if (voice && voice[0]) strcpy_s(stage.voice_filename, voice);
	if (ui && ui[0])    strcpy_s(stage.ui_name, ui);
	if (bg640 && bg640[0]) strcpy_s(stage.background[0], bg640);
	if (bg1024 && bg1024[0]) strcpy_s(stage.background[1], bg1024);

	if (formula.has_value()) {
		stage.formula = *formula;
	} else {
		stage.formula = Locked_sexp_true;
	}

	Fiction_viewer_stages.insert(Fiction_viewer_stages.begin() + target, stage);
	mcp_sexp_forest_mark_dirty({ stage.formula });

	mark_modified("MCP: create fiction viewer stage %d", target + 1);

	req->result_json = make_json_tool_result(build_fiction_viewer_stage_json(Fiction_viewer_stages[target], target));
	req->success = true;
}

static void handle_update_fiction_viewer_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_fiction, sink)) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, (int)Fiction_viewer_stages.size(), "index", sink)) return;

	auto new_story = get_optional_filename(input, "story_filename", sink, true, MAX_FILENAME_LEN - 1);
	auto new_font  = get_optional_filename(input, "font_filename", sink, false, MAX_FILENAME_LEN - 1);
	auto new_voice = get_optional_filename(input, "voice_filename", sink, false, MAX_FILENAME_LEN - 1);
	auto new_ui    = get_optional_string(input, "ui_name", sink);
	auto new_bg640 = get_optional_filename(input, "background_640", sink, false, MAX_FILENAME_LEN - 1);
	auto new_bg1024 = get_optional_filename(input, "background_1024", sink, false, MAX_FILENAME_LEN - 1);
	auto new_formula = get_optional_integer(input, "formula", sink);
	if (sink.has_error()) return;
	if (new_formula.has_value() && !check_sexp_formula(*new_formula, OPR_BOOL, sink)) return;

	if (new_ui) {
		if (!new_ui[0])
			new_ui = fiction_ui_name_values[0];
		else if (!check_string_enum(new_ui, fiction_ui_name_values, "ui_name", sink))
			return;
	}

	fiction_viewer_stage &s = Fiction_viewer_stages[*index - 1];
	bool changed = false;

	if (new_story && strcmp(s.story_filename, new_story) != 0) {
		strcpy_s(s.story_filename, new_story);
		changed = true;
	}
	if (new_font && strcmp(s.font_filename, new_font) != 0) {
		strcpy_s(s.font_filename, new_font);
		changed = true;
	}
	if (new_voice && strcmp(s.voice_filename, new_voice) != 0) {
		strcpy_s(s.voice_filename, new_voice);
		changed = true;
	}
	if (new_ui && strcmp(s.ui_name, new_ui) != 0) {
		strcpy_s(s.ui_name, new_ui);
		changed = true;
	}
	if (new_bg640 && strcmp(s.background[0], new_bg640) != 0) {
		strcpy_s(s.background[0], new_bg640);
		changed = true;
	}
	if (new_bg1024 && strcmp(s.background[1], new_bg1024) != 0) {
		strcpy_s(s.background[1], new_bg1024);
		changed = true;
	}
	if (new_formula.has_value() && s.formula != *new_formula) {
		if (s.formula >= 0)
			free_sexp2(s.formula);
		s.formula = *new_formula;
		changed = true;
		mcp_sexp_forest_mark_dirty({ *new_formula });
	}

	if (changed)
		mark_modified("MCP: update fiction viewer stage %d", *index);

	req->result_json = make_json_tool_result(build_fiction_viewer_stage_json(s, *index - 1));
	req->success = true;
}

static void handle_delete_fiction_viewer_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_fiction, sink)) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, (int)Fiction_viewer_stages.size(), "index", sink)) return;

	// Free the SEXP formula
	int formula = Fiction_viewer_stages[*index - 1].formula;
	if (formula >= 0)
		free_sexp2(formula);

	Fiction_viewer_stages.erase(Fiction_viewer_stages.begin() + *index - 1);

	mark_modified("MCP: delete fiction viewer stage %d", *index);
	sprintf(req->result_message,
		"Deleted fiction viewer stage %d", *index);
	req->success = true;
}

static MoveSwapConfig make_fiction_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "fiction viewer stage";
	cfg.count = (int)Fiction_viewer_stages.size();
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_fiction;
	cfg.get_name = [](int index) {
		SCP_string name;
		sprintf(name, "fiction viewer stage %d", index);
		return name;
	};
	cfg.do_move = [](int from, int to) {
		array_move_element(Fiction_viewer_stages, from - 1, to - 1);
	};
	cfg.do_swap = [](int a, int b) {
		std::swap(Fiction_viewer_stages[a - 1], Fiction_viewer_stages[b - 1]);
	};
	return cfg;
}

static void handle_move_fiction_viewer_stage(json_t *input, McpToolRequest *req)
{
	auto cfg = make_fiction_move_swap_config();
	handle_generic_move(input, req, cfg);
}

static void handle_swap_fiction_viewer_stages(json_t *input, McpToolRequest *req)
{
	auto cfg = make_fiction_move_swap_config();
	handle_generic_swap(input, req, cfg);
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
		if (!check_string_enum(team_str, team_enum_values, "team", sink))
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
	json_object_set_new(obj, "text", json_string(stage.text.c_str()));
	set_optional_filename(obj, "voice_filename", stage.voice);
	json_object_set_new(obj, "recommendation_text", json_string(stage.recommendation_text.c_str()));
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

	auto text = get_required_string(input, "text", sink, false);
	if (!text) return;

	auto voice = get_optional_filename(input, "voice_filename", sink, false, MAX_FILENAME_LEN - 1);
	auto rec_text = get_optional_string(input, "recommendation_text", sink);
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

	auto new_text = get_optional_string(input, "text", sink);
	auto new_voice = get_optional_filename(input, "voice_filename", sink, false, MAX_FILENAME_LEN - 1);
	auto new_rec_text = get_optional_string(input, "recommendation_text", sink);
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
	if (new_formula.has_value() && s.formula != *new_formula) {
		int old_formula = s.formula;
		if (old_formula >= 0)
			free_sexp2(old_formula);
		s.formula = *new_formula;
		mcp_sexp_forest_mark_dirty({ s.formula });
		changed = true;
	}

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

	int formula = db->stages[*index - 1].formula;
	if (formula >= 0)
		free_sexp2(formula);

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
// Jump node tools
// ---------------------------------------------------------------------------

static json_t *build_jump_node_json(const CJumpNode &jn, int index, bool include_details = false)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(jn.GetName()));
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "position", build_vec3d_json(*jn.GetPosition()));

	if (include_details) {
		if (jn.HasDisplayName())
			json_object_set_new(obj, "display_name", json_string(jn.GetDisplayName()));
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
	json_object_set_new(obj, "name", json_string(wl.get_name()));
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
	json_object_set_new(obj, "list", json_string(list_name));
	json_object_set_new(obj, "index", json_integer(one_based_index));
	json_object_set_new(obj, "name", json_string(wpt_name));
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
		json_object_set_new(m, s.key, json_string(Spooled_music.in_bounds(idx) ? Spooled_music[idx].name : "None"));
	}

	// Event music
	json_object_set_new(m, "event_music", json_string(Soundtracks.in_bounds(Current_soundtrack_num) ? Soundtracks[Current_soundtrack_num].name : "None"));

	// Substitute music
	struct { const char *key; const char *ref; } substitutes[] = {
		{ "substitute_briefing_music", The_mission.substitute_briefing_music_name },
		{ "substitute_event_music",    The_mission.substitute_event_music_name    },
	};
	for (auto &s : substitutes) {
		if (s.ref[0] && stricmp(s.ref, "None") != 0)
			json_object_set_new(m, s.key, json_string(s.ref));
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

	mcp_register_goal_tools(tools);

	// -----------------------------------------------------------------------
	// Fiction Viewer tools
	// -----------------------------------------------------------------------

	// list_fiction_viewer_stages
	register_tool(tools, "list_fiction_viewer_stages",
		"List all fiction viewer stages. Returns each stage's index, story filename, "
		"font, voice, UI name, backgrounds, and SEXP formula root node.",
		json_object());

	// get_fiction_viewer_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "index",
			"1-based index of the stage to retrieve");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "get_fiction_viewer_stage",
			"Get full details of a fiction viewer stage by index.",
			props, req);
	}

	// create_fiction_viewer_stage
	{
		json_t *props = json_object();
		add_string_prop(props, "story_filename",
			"Text filename for the fiction stage (e.g. \"fiction.txt\"). Max " SCP_TOKEN_TO_STR(MAX_FILENAME_LEN_1) " characters.");
		add_string_prop(props, "font_filename",
			"Font name from list_fonts. Defaults to empty (uses default font).");
		add_string_prop(props, "voice_filename",
			"Voice audio filename (wav/ogg). Defaults to empty (no voice).");
		add_string_enum_prop(props, "ui_name",
			"UI layout name. Defaults to empty (engine default).",
			fiction_ui_name_values);
		add_string_prop(props, "background_640",
			"Background image for 640x480 resolution. Defaults to empty (standard background).");
		add_string_prop(props, "background_1024",
			"Background image for 1024x768 resolution. Defaults to empty (standard background).");
		add_integer_prop(props, "formula", "Root node of the SEXP formula used for this stage. "
			"Defaults to true.");
		add_integer_prop(props, "index",
			"Position to insert the stage (1 = first). If omitted, appends to the end.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("story_filename"));
		register_tool(tools, "create_fiction_viewer_stage",
			"Create a new fiction viewer stage. Fiction viewer stages display story "
			"text between missions, with optional font, voice, background, and "
			"SEXP-controlled activation. Multiple stages can exist; only the first "
			"whose formula evaluates to true is shown at runtime.",
			props, req);
	}

	// update_fiction_viewer_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "index",
			"1-based index of the stage to update");
		add_string_prop(props, "story_filename",
			"New text filename for the fiction stage. Max " SCP_TOKEN_TO_STR(MAX_FILENAME_LEN_1) " characters.");
		add_string_prop(props, "font_filename",
			"New font name from list_fonts. Empty string clears the font.");
		add_string_prop(props, "voice_filename",
			"New voice audio filename. Empty string clears the voice.");
		add_string_enum_prop(props, "ui_name",
			"New UI layout name. Empty string clears to engine default.",
			fiction_ui_name_values);
		add_string_prop(props, "background_640",
			"New background image for 640x480 resolution. Empty string clears.");
		add_string_prop(props, "background_1024",
			"New background image for 1024x768 resolution. Empty string clears.");
		add_integer_prop(props, "formula", "Root node of the SEXP formula used for this stage.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "update_fiction_viewer_stage",
			"Update properties of an existing fiction viewer stage. Only specified "
			"fields are changed; omitted fields are left unchanged.",
			props, req);
	}

	// delete_fiction_viewer_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "index",
			"1-based index of the stage to delete");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "delete_fiction_viewer_stage",
			"Delete a fiction viewer stage. Frees its SEXP formula. "
			"Remaining stages are shifted down.",
			props, req);
	}

	// move_fiction_viewer_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"Current 1-based index of the stage");
		add_integer_prop(props, "to_index",
			"Target 1-based index to move the stage to");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_fiction_viewer_stage",
			"Move a fiction viewer stage from one position to another. "
			"Indices are 1-based.",
			props, req);
	}

	// swap_fiction_viewer_stages
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first stage");
		add_integer_prop(props, "index_b",
			"1-based index of the second stage");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_fiction_viewer_stages",
			"Swap two fiction viewer stages at the given positions. "
			"Indices are 1-based.",
			props, req);
	}

	// -----------------------------------------------------------------------
	// Debriefing stage tools
	// -----------------------------------------------------------------------

	static const char *debrief_team_desc =
		"Which team's debriefing to operate on (\"Team 1\" or \"Team 2\"). "
		"Defaults to \"Team 1\". \"none\" is not valid for debriefings.";

	// list_debriefing_stages
	{
		json_t *props = json_object();
		add_string_enum_prop(props, "team", debrief_team_desc, team_enum_values);
		register_tool(tools, "list_debriefing_stages",
			"List all debriefing stages for a team. Returns each stage's index, "
			"text, voice, recommendation text, and SEXP formula root node.",
			props);
	}

	// get_debriefing_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "index",
			"1-based index of the stage to retrieve");
		add_string_enum_prop(props, "team", debrief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "get_debriefing_stage",
			"Get full details of a debriefing stage by index.",
			props, req);
	}

	// create_debriefing_stage
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
		add_string_enum_prop(props, "team", debrief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("text"));
		register_tool(tools, "create_debriefing_stage",
			"Create a new debriefing stage. Debriefing stages are shown after a mission "
			"completes; each stage has a SEXP formula controlling whether it is displayed. "
			"Maximum " SCP_TOKEN_TO_STR(MAX_DEBRIEF_STAGES) " stages per team.",
			props, req);
	}

	// update_debriefing_stage
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
		add_string_enum_prop(props, "team", debrief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "update_debriefing_stage",
			"Update properties of an existing debriefing stage. Only specified "
			"fields are changed; omitted fields are left unchanged.",
			props, req);
	}

	// delete_debriefing_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "index",
			"1-based index of the stage to delete");
		add_string_enum_prop(props, "team", debrief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("index"));
		register_tool(tools, "delete_debriefing_stage",
			"Delete a debriefing stage. Frees its SEXP formula. "
			"Remaining stages are shifted down.",
			props, req);
	}

	// move_debriefing_stage
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"Current 1-based index of the stage");
		add_integer_prop(props, "to_index",
			"Target 1-based index to move the stage to");
		add_string_enum_prop(props, "team", debrief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_debriefing_stage",
			"Move a debriefing stage from one position to another. "
			"Indices are 1-based.",
			props, req);
	}

	// swap_debriefing_stages
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first stage");
		add_integer_prop(props, "index_b",
			"1-based index of the second stage");
		add_string_enum_prop(props, "team", debrief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_debriefing_stages",
			"Swap two debriefing stages at the given positions. "
			"Indices are 1-based.",
			props, req);
	}

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
	} else if (strcmp(tool_name, "list_cmd_brief_stages") == 0) {
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
	} else if (mcp_handle_goal_tool(tool_name, input_json, req)) {
		// handled by goals unit
	} else if (strcmp(tool_name, "list_fiction_viewer_stages") == 0) {
		handle_list_fiction_viewer_stages(input_json, req);
	} else if (strcmp(tool_name, "get_fiction_viewer_stage") == 0) {
		handle_get_fiction_viewer_stage(input_json, req);
	} else if (strcmp(tool_name, "create_fiction_viewer_stage") == 0) {
		handle_create_fiction_viewer_stage(input_json, req);
	} else if (strcmp(tool_name, "update_fiction_viewer_stage") == 0) {
		handle_update_fiction_viewer_stage(input_json, req);
	} else if (strcmp(tool_name, "delete_fiction_viewer_stage") == 0) {
		handle_delete_fiction_viewer_stage(input_json, req);
	} else if (strcmp(tool_name, "move_fiction_viewer_stage") == 0) {
		handle_move_fiction_viewer_stage(input_json, req);
	} else if (strcmp(tool_name, "swap_fiction_viewer_stages") == 0) {
		handle_swap_fiction_viewer_stages(input_json, req);
	} else if (strcmp(tool_name, "list_debriefing_stages") == 0) {
		handle_list_debriefing_stages(input_json, req);
	} else if (strcmp(tool_name, "get_debriefing_stage") == 0) {
		handle_get_debriefing_stage(input_json, req);
	} else if (strcmp(tool_name, "create_debriefing_stage") == 0) {
		handle_create_debriefing_stage(input_json, req);
	} else if (strcmp(tool_name, "update_debriefing_stage") == 0) {
		handle_update_debriefing_stage(input_json, req);
	} else if (strcmp(tool_name, "delete_debriefing_stage") == 0) {
		handle_delete_debriefing_stage(input_json, req);
	} else if (strcmp(tool_name, "move_debriefing_stage") == 0) {
		handle_move_debriefing_stage(input_json, req);
	} else if (strcmp(tool_name, "swap_debriefing_stages") == 0) {
		handle_swap_debriefing_stages(input_json, req);
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
	} else if (strcmp(tool_name, "list_waypoint_lists") == 0) {
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
