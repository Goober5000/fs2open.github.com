#include "stdafx.h"
#include "mcp_mission_tools.h"
#include "mcpserver.h"
#include "mcp_json.h"
#include "mcp_array_utils.h"
#include "mcp_sexp_forest.h"
#include "mcp_reference_tools.h"

#include <jansson.h>
#include <algorithm>
#include <cstdarg>
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
#include "freddoc.h"
#include "fred.h"
#include "messageeditordlg.h"
#include "eventeditor.h"
#include "mainfrm.h"

// ---------------------------------------------------------------------------
// SEXP formula validation
// ---------------------------------------------------------------------------

static bool check_sexp_formula(int node, sexp_opr_t expected_return_type, McpToolRequest *req)
{
	// Range check
	if (!check_int_range(node, 0, Num_sexp_nodes - 1, "formula", req))
		return false;

	// Check node is in use
	if (Sexp_nodes[node].type == SEXP_NOT_USED) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"SEXP node %d is not in use", node);
		return false;
	}

	// Check operator return type
	int op_const = get_operator_const(node);
	if (op_const == OP_NOT_AN_OP) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"SEXP node %d (\"%s\") is not a valid SEXP operator",
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
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Formula node %d (\"%s\") has return type %s, but this entity requires %s",
			node, Sexp_nodes[node].text, actual_str, expected_str);
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
				sprintf(error_msg, "Cannot modify %s while the %s is open. "
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

static bool reject_team_none(const char *team_str, const char *entity_name, McpToolRequest *req)
{
	if (!stricmp(team_str, "none")) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"A team of \"none\" is not valid for a %s. Use \"Team 1\" or \"Team 2\".", entity_name);
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
	auto lookup_fn = [&](const char *str)->int
	{
		for (size_t i = 0; i < count; i++)
			if (!stricmp(entries[i].name, str))
				return sz2i(i);
		return -1;
	};

	out_flags = 0;
	for (const auto &s : strings) {
		int i = check_lookup(s.c_str(), lookup_fn, param_name, req);
		if (i < 0)
			return false;
		out_flags |= entries[i].bit;
	}
	return true;
}

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
		json_object_set_new(obj, "index", json_integer(msg_absolute_index - Num_builtin_messages));
	json_object_set_new(obj, "message", json_string(msg.message));

	const char *persona = persona_name_from_index(msg.persona_index);
	if (persona)
		json_object_set_new(obj, "persona", json_string(persona));

	if (include_details) {
		json_object_set_new(obj, "team", json_string(team_name_from_index(msg.multi_team)));
		set_optional_filename(obj, "talking_head", msg.avi_info.name);
		set_optional_filename(obj, "voice_file", msg.wave_info.name);
	}

	return obj;
}

static void handle_list_messages(json_t *input, McpToolRequest *req)
{
	// Determine range based on "source" parameter
	const char *source = get_optional_string(input, "source", true);

	if (source && !check_string_enum(source, message_enum_values, "source", req))
		return;

	int start, end;
	if (source && !stricmp(source, "builtin")) {
		start = 0;
		end = Num_builtin_messages;
	} else {
		// Default: mission messages — check for conflicting dialogs
		if (!validate(validate_dialog_for_messages, req)) return;
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
	const char *name = get_required_string(input, "name", req, false);
	if (!name) return;

	// Find the message
	int idx = find_item_with_string(Messages, &MMessage::name, name);
	if (idx < 0) {
		set_not_found_error(req, "Message", name);
		return;
	}

	// Check for conflicting dialogs if this is a mission-specific message
	if (idx >= Num_builtin_messages) {
		if (!validate(validate_dialog_for_messages, req)) return;
	}

	req->result_json = make_json_tool_result(build_message_json(Messages[idx], idx, true));
	req->success = true;
}

static void handle_create_message(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_messages, req)) return;

	auto name    = get_required_string(input, "name", req, true);
	if (!name || !check_string_length(name, NAME_LENGTH - 1, "name", req)) return;

	auto message = get_required_string(input, "message", req, false);
	if (!message || !check_string_length(message, MESSAGE_LENGTH - 1, "message", req)) return;

	auto persona_str  = get_optional_string(input, "persona", false);
	auto talking_head = get_optional_filename(input, "talking_head", false);
	auto voice_file   = get_optional_filename(input, "voice_file", false);
	auto team_str     = get_optional_string(input, "team", true);
	auto insert_index = get_optional_integer(input, "index");

	// Check for duplicate name
	if (find_item_with_string(Messages, &MMessage::name, name) >= 0) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"A message with name '%s' already exists", name);
		return;
	}

	// Look up persona by name
	int persona_index = -1;
	if (persona_str && persona_str[0]) {
		persona_index = check_lookup(persona_str, message_persona_name_lookup, "persona", req);
		if (persona_index < 0) return;
	}

	// Validate team
	int multi_team = -1;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", req))
			return;
		multi_team = team_index_from_name(team_str);
	}

	// Validate and resolve insert index
	int target_index;
	if (!insert_index.has_value()) {
		// Default: append to end
		target_index = Num_messages;
	} else {
		// Caller specifies a mission-relative index (0 = first mission message)
		// Note: the upper bound is allowed, and means append
		if (!check_int_range(*insert_index, 0, Num_messages - Num_builtin_messages, "index", req))
			return;
		target_index = Num_builtin_messages + *insert_index;
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

	// Return the created message
	json_t *data = json_object();
	json_object_set_new(data, "name", json_string(name));
	json_object_set_new(data, "index", json_integer(target_index - Num_builtin_messages));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_update_message(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_messages, req)) return;

	auto name = get_required_string(input, "name", req, true);
	if (!name) return;

	// Find the message (mission-specific only)
	int idx = find_item_with_string(Messages, &MMessage::name, name, Num_builtin_messages);
	if (idx < 0) {
		set_not_found_error(req, "Message", name);
		return;
	}

	auto new_msg = get_optional_string(input, "message", false);
	if (new_msg && !check_string_length(new_msg, MESSAGE_LENGTH - 1, "message", req)) return;

	auto persona_str = get_optional_string(input, "persona", false);
	std::optional<int> persona_index = std::nullopt;
	if (persona_str) {
		if (persona_str[0]) {
			int persona_idx = check_lookup(persona_str, message_persona_name_lookup, "persona", req);
			if (persona_idx < 0) return;
			persona_index = persona_idx;
		} else {
			persona_index = -1;
		}
	}

	auto new_head = get_optional_filename(input, "talking_head", false);
	auto new_voice = get_optional_filename(input, "voice_file", false);

	auto new_team_str = get_optional_string(input, "team", true);
	std::optional<int> new_team = std::nullopt;
	if (new_team_str) {
		if (!check_string_enum(new_team_str, team_enum_values, "team", req))
			return;
		new_team = team_index_from_name(new_team_str);
	}

	auto new_name = get_optional_string(input, "new_name", false);
	if (new_name) {
		if (!check_string_length(new_name, NAME_LENGTH - 1, "new_name", req)) return;
		if (!new_name[0]) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"A message name cannot be blank!");
			return;
		}

		// Check for duplicate if the name is actually different
		if ((stricmp(Messages[idx].name, new_name) != 0) && (find_item_with_string(Messages, &MMessage::name, new_name) >= 0)) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"A message with name '%s' already exists", new_name);
			return;
		}
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
		if (Messages[idx].avi_info.name)
			free(Messages[idx].avi_info.name);
		Messages[idx].avi_info.name = new_head[0] ? strdup(new_head) : nullptr;
		changed = true;
	}

	// Update voice file
	if (new_voice) {
		if (Messages[idx].wave_info.name)
			free(Messages[idx].wave_info.name);
		Messages[idx].wave_info.name = new_voice[0] ? strdup(new_voice) : nullptr;
		changed = true;
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
	if (!validate(validate_dialog_for_messages, req)) return;

	auto force = get_optional_bool(input, "force");

	auto name = get_required_string(input, "name", req, true);
	if (!name) return;

	// Find the message (mission-specific only)
	int idx = find_item_with_string(Messages, &MMessage::name, name, Num_builtin_messages);
	if (idx < 0) {
		set_not_found_error(req, "Message", name);
		return;
	}

	// Check for SEXP references unless force is set
	if (!force.has_value() || !*force) {
		int node;
		auto ref = query_referenced_in_sexp(sexp_ref_type::NON_OBJECT, Messages[idx].name, node);
		if (ref.second != sexp_src::NONE) {
			SCP_string desc = sexp_src_to_description(ref.first, ref.second);
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"Message '%s' is referenced in %s. Use force=true to delete anyway "
				"(references will be invalidated).", name, desc.c_str());
			return;
		}
	}

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

	snprintf(req->result_message, sizeof(req->result_message), "Deleted message: %s", name);
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
	json_object_set_new(obj, "index", json_integer(evt_index));
	json_object_set_new(obj, "formula", json_integer(evt.formula));

	bool is_chained = (evt.chain_delay >= 0);
	json_object_set_new(obj, "is_chained", json_boolean(is_chained));

	if (!include_details)
		return obj;

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

	return obj;
}

