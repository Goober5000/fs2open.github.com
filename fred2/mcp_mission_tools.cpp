#include "stdafx.h"
#include "mcp_mission_tools.h"
#include "mcpserver.h"
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
#include "messageeditordlg.h"
#include "eventeditor.h"
#include "mainfrm.h"

#define PLACEHOLDER_STRING "<placeholder>"

// ---------------------------------------------------------------------------
// SEXP syntax checking helper
// ---------------------------------------------------------------------------

// Run check_sexp_syntax on a node and return a JSON object describing any
// syntax error found, or nullptr if the syntax is clean.  Caller must
// json_decref the returned object when done (if non-null).
static json_t *build_syntax_error_json(int node)
{
	int bad_node = -1;
	int syntax_result = check_sexp_syntax(node, OPR_AMBIGUOUS, 1, &bad_node);

	if (syntax_result == SEXP_CHECK_NO_ERROR)
		return nullptr;

	json_t *obj = json_object();
	json_object_set_new(obj, "error_code", json_integer(syntax_result));
	json_object_set_new(obj, "error_message", json_string(sexp_error_message(syntax_result)));
	if (bad_node >= 0) {
		json_object_set_new(obj, "bad_node", json_integer(bad_node));
		json_object_set_new(obj, "bad_node_text", json_string(Sexp_nodes[bad_node].text));
	}
	return obj;
}

// ---------------------------------------------------------------------------
// SEXP formula validation
// ---------------------------------------------------------------------------

static bool check_sexp_formula(int node, sexp_opr_t expected_return_type, McpErrorSink &sink)
{
	// Range check
	if (!check_int_range(node, 0, Num_sexp_nodes - 1, "formula", sink))
		return false;

	// Check node is in use
	if (Sexp_nodes[node].type == SEXP_NOT_USED) {
		sink.set_error("SEXP node %d is not in use", node);
		return false;
	}

	// Check operator return type
	int op_const = get_operator_const(node);
	if (op_const == OP_NOT_AN_OP) {
		sink.set_error("SEXP node %d (\"%s\") is not a valid SEXP operator",
			node, Sexp_nodes[node].text);
		return false;
	}

	auto actual_return_type = query_operator_return_type(op_const);
	if (actual_return_type != expected_return_type) {
		const char *expected_str = (expected_return_type == OPR_NULL) ? "OPR_NULL (action)" : "OPR_BOOL (boolean)";
		const char *actual_str;
		switch (actual_return_type) {
			case OPR_NULL:   actual_str = "OPR_NULL (action)"; break;
			case OPR_BOOL:   actual_str = "OPR_BOOL (boolean)"; break;
			case OPR_NUMBER: actual_str = "OPR_NUMBER"; break;
			case OPR_STRING: actual_str = "OPR_STRING"; break;
			default:         actual_str = "unknown"; break;
		}
		sink.set_error("Formula node %d (\"%s\") has return type %s, but this entity requires %s",
			node, Sexp_nodes[node].text, actual_str, expected_str);
		return false;
	}

	// Check syntax
	json_t *syntax_err = build_syntax_error_json(node);
	if (syntax_err) {
		const char *err_msg = json_string_value(json_object_get(syntax_err, "error_message"));
		const char *bad_text = json_string_value(json_object_get(syntax_err, "bad_node_text"));
		if (bad_text && bad_text[0])
			sink.set_error("Formula node %d has syntax error: %s (at \"%s\")",
				node, err_msg ? err_msg : "unknown", bad_text);
		else
			sink.set_error("Formula node %d has syntax error: %s",
				node, err_msg ? err_msg : "unknown");
		json_decref(syntax_err);
		return false;
	}

	return true;
}

