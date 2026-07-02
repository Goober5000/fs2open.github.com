#include "stdafx.h"
#include "mcp_brief.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "mcp_array_utils.h"
#include "mcp_sexp.h"
#include "mcp_sexp_forest.h"
#include "fredrender.h"

#include <jansson.h>
#include <climits>
#include <cstring>

#include "globalincs/utility.h"

#include "globalincs/alphacolors.h"
#include "graphics/2d.h"
#include "math/vecmat.h"
#include "mission/missionbriefcommon.h"
#include "parse/parselo.h"
#include "parse/sexp.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_briefing(SCP_string &error_msg)
{
	return validate_single_dialog("briefing stages", "briefing", error_msg);
}

// ---------------------------------------------------------------------------
// Briefing stage tools
// ---------------------------------------------------------------------------

// Resolve optional "team" parameter to a briefing pointer.
// Defaults to Team 1. Rejects "none".
static briefing *get_briefing_for_team(json_t *input, McpErrorSink &sink)
{
	auto team_str = get_optional_string(input, "team", sink);
	if (sink.has_error()) return nullptr;
	int team_index = 0;  // default to Team 1

	if (team_str) {
		if (!check_string_enum(team_str, team_selector_enum_values, "team", sink))
			return nullptr;
		if (reject_team_none(team_str, "briefing", sink)) return nullptr;
		team_index = team_index_from_name(team_str);
	}

	return &Briefings[team_index];
}

// Restore a stage slot to its default state without losing its buffers.
// brief_stage::reset() nulls the icons/lines pointers, but in FRED every
// stage slot's buffers are pre-allocated at startup and must be preserved.
static void reset_stage_preserving_buffers(brief_stage &s)
{
	auto *icons = s.icons;
	auto *lines = s.lines;
	s.reset();
	s.icons = icons;
	s.lines = lines;
}

static json_t *build_brief_stage_json(const brief_stage &stage, int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "text", json_safe_string(stage.text.c_str()));
	set_optional_filename(obj, "voice_filename", stage.voice);
	json_object_set_new(obj, "camera_position", build_vec3d_json(stage.camera_pos));
	json_object_set_new(obj, "camera_orientation", build_matrix_json(stage.camera_orient));
	json_object_set_new(obj, "camera_time", json_integer(stage.camera_time));
	json_object_set_new(obj, "cut_to_next", json_boolean((stage.flags & BS_FORWARD_CUT) != 0));
	json_object_set_new(obj, "cut_from_previous", json_boolean((stage.flags & BS_BACKWARD_CUT) != 0));
	json_object_set_new(obj, "draw_grid", json_boolean(stage.draw_grid));
	json_object_set_new(obj, "grid_color", build_color_json(stage.grid_color, true));
	json_object_set_new(obj, "formula", json_integer(stage.formula));
	return obj;
}