static void handle_list_events(json_t * /*input*/, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_events, req)) return;

	json_t *arr = json_array();
	for (int i = 0; i < (int)Mission_events.size(); i++)
		json_array_append_new(arr, build_event_json(Mission_events[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_event(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_events, req)) return;

	auto name = get_required_string(input, "name", req, false);
	if (!name) return;

	int idx = find_item_with_string(Mission_events, &mission_event::name, name);
	if (idx < 0) {
		set_not_found_error(req, "Event", name);
		return;
	}

	req->result_json = make_json_tool_result(build_event_json(Mission_events[idx], idx, true));
	req->success = true;
}

static void handle_create_event(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_events, req)) return;

	auto name = get_required_string(input, "name", req, true);
	if (!name || !check_string_length(name, NAME_LENGTH - 1, "name", req)) return;

	// Check for duplicate name
	if (find_item_with_string(Mission_events, &mission_event::name, name) >= 0) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"An event with name '%s' already exists", name);
		return;
	}

	auto insert_index = get_optional_integer(input, "index");

	// Optional parameters
	auto formula       = get_optional_integer(input, "formula");
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_NULL, req)) return;
	auto is_chained    = get_optional_bool(input, "is_chained");
	auto repeat_count  = get_optional_integer(input, "repeat_count");
	auto trigger_count = get_optional_integer(input, "trigger_count");
	auto interval      = get_optional_integer(input, "interval");

	auto units = get_optional_string(input, "chain_and_interval_units", true);
	if (units && !check_string_enum(units, event_unit_enum_values, "chain_and_interval_units", req))
		return;

	auto score         = get_optional_integer(input, "score");
	auto chain_delay   = get_optional_integer(input, "chain_delay");

	// if an event is chained but a chain delay is not specified, set the delay to 0;
	// similarly, if an event is explicitly unchained, set the delay to -1
	// (note: chain delay overrides is_chained)
	if (!chain_delay.has_value() && is_chained.has_value()) {
		chain_delay = *is_chained ? 0 : -1;
	}

	// Team
	auto team_str = get_optional_string(input, "team", true);
	int multi_team = -1;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", req))
			return;
		multi_team = team_index_from_name(team_str);
	}

	auto objective_text = get_optional_string(input, "objective_text", false);
	auto objective_key_text = get_optional_string(input, "objective_key_text", false);

	// Auto-set MEF_ flags if parameters provided
	std::optional<int> mef_flags;
	if (trigger_count.has_value()) {
		if (mef_flags.has_value()) {
			*mef_flags |= MEF_USING_TRIGGER_COUNT;
		} else {
			mef_flags = MEF_USING_TRIGGER_COUNT;
		}
	}
	if (units && !stricmp(units, "milliseconds")) {
		if (mef_flags.has_value()) {
			*mef_flags |= MEF_USE_MSECS;
		} else {
			mef_flags = MEF_USE_MSECS;
		}
	}

	// Validate insert index
	int target_index;
	if (!insert_index.has_value()) {
		target_index = (int)Mission_events.size();
	} else {
		if (!check_int_range(*insert_index, 0, (int)Mission_events.size(), "index", req))
			return;
		target_index = *insert_index;
	}

	if (!formula.has_value()) {
		// Build default SEXP formula: (when (true) (do-nothing))
		int do_nothing = alloc_sexp("do-nothing", SEXP_LIST, SEXP_ATOM_OPERATOR, -1, -1);
		int true_node = alloc_sexp("true", SEXP_LIST, SEXP_ATOM_OPERATOR, -1, do_nothing);
		formula = alloc_sexp("when", SEXP_LIST, SEXP_ATOM_OPERATOR, true_node, -1);
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
	evt.flags         = mef_flags.value_or(0);
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

	// Return the created event
	json_t *data = json_object();
	json_object_set_new(data, "name", json_string(name));
	json_object_set_new(data, "index", json_integer(target_index));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_update_event(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_events, req)) return;

	auto name = get_required_string(input, "name", req, true);
	if (!name) return;

	int idx = find_item_with_string(Mission_events, &mission_event::name, name);
	if (idx < 0) {
		set_not_found_error(req, "Event", name);
		return;
	}

	mission_event &evt = Mission_events[idx];

	// Validate new_name
	auto new_name      = get_optional_string(input, "new_name", false);
	if (new_name) {
		if (!check_string_length(new_name, NAME_LENGTH - 1, "new_name", req)) return;
		if (!new_name[0]) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"An event name cannot be blank!");
			return;
		}
		if (stricmp(evt.name.c_str(), new_name) != 0 &&
			find_item_with_string(Mission_events, &mission_event::name, new_name) >= 0) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"An event with name '%s' already exists", new_name);
			return;
		}
	}

	// Extract optional fields
	auto formula       = get_optional_integer(input, "formula");
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_NULL, req)) return;
	auto is_chained    = get_optional_bool(input, "is_chained");
	auto repeat_count  = get_optional_integer(input, "repeat_count");
	auto trigger_count = get_optional_integer(input, "trigger_count");
	auto interval      = get_optional_integer(input, "interval");

	auto units = get_optional_string(input, "chain_and_interval_units", true);
	if (units && !check_string_enum(units, event_unit_enum_values, "chain_and_interval_units", req))
		return;

	auto score         = get_optional_integer(input, "score");
	auto chain_delay   = get_optional_integer(input, "chain_delay");

	if (!chain_delay.has_value() && is_chained.has_value()) {
		chain_delay = *is_chained ? 0 : -1;
	}

	// Team
	auto team_str = get_optional_string(input, "team", true);
	std::optional<int> new_team = std::nullopt;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", req))
			return;
		new_team = team_index_from_name(team_str);
	}

	auto objective_text     = get_optional_string(input, "objective_text", false);
	auto objective_key_text = get_optional_string(input, "objective_key_text", false);

	// Auto-set MEF_ flags if parameters provided
	std::optional<int> new_mef_flags;
	if (trigger_count.has_value()) {
		if (!new_mef_flags.has_value()) {
			new_mef_flags = evt.flags;
		}
		*new_mef_flags |= MEF_USING_TRIGGER_COUNT;
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
	if (!validate(validate_dialog_for_events, req)) return;

	auto force = get_optional_bool(input, "force");

	auto name = get_required_string(input, "name", req, true);
	if (!name) return;

	int idx = find_item_with_string(Mission_events, &mission_event::name, name);
	if (idx < 0) {
		set_not_found_error(req, "Event", name);
		return;
	}

	// Check for SEXP references unless force is set
	if (!force.has_value() || !*force) {
		int node;
		auto ref = query_referenced_in_sexp(sexp_ref_type::NON_OBJECT, Mission_events[idx].name.c_str(), node);
		if (ref.second != sexp_src::NONE) {
			SCP_string desc = sexp_src_to_description(ref.first, ref.second);
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"Event '%s' is referenced in %s. Use force=true to delete anyway "
				"(references will be invalidated).", name, desc.c_str());
			return;
		}
	}

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

	snprintf(req->result_message, sizeof(req->result_message), "Deleted event: %s", name);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Generic move/swap handlers