// Returns true if deletion should proceed, false if blocked by references.
// Handles the common pattern: check SEXP refs unless force is set, report error if found.
static bool check_and_report_sexp_refs(sexp_ref_type ref_type, const char *entity_label,
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

static bool validate_single_dialog(const char *items_to_modify, const char *dialog_key, SCP_string &error_msg)
{
	for (size_t i = 0; i < g_editor_info_count; ++i) {
		auto &info = g_editor_info[i];
		if (dialog_key && info.editor_key && !stricmp(dialog_key, info.editor_key)) {
			auto wnd = info.getCWndPtr();
			if (wnd && wnd->IsWindowVisible()) {
				sprintf(error_msg, "Cannot work with %s while the %s is open. "
					"Close it first, or use get_ui_status to check which editors are open.", items_to_modify, info.editor_name);
				return false;
			}
			return true;
		}
	}
	Assertion(false, "dialog key '%s' not found!", dialog_key ? dialog_key : "<nullptr>");
	return false;
}

static bool validate_dialog_for_messages(SCP_string &error_msg)
{
	return validate_single_dialog("messages", "message", error_msg)
		&& validate_single_dialog("messages", "event", error_msg);
}

static bool validate_dialog_for_events(SCP_string &error_msg)
{
	return validate_single_dialog("events", "event", error_msg);
}

static bool validate_dialog_for_cmd_brief(SCP_string &error_msg)
{
	return validate_single_dialog("command briefing stages", "command briefing", error_msg);
}

static bool validate_dialog_for_goals(SCP_string &error_msg)
{
	return validate_single_dialog("goals", "goal", error_msg);
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

static bool validate_dialog_for_sexp_nodes(SCP_string &error_msg)
{
	return validate_single_dialog("SEXP nodes", "cutscene", error_msg)
		&& validate_single_dialog("SEXP nodes", "fiction viewer", error_msg)
		&& validate_single_dialog("SEXP nodes", "briefing", error_msg)
		&& validate_single_dialog("SEXP nodes", "debriefing", error_msg)
		&& validate_single_dialog("SEXP nodes", "ship", error_msg)
		&& validate_single_dialog("SEXP nodes", "wing", error_msg)
		&& validate_single_dialog("SEXP nodes", "event", error_msg)
		&& validate_single_dialog("SEXP nodes", "goal", error_msg);
}

bool validate_no_dialogs_open(SCP_string &error_msg)
{
	SCP_string open_list;
	for (size_t i = 0; i < g_editor_info_count; ++i) {
		auto &info = g_editor_info[i];
		auto wnd = info.getCWndPtr();
		if (wnd && wnd->IsWindowVisible()) {
			if (!open_list.empty())
				open_list += ", ";
			open_list += info.editor_name;
		}
	}
	if (!open_list.empty()) {
		sprintf(error_msg, "Cannot perform this operation while editor dialogs are open: %s. "
			"Close them first, or use get_ui_status to check which editors are open.", open_list.c_str());
		return false;
	}
	return true;
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

static const SCP_vector<const char *> team_enum_values = { "none", "Team 1", "Team 2" };
static const SCP_vector<const char *> message_enum_values = { "mission", "builtin" };
static const SCP_vector<const char *> event_unit_enum_values = { "milliseconds", "seconds" };

// ---------------------------------------------------------------------------
// Team helpers
// ---------------------------------------------------------------------------

static const char *team_name_from_index(int multi_team)
{
	switch (multi_team) {
		case 0:  return "Team 1";
		case 1:  return "Team 2";
		default: return "none";
	}
}

static int team_index_from_name(const char *name)
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
// Flag helpers
// ---------------------------------------------------------------------------

struct flag_entry { const char *name; int bit; };

static SCP_vector<const char *> flags_to_list(const flag_entry entries[], size_t count)
{
	SCP_vector<const char *> vec;
	for (size_t i = 0; i < count; i++)
		vec.push_back(entries[i].name);
	return vec;
}

static json_t *flags_to_json_array(int bitmask, const flag_entry entries[], size_t count)
{
	json_t *arr = json_array();
	for (size_t i = 0; i < count; i++)
		if (bitmask & entries[i].bit)
			json_array_append_new(arr, json_string(entries[i].name));
	return arr;
}

static bool parse_flags_array(const SCP_vector<SCP_string> &strings, const flag_entry entries[], size_t count,
	const char *param_name, int &out_flags, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto lookup_fn = [&](const char *str)->int
	{
		for (size_t i = 0; i < count; i++)
			if (!stricmp(entries[i].name, str))
				return sz2i(i);
		return -1;
	};

	out_flags = 0;
	for (const auto &s : strings) {
		int i = check_lookup(s.c_str(), lookup_fn, param_name, sink);
		if (i < 0)
			return false;
		out_flags |= entries[i].bit;
	}
	return true;
}

// ---------------------------------------------------------------------------
// SEXP variable type/flag data
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> sexp_var_type_values = { "number", "string" };

static const flag_entry sexp_var_flag_entries[] = {
	{ "save_on_mission_progress", SEXP_VARIABLE_SAVE_ON_MISSION_PROGRESS },
	{ "save_on_mission_close",    SEXP_VARIABLE_SAVE_ON_MISSION_CLOSE },
	{ "save_to_player_file",      SEXP_VARIABLE_SAVE_TO_PLAYER_FILE },
	{ "network",                  SEXP_VARIABLE_NETWORK },
};

static constexpr size_t sexp_var_flag_entries_count = sizeof(sexp_var_flag_entries) / sizeof(sexp_var_flag_entries[0]);

// ---------------------------------------------------------------------------
// Autosave helper
// ---------------------------------------------------------------------------

static void mark_modified(const char *fmt, ...)
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
	const char *source = get_optional_string(input, "source", sink, true);
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
	const char *name = get_required_string(input, "name", sink, false);
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

	auto name    = get_required_string(input, "name", sink, true);
	if (!name || !check_string_length(name, NAME_LENGTH - 1, "name", sink)) return;

	auto message = get_required_string(input, "message", sink, false);
	if (!message || !check_string_length(message, MESSAGE_LENGTH - 1, "message", sink)) return;

	auto persona_str  = get_optional_string(input, "persona", sink, false);
	auto talking_head = get_optional_filename(input, "talking_head", sink, false);
	auto voice_file   = get_optional_filename(input, "voice_filename", sink, false);
	auto team_str     = get_optional_string(input, "team", sink, true);
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

	auto new_msg      = get_optional_string(input, "message", sink, false);
	auto persona_str  = get_optional_string(input, "persona", sink, false);
	auto new_head     = get_optional_filename(input, "talking_head", sink, false);
	auto new_voice    = get_optional_filename(input, "voice_filename", sink, false);
	auto new_team_str = get_optional_string(input, "team", sink, true);
	auto new_name     = get_optional_string(input, "new_name", sink, false);
	if (sink.has_error()) return;

	if (new_msg && !check_string_length(new_msg, MESSAGE_LENGTH - 1, "message", sink)) return;

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

	// Remove from array by shifting (matches FRED's CMessageEditorDlg::OnDelete pattern)
	array_remove_slot(Messages, Num_messages, idx);

	mark_modified("MCP: delete message %s", name);

	sprintf(req->result_message, "Deleted message: %s", name);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Event annotation path helpers
// ---------------------------------------------------------------------------

// After moving an event from `from` to `to`, update all annotation paths
// so that the first element (which is the event index) stays correct.
static void update_annotation_paths_for_move(int from, int to)
{
	for (auto &ea : Event_annotations) {
		if (ea.path.empty())
			continue;
		int &event_idx = ea.path.front();
		if (event_idx == from) {
			event_idx = to;
		} else if (from < to) {
			// Elements in (from, to] shifted down by 1
			if (event_idx > from && event_idx <= to)
				event_idx--;
		} else {
			// Elements in [to, from) shifted up by 1
			if (event_idx >= to && event_idx < from)
				event_idx++;
		}
	}
}

// After swapping events at indices a and b, update all annotation paths.
static void update_annotation_paths_for_swap(int a, int b)
{
	for (auto &ea : Event_annotations) {
		if (ea.path.empty())
			continue;
		int &event_idx = ea.path.front();
		if (event_idx == a)
			event_idx = b;
		else if (event_idx == b)
			event_idx = a;
	}
}

// After inserting an event at `index`, shift all annotation paths >= index up by 1.
static void update_annotation_paths_for_insert(int index)
{
	for (auto &ea : Event_annotations) {
		if (ea.path.empty())
			continue;
		if (ea.path.front() >= index)
			ea.path.front()++;
	}
}

// Before deleting an event at `index`, remove annotations for that event
// and shift annotations for later events down by 1.
static void update_annotation_paths_for_delete(int index)
{
	Event_annotations.erase(
		std::remove_if(Event_annotations.begin(), Event_annotations.end(),
			[index](const event_annotation &ea) {
				return !ea.path.empty() && ea.path.front() == index;
			}),
		Event_annotations.end());

	for (auto &ea : Event_annotations) {
		if (ea.path.empty())
			continue;
		if (ea.path.front() > index)
			ea.path.front()--;
	}
}

// ---------------------------------------------------------------------------
// Event tool handlers (run on main thread)
// ---------------------------------------------------------------------------

static json_t *build_event_json(const mission_event &evt, int evt_index, bool include_details = false)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(evt.name.c_str()));
	json_object_set_new(obj, "index", json_integer(evt_index + 1));
	json_object_set_new(obj, "formula", json_integer(evt.formula));

	bool is_chained = (evt.chain_delay >= 0);
	json_object_set_new(obj, "is_chained", json_boolean(is_chained));

	if (include_details) {
		// Detail fields
		json_object_set_new(obj, "repeat_count", json_integer(evt.repeat_count));
		if (evt.flags & MEF_USING_TRIGGER_COUNT)
			json_object_set_new(obj, "trigger_count", json_integer(evt.trigger_count));
		json_object_set_new(obj, "interval", json_integer(evt.interval));

		json_object_set_new(obj, "chain_and_interval_units",
			json_string((evt.flags & MEF_USE_MSECS) ? "milliseconds" : "seconds"));

		json_object_set_new(obj, "score", json_integer(evt.score));
		if (is_chained)
			json_object_set_new(obj, "chain_delay", json_integer(evt.chain_delay));
		json_object_set_new(obj, "team", json_string(team_name_from_index(evt.team)));

		set_optional_string(obj, "objective_text", evt.objective_text.c_str(), true);
		set_optional_string(obj, "objective_key_text", evt.objective_key_text.c_str(), true);
	}

	return obj;
}

static void handle_list_events(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_events, sink)) return;

	json_t *arr = json_array();
	for (int i = 0; i < (int)Mission_events.size(); i++)
		json_array_append_new(arr, build_event_json(Mission_events[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_event(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_events, sink)) return;

	auto name = get_required_string(input, "name", sink, false);
	if (!name) return;

	int idx = find_item_with_string(Mission_events, &mission_event::name, name);
	if (idx < 0) {
		set_not_found_error(sink,"Event", name);
		return;
	}

	req->result_json = make_json_tool_result(build_event_json(Mission_events[idx], idx, true));
	req->success = true;
}

static void handle_create_event(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_events, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name || !check_string_length(name, NAME_LENGTH - 1, "name", sink)) return;

	// Check for duplicate name
	if (find_item_with_string(Mission_events, &mission_event::name, name) >= 0) {
		sink.set_error("An event with name '%s' already exists", name);
		return;
	}

	auto insert_index = get_optional_integer(input, "index", sink);

	// Optional parameters
	auto formula       = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_NULL, sink)) return;
	auto is_chained    = get_optional_bool(input, "is_chained", sink);
	auto repeat_count  = get_optional_integer(input, "repeat_count", sink);
	auto trigger_count = get_optional_integer(input, "trigger_count", sink);
	auto interval      = get_optional_integer(input, "interval", sink);

	auto units = get_optional_string(input, "chain_and_interval_units", sink, true);
	if (units && !check_string_enum(units, event_unit_enum_values, "chain_and_interval_units", sink))
		return;

	auto score         = get_optional_integer(input, "score", sink);
	auto chain_delay   = get_optional_integer(input, "chain_delay", sink);

	// if an event is chained but a chain delay is not specified, set the delay to 0;
	// similarly, if an event is explicitly unchained, set the delay to -1
	// (note: chain delay overrides is_chained)
	if (!chain_delay.has_value() && is_chained.has_value()) {
		chain_delay = *is_chained ? 0 : -1;
	}

	// Team
	auto team_str = get_optional_string(input, "team", sink, true);
	int multi_team = -1;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", sink))
			return;
		multi_team = team_index_from_name(team_str);
	}

	auto objective_text = get_optional_string(input, "objective_text", sink, false);
	auto objective_key_text = get_optional_string(input, "objective_key_text", sink, false);
	if (sink.has_error()) return;

	// Range validation
	if (repeat_count.has_value() && *repeat_count == 0) {
		sink.set_error("Parameter 'repeat_count' cannot be 0 (use -1 for infinite, or a positive value)");
		return;
	}
	if (trigger_count.has_value() && *trigger_count == 0) {
		sink.set_error("Parameter 'trigger_count' cannot be 0 (use -1 for infinite, or a positive value)");
		return;
	}
	if (interval.has_value() && *interval < 1) {
		sink.set_error("Parameter 'interval' must be >= 1");
		return;
	}
	if (chain_delay.has_value() && *chain_delay < -1) {
		sink.set_error("Parameter 'chain_delay' must be >= -1 (-1 = not chained)");
		return;
	}

	// Auto-set MEF_ flags if parameters provided
	int mef_flags = 0;
	if (trigger_count.has_value() && *trigger_count > 0)
		mef_flags |= MEF_USING_TRIGGER_COUNT;
	if (units && !stricmp(units, "milliseconds"))
		mef_flags |= MEF_USE_MSECS;

	// Validate insert index
	int target_index;
	if (!insert_index.has_value()) {
		target_index = (int)Mission_events.size();
	} else {
		if (!check_int_range(*insert_index, 1, (int)Mission_events.size() + 1, "index", sink))
			return;
		target_index = *insert_index - 1;
	}

	if (!formula.has_value()) {
		// Build default SEXP formula: (when (true) (do-nothing))
		int do_nothing = alloc_sexp("do-nothing", SEXP_LIST, SEXP_ATOM_OPERATOR, -1, -1);
		int true_node = alloc_sexp("true", SEXP_LIST, SEXP_ATOM_OPERATOR, -1, do_nothing);
		int when_node = alloc_sexp("when", SEXP_LIST, SEXP_ATOM_OPERATOR, true_node, -1);
		if (do_nothing < 0 || true_node < 0 || when_node < 0) {
			// Clean up any nodes that were successfully allocated
			if (when_node >= 0) free_sexp2(when_node);
			else if (true_node >= 0) free_sexp2(true_node);
			else if (do_nothing >= 0) free_sexp2(do_nothing);
			sink.set_error("Failed to allocate SEXP nodes for default event formula");
			return;
		}
		formula = when_node;
	}

	// Construct the event
	mission_event evt;
	evt.name = name;
	evt.formula = *formula;
	evt.repeat_count  = repeat_count.value_or(1);
	evt.trigger_count = trigger_count.value_or(1);
	evt.interval      = interval.value_or(1);
	evt.score         = score.value_or(0);
	evt.chain_delay   = chain_delay.value_or(-1);	// -1 means no chain
	evt.team          = multi_team;
	evt.flags         = mef_flags;
	evt.mission_log_flags = 0;

	if (objective_text && objective_text[0])
		evt.objective_text = objective_text;
	if (objective_key_text && objective_key_text[0])
		evt.objective_key_text = objective_key_text;

	// Insert
	update_annotation_paths_for_insert(target_index);
	Mission_events.insert(Mission_events.begin() + target_index, evt);
	mcp_sexp_forest_mark_dirty({ *formula });

	mark_modified("MCP: create event %s", name);

	req->result_json = make_json_tool_result(build_event_json(Mission_events[target_index], target_index, true));
	req->success = true;
}

static void handle_update_event(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_events, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int idx = find_item_with_string(Mission_events, &mission_event::name, name);
	if (idx < 0) {
		set_not_found_error(sink,"Event", name);
		return;
	}

	mission_event &evt = Mission_events[idx];

	// Validate new_name
	auto new_name      = get_optional_string(input, "new_name", sink, false);
	if (new_name) {
		if (!check_general_rename(new_name, evt.name.c_str(),
			[](const char *n) { return find_item_with_string(Mission_events, &mission_event::name, n) >= 0; },
			"Event", sink)) return;
	}

	// Extract optional fields
	auto formula       = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_NULL, sink)) return;
	auto is_chained    = get_optional_bool(input, "is_chained", sink);
	auto repeat_count  = get_optional_integer(input, "repeat_count", sink);
	auto trigger_count = get_optional_integer(input, "trigger_count", sink);
	auto interval      = get_optional_integer(input, "interval", sink);

	auto units = get_optional_string(input, "chain_and_interval_units", sink, true);
	if (units && !check_string_enum(units, event_unit_enum_values, "chain_and_interval_units", sink))
		return;

	auto score         = get_optional_integer(input, "score", sink);
	auto chain_delay   = get_optional_integer(input, "chain_delay", sink);

	if (!chain_delay.has_value() && is_chained.has_value()) {
		chain_delay = *is_chained ? 0 : -1;
	}

	// Team
	auto team_str = get_optional_string(input, "team", sink, true);
	std::optional<int> new_team = std::nullopt;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", sink))
			return;
		new_team = team_index_from_name(team_str);
	}

	auto objective_text     = get_optional_string(input, "objective_text", sink, false);
	auto objective_key_text = get_optional_string(input, "objective_key_text", sink, false);
	if (sink.has_error()) return;

	// Range validation
	if (repeat_count.has_value() && *repeat_count == 0) {
		sink.set_error("Parameter 'repeat_count' cannot be 0 (use -1 for infinite, or a positive value)");
		return;
	}
	if (trigger_count.has_value() && *trigger_count == 0) {
		sink.set_error("Parameter 'trigger_count' cannot be 0 (use -1 for infinite, or a positive value)");
		return;
	}
	if (interval.has_value() && *interval < 1) {
		sink.set_error("Parameter 'interval' must be >= 1");
		return;
	}
	if (chain_delay.has_value() && *chain_delay < -1) {
		sink.set_error("Parameter 'chain_delay' must be >= -1 (-1 = not chained)");
		return;
	}

	// Auto-set MEF_ flags if parameters provided
	std::optional<int> new_mef_flags;
	if (trigger_count.has_value()) {
		if (!new_mef_flags.has_value()) {
			new_mef_flags = evt.flags;
		}
		if (*trigger_count > 0)
			*new_mef_flags |= MEF_USING_TRIGGER_COUNT;
		else
			*new_mef_flags &= ~MEF_USING_TRIGGER_COUNT;
	}
	if (units) {
		if (!new_mef_flags.has_value()) {
			new_mef_flags = evt.flags;
		}
		if (!stricmp(units, "milliseconds"))
			*new_mef_flags |= MEF_USE_MSECS;
		else
			*new_mef_flags &= ~MEF_USE_MSECS;
	}

	bool changed = false;

	if (formula.has_value() && evt.formula != *formula) {
		if (evt.formula >= 0)
			free_sexp2(evt.formula);
		evt.formula = *formula;
		changed = true;
		mcp_sexp_forest_mark_dirty({ *formula });
	}
	if (repeat_count.has_value() && evt.repeat_count != *repeat_count) {
		evt.repeat_count = *repeat_count;
		changed = true;
	}
	if (trigger_count.has_value() && evt.trigger_count != *trigger_count) {
		evt.trigger_count = *trigger_count;
		changed = true;
	}
	if (interval.has_value() && evt.interval != *interval) {
		evt.interval = *interval;
		changed = true;
	}
	if (score.has_value() && evt.score != *score) {
		evt.score = *score;
		changed = true;
	}
	if (chain_delay.has_value() && evt.chain_delay != *chain_delay) {
		evt.chain_delay = *chain_delay;
		changed = true;
	}
	if (new_team.has_value() && evt.team != *new_team) {
		evt.team = *new_team;
		changed = true;
	}
	if (new_mef_flags.has_value() && evt.flags != *new_mef_flags) {
		evt.flags = *new_mef_flags;
		changed = true;
	}
	if (objective_text && evt.objective_text != objective_text) {
		evt.objective_text = objective_text;
		changed = true;
	}
	if (objective_key_text && evt.objective_key_text != objective_key_text) {
		evt.objective_key_text = objective_key_text;
		changed = true;
	}

	// Rename last — updates SEXP references
	if (new_name && stricmp(evt.name.c_str(), new_name) != 0) {
		update_sexp_references(evt.name.c_str(), new_name, OPF_EVENT_NAME);
		evt.name = new_name;
		changed = true;
	}

	if (changed)
		mark_modified("MCP: update event %s", evt.name.c_str());

	req->result_json = make_json_tool_result(build_event_json(evt, idx, true));
	req->success = true;
}