static void handle_list_briefing_stages(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	json_t *arr = json_array();
	for (int i = 0; i < br->num_stages; i++)
		json_array_append_new(arr, build_brief_stage_json(br->stages[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_briefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, br->num_stages, "index", sink)) return;

	req->result_json = make_json_tool_result(build_brief_stage_json(br->stages[*index - 1], *index - 1));
	req->success = true;
}

static void handle_create_briefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	if (br->num_stages >= MAX_BRIEF_STAGES) {
		sink.set_error("Cannot add more than %d briefing stages", MAX_BRIEF_STAGES);
		return;
	}

	auto text = get_required_string(input, "text", sink, false, MULTITEXT_LENGTH - 1);
	if (!text) return;

	auto voice             = get_optional_filename(input, "voice_filename", sink, false);
	auto camera_pos        = get_optional_vec3d(input, "camera_position", sink);
	auto camera_orient     = get_optional_matrix(input, "camera_orientation", sink);
	auto camera_time       = get_optional_integer(input, "camera_time", sink);
	auto cut_to_next       = get_optional_bool(input, "cut_to_next", sink);
	auto cut_from_previous = get_optional_bool(input, "cut_from_previous", sink);
	auto draw_grid         = get_optional_bool(input, "draw_grid", sink);
	auto grid_color        = get_optional_color(input, "grid_color", sink);
	auto formula           = get_optional_integer(input, "formula", sink);
	auto insert_index      = get_optional_integer(input, "index", sink);
	auto copy_from_prev    = get_optional_bool(input, "copy_from_previous", sink);
	if (sink.has_error()) return;
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, sink)) return;
	if (camera_time.has_value() && !check_int_range(*camera_time, 0, INT_MAX, "camera_time", sink)) return;

	int target;
	if (!insert_index.has_value()) {
		target = br->num_stages;
	} else {
		if (!check_int_range(*insert_index, 1, br->num_stages + 1, "index", sink))
			return;
		target = *insert_index - 1;
	}

	if (!array_insert_slot_swap(br->stages, br->num_stages, MAX_BRIEF_STAGES, target)) {
		sink.set_error("Cannot add more than %d briefing stages", MAX_BRIEF_STAGES);
		return;
	}

	// The slot arriving at target holds the former one-past-end element's
	// stale state; wipe it before filling it in.
	brief_stage &s = br->stages[target];
	reset_stage_preserving_buffers(s);

	// Determine the stage to inherit from: the stage preceding the insertion
	// point, or when inserting at the front, the stage that used to be first.
	int source = -1;
	if (copy_from_prev.value_or(true)) {
		if (target > 0)
			source = target - 1;
		else if (br->num_stages > 1)
			source = 1;
	}

	if (source >= 0) {
		// Copy camera, flags, grid settings, and icon/line contents.  Voice is
		// per-stage audio, and copying the formula root would alias the SEXP
		// tree between stages, so neither is inherited.
		const brief_stage &src = br->stages[source];
		s.camera_pos = src.camera_pos;
		s.camera_orient = src.camera_orient;
		s.camera_time = src.camera_time;
		s.flags = src.flags;
		s.draw_grid = src.draw_grid;
		s.grid_color = src.grid_color;
		s.num_icons = src.num_icons;
		memcpy(s.icons, src.icons, sizeof(brief_icon) * MAX_STAGE_ICONS);
		s.num_lines = src.num_lines;
		memcpy(s.lines, src.lines, sizeof(brief_line) * MAX_BRIEF_STAGE_LINES);
	} else {
		s.camera_pos = view_pos;
		s.camera_orient = view_orient;
		s.camera_time = 500;
	}

	// Explicit parameters override inherited/default values
	s.text = text;
	strcpy_s(s.voice, (voice && voice[0]) ? voice : "none");
	if (camera_pos.has_value())
		s.camera_pos = *camera_pos;
	if (camera_orient.has_value())
		s.camera_orient = *camera_orient;
	if (camera_time.has_value())
		s.camera_time = *camera_time;
	if (cut_to_next.has_value())
		s.flags = *cut_to_next ? (s.flags | BS_FORWARD_CUT) : (s.flags & ~BS_FORWARD_CUT);
	if (cut_from_previous.has_value())
		s.flags = *cut_from_previous ? (s.flags | BS_BACKWARD_CUT) : (s.flags & ~BS_BACKWARD_CUT);
	if (draw_grid.has_value())
		s.draw_grid = *draw_grid;
	if (grid_color.has_value())
		s.grid_color = *grid_color;

	if (formula.has_value()) {
		s.formula = *formula;
	} else {
		s.formula = Locked_sexp_true;
	}

	mcp_sexp_forest_mark_dirty({ s.formula });
	mark_modified("MCP: create briefing stage %d", target + 1);

	req->result_json = make_json_tool_result(build_brief_stage_json(br->stages[target], target));
	req->success = true;
}

static void handle_update_briefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, br->num_stages, "index", sink)) return;

	auto new_text          = get_optional_string(input, "text", sink, MULTITEXT_LENGTH - 1);
	auto new_voice         = get_optional_filename(input, "voice_filename", sink, false);
	auto new_camera_pos    = get_optional_vec3d(input, "camera_position", sink);
	auto new_camera_orient = get_optional_matrix(input, "camera_orientation", sink);
	auto new_camera_time   = get_optional_integer(input, "camera_time", sink);
	auto cut_to_next       = get_optional_bool(input, "cut_to_next", sink);
	auto cut_from_previous = get_optional_bool(input, "cut_from_previous", sink);
	auto new_draw_grid     = get_optional_bool(input, "draw_grid", sink);
	auto new_grid_color    = get_optional_color(input, "grid_color", sink);
	auto new_formula       = get_optional_integer(input, "formula", sink);
	if (sink.has_error()) return;
	if (new_formula.has_value() && !check_sexp_formula(*new_formula, OPR_BOOL, sink)) return;
	if (new_camera_time.has_value() && !check_int_range(*new_camera_time, 0, INT_MAX, "camera_time", sink)) return;

	brief_stage &s = br->stages[*index - 1];
	bool changed = false;

	if (new_text && s.text != new_text) {
		s.text = new_text;
		changed = true;
	}
	if (new_voice) {
		const char *effective_voice = new_voice[0] ? new_voice : "none";
		if (strcmp(s.voice, effective_voice) != 0) {
			strcpy_s(s.voice, effective_voice);
			changed = true;
		}
	}
	if (new_camera_pos.has_value() && !vm_vec_same(&s.camera_pos, &*new_camera_pos)) {
		s.camera_pos = *new_camera_pos;
		changed = true;
	}
	if (new_camera_orient.has_value() && !vm_matrix_same(&s.camera_orient, &*new_camera_orient)) {
		s.camera_orient = *new_camera_orient;
		changed = true;
	}
	if (new_camera_time.has_value() && s.camera_time != *new_camera_time) {
		s.camera_time = *new_camera_time;
		changed = true;
	}

	int flags = s.flags;
	if (cut_to_next.has_value())
		flags = *cut_to_next ? (flags | BS_FORWARD_CUT) : (flags & ~BS_FORWARD_CUT);
	if (cut_from_previous.has_value())
		flags = *cut_from_previous ? (flags | BS_BACKWARD_CUT) : (flags & ~BS_BACKWARD_CUT);
	if (flags != s.flags) {
		s.flags = flags;
		changed = true;
	}

	if (new_draw_grid.has_value() && s.draw_grid != *new_draw_grid) {
		s.draw_grid = *new_draw_grid;
		changed = true;
	}
	if (new_grid_color.has_value() && !gr_compare_color_values(s.grid_color, *new_grid_color)) {
		s.grid_color = *new_grid_color;
		changed = true;
	}
	if (new_formula.has_value() && replace_cue(s.formula, *new_formula)) {
		changed = true;
	}

	if (changed)
		mark_modified("MCP: update briefing stage %d", *index);

	req->result_json = make_json_tool_result(build_brief_stage_json(s, *index - 1));
	req->success = true;
}