// ---------------------------------------------------------------------------

// since this may return a dangling pointer, be sure not to store it in a variable!
#define CFG_GET_NAME(cfg, index) (cfg).get_name ? (cfg).get_name(index) : (cfg).get_name_fallback(index).c_str()

// Configuration for entity-specific move/swap behavior.
// Lambdas encapsulate offsets, annotation updates, and array access.
struct MoveSwapConfig
{
	const char *entity_name;
	int count;
	std::function<bool(SCP_string&)> validate_dialog;
	std::function<const char *(int)> get_name;
	std::function<void(int, int)> do_move;
	std::function<void(int, int)> do_swap;

	SCP_string get_name_fallback(int index) const
	{
		SCP_string name;
		sprintf(name, "%s %d", entity_name, index);
		return name;
	}
};

static void handle_generic_move(json_t *input, McpToolRequest *req, const MoveSwapConfig &cfg)
{
	if (!validate(cfg.validate_dialog, req)) return;

	auto from_index = get_required_integer(input, "from_index", req);
	if (!from_index.has_value() || !check_int_range(*from_index, 0, cfg.count - 1, "from_index", req)) return;
	auto to_index = get_required_integer(input, "to_index", req);
	if (!to_index.has_value() || !check_int_range(*to_index, 0, cfg.count - 1, "to_index", req)) return;

	if (from_index == to_index) {
		json_t *data = json_object();
		json_object_set_new(data, "name", json_string(CFG_GET_NAME(cfg, *from_index)));
		json_object_set_new(data, "index", json_integer(*from_index));
		req->result_json = make_json_tool_result(data);
		req->success = true;
		return;
	}

	cfg.do_move(*from_index, *to_index);

	mark_modified("MCP: move %s %s from %d to %d", cfg.entity_name, CFG_GET_NAME(cfg, *to_index), *from_index, *to_index);

	json_t *data = json_object();
	json_object_set_new(data, "name", json_string(CFG_GET_NAME(cfg, *to_index)));
	json_object_set_new(data, "index", json_integer(*to_index));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_generic_swap(json_t *input, McpToolRequest *req, const MoveSwapConfig &cfg)
{
	if (!validate(cfg.validate_dialog, req)) return;

	auto index_a = get_required_integer(input, "index_a", req);
	if (!index_a.has_value() || !check_int_range(*index_a, 0, cfg.count - 1, "index_a", req)) return;
	auto index_b = get_required_integer(input, "index_b", req);
	if (!index_b.has_value() || !check_int_range(*index_b, 0, cfg.count - 1, "index_b", req)) return;

	if (index_a != index_b) {
		cfg.do_swap(*index_a, *index_b);

		mark_modified("MCP: swap %ss %s and %s", cfg.entity_name, CFG_GET_NAME(cfg, *index_b), CFG_GET_NAME(cfg, *index_a));
	}

	json_t *data = json_object();
	json_t *a_obj = json_object();
	json_object_set_new(a_obj, "name", json_string(CFG_GET_NAME(cfg, *index_a)));
	json_object_set_new(a_obj, "index", json_integer(*index_a));
	json_t *b_obj = json_object();
	json_object_set_new(b_obj, "name", json_string(CFG_GET_NAME(cfg, *index_b)));
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
	cfg.validate_dialog = validate_dialog_for_messages;
	cfg.get_name = [](int i) -> const char * {
		return Messages[Num_builtin_messages + i].name;
	};
	cfg.do_move = [](int from, int to) {
		array_move_element(Messages, Num_builtin_messages + from, Num_builtin_messages + to);
	};
	cfg.do_swap = [](int a, int b) {
		std::swap(Messages[Num_builtin_messages + a], Messages[Num_builtin_messages + b]);
	};
	return cfg;
}

static MoveSwapConfig make_event_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "event";
	cfg.count = (int)Mission_events.size();
	cfg.validate_dialog = validate_dialog_for_events;
	cfg.get_name = [](int i) -> const char * {
		return Mission_events[i].name.c_str();
	};
	cfg.do_move = [](int from, int to) {
		update_annotation_paths_for_move(from, to);
		array_move_element(Mission_events, from, to);
	};
	cfg.do_swap = [](int a, int b) {
		update_annotation_paths_for_swap(a, b);
		std::swap(Mission_events[a], Mission_events[b]);
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
	auto team_str = get_optional_string(input, "team", true);
	int team_index = 0;  // default to Team 1

	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", req))
			return nullptr;
		if (reject_team_none(team_str, "command briefing", req)) return nullptr;
		team_index = team_index_from_name(team_str);
	}

	return &Cmd_briefs[team_index];
}