static void handle_delete_event(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_events, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int idx = find_item_with_string(Mission_events, &mission_event::name, name);
	if (idx < 0) {
		set_not_found_error(sink,"Event", name);
		return;
	}

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	if (!check_and_report_sexp_refs(sexp_ref_type::NON_OBJECT, "Event", Mission_events[idx].name.c_str(), force, sink))
		return;

	// Invalidate SEXP references
	SCP_string buf = SCP_string("<") + Mission_events[idx].name + ">";
	update_sexp_references(Mission_events[idx].name.c_str(), buf.c_str(), OPF_EVENT_NAME);

	// Free the SEXP formula
	if (Mission_events[idx].formula >= 0)
		free_sexp2(Mission_events[idx].formula);

	// Update annotations and remove from vector
	update_annotation_paths_for_delete(idx);
	Mission_events.erase(Mission_events.begin() + idx);

	mark_modified("MCP: delete event %s", name);

	sprintf(req->result_message, "Deleted event: %s", name);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Generic move/swap handlers
// ---------------------------------------------------------------------------

// Configuration for entity-specific move/swap behavior.
// Lambdas encapsulate offsets, annotation updates, and array access.
struct MoveSwapConfig
{
	const char *entity_name = nullptr;
	int count = 0;
	bool one_based = false;		// if true, user-facing indices start at 1 instead of 0
	std::function<bool(SCP_string&)> validate_dialog = nullptr;
	std::function<SCP_string(int)> get_name = nullptr;	// copy-return isn't ideal, but certain use cases work better with it and the MCP isn't performance-critical
	std::function<void(int, int)> do_move = nullptr;
	std::function<void(int, int)> do_swap = nullptr;

	int min_index() const { return one_based ? 1 : 0; }
	int max_index() const { return one_based ? count : count - 1; }
};

static void handle_generic_move(json_t *input, McpToolRequest *req, const MoveSwapConfig &cfg)
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

static void handle_generic_swap(json_t *input, McpToolRequest *req, const MoveSwapConfig &cfg)
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

static MoveSwapConfig make_event_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "event";
	cfg.count = (int)Mission_events.size();
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_events;
	cfg.get_name = [](int i) {
		return Mission_events[i - 1].name;
	};
	cfg.do_move = [](int from, int to) {
		update_annotation_paths_for_move(from - 1, to - 1);
		array_move_element(Mission_events, from - 1, to - 1);
	};
	cfg.do_swap = [](int a, int b) {
		update_annotation_paths_for_swap(a - 1, b - 1);
		std::swap(Mission_events[a - 1], Mission_events[b - 1]);
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

static void handle_move_event(json_t *input, McpToolRequest *req)
{
	auto cfg = make_event_move_swap_config();
	handle_generic_move(input, req, cfg);
}

static void handle_swap_events(json_t *input, McpToolRequest *req)
{
	auto cfg = make_event_move_swap_config();
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Command briefing tool handlers (run on main thread)
// ---------------------------------------------------------------------------

// Resolve optional "team" parameter to a cmd_brief pointer.
// Returns nullptr and sets error on failure.
static cmd_brief *get_cmd_brief_for_team(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto team_str = get_optional_string(input, "team", sink, true);
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

	auto *cb = get_cmd_brief_for_team(input, req);
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

	auto *cb = get_cmd_brief_for_team(input, req);
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

	auto *cb = get_cmd_brief_for_team(input, req);
	if (!cb) return;

	if (cb->num_stages >= CMD_BRIEF_STAGES_MAX) {
		sink.set_error("Cannot add more than %d command briefing stages", CMD_BRIEF_STAGES_MAX);
		return;
	}

	auto text = get_required_string(input, "text", sink, false);
	if (!text) return;

	auto ani = get_optional_filename(input, "animation_filename", sink, false);
	auto wave = get_optional_filename(input, "voice_filename", sink, false);
	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	// Validate filename lengths
	if (ani && !check_string_length(ani, MAX_FILENAME_LEN - 1, "animation_filename", sink)) return;
	if (wave && !check_string_length(wave, MAX_FILENAME_LEN - 1, "voice_filename", sink)) return;

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

	auto *cb = get_cmd_brief_for_team(input, req);
	if (!cb) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, cb->num_stages, "index", sink)) return;

	auto new_text = get_optional_string(input, "text", sink, false);
	auto new_ani  = get_optional_filename(input, "animation_filename", sink, false);
	auto new_wave = get_optional_filename(input, "voice_filename", sink, false);
	if (sink.has_error()) return;

	// Validate filename lengths
	if (new_ani && !check_string_length(new_ani, MAX_FILENAME_LEN - 1, "animation_filename", sink)) return;
	if (new_wave && !check_string_length(new_wave, MAX_FILENAME_LEN - 1, "voice_filename", sink)) return;

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

	auto *cb = get_cmd_brief_for_team(input, req);
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
	auto *cb = get_cmd_brief_for_team(input, req);
	if (!cb) return;

	auto cfg = make_cmd_brief_move_swap_config(cb);
	handle_generic_move(input, req, cfg);
}

static void handle_swap_cmd_brief_stages(json_t *input, McpToolRequest *req)
{
	auto *cb = get_cmd_brief_for_team(input, req);
	if (!cb) return;

	auto cfg = make_cmd_brief_move_swap_config(cb);
	handle_generic_swap(input, req, cfg);
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
	json_object_set_new(obj, "name", json_string(goal.name.c_str()));
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "goal_type", json_string(goal_type_name(goal.type)));
	json_object_set_new(obj, "formula", json_integer(goal.formula));

	if (include_details) {
		json_object_set_new(obj, "is_valid", json_boolean(!(goal.type & INVALID_GOAL)));
		json_object_set_new(obj, "message", json_string(goal.message.c_str()));
		json_object_set_new(obj, "score", json_integer(goal.score));
		json_object_set_new(obj, "team", json_string(team_name_from_index(goal.team)));
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

	auto name = get_required_string(input, "name", sink, false);
	if (!name) return;

	int idx = find_item_with_string(Mission_goals, &mission_goal::name, name);
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

	auto name = get_required_string(input, "name", sink, true);
	if (!name || !check_string_length(name, NAME_LENGTH - 1, "name", sink)) return;

	// Check for duplicate name
	if (find_item_with_string(Mission_goals, &mission_goal::name, name) >= 0) {
		sink.set_error("A goal with name '%s' already exists", name);
		return;
	}

	auto insert_index = get_optional_integer(input, "index", sink);

	// Optional parameters
	auto type_str   = get_optional_string(input, "goal_type", sink, true);
	auto formula    = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, sink)) return;
	auto message    = get_optional_string(input, "message", sink, false);
	auto score      = get_optional_integer(input, "score", sink);
	auto team_str   = get_optional_string(input, "team", sink, true);
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
		// Build default SEXP formula: (true) — matching FRED2 editor pattern
		formula = alloc_sexp("true", SEXP_LIST, SEXP_ATOM_OPERATOR, -1, -1);
		if (*formula < 0) {
			sink.set_error("Failed to allocate SEXP node for default goal formula");
			return;
		}
	}

	// Construct the goal
	mission_goal goal;
	goal.name = name;
	goal.type = goal_type;
	goal.formula = *formula;
	goal.score = score.value_or(0);
	goal.team = team;
	goal.flags = 0;

	if (message && message[0])
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

	int idx = find_item_with_string(Mission_goals, &mission_goal::name, name);
	if (idx < 0) {
		set_not_found_error(sink,"Goal", name);
		return;
	}

	mission_goal &goal = Mission_goals[idx];

	// Validate new_name
	auto new_name = get_optional_string(input, "new_name", sink, false);
	if (new_name) {
		if (!check_general_rename(new_name, goal.name.c_str(),
			[](const char *n) { return find_item_with_string(Mission_goals, &mission_goal::name, n) >= 0; },
			"Goal", sink)) return;
	}

	// Extract optional fields
	auto type_str   = get_optional_string(input, "goal_type", sink, true);
	auto formula    = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, sink)) return;
	auto message    = get_optional_string(input, "message", sink, false);
	auto score      = get_optional_integer(input, "score", sink);
	auto team_str   = get_optional_string(input, "team", sink, true);
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

	int idx = find_item_with_string(Mission_goals, &mission_goal::name, name);
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

	// Free the SEXP formula
	if (Mission_goals[idx].formula >= 0)
		free_sexp2(Mission_goals[idx].formula);

	Mission_goals.erase(Mission_goals.begin() + idx);

	mark_modified("MCP: delete goal %s", name);

	sprintf(req->result_message, "Deleted goal: %s", name);
	req->success = true;
}

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
// Fiction Viewer stage tools
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> fiction_ui_name_values = { "FS2", "WCS" };

