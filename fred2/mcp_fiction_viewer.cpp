#include "stdafx.h"
#include "mcp_fiction_viewer.h"
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

#include "missionui/fictionviewer.h"
#include "parse/parselo.h"
#include "parse/sexp.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_fiction(SCP_string &error_msg)
{
	return validate_single_dialog("fiction viewer stages", "fiction viewer", error_msg);
}

// ---------------------------------------------------------------------------
// Entity-specific move/swap configs
// ---------------------------------------------------------------------------

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
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_fiction_viewer_tools(json_t *tools)
{
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
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_fiction_viewer_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_fiction_viewer_stages") == 0) {
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
	} else {
		return false;
	}
	return true;
}