static json_t *build_cmd_brief_stage_json(const cmd_brief_stage &stage, int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "index", json_integer(index));
	json_object_set_new(obj, "text", json_string(stage.text.c_str()));
	if (stage.ani_filename && stricmp(stage.ani_filename, "<default>") != 0)
		set_optional_filename(obj, "ani_filename", stage.ani_filename);
	set_optional_filename(obj, "wave_filename", stage.wave_filename);
	return obj;
}

static void handle_list_cmd_brief_stages(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_cmd_brief, req)) return;

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
	if (!validate(validate_dialog_for_cmd_brief, req)) return;

	auto *cb = get_cmd_brief_for_team(input, req);
	if (!cb) return;

	auto index = get_required_integer(input, "index", req);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 0, cb->num_stages - 1, "index", req)) return;

	req->result_json = make_json_tool_result(build_cmd_brief_stage_json(cb->stage[*index], *index));
	req->success = true;
}

static void handle_create_cmd_brief_stage(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_cmd_brief, req)) return;

	auto *cb = get_cmd_brief_for_team(input, req);
	if (!cb) return;

	if (cb->num_stages >= CMD_BRIEF_STAGES_MAX) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Cannot add more than %d command briefing stages", CMD_BRIEF_STAGES_MAX);
		return;
	}

	auto text = get_required_string(input, "text", req, false);
	if (!text) return;

	auto ani = get_optional_filename(input, "ani_filename", false);
	auto wave = get_optional_filename(input, "wave_filename", false);
	auto insert_index = get_optional_integer(input, "index");

	// Validate filename lengths
	if (ani && !check_string_length(ani, MAX_FILENAME_LEN - 1, "ani_filename", req)) return;
	if (wave && !check_string_length(wave, MAX_FILENAME_LEN - 1, "wave_filename", req)) return;

	// Resolve insert position
	int target;
	if (!insert_index.has_value()) {
		target = cb->num_stages;
	} else {
		// Upper bound is num_stages (append position)
		if (!check_int_range(*insert_index, 0, cb->num_stages, "index", req))
			return;
		target = *insert_index;
	}

	// Insert slot
	if (!array_insert_slot(cb->stage, cb->num_stages, CMD_BRIEF_STAGES_MAX, target)) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Cannot add more than %d command briefing stages", CMD_BRIEF_STAGES_MAX);
		return;
	}

	// Fill new stage
	cmd_brief_stage &s = cb->stage[target];
	s.text = text;
	strcpy_s(s.ani_filename, (ani && ani[0]) ? ani : "<default>");
	strcpy_s(s.wave_filename, (wave && wave[0]) ? wave : "none");
	s.wave = -1;

	mark_modified("MCP: create cmd brief stage %d", target);

	json_t *data = json_object();
	json_object_set_new(data, "index", json_integer(target));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_update_cmd_brief_stage(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_cmd_brief, req)) return;

	auto *cb = get_cmd_brief_for_team(input, req);
	if (!cb) return;

	auto index = get_required_integer(input, "index", req);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 0, cb->num_stages - 1, "index", req)) return;

	auto new_text = get_optional_string(input, "text", false);
	auto new_ani  = get_optional_filename(input, "ani_filename", false);
	auto new_wave = get_optional_filename(input, "wave_filename", false);

	// Validate filename lengths
	if (new_ani && !check_string_length(new_ani, MAX_FILENAME_LEN - 1, "ani_filename", req)) return;
	if (new_wave && !check_string_length(new_wave, MAX_FILENAME_LEN - 1, "wave_filename", req)) return;

	cmd_brief_stage &s = cb->stage[*index];
	bool changed = false;

	if (new_text && strcmp(s.text.c_str(), new_text) != 0) {
		s.text = new_text;
		changed = true;
	}
	if (new_ani) {
		strcpy_s(s.ani_filename, new_ani);
		changed = true;
	}
	if (new_wave) {
		strcpy_s(s.wave_filename, new_wave);
		changed = true;
	}

	if (changed) {
		mark_modified("MCP: update cmd brief stage %d", *index);
	}

	req->result_json = make_json_tool_result(build_cmd_brief_stage_json(s, *index));
	req->success = true;
}

static void handle_delete_cmd_brief_stage(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_cmd_brief, req)) return;

	auto *cb = get_cmd_brief_for_team(input, req);
	if (!cb) return;

	auto index = get_required_integer(input, "index", req);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 0, cb->num_stages - 1, "index", req)) return;

	array_remove_slot(cb->stage, cb->num_stages, *index);

	mark_modified("MCP: delete cmd brief stage %d", *index);

	snprintf(req->result_message, sizeof(req->result_message),
		"Deleted command briefing stage %d", *index);
	req->success = true;
}