static json_t *build_fiction_viewer_stage_json(const fiction_viewer_stage &stage, int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "story_filename", json_string(stage.story_filename));
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

	auto story = get_required_filename(input, "story_filename", sink);
	if (!story || !check_string_length(story, MAX_FILENAME_LEN - 1, "story_filename", sink)) return;

	auto font  = get_optional_filename(input, "font_filename", sink, true);
	auto voice = get_optional_filename(input, "voice_filename", sink, true);
	auto ui    = get_optional_string(input, "ui_name", sink, true);
	auto bg640 = get_optional_filename(input, "background_640", sink, true);
	auto bg1024 = get_optional_filename(input, "background_1024", sink, true);
	auto formula = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, sink)) return;
	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	if (font && !check_string_length(font, MAX_FILENAME_LEN - 1, "font_filename", sink)) return;
	if (voice && !check_string_length(voice, MAX_FILENAME_LEN - 1, "voice_filename", sink)) return;
	if (ui && !check_string_enum(ui, fiction_ui_name_values, "ui_name", sink)) return;
	if (bg640 && !check_string_length(bg640, MAX_FILENAME_LEN - 1, "background_640", sink)) return;
	if (bg1024 && !check_string_length(bg1024, MAX_FILENAME_LEN - 1, "background_1024", sink)) return;

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
	if (font)  strcpy_s(stage.font_filename, font);
	if (voice) strcpy_s(stage.voice_filename, voice);
	if (ui)    strcpy_s(stage.ui_name, ui);
	if (bg640) strcpy_s(stage.background[0], bg640);
	if (bg1024) strcpy_s(stage.background[1], bg1024);

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

	auto new_story = get_optional_filename(input, "story_filename", sink, false);
	auto new_font  = get_optional_filename(input, "font_filename", sink, false);
	auto new_voice = get_optional_filename(input, "voice_filename", sink, false);
	auto new_ui    = get_optional_string(input, "ui_name", sink, false);
	auto new_bg640 = get_optional_filename(input, "background_640", sink, false);
	auto new_bg1024 = get_optional_filename(input, "background_1024", sink, false);
	auto new_formula = get_optional_integer(input, "formula", sink);
	if (sink.has_error()) return;
	if (new_formula.has_value() && !check_sexp_formula(*new_formula, OPR_BOOL, sink)) return;

	if (new_story && !check_string_length(new_story, MAX_FILENAME_LEN - 1, "story_filename", sink)) return;
	if (new_font && !check_string_length(new_font, MAX_FILENAME_LEN - 1, "font_filename", sink)) return;
	if (new_voice && !check_string_length(new_voice, MAX_FILENAME_LEN - 1, "voice_filename", sink)) return;
	if (new_ui) {
		if (!new_ui[0])
			new_ui = fiction_ui_name_values[0];
		else if (!check_string_enum(new_ui, fiction_ui_name_values, "ui_name", sink))
			return;
	}
	if (new_bg640 && !check_string_length(new_bg640, MAX_FILENAME_LEN - 1, "background_640", sink)) return;
	if (new_bg1024 && !check_string_length(new_bg1024, MAX_FILENAME_LEN - 1, "background_1024", sink)) return;

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
static debriefing *get_debriefing_for_team(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto team_str = get_optional_string(input, "team", sink, true);
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

	auto *db = get_debriefing_for_team(input, req);
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

	auto *db = get_debriefing_for_team(input, req);
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

	auto *db = get_debriefing_for_team(input, req);
	if (!db) return;

	if (db->num_stages >= MAX_DEBRIEF_STAGES) {
		sink.set_error("Cannot add more than %d debriefing stages", MAX_DEBRIEF_STAGES);
		return;
	}

	auto text = get_required_string(input, "text", sink, false);
	if (!text) return;

	auto voice = get_optional_filename(input, "voice_filename", sink, false);
	auto rec_text = get_optional_string(input, "recommendation_text", sink, false);
	auto formula = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, sink)) return;
	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	if (voice && !check_string_length(voice, MAX_FILENAME_LEN - 1, "voice_filename", sink)) return;

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

	auto *db = get_debriefing_for_team(input, req);
	if (!db) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, db->num_stages, "index", sink)) return;

	auto new_text = get_optional_string(input, "text", sink, false);
	auto new_voice = get_optional_filename(input, "voice_filename", sink, false);
	auto new_rec_text = get_optional_string(input, "recommendation_text", sink, false);
	auto new_formula = get_optional_integer(input, "formula", sink);
	if (sink.has_error()) return;
	if (new_formula.has_value() && !check_sexp_formula(*new_formula, OPR_BOOL, sink)) return;

	if (new_voice && !check_string_length(new_voice, MAX_FILENAME_LEN - 1, "voice_filename", sink)) return;

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

	auto *db = get_debriefing_for_team(input, req);
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
	auto *db = get_debriefing_for_team(input, req);
	if (!db) return;

	auto cfg = make_debriefing_move_swap_config(db);
	handle_generic_move(input, req, cfg);
}

static void handle_swap_debriefing_stages(json_t *input, McpToolRequest *req)
{
	auto *db = get_debriefing_for_team(input, req);
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
			json_object_set_new(obj, "model_filename", json_string(model_filename));
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

	const char *name = get_required_string(input, "name", sink, false);
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

	auto name = get_required_string(input, "name", sink, true);
	if (!name || !check_string_length(name, NAME_LENGTH - 1, "name", sink)) return;

	auto pos = get_required_vec3d(input, "position", sink);
	if (!pos.has_value()) return;

	auto display_name = get_optional_string(input, "display_name", sink, false);
	auto color_val    = get_optional_color(input, "color", sink);
	auto model_file   = get_optional_string(input, "model_filename", sink, false);
	auto show_polys   = get_optional_bool(input, "show_polys", sink);
	auto hidden       = get_optional_bool(input, "hidden", sink);
	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	// Validate name against all object types
	if (!check_object_rename("jump node", name, sink)) return;

	if (display_name && !check_string_length(display_name, NAME_LENGTH - 1, "display_name", sink)) return;
	if (model_file && !check_string_length(model_file, MAX_FILENAME_LEN - 1, "model_filename", sink)) return;

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

	Jumpnode_editor_dialog.initialize_data(1);
	mark_modified("MCP: create jump node %s", name);

	req->result_json = make_json_tool_result(build_jump_node_json(Jump_nodes[target_index], target_index, true));
	req->success = true;
}

static void handle_update_jump_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	const char *name = get_required_string(input, "name", sink, false);
	if (!name) return;

	auto new_name     = get_optional_string(input, "new_name", sink, true);
	auto new_pos      = get_optional_vec3d(input, "position", sink);
	auto display_name = get_optional_string(input, "display_name", sink, false);
	auto color_val    = get_optional_color(input, "color", sink);
	auto model_file   = get_optional_string(input, "model_filename", sink, false);
	auto show_polys   = get_optional_bool(input, "show_polys", sink);
	auto hidden       = get_optional_bool(input, "hidden", sink);
	if (sink.has_error()) return;

	if (new_name && !check_string_length(new_name, NAME_LENGTH - 1, "new_name", sink)) return;
	if (display_name && !check_string_length(display_name, NAME_LENGTH - 1, "display_name", sink)) return;
	if (model_file && !check_string_length(model_file, MAX_FILENAME_LEN - 1, "model_filename", sink)) return;

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
		if (display_name[0] == '\0') {
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

	const char *name = get_required_string(input, "name", sink, false);
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

	Jump_nodes.erase(Jump_nodes.begin() + index);

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

	const char *name = get_required_string(input, "name", sink, false);
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
}

static void rename_waypoint_sexp_refs_to_temp(const char *list_name, int one_based, char *temp_buf, size_t buf_size)
{
	char name[NAME_LENGTH];
	waypoint_stuff_name(name, list_name, one_based);
	snprintf(temp_buf, buf_size, "<temp_wpt_%d>", one_based);
	update_sexp_references(name, temp_buf);
	ai_update_goal_references(sexp_ref_type::WAYPOINT, name, temp_buf);
}

static void rename_waypoint_sexp_refs_from_temp(const char *temp_name, const char *list_name, int new_1based)
{
	char new_name[NAME_LENGTH];
	waypoint_stuff_name(new_name, list_name, new_1based);
	update_sexp_references(temp_name, new_name);
	ai_update_goal_references(sexp_ref_type::WAYPOINT, temp_name, new_name);
}

static void handle_create_waypoint_list(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_waypoint_lists, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name || !check_string_length(name, NAME_LENGTH - 1, "name", sink)) return;

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

	const char *name = get_required_string(input, "name", sink, false);
	if (!name) return;

	auto new_name = get_optional_string(input, "new_name", sink, true);
	if (sink.has_error()) return;

	if (new_name && !check_string_length(new_name, NAME_LENGTH - 1, "new_name", sink)) return;

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

	const char *name = get_required_string(input, "name", sink, false);
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

	auto list = get_required_string(input, "list", sink, false);
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

	auto list = get_required_string(input, "list", sink, false);
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

	auto list = get_required_string(input, "list", sink, false);
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
	const char *list_name = wl.get_name();
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
	const char *list_name = wl.get_name();
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
	auto list = get_required_string(input, "list", sink, false);
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
	auto list = get_required_string(input, "list", sink, false);
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
// SEXP text serialization
// ---------------------------------------------------------------------------

static void handle_sexp_to_text(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;
	auto node = get_required_integer(input, "node", sink);
	if (!node) return;
	if (!check_int_range(*node, 0, Num_sexp_nodes - 1, "node", sink)) return;

	int n = *node;

	if (Sexp_nodes[n].type == SEXP_NOT_USED) {
		sink.set_error("Node %d is not in use", n);
		return;
	}

	SCP_string text;
	convert_sexp_to_string(text, n, SEXP_SAVE_MODE);

	json_t *result = json_object();
	json_object_set_new(result, "node", json_integer(n));
	json_object_set_new(result, "text", json_string(text.c_str()));
	req->result_json = make_json_tool_result(result);
	req->success = true;
}

// ---------------------------------------------------------------------------
// SEXP node navigation
// ---------------------------------------------------------------------------

static const char *get_sexp_kind_name(int n)
{
	switch (SEXP_NODE_TYPE(n)) {
		case SEXP_LIST: return "list";
		case SEXP_ATOM: return "atom";
		default:        return "not_used";
	}
}

static const char *get_sexp_value_type_name(int n)
{
	int kind = SEXP_NODE_TYPE(n);
	if (kind != SEXP_ATOM && kind != SEXP_LIST)
		return nullptr;
	bool is_variable = (Sexp_nodes[n].type & SEXP_FLAG_VARIABLE) != 0;
	switch (Sexp_nodes[n].subtype) {
		case SEXP_ATOM_OPERATOR:       return "operator";
		case SEXP_ATOM_NUMBER:         return is_variable ? "numeric_variable" : "numeric_literal";
		case SEXP_ATOM_STRING:         return is_variable ? "string_variable" : "string_literal";
		case SEXP_ATOM_CONTAINER_NAME: return "container_name";
		case SEXP_ATOM_CONTAINER_DATA: return "container_data";
		default:                       return nullptr;
	}
}

static constexpr int MAX_SEXP_WALK_NODES = 500;

