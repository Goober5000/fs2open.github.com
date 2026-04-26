#include "stdafx.h"
#include "mcp_messages.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcp_sexp_forest.h"
#include "mcpserver.h"
#include "mcp_array_utils.h"

#include <jansson.h>
#include <cstring>

#include "globalincs/utility.h"

#include "mission/missionmessage.h"
#include "parse/sexp.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_messages(SCP_string &error_msg)
{
	return validate_single_dialog("messages", "message", error_msg)
		&& validate_single_dialog("messages", "event", error_msg);
}

// ---------------------------------------------------------------------------
// Persona helpers
// ---------------------------------------------------------------------------

static const char *persona_name_from_index(int persona_index)
{
	if (persona_index >= 0 && persona_index < (int)Personas.size())
		return Personas[persona_index].name;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Enum helpers
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> message_enum_values = { "mission", "builtin" };

// ---------------------------------------------------------------------------
// Message tool handlers (run on main thread)
// ---------------------------------------------------------------------------

// include_details adds team, talking_head, and voice_file fields.
static json_t *build_message_json(const MMessage &msg, int msg_absolute_index, bool include_details = false)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(msg.name));
	if (msg_absolute_index >= Num_builtin_messages)
		json_object_set_new(obj, "index", json_integer(msg_absolute_index - Num_builtin_messages + 1));
	json_object_set_new(obj, "message", json_string(msg.message));

	const char *persona = persona_name_from_index(msg.persona_index);
	if (persona)
		json_object_set_new(obj, "persona", json_string(persona));

	if (include_details) {
		json_object_set_new(obj, "team", json_string(team_name_from_index(msg.multi_team)));
		set_optional_filename(obj, "talking_head", msg.avi_info.name);
		set_optional_filename(obj, "voice_filename", msg.wave_info.name);
	}

	return obj;
}