static MoveSwapConfig make_cmd_brief_move_swap_config(cmd_brief *cb)
{
	MoveSwapConfig cfg;
	cfg.entity_name = "cmd brief stage";
	cfg.count = cb->num_stages;
	cfg.validate_dialog = validate_dialog_for_cmd_brief;
	cfg.get_name = nullptr;	// stages don't have names; use the fallback function
	cfg.do_move = [cb](int from, int to) {
		array_move_element(cb->stage, from, to);
	};
	cfg.do_swap = [cb](int a, int b) {
		std::swap(cb->stage[a], cb->stage[b]);
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
	json_object_set_new(obj, "index", json_integer(index));
	json_object_set_new(obj, "goal_type", json_string(goal_type_name(goal.type)));
	json_object_set_new(obj, "formula", json_integer(goal.formula));

	if (!include_details)
		return obj;

	json_object_set_new(obj, "is_valid", json_boolean(!(goal.type & INVALID_GOAL)));
	json_object_set_new(obj, "message", json_string(goal.message.c_str()));
	json_object_set_new(obj, "score", json_integer(goal.score));
	json_object_set_new(obj, "team", json_string(team_name_from_index(goal.team)));
	json_object_set_new(obj, "no_music", json_boolean(goal.flags & MGF_NO_MUSIC));

	return obj;
}

static void handle_list_goals(json_t * /*input*/, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_goals, req)) return;

	json_t *arr = json_array();
	for (int i = 0; i < (int)Mission_goals.size(); i++)
		json_array_append_new(arr, build_goal_json(Mission_goals[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_goal(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_goals, req)) return;

	auto name = get_required_string(input, "name", req, false);
	if (!name) return;

	int idx = find_item_with_string(Mission_goals, &mission_goal::name, name);
	if (idx < 0) {
		set_not_found_error(req, "Goal", name);
		return;
	}

	req->result_json = make_json_tool_result(build_goal_json(Mission_goals[idx], idx, true));
	req->success = true;
}

static void handle_create_goal(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_goals, req)) return;

	auto name = get_required_string(input, "name", req, true);
	if (!name || !check_string_length(name, NAME_LENGTH - 1, "name", req)) return;

	// Check for duplicate name
	if (find_item_with_string(Mission_goals, &mission_goal::name, name) >= 0) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"A goal with name '%s' already exists", name);
		return;
	}

	auto insert_index = get_optional_integer(input, "index");

	// Optional parameters
	auto type_str   = get_optional_string(input, "goal_type", true);
	auto formula    = get_optional_integer(input, "formula");
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, req)) return;
	auto message    = get_optional_string(input, "message", false);
	auto score      = get_optional_integer(input, "score");
	auto team_str   = get_optional_string(input, "team", true);
	auto invalid    = get_optional_bool(input, "invalid");
	auto no_music   = get_optional_bool(input, "no_music");

	// Resolve type
	int goal_type = PRIMARY_GOAL;
	if (type_str) {
		if (!check_string_enum(type_str, goal_type_enum_values, "goal_type", req))
			return;
		goal_type = goal_type_from_name(type_str);
	}

	// Resolve team
	int team = 0;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", req))
			return;
		team = team_index_from_name(team_str);
	}

	// Validate insert index
	int target_index;
	if (!insert_index.has_value()) {
		target_index = (int)Mission_goals.size();
	} else {
		if (!check_int_range(*insert_index, 0, (int)Mission_goals.size(), "index", req))
			return;
		target_index = *insert_index;
	}

	if (!formula.has_value()) {
		// Build default SEXP formula: (true) — matching FRED2 editor pattern
		formula = alloc_sexp("true", SEXP_LIST, SEXP_ATOM_OPERATOR, -1, -1);
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

	json_t *data = json_object();
	json_object_set_new(data, "name", json_string(name));
	json_object_set_new(data, "index", json_integer(target_index));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_update_goal(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_goals, req)) return;

	auto name = get_required_string(input, "name", req, true);
	if (!name) return;

	int idx = find_item_with_string(Mission_goals, &mission_goal::name, name);
	if (idx < 0) {
		set_not_found_error(req, "Goal", name);
		return;
	}

	mission_goal &goal = Mission_goals[idx];

	// Validate new_name
	auto new_name = get_optional_string(input, "new_name", false);
	if (new_name) {
		if (!check_string_length(new_name, NAME_LENGTH - 1, "new_name", req)) return;
		if (!new_name[0]) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"A goal name cannot be blank!");
			return;
		}
		if (stricmp(goal.name.c_str(), new_name) != 0 &&
			find_item_with_string(Mission_goals, &mission_goal::name, new_name) >= 0) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"A goal with name '%s' already exists", new_name);
			return;
		}
	}

	// Extract optional fields
	auto type_str   = get_optional_string(input, "goal_type", true);
	auto formula    = get_optional_integer(input, "formula");
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, req)) return;
	auto message    = get_optional_string(input, "message", false);
	auto score      = get_optional_integer(input, "score");
	auto team_str   = get_optional_string(input, "team", true);
	auto invalid    = get_optional_bool(input, "invalid");
	auto no_music   = get_optional_bool(input, "no_music");

	// Validate type
	int new_type = -1;
	if (type_str) {
		if (!check_string_enum(type_str, goal_type_enum_values, "goal_type", req))
			return;
		new_type = goal_type_from_name(type_str);
	}

	// Validate team
	std::optional<int> new_team = std::nullopt;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", req))
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
	if (!validate(validate_dialog_for_goals, req)) return;

	auto force = get_optional_bool(input, "force");

	auto name = get_required_string(input, "name", req, true);
	if (!name) return;

	int idx = find_item_with_string(Mission_goals, &mission_goal::name, name);
	if (idx < 0) {
		set_not_found_error(req, "Goal", name);
		return;
	}

	// Check for SEXP references unless force is set
	if (!force.has_value() || !*force) {
		int node;
		auto ref = query_referenced_in_sexp(sexp_ref_type::NON_OBJECT, Mission_goals[idx].name.c_str(), node);
		if (ref.second != sexp_src::NONE) {
			SCP_string desc = sexp_src_to_description(ref.first, ref.second);
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"Goal '%s' is referenced in %s. Use force=true to delete anyway "
				"(references will be invalidated).", name, desc.c_str());
			return;
		}
	}

	// Invalidate SEXP references
	SCP_string buf = SCP_string("<") + Mission_goals[idx].name + ">";
	update_sexp_references(Mission_goals[idx].name.c_str(), buf.c_str(), OPF_GOAL_NAME);

	// Free the SEXP formula
	if (Mission_goals[idx].formula >= 0)
		free_sexp2(Mission_goals[idx].formula);

	Mission_goals.erase(Mission_goals.begin() + idx);

	mark_modified("MCP: delete goal %s", name);

	snprintf(req->result_message, sizeof(req->result_message), "Deleted goal: %s", name);
	req->success = true;
}

static MoveSwapConfig make_goal_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "goal";
	cfg.count = (int)Mission_goals.size();
	cfg.validate_dialog = validate_dialog_for_goals;
	cfg.get_name = [](int i) -> const char * {
		return Mission_goals[i].name.c_str();
	};
	cfg.do_move = [](int from, int to) {
		array_move_element(Mission_goals, from, to);
	};
	cfg.do_swap = [](int a, int b) {
		std::swap(Mission_goals[a], Mission_goals[b]);
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
	json_object_set_new(obj, "index", json_integer(index));
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
	if (!validate(validate_dialog_for_fiction, req)) return;

	json_t *arr = json_array();
	for (int i = 0; i < (int)Fiction_viewer_stages.size(); i++)
		json_array_append_new(arr, build_fiction_viewer_stage_json(Fiction_viewer_stages[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_fiction_viewer_stage(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_fiction, req)) return;

	auto index = get_required_integer(input, "index", req);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 0, (int)Fiction_viewer_stages.size() - 1, "index", req)) return;

	req->result_json = make_json_tool_result(build_fiction_viewer_stage_json(Fiction_viewer_stages[*index], *index));
	req->success = true;
}