static json_t *build_sexp_node_json(int n)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "node", json_integer(n));
	json_object_set_new(obj, "value", json_string(Sexp_nodes[n].text));

	json_object_set_new(obj, "kind", json_string(get_sexp_kind_name(n)));
	const char *node_type = get_sexp_value_type_name(n);
	json_object_set_new(obj, "value_type", node_type ? json_string(node_type) : json_null());

	json_object_set_new(obj, "node_parent", json_integer(Sexp_nodes[n].parent));
	json_object_set_new(obj, "node_first", json_integer(Sexp_nodes[n].first));
	json_object_set_new(obj, "node_rest", json_integer(Sexp_nodes[n].rest));

	int op_index = get_operator_index(n);
	if (op_index >= 0) {
		int ret = query_operator_return_type(op_index);
		json_object_set_new(obj, "return_type", json_string(get_opr_type_name(ret)));
	} else {
		json_object_set_new(obj, "return_type", json_null());
	}

	return obj;
}

static void handle_get_sexp_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;
	auto node = get_required_integer(input, "node", sink);
	if (!node) return;
	if (!check_int_range(*node, 0, Num_sexp_nodes - 1, "node", sink)) return;

	int n = *node;
	if (SEXP_NODE_TYPE(n) == SEXP_NOT_USED) {
		sink.set_error("Node %d is not in use", n);
		return;
	}

	req->result_json = make_json_tool_result(build_sexp_node_json(n));
	req->success = true;
}

struct walk_entry {
	int node;
	int depth;
};

static void collect_walk_entries(int n, SCP_vector<walk_entry> &entries, int depth, int max_depth)
{
	if (n < 0 || n >= Num_sexp_nodes || (int)entries.size() >= MAX_SEXP_WALK_NODES)
		return;
	if (SEXP_NODE_TYPE(n) == SEXP_NOT_USED)
		return;
	if (max_depth >= 0 && depth > max_depth)
		return;

	entries.push_back({n, depth});
	collect_walk_entries(Sexp_nodes[n].first, entries, depth + 1, max_depth);
	collect_walk_entries(Sexp_nodes[n].rest, entries, depth, max_depth);
}

static void handle_walk_sexp_tree(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;
	auto node = get_required_integer(input, "node", sink);
	if (!node) return;
	if (!check_int_range(*node, 0, Num_sexp_nodes - 1, "node", sink)) return;

	int n = *node;
	if (SEXP_NODE_TYPE(n) == SEXP_NOT_USED) {
		sink.set_error("Node %d is not in use", n);
		return;
	}

	auto depth = get_optional_integer(input, "depth", sink);
	if (sink.has_error()) return;
	int max_depth = depth.has_value() ? *depth : -1;

	// Pass 1: collect entries with depths
	SCP_vector<walk_entry> entries;
	collect_walk_entries(n, entries, 0, max_depth);

	// Build node index -> position map
	SCP_unordered_map<int, int> pos_map;
	for (int i = 0; i < (int)entries.size(); i++)
		pos_map[entries[i].node] = i;

	// Pass 2: build JSON with walk references and depth
	json_t *arr = json_array();
	for (const auto &entry : entries) {
		json_t *obj = build_sexp_node_json(entry.node);

		json_object_set_new(obj, "depth", json_integer(entry.depth));

		auto it_first = pos_map.find(Sexp_nodes[entry.node].first);
		json_object_set_new(obj, "walk_first",
			(it_first != pos_map.end()) ? json_integer(it_first->second) : json_integer(-1));

		auto it_rest = pos_map.find(Sexp_nodes[entry.node].rest);
		json_object_set_new(obj, "walk_rest",
			(it_rest != pos_map.end()) ? json_integer(it_rest->second) : json_integer(-1));

		json_array_append_new(arr, obj);
	}

	json_t *result = json_object();
	json_object_set_new(result, "root", json_integer(n));
	json_object_set_new(result, "nodes", arr);
	if ((int)entries.size() >= MAX_SEXP_WALK_NODES)
		json_object_set_new(result, "truncated", json_true());

	req->result_json = make_json_tool_result(result);
	req->success = true;
}

// ---------------------------------------------------------------------------
// SEXP text parsing
// ---------------------------------------------------------------------------

static void handle_text_to_sexp(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;
	const char *text = get_required_string(input, "text", sink, true);
	if (!text)
		return;

	// Save global parse state (Mp, Current_filename, Warning_count, Error_count)
	pause_parse();

	SCP_string buf(text);
	Mp = buf.data();
	strcpy_s(Current_filename, "text_to_sexp");

	// Enable error collection so error_display() doesn't show modal dialogs
	Parse_collect_errors = true;
	Parse_errors.clear();

	int n = get_sexp_main();

	Parse_collect_errors = false;

	// Restore global parse state
	unpause_parse();

	// Check for collected parse errors
	if (!Parse_errors.empty()) {
		// Free any partially-allocated SEXP nodes
		if (n >= 0)
			free_sexp2(n);

		json_t *result = json_object();
		json_t *errors = json_array();
		for (const auto &e : Parse_errors) {
			json_t *entry = json_object();
			json_object_set_new(entry, "level", json_string(e.level == 0 ? "warning" : "error"));
			json_object_set_new(entry, "line", json_integer(e.line));
			json_object_set_new(entry, "message", json_string(e.message.c_str()));
			json_array_append_new(errors, entry);
		}
		json_object_set_new(result, "parse_errors", errors);
		int error_count = (int)Parse_errors.size();
		Parse_errors.clear();

		req->result_json = make_json_tool_result(result);
		json_object_set_new(req->result_json, "isError", json_true());
		sink.set_error("SEXP text had %d parse error(s); see parse_errors in result",
			error_count);
		return;
	} else if (n < 0) {
		sink.set_error("Could not parse SEXP; get_sexp_main() returned %d", n);
		return;
	}

	// Run syntax check
	json_t *result = json_object();
	json_object_set_new(result, "node", json_integer(n));

	json_t *syntax_err = build_syntax_error_json(n);
	if (syntax_err)
		json_object_set_new(result, "syntax_error", syntax_err);

	// Round-trip the text for verification
	SCP_string round_tripped;
	convert_sexp_to_string(round_tripped, n, SEXP_SAVE_MODE);
	json_object_set_new(result, "parsed_text", json_string(round_tripped.c_str()));

	req->result_json = make_json_tool_result(result);
	req->success = true;

	mcp_sexp_forest_mark_dirty({ n });
}

// ---------------------------------------------------------------------------
// SEXP tree freeing
// ---------------------------------------------------------------------------

static bool is_sexp_attached_to_mission(int node)
{
	// Mission cutscenes
	for (const auto &cs : The_mission.cutscenes) {
		if (cs.formula == node)
			return true;
	}

	// Fiction viewer stages
	for (const auto &stage : Fiction_viewer_stages) {
		if (stage.formula == node)
			return true;
	}

	// Briefing stages (per team)
	for (int t = 0; t < MAX_TVT_TEAMS; t++) {
		for (int s = 0; s < Briefings[t].num_stages; s++) {
			if (Briefings[t].stages[s].formula == node)
				return true;
		}
	}

	// Debriefing stages (per team)
	for (int t = 0; t < MAX_TVT_TEAMS; t++) {
		for (int s = 0; s < Debriefings[t].num_stages; s++) {
			if (Debriefings[t].stages[s].formula == node)
				return true;
		}
	}

	// Ship arrival/departure cues
	for (auto objp: list_range(&obj_used_list)) {
		if (objp->type == OBJ_SHIP || objp->type == OBJ_START) {
			auto shipp = &Ships[objp->instance];
			if (shipp->arrival_cue == node || shipp->departure_cue == node)
				return true;
		}
	}

	// Wing arrival/departure cues
	for (int i = 0; i < Num_wings; i++) {
		if (Wings[i].arrival_cue == node || Wings[i].departure_cue == node)
			return true;
	}

	// Events
	for (const auto &evt : Mission_events) {
		if (evt.formula == node)
			return true;
	}

	// Goals
	for (const auto &goal : Mission_goals) {
		if (goal.formula == node)
			return true;
	}

	return false;
}

static void handle_free_sexp(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;
	auto node = get_required_integer(input, "node", sink);
	if (!node.has_value() || !check_int_range(*node, 0, Num_sexp_nodes - 1, "node", sink))
		return;

	int n = *node;

	if (Sexp_nodes[n].type == SEXP_NOT_USED) {
		sink.set_error("Node %d is not in use", n);
		return;
	}

	if (n == Locked_sexp_true || n == Locked_sexp_false) {
		sink.set_error("Node %d is a locked singleton (%s) and cannot be freed", n, Sexp_nodes[n].text);
		return;
	}

	if (is_sexp_attached_to_mission(n)) {
		sink.set_error("Node %d is attached to a mission entity (event, goal, briefing, etc.) and cannot be freed directly", n);
		return;
	}

	int freed_count = free_sexp2(n);

	json_t *result = json_object();
	json_object_set_new(result, "node", json_integer(n));
	json_object_set_new(result, "freed_count", json_integer(freed_count));
	req->result_json = make_json_tool_result(result);
	req->success = true;
}

// ---------------------------------------------------------------------------
// create_sexp_node handler (run on main thread)
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> sexp_arg_type_values = { "number", "string", "boolean", "node" };

// Check if an MCP argument type is compatible with the expected OPF type at a
// given operator argument position.
//   opf         - expected OPF_* type from query_operator_argument_type
//   arg_type    - one of "number", "string", "boolean", "node"
//   is_variable - true if the value has an @ prefix (variable reference)
//   node_opr    - for "node" type, the OPR return type of the referenced operator; ignored otherwise
static bool is_arg_type_compatible(int opf, const char *arg_type, bool is_variable, int node_opr)
{
	if (opf == OPF_NONE || opf == OPF_UNUSED)
		return false;

	// Variables are compatible with OPF_VARIABLE_NAME and OPF_AMBIGUOUS,
	// plus the same OPFs as their base type
	if (is_variable) {
		if (opf == OPF_VARIABLE_NAME || opf == OPF_AMBIGUOUS)
			return true;
	}

	if (!stricmp(arg_type, "boolean"))
		return opf == OPF_BOOL;

	if (!stricmp(arg_type, "node"))
		return sexp_query_type_match(opf, node_opr);

	if (!stricmp(arg_type, "number")) {
		if (opf == OPF_AMBIGUOUS)
			return true;
		sexp_opr_t opr;
		if (map_opf_to_opr((sexp_opf_t)opf, opr))
			return opr == OPR_NUMBER || opr == OPR_POSITIVE;
		return false;
	}

	if (!stricmp(arg_type, "string")) {
		if (opf == OPF_AMBIGUOUS)
			return true;
		// Sub-expression OPFs (those that map to an OPR type) don't accept strings
		sexp_opr_t opr;
		if (map_opf_to_opr((sexp_opf_t)opf, opr))
			return false;
		// Data OPFs (ship, wing, message, etc.) accept strings
		return true;
	}

	return false;
}