static void handle_delete_briefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, br->num_stages, "index", sink)) return;

	int formula = br->stages[*index - 1].formula;
	if (formula >= 0 && formula != Locked_sexp_true && formula != Locked_sexp_false)
		free_sexp2(formula);

	array_remove_slot_swap(br->stages, br->num_stages, *index - 1);

	// The removed stage was parked at the one-past-end slot; wipe its
	// leftovers so stale text/icons can't resurface.
	reset_stage_preserving_buffers(br->stages[br->num_stages]);

	mark_modified("MCP: delete briefing stage %d", *index);
	sprintf(req->result_message,
		"Deleted briefing stage %d", *index);
	req->success = true;
}

static MoveSwapConfig make_briefing_move_swap_config(briefing *br)
{
	MoveSwapConfig cfg;
	cfg.entity_name = "briefing stage";
	cfg.count = br->num_stages;
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_briefing;
	cfg.get_name = [](int index) {
		SCP_string name;
		sprintf(name, "briefing stage %d", index);
		return name;
	};
	cfg.do_move = [br](int from, int to) {
		array_move_element_swap(br->stages, br->num_stages, from - 1, to - 1);
	};
	cfg.do_swap = [br](int a, int b) {
		swap(br->stages[a - 1], br->stages[b - 1]);
	};
	return cfg;
}

static void handle_move_briefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	auto cfg = make_briefing_move_swap_config(br);
	handle_generic_move(input, req, cfg);
}

static void handle_swap_briefing_stages(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	auto cfg = make_briefing_move_swap_config(br);
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

static const char *brief_team_desc =
	"Which team's briefing to operate on (\"Team 1\" or \"Team 2\"). "
	"Defaults to \"Team 1\". \"none\" is not valid for briefings.";

static void register_list_briefing_stages(json_t *tools)
{
	json_t *props = json_object();
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	register_tool(tools, "list_briefing_stages",
		"List all briefing stages for a team. Returns each stage's index, "
		"text, voice, camera view, cut flags, grid settings, and SEXP "
		"formula root node.",
		props);
}

static void register_get_briefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index",
		"1-based index of the stage to retrieve");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "get_briefing_stage",
		"Get full details of a briefing stage by index.",
		props, req);
}