static void handle_create_fiction_viewer_stage(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_fiction, req)) return;

	auto story = get_required_filename(input, "story_filename", req);
	if (!story || !check_string_length(story, MAX_FILENAME_LEN - 1, "story_filename", req)) return;

	auto font  = get_optional_filename(input, "font_filename", true);
	auto voice = get_optional_filename(input, "voice_filename", true);
	auto ui    = get_optional_string(input, "ui_name", true);
	auto bg640 = get_optional_filename(input, "background_640", true);
	auto bg1024 = get_optional_filename(input, "background_1024", true);
	auto formula = get_optional_integer(input, "formula");
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, req)) return;
	auto insert_index = get_optional_integer(input, "index");

	if (font && !check_string_length(font, MAX_FILENAME_LEN - 1, "font_filename", req)) return;
	if (voice && !check_string_length(voice, MAX_FILENAME_LEN - 1, "voice_filename", req)) return;
	if (ui && !check_string_enum(ui, fiction_ui_name_values, "ui_name", req)) return;
	if (bg640 && !check_string_length(bg640, MAX_FILENAME_LEN - 1, "background_640", req)) return;
	if (bg1024 && !check_string_length(bg1024, MAX_FILENAME_LEN - 1, "background_1024", req)) return;

	int target;
	if (!insert_index.has_value()) {
		target = (int)Fiction_viewer_stages.size();
	} else {
		if (!check_int_range(*insert_index, 0, (int)Fiction_viewer_stages.size(), "index", req))
			return;
		target = *insert_index;
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

	mark_modified("MCP: create fiction viewer stage %d", target);

	json_t *result = json_object();
	json_object_set_new(result, "index", json_integer(target));
	req->result_json = make_json_tool_result(result);
	req->success = true;
}

static void handle_update_fiction_viewer_stage(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_fiction, req)) return;

	auto index = get_required_integer(input, "index", req);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 0, (int)Fiction_viewer_stages.size() - 1, "index", req)) return;

	auto new_story = get_optional_filename(input, "story_filename", false);
	auto new_font  = get_optional_filename(input, "font_filename", false);
	auto new_voice = get_optional_filename(input, "voice_filename", false);
	auto new_ui    = get_optional_string(input, "ui_name", false);
	auto new_bg640 = get_optional_filename(input, "background_640", false);
	auto new_bg1024 = get_optional_filename(input, "background_1024", false);
	auto new_formula = get_optional_integer(input, "formula");
	if (new_formula.has_value() && !check_sexp_formula(*new_formula, OPR_BOOL, req)) return;

	if (new_story && !check_string_length(new_story, MAX_FILENAME_LEN - 1, "story_filename", req)) return;
	if (new_font && !check_string_length(new_font, MAX_FILENAME_LEN - 1, "font_filename", req)) return;
	if (new_voice && !check_string_length(new_voice, MAX_FILENAME_LEN - 1, "voice_filename", req)) return;
	if (new_ui) {
		if (!new_ui[0])
			new_ui = fiction_ui_name_values[0];
		else if (!check_string_enum(new_ui, fiction_ui_name_values, "ui_name", req))
			return;
	}
	if (new_bg640 && !check_string_length(new_bg640, MAX_FILENAME_LEN - 1, "background_640", req)) return;
	if (new_bg1024 && !check_string_length(new_bg1024, MAX_FILENAME_LEN - 1, "background_1024", req)) return;

	fiction_viewer_stage &s = Fiction_viewer_stages[*index];
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

	req->result_json = make_json_tool_result(build_fiction_viewer_stage_json(s, *index));
	req->success = true;
}

static void handle_delete_fiction_viewer_stage(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_fiction, req)) return;

	auto index = get_required_integer(input, "index", req);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 0, (int)Fiction_viewer_stages.size() - 1, "index", req)) return;

	// Free the SEXP formula
	int formula = Fiction_viewer_stages[*index].formula;
	if (formula >= 0)
		free_sexp2(formula);

	Fiction_viewer_stages.erase(Fiction_viewer_stages.begin() + *index);

	mark_modified("MCP: delete fiction viewer stage %d", *index);
	snprintf(req->result_message, sizeof(req->result_message),
		"Deleted fiction viewer stage %d", *index);
	req->success = true;
}

static MoveSwapConfig make_fiction_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "fiction viewer stage";
	cfg.count = (int)Fiction_viewer_stages.size();
	cfg.validate_dialog = validate_dialog_for_fiction;
	cfg.get_name = nullptr;	// stages don't have names; use the fallback function
	cfg.do_move = [](int from, int to) {
		array_move_element(Fiction_viewer_stages, from, to);
	};
	cfg.do_swap = [](int a, int b) {
		std::swap(Fiction_viewer_stages[a], Fiction_viewer_stages[b]);
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
	auto team_str = get_optional_string(input, "team", true);
	int team_index = 0;  // default to Team 1

	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", req))
			return nullptr;
		if (reject_team_none(team_str, "debriefing", req)) return nullptr;
		team_index = team_index_from_name(team_str);
	}

	return &Debriefings[team_index];
}

static json_t *build_debrief_stage_json(const debrief_stage &stage, int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "index", json_integer(index));
	json_object_set_new(obj, "text", json_string(stage.text.c_str()));
	set_optional_filename(obj, "voice", stage.voice);
	json_object_set_new(obj, "recommendation_text", json_string(stage.recommendation_text.c_str()));
	json_object_set_new(obj, "formula", json_integer(stage.formula));
	return obj;
}

static void handle_list_debriefing_stages(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_debriefing, req)) return;

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
	if (!validate(validate_dialog_for_debriefing, req)) return;

	auto *db = get_debriefing_for_team(input, req);
	if (!db) return;

	auto index = get_required_integer(input, "index", req);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 0, db->num_stages - 1, "index", req)) return;

	req->result_json = make_json_tool_result(build_debrief_stage_json(db->stages[*index], *index));
	req->success = true;
}