// Parse and allocate a single argument node.  Returns the allocated node index,
// or -1 on error (with an error message via sink).  `next` is the rest pointer
// for the new node (building the chain right-to-left).
static int create_sexp_arg_node(const char *type_str, const char *value, int next, int arg_index, McpErrorSink &sink)
{
	// Variable handling for number/string types
	if ((!stricmp(type_str, "number") || !stricmp(type_str, "string")) && value[0] == '@') {
		const char *var_name = value + 1;
		int var_idx = get_index_sexp_variable_name(var_name);
		if (var_idx < 0) {
			sink.set_error("Unknown SEXP variable '%s' in argument %d", var_name, arg_index);
			return -1;
		}
		int subtype = !stricmp(type_str, "number") ? SEXP_ATOM_NUMBER : SEXP_ATOM_STRING;
		return alloc_sexp(Sexp_variables[var_idx].variable_name,
			SEXP_ATOM | SEXP_FLAG_VARIABLE, subtype, -1, next);
	}

	if (!stricmp(type_str, "number")) {
		return alloc_sexp(value, SEXP_ATOM, SEXP_ATOM_NUMBER, -1, next);
	}

	if (!stricmp(type_str, "string")) {
		return alloc_sexp(value, SEXP_ATOM, SEXP_ATOM_STRING, -1, next);
	}

	if (!stricmp(type_str, "boolean")) {
		int locked_node;
		if (!stricmp(value, "true"))
			locked_node = Locked_sexp_true;
		else if (!stricmp(value, "false"))
			locked_node = Locked_sexp_false;
		else {
			sink.set_error("Boolean argument %d must be \"true\" or \"false\", got \"%s\"", arg_index, value);
			return -1;
		}
		return alloc_sexp("", SEXP_LIST, SEXP_ATOM_LIST, locked_node, next);
	}

	if (!stricmp(type_str, "node")) {
		char *endptr;
		long idx = strtol(value, &endptr, 10);
		if (*endptr != '\0' || endptr == value) {
			sink.set_error("Node argument %d: value must be an integer node index, got \"%s\"", arg_index, value);
			return -1;
		}
		// -1 is a placeholder meaning "empty slot, to be filled later"
		if (idx == -1) {
			return alloc_sexp(PLACEHOLDER_STRING, SEXP_ATOM, SEXP_ATOM_STRING, -1, next);
		}
		if (idx < 0 || idx >= Num_sexp_nodes) {
			sink.set_error("Node argument %d: node index %ld is out of range (0-%d)", arg_index, idx, Num_sexp_nodes - 1);
			return -1;
		}
		if (Sexp_nodes[(int)idx].type == SEXP_NOT_USED) {
			sink.set_error("Node argument %d: node %ld is not in use", arg_index, idx);
			return -1;
		}
		// Always wrap in SEXP_LIST to avoid corrupting existing tree linkage
		return alloc_sexp("", SEXP_LIST, SEXP_ATOM_LIST, (int)idx, next);
	}

	// Should not reach here if type was validated
	sink.set_error("Unknown argument type \"%s\" in argument %d", type_str, arg_index);
	return -1;
}

static void handle_create_sexp_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	auto op_name = get_required_string(input, "operator", sink, true);
	if (!op_name) return;
	if (!check_string_length(op_name, TOKEN_LENGTH - 1, "operator", sink)) return;

	// Validate operator name
	int op_idx = get_operator_index(op_name);
	if (op_idx < 0) {
		sink.set_error("Unknown SEXP operator: '%s'", op_name);
		return;
	}

	// Create operator atom
	int op_node = alloc_sexp(op_name, SEXP_ATOM, SEXP_ATOM_OPERATOR, -1, -1);

	// Handle locked singletons (true/false operators)
	bool op_is_locked = (op_node == Locked_sexp_true || op_node == Locked_sexp_false);

	// Parse arguments (if any)
	json_t *warnings = nullptr;
	json_t *args = json_object_get(input, "operator_arguments");
	if (args && json_is_array(args) && json_array_size(args) > 0) {
		size_t num_args = json_array_size(args);

		// Build argument chain right-to-left
		int next = -1;
		for (int i = (int)num_args - 1; i >= 0; i--) {
			json_t *arg = json_array_get(args, i);
			if (!json_is_object(arg)) {
				sink.set_error("Argument %d is not an object", i);
				// Free any nodes we've already allocated
				if (next != -1)
					free_sexp2(next);
				if (!op_is_locked)
					free_sexp2(op_node);
				if (warnings) json_decref(warnings);
				return;
			}

			const char *type_str = json_string_value(json_object_get(arg, "type"));
			const char *value = json_string_value(json_object_get(arg, "value"));
			if (!type_str || !value) {
				sink.set_error("Argument %d: 'type' and 'value' are required", i);
				if (next != -1)
					free_sexp2(next);
				if (!op_is_locked)
					free_sexp2(op_node);
				if (warnings) json_decref(warnings);
				return;
			}

			if (!check_string_enum(type_str, sexp_arg_type_values, "type", sink)) {
				if (next != -1)
					free_sexp2(next);
				if (!op_is_locked)
					free_sexp2(op_node);
				if (warnings) json_decref(warnings);
				return;
			}

			// A node value of -1 is a placeholder that bypasses type checking
			bool is_placeholder = (!stricmp(type_str, "node") && !strcmp(value, "-1"))
				|| (!stricmp(type_str, "string") && !strcmp(value, PLACEHOLDER_STRING));

			// Type-check this argument against the operator's expected type
			int expected_opf = query_operator_argument_type(op_idx, i);
			if (is_placeholder) {
				// Placeholder nodes skip type checking entirely
			} else if (expected_opf == OPF_NONE) {
				// Argument index exceeds operator's expected count; warn but allow
				if (!warnings) warnings = json_array();
				SCP_string wmsg;
				sprintf(wmsg, "Argument %d exceeds expected argument count for '%s'; type not checked", i, op_name);
				json_array_append_new(warnings, json_string(wmsg.c_str()));
			} else {
				bool is_variable = (!stricmp(type_str, "number") || !stricmp(type_str, "string")) && value[0] == '@';
				int node_opr = -1;

				if (!stricmp(type_str, "node")) {
					// Determine the referenced operator's return type
					char *endptr;
					long node_idx = strtol(value, &endptr, 10);
					if (*endptr == '\0' && endptr != value && node_idx >= 0 && node_idx < Num_sexp_nodes) {
						int ref_node = (int)node_idx;
						// If this is a list wrapper, the operator is the first child
						if (Sexp_nodes[ref_node].subtype == SEXP_ATOM_LIST && Sexp_nodes[ref_node].first >= 0)
							ref_node = Sexp_nodes[ref_node].first;
						int ref_op = get_operator_index(ref_node);
						if (ref_op >= 0)
							node_opr = query_operator_return_type(ref_op);
					}
				}

				if (!is_arg_type_compatible(expected_opf, type_str, is_variable, node_opr)) {
					sink.set_error("Argument %d: type '%s' is not compatible with expected type '%s' for operator '%s'",
						i, type_str, opf_to_string(expected_opf), op_name);
					if (next != -1)
						free_sexp2(next);
					if (!op_is_locked)
						free_sexp2(op_node);
					if (warnings) json_decref(warnings);
					return;
				}
			}

			int arg_node = create_sexp_arg_node(type_str, value, next, i, sink);
			if (arg_node < 0) {
				// Error already set by create_sexp_arg_node
				if (next != -1)
					free_sexp2(next);
				if (!op_is_locked)
					free_sexp2(op_node);
				if (warnings) json_decref(warnings);
				return;
			}
			next = arg_node;
		}

		// Warn if fewer arguments than the operator expects
		if ((int)num_args < Operators[op_idx].min) {
			if (!warnings) warnings = json_array();
			SCP_string wmsg;
			sprintf(wmsg, "Operator '%s' expects at least %d argument(s), but only " SIZE_T_ARG " provided", op_name, Operators[op_idx].min, num_args);
			json_array_append_new(warnings, json_string(wmsg.c_str()));
		}

		// Link arguments to operator
		if (op_is_locked) {
			// Locked singletons can't have rest modified; wrap in a list first
			op_node = alloc_sexp("", SEXP_LIST, SEXP_ATOM_LIST, op_node, -1);
		}
		Sexp_nodes[op_node].rest = next;

		// Set parent pointers on argument chain
		for (int arg = next; arg != -1; arg = Sexp_nodes[arg].rest)
			Sexp_nodes[arg].parent = op_node;
	} else if (Operators[op_idx].min > 0) {
		// No arguments provided but operator expects some
		warnings = json_array();
		SCP_string wmsg;
		sprintf(wmsg, "Operator '%s' expects at least %d argument(s), but none were provided", op_name, Operators[op_idx].min);
		json_array_append_new(warnings, json_string(wmsg.c_str()));
	}

	mcp_sexp_forest_mark_dirty({ op_node });

	json_t *result_obj = build_sexp_node_json(op_node);
	if (warnings)
		json_object_set_new(result_obj, "warnings", warnings);
	req->result_json = make_json_tool_result(result_obj);
	req->success = true;
}

// ---------------------------------------------------------------------------
// SEXP variable tool handlers (run on main thread)
// ---------------------------------------------------------------------------

static json_t *build_sexp_variable_json(int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(Sexp_variables[index].variable_name));
	json_object_set_new(obj, "default_value", json_string(Sexp_variables[index].text));

	if (Sexp_variables[index].type & SEXP_VARIABLE_NUMBER)
		json_object_set_new(obj, "type", json_string("number"));
	else
		json_object_set_new(obj, "type", json_string("string"));

	json_object_set_new(obj, "flags",
		flags_to_json_array(Sexp_variables[index].type, sexp_var_flag_entries, sexp_var_flag_entries_count));

	return obj;
}

static bool validate_sexp_variable_name(const char *name, McpErrorSink &sink, int exclude_index = -1)
{
	if (!check_string_length(name, TOKEN_LENGTH - 1, "name", sink))
		return false;

	auto rval = strcspn(name, "@()[] ;\"\\/");
	if (rval != strlen(name)) {
		sink.set_error("Invalid character '%c' in variable name", name[rval]);
		return false;
	}

	int existing = get_index_sexp_variable_name(name);
	if (existing >= 0 && existing != exclude_index) {
		sink.set_error("A SEXP variable with name '%s' already exists", name);
		return false;
	}

	return true;
}

static bool validate_sexp_variable_number_value(const char *value, McpErrorSink &sink)
{
	// Validate that value is a valid integer (optional leading minus, then digits)
	const char *p = value;
	if (*p == '-' || *p == '+')
		p++;
	if (*p == '\0') {
		sink.set_error("default_value must be a valid integer for number type variables, got '%s'", value);
		return false;
	}
	while (*p) {
		if (*p < '0' || *p > '9') {
			sink.set_error("default_value must be a valid integer for number type variables, got '%s'", value);
			return false;
		}
		p++;
	}

	// Check numeric range (errno catches overflow on platforms where long == int)
	errno = 0;
	long val = strtol(value, nullptr, 10);
	if (errno == ERANGE || val < INT_MIN || val > INT_MAX) {
		sink.set_error("default_value is out of range for a 32-bit integer, got '%s'", value);
		return false;
	}

	return true;
}

static bool validate_sexp_variable_flags(int flags, McpErrorSink &sink)
{
	if ((flags & SEXP_VARIABLE_SAVE_ON_MISSION_PROGRESS) && (flags & SEXP_VARIABLE_SAVE_ON_MISSION_CLOSE)) {
		sink.set_error("save_on_mission_progress and save_on_mission_close are mutually exclusive");
		return false;
	}
	return true;
}