static void handle_list_messages(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	// Determine range based on "source" parameter
	auto source = get_optional_string(input, "source", sink);
	if (sink.has_error()) return;

	if (source && !check_string_enum(source, message_enum_values, "source", sink))
		return;

	int start, end;
	if (source && !stricmp(source, "builtin")) {
		start = 0;
		end = Num_builtin_messages;
	} else {
		// Default: mission messages — check for conflicting dialogs
		if (!validate(validate_dialog_for_messages, sink)) return;
		start = Num_builtin_messages;
		end = Num_messages;
	}

	json_t *arr = json_array();
	for (int i = start; i < end; i++)
		json_array_append_new(arr, build_message_json(Messages[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_message(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	// Find the message
	int idx = find_item_with_string(Messages, &MMessage::name, name);
	if (idx < 0) {
		set_not_found_error(sink,"Message", name);
		return;
	}

	// Check for conflicting dialogs if this is a mission-specific message
	if (idx >= Num_builtin_messages) {
		if (!validate(validate_dialog_for_messages, sink)) return;
	}

	req->result_json = make_json_tool_result(build_message_json(Messages[idx], idx, true));
	req->success = true;
}

static void handle_create_message(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_messages, sink)) return;

	auto name    = get_required_string(input, "name", sink, true, NAME_LENGTH - 1);
	if (!name) return;

	auto message = get_required_string(input, "message", sink, false, MESSAGE_LENGTH - 1);
	if (!message) return;

	auto persona_str  = get_optional_string(input, "persona", sink);
	auto talking_head = get_optional_filename(input, "talking_head", sink, false);
	auto voice_file   = get_optional_filename(input, "voice_filename", sink, false);
	auto team_str     = get_optional_string(input, "team", sink);
	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	// Check for duplicate name
	if (find_item_with_string(Messages, &MMessage::name, name) >= 0) {
		sink.set_error("A message with name '%s' already exists", name);
		return;
	}

	// Look up persona by name
	int persona_index = -1;
	if (persona_str && persona_str[0]) {
		persona_index = check_lookup(persona_str, message_persona_name_lookup, "persona", sink);
		if (persona_index < 0) return;
	}

	// Validate team
	int multi_team = -1;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", sink))
			return;
		multi_team = team_index_from_name(team_str);
	}

	// Validate and resolve insert index
	int target_index;
	if (!insert_index.has_value()) {
		// Default: append to end
		target_index = Num_messages;
	} else {
		// Caller specifies a mission-relative index (1 = first mission message)
		// Note: the upper bound is allowed, and means append
		if (!check_int_range(*insert_index, 1, Num_messages - Num_builtin_messages + 1, "index", sink))
			return;
		target_index = Num_builtin_messages + *insert_index - 1;
	}

	// Insert a slot at the target position
	array_insert_slot(Messages, Num_messages, target_index);

	MMessage &msg = Messages[target_index];
	// we can't use memset here, so explicitly assign all fields
	strcpy_s(msg.name, name);
	strcpy_s(msg.message, message);
	msg.persona_index = persona_index;
	msg.multi_team = multi_team;
	msg.mood = DEFAULT_MOOD;
	msg.note.clear();
	msg.excluded_moods.clear();

	message_filter_clear(msg.sender_filter);
	message_filter_clear(msg.subject_filter);
	message_filter_clear(msg.outer_filter);
	msg.outer_filter_radius = -1;
	msg.boost_level = 0;

	msg.avi_info.name = (talking_head && talking_head[0]) ? strdup(talking_head) : nullptr;
	msg.wave_info.name = (voice_file && voice_file[0]) ? strdup(voice_file) : nullptr;

	mark_modified("MCP: create message %s", name);

	req->result_json = make_json_tool_result(build_message_json(Messages[target_index], target_index, true));
	req->success = true;
}

static void handle_update_message(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_messages, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	// Find the message (mission-specific only)
	int idx = find_item_with_string(Messages, &MMessage::name, name, Num_builtin_messages);
	if (idx < 0) {
		set_not_found_error(sink,"Message", name);
		return;
	}

	auto new_msg      = get_optional_string(input, "message", sink, MESSAGE_LENGTH - 1);
	auto persona_str  = get_optional_string(input, "persona", sink);
	auto new_head     = get_optional_filename(input, "talking_head", sink, false);
	auto new_voice    = get_optional_filename(input, "voice_filename", sink, false);
	auto new_team_str = get_optional_string(input, "team", sink);
	auto new_name     = get_optional_string(input, "new_name", sink);
	if (sink.has_error()) return;

	std::optional<int> persona_index = std::nullopt;
	if (persona_str) {
		if (persona_str[0]) {
			int persona_idx = check_lookup(persona_str, message_persona_name_lookup, "persona", sink);
			if (persona_idx < 0) return;
			persona_index = persona_idx;
		} else {
			persona_index = -1;
		}
	}

	std::optional<int> new_team = std::nullopt;
	if (new_team_str) {
		if (!check_string_enum(new_team_str, team_enum_values, "team", sink))
			return;
		new_team = team_index_from_name(new_team_str);
	}

	if (new_name) {
		if (!check_general_rename(new_name, Messages[idx].name,
			[](const char *n) { return find_item_with_string(Messages, &MMessage::name, n) >= 0; },
			"Message", sink)) return;
	}

	bool changed = false;

	// Update message text
	if (new_msg) {
		if (strcmp(Messages[idx].message, new_msg) != 0) {
			strcpy_s(Messages[idx].message, new_msg);
			changed = true;
		}
	}

	// Update persona
	if (persona_index.has_value()) {
		if (Messages[idx].persona_index != *persona_index) {
			Messages[idx].persona_index = *persona_index;
			changed = true;
		}
	}

	// Update talking head
	if (new_head) {
		const char *old_head = Messages[idx].avi_info.name;
		bool old_empty = !old_head || !old_head[0];
		bool new_empty = !new_head[0];
		if (old_empty != new_empty || (!old_empty && stricmp(old_head, new_head) != 0)) {
			if (old_head)
				free(Messages[idx].avi_info.name);
			Messages[idx].avi_info.name = new_head[0] ? strdup(new_head) : nullptr;
			changed = true;
		}
	}

	// Update voice file
	if (new_voice) {
		const char *old_voice = Messages[idx].wave_info.name;
		bool old_empty = !old_voice || !old_voice[0];
		bool new_empty = !new_voice[0];
		if (old_empty != new_empty || (!old_empty && stricmp(old_voice, new_voice) != 0)) {
			if (old_voice)
				free(Messages[idx].wave_info.name);
			Messages[idx].wave_info.name = new_voice[0] ? strdup(new_voice) : nullptr;
			changed = true;
		}
	}

	// Update team
	if (new_team.has_value()) {
		if (Messages[idx].multi_team != *new_team) {
			Messages[idx].multi_team = *new_team;
			changed = true;
		}
	}

	// Update name (must be last — invalidates `name` pointer and updates SEXP refs)
	if (new_name && (stricmp(Messages[idx].name, new_name) != 0)) {
		// Update SEXP references before changing the name
		update_sexp_references(Messages[idx].name, new_name, OPF_MESSAGE);
		update_sexp_references(Messages[idx].name, new_name, OPF_MESSAGE_OR_STRING);
		mcp_sexp_forest_mark_dirty();
		strcpy_s(Messages[idx].name, new_name);
		changed = true;
	}

	if (changed) {
		mark_modified("MCP: update message %s", Messages[idx].name);
	}

	// Return the updated message
	req->result_json = make_json_tool_result(build_message_json(Messages[idx], idx, true));
	req->success = true;
}

static void handle_delete_message(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_messages, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	// Find the message (mission-specific only)
	int idx = find_item_with_string(Messages, &MMessage::name, name, Num_builtin_messages);
	if (idx < 0) {
		set_not_found_error(sink,"Message", name);
		return;
	}

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	if (!check_and_report_sexp_refs(sexp_ref_type::NON_OBJECT, "Message", Messages[idx].name, force, sink))
		return;

	// Free allocated strings
	if (Messages[idx].avi_info.name)
		free(Messages[idx].avi_info.name);
	if (Messages[idx].wave_info.name)
		free(Messages[idx].wave_info.name);

	// Invalidate SEXP references (wrap name in angle brackets)
	char buf[NAME_LENGTH + 4];
	snprintf(buf, sizeof(buf), "<%s>", Messages[idx].name);
	update_sexp_references(Messages[idx].name, buf, OPF_MESSAGE);
	update_sexp_references(Messages[idx].name, buf, OPF_MESSAGE_OR_STRING);
	mcp_sexp_forest_mark_dirty();

	// Remove from array by shifting (matches FRED's CMessageEditorDlg::OnDelete pattern)
	array_remove_slot(Messages, Num_messages, idx);

	mark_modified("MCP: delete message %s", name);

	sprintf(req->result_message, "Deleted message: %s", name);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Move/swap config and handlers
// ---------------------------------------------------------------------------

static MoveSwapConfig make_message_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "message";
	cfg.count = Num_messages - Num_builtin_messages;
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_messages;
	cfg.get_name = [](int i) {
		return Messages[Num_builtin_messages + i - 1].name;
	};
	cfg.do_move = [](int from, int to) {
		array_move_element(Messages, Num_builtin_messages + from - 1, Num_builtin_messages + to - 1);
	};
	cfg.do_swap = [](int a, int b) {
		std::swap(Messages[Num_builtin_messages + a - 1], Messages[Num_builtin_messages + b - 1]);
	};
	return cfg;
}

static void handle_move_message(json_t *input, McpToolRequest *req)
{
	auto cfg = make_message_move_swap_config();
	handle_generic_move(input, req, cfg);
}

static void handle_swap_messages(json_t *input, McpToolRequest *req)
{
	auto cfg = make_message_move_swap_config();
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_message_tools(json_t *tools)
{
	// list_messages
	{
		json_t *props = json_object();
		add_string_enum_prop(props, "source",
			"Which messages to list: \"mission\" (default) for mission-specific messages, "
			"or \"builtin\" for built-in engine messages from messages.tbl",
			message_enum_values);
		register_tool(tools, "list_messages",
			"List messages. By default lists mission-specific messages. "
			"Use source=\"builtin\" to list built-in engine messages instead. "
			"Returns each message's name, text, and persona.",
			props);
	}

	// get_message
	register_tool_with_required_string(tools, "get_message",
		"Get full details of a message by name, including text, persona, "
		"talking head animation, voice file, and team assignment.",
		"name", "Name of the message to retrieve");

	// create_message
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Unique name for the message");
		add_string_prop(props, "message", "The message text displayed in-game");
		add_string_prop(props, "persona",
			"Name of the persona who delivers this message (e.g. \"Wingman 1\")");
		add_string_prop(props, "talking_head", "Filename for the talking head animation");
		add_string_prop(props, "voice_filename", "Filename for the voice audio");
		add_string_enum_prop(props, "team",
			"Multiplayer team assignment (\"none\" for all teams)",
			team_enum_values);
		add_integer_prop(props, "index",
			"Position to insert the message among mission messages (1 = first). "
			"If omitted, appends to the end.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("message"));
		register_tool(tools, "create_message",
			"Create a new mission message. Messages are used by send-message SEXPs to "
			"display in-game dialogue with optional voice and talking head animation.",
			props, req);
	}

	// update_message
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the existing message to update");
		add_string_prop(props, "new_name", "New name for the message");
		add_string_prop(props, "message", "New message text");
		add_string_prop(props, "persona",
			"Name of the persona who delivers this message (e.g. \"Wingman 1\") (empty string to clear)");
		add_string_prop(props, "talking_head", "Filename for the talking head animation (empty string to clear)");
		add_string_prop(props, "voice_filename", "Filename for the voice audio (empty string to clear)");
		add_string_enum_prop(props, "team",
			"Multiplayer team assignment (\"none\" for all teams)",
			team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_message",
			"Update properties of an existing mission message. Only specified fields are "
			"changed; omitted fields are left unchanged. Renaming automatically updates "
			"all SEXP references to the message.",
			props, req);
	}

	// delete_message
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the message to delete");
		add_bool_prop(props, "force",
			"If true, delete even if the message is referenced in SEXPs (references "
			"will be invalidated). Default: false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "delete_message",
			"Delete a mission message. SEXP references to the deleted message are "
			"invalidated (wrapped in angle brackets).",
			props, req);
	}

	// move_message
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"Current 1-based index of the message among mission messages");
		add_integer_prop(props, "to_index",
			"Target 1-based index to move the message to");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_message",
			"Move a mission message from one position to another. "
			"Indices are 1-based within mission messages.",
			props, req);
	}

	// swap_messages
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first message among mission messages");
		add_integer_prop(props, "index_b",
			"1-based index of the second message among mission messages");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_messages",
			"Swap two mission messages at the given positions. "
			"Indices are 1-based within mission messages.",
			props, req);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_message_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_messages") == 0) {
		handle_list_messages(input_json, req);
	} else if (strcmp(tool_name, "get_message") == 0) {
		handle_get_message(input_json, req);
	} else if (strcmp(tool_name, "create_message") == 0) {
		handle_create_message(input_json, req);
	} else if (strcmp(tool_name, "update_message") == 0) {
		handle_update_message(input_json, req);
	} else if (strcmp(tool_name, "delete_message") == 0) {
		handle_delete_message(input_json, req);
	} else if (strcmp(tool_name, "move_message") == 0) {
		handle_move_message(input_json, req);
	} else if (strcmp(tool_name, "swap_messages") == 0) {
		handle_swap_messages(input_json, req);
	} else {
		return false;
	}
	return true;
}