static void handle_create_debriefing_stage(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_debriefing, req)) return;

	auto *db = get_debriefing_for_team(input, req);
	if (!db) return;

	if (db->num_stages >= MAX_DEBRIEF_STAGES) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Cannot add more than %d debriefing stages", MAX_DEBRIEF_STAGES);
		return;
	}

	auto text = get_required_string(input, "text", req, false);
	if (!text) return;

	auto voice = get_optional_filename(input, "voice", false);
	auto rec_text = get_optional_string(input, "recommendation_text", false);
	auto formula = get_optional_integer(input, "formula");
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, req)) return;
	auto insert_index = get_optional_integer(input, "index");

	if (voice && !check_string_length(voice, MAX_FILENAME_LEN - 1, "voice", req)) return;

	int target;
	if (!insert_index.has_value()) {
		target = db->num_stages;
	} else {
		if (!check_int_range(*insert_index, 0, db->num_stages, "index", req))
			return;
		target = *insert_index;
	}

	if (!array_insert_slot(db->stages, db->num_stages, MAX_DEBRIEF_STAGES, target)) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Cannot add more than %d debriefing stages", MAX_DEBRIEF_STAGES);
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
	mark_modified("MCP: create debriefing stage %d", target);

	json_t *data = json_object();
	json_object_set_new(data, "index", json_integer(target));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_update_debriefing_stage(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_debriefing, req)) return;

	auto *db = get_debriefing_for_team(input, req);
	if (!db) return;

	auto index = get_required_integer(input, "index", req);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 0, db->num_stages - 1, "index", req)) return;

	auto new_text = get_optional_string(input, "text", false);
	auto new_voice = get_optional_filename(input, "voice", false);
	auto new_rec_text = get_optional_string(input, "recommendation_text", false);
	auto new_formula = get_optional_integer(input, "formula");
	if (new_formula.has_value() && !check_sexp_formula(*new_formula, OPR_BOOL, req)) return;

	if (new_voice && !check_string_length(new_voice, MAX_FILENAME_LEN - 1, "voice", req)) return;

	debrief_stage &s = db->stages[*index];
	bool changed = false;

	if (new_text) {
		s.text = new_text;
		changed = true;
	}
	if (new_voice) {
		strcpy_s(s.voice, new_voice);
		changed = true;
	}
	if (new_rec_text) {
		s.recommendation_text = new_rec_text;
		changed = true;
	}
	if (new_formula.has_value()) {
		int old_formula = s.formula;
		if (old_formula >= 0)
			free_sexp2(old_formula);
		s.formula = *new_formula;
		mcp_sexp_forest_mark_dirty({ s.formula });
		changed = true;
	}

	if (changed)
		mark_modified("MCP: update debriefing stage %d", *index);

	req->result_json = make_json_tool_result(build_debrief_stage_json(s, *index));
	req->success = true;
}

static void handle_delete_debriefing_stage(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_debriefing, req)) return;

	auto *db = get_debriefing_for_team(input, req);
	if (!db) return;

	auto index = get_required_integer(input, "index", req);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 0, db->num_stages - 1, "index", req)) return;

	int formula = db->stages[*index].formula;
	if (formula >= 0)
		free_sexp2(formula);

	array_remove_slot(db->stages, db->num_stages, *index);

	mark_modified("MCP: delete debriefing stage %d", *index);
	snprintf(req->result_message, sizeof(req->result_message),
		"Deleted debriefing stage %d", *index);
	req->success = true;
}