static void update_sexp_node_variable_references(const char *old_name, const char *new_name)
{
	for (int i = 0; i < Num_sexp_nodes; i++) {
		if ((Sexp_nodes[i].type & SEXP_FLAG_VARIABLE) && !strcmp(Sexp_nodes[i].text, old_name)) {
			strcpy_s(Sexp_nodes[i].text, new_name);
		}
	}
}

static int reset_sexp_node_variable_references(const char *var_name, int var_type)
{
	int count = 0;
	const char *placeholder = (var_type & SEXP_VARIABLE_NUMBER) ? "number" : "string";

	for (int i = 0; i < Num_sexp_nodes; i++) {
		if ((Sexp_nodes[i].type & SEXP_FLAG_VARIABLE) && !strcmp(Sexp_nodes[i].text, var_name)) {
			Sexp_nodes[i].type &= ~SEXP_FLAG_VARIABLE;
			strcpy_s(Sexp_nodes[i].text, placeholder);
			count++;
		}
	}
	return count;
}

static bool is_variable_referenced_in_sexp_nodes(const char *var_name)
{
	for (int i = 0; i < Num_sexp_nodes; i++) {
		if ((Sexp_nodes[i].type & SEXP_FLAG_VARIABLE) && !strcmp(Sexp_nodes[i].text, var_name))
			return true;
	}
	return false;
}