static void register_create_briefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_string_prop(props, "text", "The briefing text displayed during this stage");
	add_string_prop(props, "voice_filename",
		"Voice audio filename (wav/ogg). Defaults to empty (no voice).");
	add_vec3d_prop(props, "camera_position",
		"Camera position for this stage's view of the briefing map. "
		"Defaults to the inherited or current editor viewpoint.");
	add_matrix_prop(props, "camera_orientation",
		"Camera orientation for this stage's view of the briefing map. "
		"Defaults to the inherited or current editor viewpoint.");
	add_integer_prop(props, "camera_time",
		"Time in milliseconds for the camera to move to this stage's view. "
		"Defaults to the inherited value, or 500.");
	add_bool_prop(props, "cut_to_next",
		"Cut instantly to the next stage instead of moving the camera. Defaults to false.");
	add_bool_prop(props, "cut_from_previous",
		"Cut instantly from the previous stage instead of moving the camera. Defaults to false.");
	add_bool_prop(props, "draw_grid",
		"Whether the briefing map grid is drawn during this stage. Defaults to true.");
	add_color_prop(props, "grid_color",
		"Color of the briefing map grid. Defaults to the standard grid color.");
	add_integer_prop(props, "formula", "Root node of the SEXP formula used for this stage. "
		"Defaults to true (stage always shown).");
	add_integer_prop(props, "index",
		"Position to insert the stage (1 = first). If omitted, appends to the end.");
	add_bool_prop(props, "copy_from_previous",
		"Inherit camera view, cut flags, grid settings, icons, and lines from "
		"the stage preceding the insertion point (or the following stage when "
		"inserting at the front), as the FRED editor does. Voice and formula "
		"are never inherited. Defaults to true. Explicit parameters always "
		"override inherited values.");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("text"));
	register_tool(tools, "create_briefing_stage",
		"Create a new briefing stage. Briefing stages are shown on the briefing "
		"map before a mission starts; each stage has text, an optional voice, a "
		"camera view, and a SEXP formula controlling whether it is displayed. "
		"Maximum " SCP_TOKEN_TO_STR(MAX_BRIEF_STAGES) " stages per team.",
		props, req);
}

static void register_update_briefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index",
		"1-based index of the stage to update");
	add_string_prop(props, "text", "New briefing text for this stage");
	add_string_prop(props, "voice_filename",
		"New voice audio filename. Empty string clears the voice.");
	add_vec3d_prop(props, "camera_position",
		"New camera position for this stage's view of the briefing map");
	add_matrix_prop(props, "camera_orientation",
		"New camera orientation for this stage's view of the briefing map");
	add_integer_prop(props, "camera_time",
		"New time in milliseconds for the camera to move to this stage's view");
	add_bool_prop(props, "cut_to_next",
		"Cut instantly to the next stage instead of moving the camera");
	add_bool_prop(props, "cut_from_previous",
		"Cut instantly from the previous stage instead of moving the camera");
	add_bool_prop(props, "draw_grid",
		"Whether the briefing map grid is drawn during this stage");
	add_color_prop(props, "grid_color",
		"New color of the briefing map grid");
	add_integer_prop(props, "formula", "Root node of the SEXP formula used for this stage.");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "update_briefing_stage",
		"Update properties of an existing briefing stage. Only specified "
		"fields are changed; omitted fields are left unchanged.",
		props, req);
}

static void register_delete_briefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index",
		"1-based index of the stage to delete");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "delete_briefing_stage",
		"Delete a briefing stage, including its icons and lines. Frees its "
		"SEXP formula. Remaining stages are shifted down.",
		props, req);
}

static void register_move_briefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "from_index",
		"Current 1-based index of the stage");
	add_integer_prop(props, "to_index",
		"Target 1-based index to move the stage to");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("from_index"));
	json_array_append_new(req, json_string("to_index"));
	register_tool(tools, "move_briefing_stage",
		"Move a briefing stage from one position to another. "
		"Indices are 1-based.",
		props, req);
}

static void register_swap_briefing_stages(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index_a",
		"1-based index of the first stage");
	add_integer_prop(props, "index_b",
		"1-based index of the second stage");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index_a"));
	json_array_append_new(req, json_string("index_b"));
	register_tool(tools, "swap_briefing_stages",
		"Swap two briefing stages at the given positions. "
		"Indices are 1-based.",
		props, req);
}

// ---------------------------------------------------------------------------
// Tool table
// ---------------------------------------------------------------------------

const McpToolDef mcp_brief_tool_defs[] = {
	{ "list_briefing_stages",  register_list_briefing_stages,  nullptr, handle_list_briefing_stages,  false },
	{ "get_briefing_stage",    register_get_briefing_stage,    nullptr, handle_get_briefing_stage,    false },
	{ "create_briefing_stage", register_create_briefing_stage, nullptr, handle_create_briefing_stage, false },
	{ "update_briefing_stage", register_update_briefing_stage, nullptr, handle_update_briefing_stage, false },
	{ "delete_briefing_stage", register_delete_briefing_stage, nullptr, handle_delete_briefing_stage, false },
	{ "move_briefing_stage",   register_move_briefing_stage,   nullptr, handle_move_briefing_stage,   false },
	{ "swap_briefing_stages",  register_swap_briefing_stages,  nullptr, handle_swap_briefing_stages,  false },
};
const size_t mcp_brief_tool_def_count = sizeof(mcp_brief_tool_defs) / sizeof(mcp_brief_tool_defs[0]);