static MoveSwapConfig make_debriefing_move_swap_config(debriefing *db)
{
	MoveSwapConfig cfg;
	cfg.entity_name = "debriefing stage";
	cfg.count = db->num_stages;
	cfg.validate_dialog = validate_dialog_for_debriefing;
	cfg.get_name = nullptr;
	cfg.do_move = [db](int from, int to) {
		array_move_element(db->stages, from, to);
	};
	cfg.do_swap = [db](int a, int b) {
		std::swap(db->stages[a], db->stages[b]);
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
// SEXP text serialization
// ---------------------------------------------------------------------------

static void handle_sexp_to_text(json_t *input, McpToolRequest *req)
{
	if (!validate(validate_dialog_for_sexp_nodes, req)) return;
	auto node = get_required_integer(input, "node", req);
	if (!node) return;
	if (!check_int_range(*node, 0, Num_sexp_nodes - 1, "node", req)) return;

	int n = *node;

	if (Sexp_nodes[n].type == SEXP_NOT_USED) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Node %d is not in use", n);
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
	if (!validate(validate_dialog_for_sexp_nodes, req)) return;
	auto node = get_required_integer(input, "node", req);
	if (!node) return;
	if (!check_int_range(*node, 0, Num_sexp_nodes - 1, "node", req)) return;

	int n = *node;
	if (SEXP_NODE_TYPE(n) == SEXP_NOT_USED) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Node %d is not in use", n);
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
	if (!validate(validate_dialog_for_sexp_nodes, req)) return;
	auto node = get_required_integer(input, "node", req);
	if (!node) return;
	if (!check_int_range(*node, 0, Num_sexp_nodes - 1, "node", req)) return;

	int n = *node;
	if (SEXP_NODE_TYPE(n) == SEXP_NOT_USED) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Node %d is not in use", n);
		return;
	}

	auto depth = get_optional_integer(input, "depth");
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
	if (!validate(validate_dialog_for_sexp_nodes, req)) return;
	const char *text = get_required_string(input, "text", req, true);
	if (!text)
		return;

	// Save/restore global parse state
	char *old_Mp = Mp;
	char old_Current_filename[MAX_PATH_LEN];
	strcpy_s(old_Current_filename, Current_filename);

	SCP_string buf(text);
	Mp = buf.data();
	strcpy_s(Current_filename, "text_to_sexp");

	// Enable error collection so error_display() doesn't show modal dialogs
	Parse_collect_errors = true;
	Parse_errors.clear();

	int n = get_sexp_main();

	Parse_collect_errors = false;
	Mp = old_Mp;
	strcpy_s(Current_filename, old_Current_filename);

	// Check for collected parse errors
	if (!Parse_errors.empty()) {
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
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"SEXP text had %d parse error(s); see parse_errors in result",
			error_count);
		return;
	} else if (n < 0) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Could not parse SEXP; get_sexp_main() returned %d", n);
		return;
	}

	// Run syntax check
	int bad_node = -1;
	int syntax_result = check_sexp_syntax(n, OPR_AMBIGUOUS, 1, &bad_node);

	json_t *result = json_object();
	json_object_set_new(result, "node", json_integer(n));

	if (syntax_result != SEXP_CHECK_NO_ERROR) {
		json_t *warnings = json_object();
		json_object_set_new(warnings, "error_code", json_integer(syntax_result));
		json_object_set_new(warnings, "error_message", json_string(sexp_error_message(syntax_result)));
		if (bad_node >= 0) {
			json_object_set_new(warnings, "bad_node", json_integer(bad_node));
			json_object_set_new(warnings, "bad_node_text",
				json_string(Sexp_nodes[bad_node].text));
		}
		json_object_set_new(result, "syntax_error", warnings);
	}

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

	// Ship arrival/departure cues (parse objects)
	for (const auto &po : Parse_objects) {
		if (po.arrival_cue == node || po.departure_cue == node)
			return true;
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
	if (!validate(validate_dialog_for_sexp_nodes, req)) return;
	auto node = get_required_integer(input, "node", req);
	if (!node.has_value() || !check_int_range(*node, 0, Num_sexp_nodes - 1, "node", req))
		return;

	int n = *node;

	if (Sexp_nodes[n].type == SEXP_NOT_USED) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Node %d is not in use", n);
		return;
	}

	if (n == Locked_sexp_true || n == Locked_sexp_false) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Node %d is a locked singleton (%s) and cannot be freed", n, Sexp_nodes[n].text);
		return;
	}

	if (is_sexp_attached_to_mission(n)) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Node %d is attached to a mission entity (event, goal, briefing, etc.) and cannot be freed directly", n);
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
	"sexp_to_text",
	"get_sexp_node",
	"walk_sexp_tree",
	"text_to_sexp",
	"free_sexp",
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
		add_string_prop(props, "voice_file", "Filename for the voice audio");
		add_string_enum_prop(props, "team",
			"Multiplayer team assignment (\"none\" for all teams)",
			team_enum_values);
		add_integer_prop(props, "index",
			"Position to insert the message among mission messages (0 = first). "
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
		add_string_prop(props, "voice_file", "Filename for the voice audio (empty string to clear)");
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
			"Current 0-based index of the message among mission messages");
		add_integer_prop(props, "to_index",
			"Target 0-based index to move the message to");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_message",
			"Move a mission message from one position to another. "
			"Indices are 0-based within mission messages.",
			props, req);
	}

	// swap_messages
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"0-based index of the first message among mission messages");
		add_integer_prop(props, "index_b",
			"0-based index of the second message among mission messages");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_messages",
			"Swap two mission messages at the given positions. "
			"Indices are 0-based within mission messages.",
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
			"Position to insert the event (0 = first). If omitted, appends to the end.");
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
			"Current 0-based index of the event");
		add_integer_prop(props, "to_index",
			"Target 0-based index to move the event to");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_event",
			"Move a mission event from one position to another. "
			"Indices are 0-based. Updates event annotation paths.",
			props, req);
	}

	// swap_events
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"0-based index of the first event");
		add_integer_prop(props, "index_b",
			"0-based index of the second event");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_events",
			"Swap two mission events at the given positions. "
			"Indices are 0-based. Updates event annotation paths.",
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
		add_integer_prop(props, "index", "0-based index of the stage to retrieve");
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
		add_string_prop(props, "ani_filename",
			"Animation filename (ani/eff/png). Defaults to \"<default>\".");
		add_string_prop(props, "wave_filename",
			"Voice audio filename (wav/ogg). Defaults to \"none\".");
		add_integer_prop(props, "index",
			"Position to insert the stage (0 = first). If omitted, appends to the end.");
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
		add_integer_prop(props, "index", "0-based index of the stage to update");
		add_string_prop(props, "text", "New text for this stage");
		add_string_prop(props, "ani_filename", "New animation filename (ani/eff/png) (empty string to reset to default)");
		add_string_prop(props, "wave_filename", "New voice audio filename (wav/ogg) (empty string to clear)");
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
		add_integer_prop(props, "index", "0-based index of the stage to delete");
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
			"Current 0-based index of the stage");
		add_integer_prop(props, "to_index",
			"Target 0-based index to move the stage to");
		add_string_enum_prop(props, "team", cmd_brief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_cmd_brief_stage",
			"Move a command briefing stage from one position to another. "
			"Indices are 0-based.",
			props, req);
	}

	// swap_cmd_brief_stages
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"0-based index of the first stage");
		add_integer_prop(props, "index_b",
			"0-based index of the second stage");
		add_string_enum_prop(props, "team", cmd_brief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_cmd_brief_stages",
			"Swap two command briefing stages at the given positions. "
			"Indices are 0-based.",
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
			"Position to insert the goal (0 = first). If omitted, appends to the end.");
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
			"Current 0-based index of the goal");
		add_integer_prop(props, "to_index",
			"Target 0-based index to move the goal to");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_goal",
			"Move a mission goal from one position to another. "
			"Indices are 0-based.",
			props, req);
	}

	// swap_goals
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"0-based index of the first goal");
		add_integer_prop(props, "index_b",
			"0-based index of the second goal");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_goals",
			"Swap two mission goals at the given positions. "
			"Indices are 0-based.",
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
			"0-based index of the stage to retrieve");
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
			"Position to insert the stage (0 = first). If omitted, appends to the end.");
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
			"0-based index of the stage to update");
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
			"0-based index of the stage to delete");
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
			"Current 0-based index of the stage");
		add_integer_prop(props, "to_index",
			"Target 0-based index to move the stage to");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_fiction_viewer_stage",
			"Move a fiction viewer stage from one position to another. "
			"Indices are 0-based.",
			props, req);
	}

	// swap_fiction_viewer_stages
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"0-based index of the first stage");
		add_integer_prop(props, "index_b",
			"0-based index of the second stage");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_fiction_viewer_stages",
			"Swap two fiction viewer stages at the given positions. "
			"Indices are 0-based.",
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
			"0-based index of the stage to retrieve");
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
		add_string_prop(props, "voice",
			"Voice audio filename (wav/ogg). Defaults to empty (no voice).");
		add_string_prop(props, "recommendation_text",
			"Recommendation text displayed during this stage. Defaults to empty.");
		add_integer_prop(props, "formula", "Root node of the SEXP formula used for this stage. "
			"Defaults to true (stage always shown).");
		add_integer_prop(props, "index",
			"Position to insert the stage (0 = first). If omitted, appends to the end.");
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
			"0-based index of the stage to update");
		add_string_prop(props, "text", "New debriefing text for this stage");
		add_string_prop(props, "voice",
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
			"0-based index of the stage to delete");
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
			"Current 0-based index of the stage");
		add_integer_prop(props, "to_index",
			"Target 0-based index to move the stage to");
		add_string_enum_prop(props, "team", debrief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_debriefing_stage",
			"Move a debriefing stage from one position to another. "
			"Indices are 0-based.",
			props, req);
	}

	// swap_debriefing_stages
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"0-based index of the first stage");
		add_integer_prop(props, "index_b",
			"0-based index of the second stage");
		add_string_enum_prop(props, "team", debrief_team_desc, team_enum_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_debriefing_stages",
			"Swap two debriefing stages at the given positions. "
			"Indices are 0-based.",
			props, req);
	}

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
	} else {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Unknown mission tool: %s", tool_name);
	}
}