static void handle_list_sexp_variables(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	json_t *arr = json_array();
	for (int i = 0; i < MAX_SEXP_VARIABLES; i++) {
		if (Sexp_variables[i].type & SEXP_VARIABLE_SET)
			json_array_append_new(arr, build_sexp_variable_json(i));
	}

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_sexp_variable(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int idx = get_index_sexp_variable_name(name);
	if (idx < 0) {
		set_not_found_error(sink,"SEXP variable", name);
		return;
	}

	req->result_json = make_json_tool_result(build_sexp_variable_json(idx));
	req->success = true;
}

static void handle_create_sexp_variable(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;
	if (!validate_sexp_variable_name(name, sink)) return;

	auto default_value = get_required_string(input, "default_value", sink, false);
	if (!default_value) return;
	if (!check_string_length(default_value, TOKEN_LENGTH - 1, "default_value", sink)) return;

	auto type_str = get_required_string(input, "type", sink, true);
	if (!type_str) return;
	if (!check_string_enum(type_str, sexp_var_type_values, "type", sink)) return;

	int type_bits;
	if (!stricmp(type_str, "number")) {
		type_bits = SEXP_VARIABLE_NUMBER;
		if (!validate_sexp_variable_number_value(default_value, sink)) return;
	} else {
		type_bits = SEXP_VARIABLE_STRING;
	}

	// Parse optional flags
	int flag_bits = 0;
	auto flags_arr = get_optional_string_array(input, "flags", sink);
	if (!flags_arr.has_value() && json_object_get(input, "flags"))
		return;  // non-string element error already reported by sink
	if (flags_arr.has_value()) {
		if (!parse_flags_array(*flags_arr, sexp_var_flag_entries, sexp_var_flag_entries_count, "flags", flag_bits, req))
			return;
		if (!validate_sexp_variable_flags(flag_bits, sink)) return;
	}

	// Check capacity
	if (sexp_variable_count() >= MAX_SEXP_VARIABLES) {
		sink.set_error("Maximum number of SEXP variables (%d) reached", MAX_SEXP_VARIABLES);
		return;
	}

	int result = sexp_add_variable(default_value, name, type_bits | flag_bits);
	if (result < 0) {
		sink.set_error("Failed to add SEXP variable (no free slot)");
		return;
	}

	sexp_variable_sort();
	mark_modified("MCP: create SEXP variable %s", name);

	// Re-lookup after sort since index may have changed
	int new_idx = get_index_sexp_variable_name(name);
	req->result_json = make_json_tool_result(build_sexp_variable_json(new_idx));
	req->success = true;
}

static void handle_update_sexp_variable(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int idx = get_index_sexp_variable_name(name);
	if (idx < 0) {
		set_not_found_error(sink,"SEXP variable", name);
		return;
	}

	// Save originals
	char old_name[TOKEN_LENGTH];
	strcpy_s(old_name, Sexp_variables[idx].variable_name);
	int old_type = Sexp_variables[idx].type;

	// Extract optional fields
	auto new_name = get_optional_string(input, "new_name", sink, true);
	auto default_value = get_optional_string(input, "default_value", sink, false);
	auto type_str = get_optional_string(input, "type", sink, true);
	if (sink.has_error()) return;

	if (new_name) {
		if (!validate_sexp_variable_name(new_name, sink, idx)) return;
	}
	if (default_value) {
		if (!check_string_length(default_value, TOKEN_LENGTH - 1, "default_value", sink)) return;
	}

	int type_bits;
	if (type_str) {
		if (!check_string_enum(type_str, sexp_var_type_values, "type", sink)) return;
		type_bits = !stricmp(type_str, "number") ? SEXP_VARIABLE_NUMBER : SEXP_VARIABLE_STRING;
	} else {
		type_bits = old_type & (SEXP_VARIABLE_NUMBER | SEXP_VARIABLE_STRING);
	}

	int flag_bits;
	auto flags_arr = get_optional_string_array(input, "flags", sink);
	if (!flags_arr.has_value() && json_object_get(input, "flags"))
		return;  // non-string element error already reported by sink
	if (flags_arr.has_value()) {
		if (!parse_flags_array(*flags_arr, sexp_var_flag_entries, sexp_var_flag_entries_count, "flags", flag_bits, req))
			return;
		if (!validate_sexp_variable_flags(flag_bits, sink)) return;
	} else {
		flag_bits = old_type & (SEXP_VARIABLE_SAVE_ON_MISSION_PROGRESS | SEXP_VARIABLE_SAVE_ON_MISSION_CLOSE |
			SEXP_VARIABLE_SAVE_TO_PLAYER_FILE | SEXP_VARIABLE_NETWORK);
	}

	// Validate number value if applicable
	const char *final_value = default_value ? default_value : Sexp_variables[idx].text;
	if ((type_bits & SEXP_VARIABLE_NUMBER) && default_value) {
		if (!validate_sexp_variable_number_value(default_value, sink)) return;
	}
	// Also validate if type changed to number and we're keeping the old value
	if ((type_bits & SEXP_VARIABLE_NUMBER) && !(old_type & SEXP_VARIABLE_NUMBER) && !default_value) {
		if (!validate_sexp_variable_number_value(Sexp_variables[idx].text, sink)) return;
	}

	const char *final_name = new_name ? new_name : old_name;
	int final_type = type_bits | flag_bits;

	bool changed = false;

	// Update SEXP node references if name changed
	bool renamed = new_name && strcmp(old_name, new_name) != 0;
	if (renamed) {
		update_sexp_node_variable_references(old_name, new_name);
		changed = true;
	}

	// Check if anything actually changed
	if (!changed) {
		if (default_value && strcmp(Sexp_variables[idx].text, default_value) != 0)
			changed = true;
		else if (final_type != (old_type & ~(SEXP_VARIABLE_SET | SEXP_VARIABLE_MODIFIED)))
			changed = true;
	}

	// Apply the modification
	sexp_fred_modify_variable(final_value, final_name, idx, final_type);

	if (renamed)
		sexp_variable_sort();

	if (changed) {
		// Only the rename path modifies Sexp_nodes; value/type/flag changes don't
		if (renamed)
			mcp_sexp_forest_mark_dirty();
		mark_modified("MCP: update SEXP variable %s", final_name);
	}

	// Re-lookup after potential sort
	int new_idx = get_index_sexp_variable_name(final_name);
	req->result_json = make_json_tool_result(build_sexp_variable_json(new_idx));
	req->success = true;
}

static void handle_delete_sexp_variable(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int idx = get_index_sexp_variable_name(name);
	if (idx < 0) {
		set_not_found_error(sink,"SEXP variable", name);
		return;
	}

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	// Check for references unless force is set
	if (!force.has_value() || !*force) {
		if (is_variable_referenced_in_sexp_nodes(name)) {
			sink.set_error("SEXP variable '%s' is referenced in SEXP expressions. "
				"Use force=true to delete anyway (references will be reset to placeholder values).", name);
			return;
		}
	}

	int var_type = Sexp_variables[idx].type;

	// Reset any SEXP node references
	reset_sexp_node_variable_references(name, var_type);

	sexp_variable_delete(idx);
	sexp_variable_sort();
	mcp_sexp_forest_mark_dirty();
	mark_modified("MCP: delete SEXP variable %s", name);

	sprintf(req->result_message, "Deleted SEXP variable: %s", name);
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
	"walk_sexp_tree",
	"text_to_sexp",
	"free_sexp",
	"create_sexp_node",
	"list_sexp_variables",
	"get_sexp_variable",
	"create_sexp_variable",
	"update_sexp_variable",
	"delete_sexp_variable",
	nullptr
};

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_mission_tools(json_t *tools)
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

	// list_events
	register_tool(tools, "list_events",
		"List all mission events. Returns each event's name, index, SEXP formula root node, "
		"and whether it is chained.",
		json_object());

	// get_event
	register_tool_with_required_string(tools, "get_event",
		"Get full details of a mission event by name, including repeat/trigger counts, "
		"interval, score, chain delay, team, objective text, flags, and log flags.",
		"name", "Name of the event to retrieve");

	// create_event
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Unique name for the event");
		add_integer_prop(props, "formula", "Root node of the SEXP formula used for this event");
		add_bool_prop(props, "is_chained", "Whether this event is chained to the one preceding it "
			"(if this is provided and chain_delay is not, true sets the delay to 0 and false clears it)");
		add_integer_prop(props, "repeat_count",
			"Number of times to test this event (1 = once, -1 = infinite)");
		add_integer_prop(props, "trigger_count",
			"Number of times to allow this event to trigger (auto-sets using_trigger_count flag)");
		add_integer_prop(props, "interval",
			"Evaluation interval in seconds (or milliseconds if use_msecs flag is set)");
		add_string_enum_prop(props, "chain_and_interval_units",
			"Units for 'interval' and 'chain_delay' (defaults to seconds)",
			event_unit_enum_values);
		add_integer_prop(props, "score", "Score awarded when event is satisfied");
		add_integer_prop(props, "chain_delay",
			"Delay before evaluating this chained event (-1 = not chained)");
		add_string_enum_prop(props, "team",
			"Multiplayer team assignment (\"none\" for all teams)",
			team_enum_values);
		add_string_prop(props, "objective_text", "Directive text displayed in the HUD");
		add_string_prop(props, "objective_key_text", "Localization key for the objective text");
		add_integer_prop(props, "index",
			"Position to insert the event (1 = first). If omitted, appends to the end.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "create_event",
			"Create a new mission event. Mission events cause actions to occur during a mission, "
			"subject to their conditions.",
			props, req);
	}

	// update_event
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the existing event to update");
		add_string_prop(props, "new_name", "New name for the event");
		add_integer_prop(props, "formula", "Root node of the SEXP formula used for this event");
		add_bool_prop(props, "is_chained", "Whether this event is chained to the one preceding it "
			"(if this is provided and chain_delay is not, true sets the delay to 0 and false clears it)");
		add_integer_prop(props, "repeat_count",
			"Number of times to test this event (1 = once, -1 = infinite)");
		add_integer_prop(props, "trigger_count",
			"Number of times to allow this event to trigger (auto-sets using_trigger_count flag)");
		add_integer_prop(props, "interval",
			"Evaluation interval in seconds (or milliseconds if use_msecs flag is set)");
		add_string_enum_prop(props, "chain_and_interval_units",
			"Units for 'interval' and 'chain_delay' (defaults to seconds)",
			event_unit_enum_values);
		add_integer_prop(props, "score", "Score awarded when event is satisfied");
		add_integer_prop(props, "chain_delay",
			"Delay before evaluating this chained event (-1 = not chained)");
		add_string_enum_prop(props, "team",
			"Multiplayer team assignment (\"none\" for all teams)",
			team_enum_values);
		add_string_prop(props, "objective_text",
			"Directive text displayed in the HUD (empty string to clear)");
		add_string_prop(props, "objective_key_text",
			"Localization key for the objective text (empty string to clear)");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_event",
			"Update properties of an existing mission event. Only specified fields are "
			"changed; omitted fields are left unchanged. Renaming automatically updates "
			"all SEXP references to the event.",
			props, req);
	}

	// delete_event
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the event to delete");
		add_bool_prop(props, "force",
			"If true, delete even if the event is referenced in SEXPs (references "
			"will be invalidated). Default: false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "delete_event",
			"Delete a mission event. Frees its SEXP formula and updates annotation paths. "
			"SEXP references to the deleted event are invalidated (wrapped in angle brackets).",
			props, req);
	}

	// move_event
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"Current 1-based index of the event");
		add_integer_prop(props, "to_index",
			"Target 1-based index to move the event to");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_event",
			"Move a mission event from one position to another. "
			"Indices are 1-based. Updates event annotation paths.",
			props, req);
	}

	// swap_events
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first event");
		add_integer_prop(props, "index_b",
			"1-based index of the second event");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_events",
			"Swap two mission events at the given positions. "
			"Indices are 1-based. Updates event annotation paths.",
			props, req);
	}

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

	// -----------------------------------------------------------------------
	// Goal tools
	// -----------------------------------------------------------------------

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

	// -----------------------------------------------------------------------
	// SEXP tools
	// -----------------------------------------------------------------------

	// sexp_to_text
	{
		json_t *props = json_object();
		add_integer_prop(props, "node", "Root node index of the SEXP tree to serialize");
		json_t *req = json_array();
		json_array_append_new(req, json_string("node"));
		register_tool(tools, "sexp_to_text",
			"Convert a SEXP node tree to its text representation. "
			"Takes a root node index and returns the S-expression as a formatted string, "
			"suitable for copy/paste or inspection. Read-only; does not modify any nodes.",
			props, req);
	}

	// get_sexp_node
	{
		json_t *props = json_object();
		add_integer_prop(props, "node", "Index of the SEXP node to inspect");
		json_t *req = json_array();
		json_array_append_new(req, json_string("node"));
		register_tool(tools, "get_sexp_node",
			"Get details about a single SEXP node: its kind, value, value type, "
			"child/sibling indices (node_first/node_rest), and operator return type. "
			"Use 'node_first' to descend into a child list (CAR) and 'node_rest' to move "
			"to the next sibling (CDR).",
			props, req);
	}

	// walk_sexp_tree
	{
		json_t *props = json_object();
		add_integer_prop(props, "node", "Root node index to start traversal from");
		add_integer_prop(props, "depth", "Maximum recursion depth (default: unlimited)");
		json_t *req = json_array();
		json_array_append_new(req, json_string("node"));
		register_tool(tools, "walk_sexp_tree",
			"Walk the SEXP subtree rooted at the given node. Returns a flat array "
			"of node descriptors with kind, value, value type, child/sibling indices, "
			"and walk_first/walk_rest indices into the array for easy traversal.",
			props, req);
	}

	// text_to_sexp
	{
		json_t *props = json_object();
		add_string_prop(props, "text",
			"SEXP text to parse, e.g. \"( when ( true ) ( do-nothing ) )\"");
		json_t *req = json_array();
		json_array_append_new(req, json_string("text"));
		register_tool(tools, "text_to_sexp",
			"Parse SEXP text into a node tree. Returns the root node index of the "
			"newly allocated tree. The caller is responsible for attaching the tree "
			"to an event or goal formula, or freeing it. Also returns the round-tripped "
			"text, any parsing errors encountered, and the first (if any) syntax error.",
			props, req);
	}

	// free_sexp
	{
		json_t *props = json_object();
		add_integer_prop(props, "node", "Root node index of the SEXP tree to free");
		json_t *req = json_array();
		json_array_append_new(req, json_string("node"));
		register_tool(tools, "free_sexp",
			"Free a SEXP node tree. Recursively frees the entire tree rooted at the "
			"given node. Refuses to free locked singleton nodes or nodes attached to "
			"mission entities (events, goals, briefings, arrival/departure cues, etc.).",
			props, req);
	}

	// create_sexp_node
	{
		json_t *props = json_object();
		add_string_prop(props, "operator",
			"Name of the SEXP operator (e.g. \"when\", \"is-destroyed-delay\")");

		json_t *arg_props = json_object();
		add_string_enum_prop(arg_props, "type",
			"Argument type",
			sexp_arg_type_values);
		add_string_prop(arg_props, "value",
			"Argument value. For number/string: literal value (prefix with @ "
			"for SEXP variable). For boolean: \"true\" or \"false\". "
			"For node: a node index, or \"-1\" as a placeholder. "
			"A node value of -1 or a string value of " PLACEHOLDER_STRING
			" bypasses type checking, serves as a placeholder, and can be "
			"used in any argument position as an empty slot to fill later.");
		json_t *arg_req = json_array();
		json_array_append_new(arg_req, json_string("type"));
		json_array_append_new(arg_req, json_string("value"));
		add_object_array_prop(props, "operator_arguments",
			"List of arguments for the operator. Each argument has a 'type' "
			"(number, string, boolean, node) and a 'value'.",
			arg_props, arg_req);

		json_t *req = json_array();
		json_array_append_new(req, json_string("operator"));
		register_tool(tools, "create_sexp_node",
			"Create a SEXP expression node with an operator and optional arguments. "
			"Returns the root operator node, suitable for assigning as an event or "
			"goal formula. Does not enforce argument count or check syntax.",
			props, req);
	}

	// list_sexp_variables
	register_tool(tools, "list_sexp_variables",
		"List all SEXP variables defined in this mission. "
		"Returns each variable's name, default value, type (number or string), and flags.",
		nullptr);

	// get_sexp_variable
	register_tool_with_required_string(tools, "get_sexp_variable",
		"Get full details of a SEXP variable by name, including default value, "
		"type, and persistence/network flags.",
		"name", "Name of the variable to retrieve");

	// create_sexp_variable
	{
		auto flag_names = flags_to_list(sexp_var_flag_entries, sexp_var_flag_entries_count);
		json_t *props = json_object();
		add_string_prop(props, "name",
			"Unique variable name. Cannot contain spaces or the characters @, (, ).");
		add_string_prop(props, "default_value",
			"Default value for the variable. Must be a valid integer for number type.");
		add_string_enum_prop(props, "type",
			"Data type of the variable",
			sexp_var_type_values);
		add_string_array_prop(props, "flags",
			"Persistence and network flags. save_on_mission_progress and save_on_mission_close "
			"are mutually exclusive.",
			flag_names);
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("default_value"));
		json_array_append_new(req, json_string("type"));
		register_tool(tools, "create_sexp_variable",
			"Create a new SEXP variable. Variables are automatically kept in sorted alphabetical order.",
			props, req);
	}

	// update_sexp_variable
	{
		auto flag_names = flags_to_list(sexp_var_flag_entries, sexp_var_flag_entries_count);
		json_t *props = json_object();
		add_string_prop(props, "name",
			"Name of the existing variable to update");
		add_string_prop(props, "new_name",
			"New name for the variable. All SEXP node references will be updated automatically.");
		add_string_prop(props, "default_value",
			"New default value. Must be a valid integer for number type.");
		add_string_enum_prop(props, "type",
			"New data type. Changing type may invalidate existing SEXP references.",
			sexp_var_type_values);
		add_string_array_prop(props, "flags",
			"New persistence and network flags (replaces all existing flags). "
			"save_on_mission_progress and save_on_mission_close are mutually exclusive.",
			flag_names);
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_sexp_variable",
			"Update a SEXP variable's properties. Only provided fields are changed. "
			"If renaming, all SEXP node references are updated automatically.",
			props, req);
	}

	// delete_sexp_variable
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the variable to delete");
		add_bool_prop(props, "force",
			"If true, delete even if referenced in SEXP expressions "
			"(references will be reset to placeholder values)");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "delete_sexp_variable",
			"Delete a SEXP variable. By default, refuses to delete if the variable "
			"is referenced in any SEXP expressions.",
			props, req);
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
	} else if (strcmp(tool_name, "list_events") == 0) {
		handle_list_events(input_json, req);
	} else if (strcmp(tool_name, "get_event") == 0) {
		handle_get_event(input_json, req);
	} else if (strcmp(tool_name, "create_event") == 0) {
		handle_create_event(input_json, req);
	} else if (strcmp(tool_name, "update_event") == 0) {
		handle_update_event(input_json, req);
	} else if (strcmp(tool_name, "delete_event") == 0) {
		handle_delete_event(input_json, req);
	} else if (strcmp(tool_name, "move_event") == 0) {
		handle_move_event(input_json, req);
	} else if (strcmp(tool_name, "swap_events") == 0) {
		handle_swap_events(input_json, req);
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
	} else if (strcmp(tool_name, "list_goals") == 0) {
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
	} else if (strcmp(tool_name, "sexp_to_text") == 0) {
		handle_sexp_to_text(input_json, req);
	} else if (strcmp(tool_name, "get_sexp_node") == 0) {
		handle_get_sexp_node(input_json, req);
	} else if (strcmp(tool_name, "walk_sexp_tree") == 0) {
		handle_walk_sexp_tree(input_json, req);
	} else if (strcmp(tool_name, "text_to_sexp") == 0) {
		handle_text_to_sexp(input_json, req);
	} else if (strcmp(tool_name, "free_sexp") == 0) {
		handle_free_sexp(input_json, req);
	} else if (strcmp(tool_name, "create_sexp_node") == 0) {
		handle_create_sexp_node(input_json, req);
	} else if (strcmp(tool_name, "list_sexp_variables") == 0) {
		handle_list_sexp_variables(input_json, req);
	} else if (strcmp(tool_name, "get_sexp_variable") == 0) {
		handle_get_sexp_variable(input_json, req);
	} else if (strcmp(tool_name, "create_sexp_variable") == 0) {
		handle_create_sexp_variable(input_json, req);
	} else if (strcmp(tool_name, "update_sexp_variable") == 0) {
		handle_update_sexp_variable(input_json, req);
	} else if (strcmp(tool_name, "delete_sexp_variable") == 0) {
		handle_delete_sexp_variable(input_json, req);
	} else {
		McpErrorSink sink(req);
		sink.set_error("Unknown mission tool: %s", tool_name);
	}
}
