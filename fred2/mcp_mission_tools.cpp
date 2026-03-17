#include "stdafx.h"
#include "mcp_mission_tools.h"
#include "mcpserver.h"
#include "mcp_json.h"

#include <jansson.h>
#include <cstring>

#include "mission/missionmessage.h"
#include "parse/sexp.h"
#include "freddoc.h"
#include "fred.h"
#include "messageeditordlg.h"
#include "eventeditor.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

// Returns nullptr if no conflicting dialog is open, or an error message if one is.
static const char *check_dialog_conflict_for_messages()
{
	if (Message_editor_dlg && Message_editor_dlg->IsWindowVisible())
		return "Cannot modify messages while the Message Editor is open. "
			"Close it first, or use get_ui_status to check which editors are open.";
	if (Event_editor_dlg && Event_editor_dlg->IsWindowVisible())
		return "Cannot modify messages while the Event Editor is open. "
			"Close it first, or use get_ui_status to check which editors are open.";
	return nullptr;
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
// Message tool handlers (run on main thread)
// ---------------------------------------------------------------------------

static void handle_list_messages(json_t *input, McpToolRequest *req)
{
	// Determine range based on "source" parameter
	const char *source = nullptr;
	if (input) {
		json_t *v = json_object_get(input, "source");
		if (v && json_is_string(v))
			source = json_string_value(v);
	}

	int start, end;
	if (source && !stricmp(source, "builtin")) {
		start = 0;
		end = Num_builtin_messages;
	} else {
		// Default: mission messages — check for conflicting dialogs
		const char *conflict = check_dialog_conflict_for_messages();
		if (conflict) {
			req->success = false;
			strncpy(req->result_message, conflict, sizeof(req->result_message) - 1);
			req->result_message[sizeof(req->result_message) - 1] = '\0';
			return;
		}
		start = Num_builtin_messages;
		end = Num_messages;
	}

	json_t *arr = json_array();
	for (int i = start; i < end; i++) {
		json_t *entry = json_object();
		json_object_set_new(entry, "name", json_string(Messages[i].name));
		json_object_set_new(entry, "message", json_string(Messages[i].message));

		const char *persona = persona_name_from_index(Messages[i].persona_index);
		if (persona)
			json_object_set_new(entry, "persona", json_string(persona));

		json_array_append_new(arr, entry);
	}

	json_t *data = json_object();
	json_object_set_new(data, "messages", arr);
	json_object_set_new(data, "count", json_integer(end - start));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_get_message(json_t *input, McpToolRequest *req)
{
	const char *name = nullptr;
	if (input) {
		json_t *v = json_object_get(input, "name");
		if (v && json_is_string(v))
			name = json_string_value(v);
	}
	if (!name || !name[0]) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message), "Missing required parameter: name");
		return;
	}

	// Find the message
	int idx = -1;
	for (int i = 0; i < Num_messages; i++) {
		if (!stricmp(Messages[i].name, name)) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message), "Message not found: %s", name);
		return;
	}

	// Check for conflicting dialogs if this is a mission-specific message
	if (idx >= Num_builtin_messages) {
		const char *conflict = check_dialog_conflict_for_messages();
		if (conflict) {
			req->success = false;
			strncpy(req->result_message, conflict, sizeof(req->result_message) - 1);
			req->result_message[sizeof(req->result_message) - 1] = '\0';
			return;
		}
	}

	json_t *data = json_object();
	json_object_set_new(data, "name", json_string(Messages[idx].name));
	json_object_set_new(data, "message", json_string(Messages[idx].message));

	const char *persona = persona_name_from_index(Messages[idx].persona_index);
	if (persona)
		json_object_set_new(data, "persona", json_string(persona));

	json_object_set_new(data, "team", json_integer(Messages[idx].multi_team));

	if (Messages[idx].avi_info.name)
		json_object_set_new(data, "talking_head", json_string(Messages[idx].avi_info.name));
	if (Messages[idx].wave_info.name)
		json_object_set_new(data, "voice_file", json_string(Messages[idx].wave_info.name));

	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_create_message(json_t *input, McpToolRequest *req)
{
	const char *conflict = check_dialog_conflict_for_messages();
	if (conflict) {
		req->success = false;
		strncpy(req->result_message, conflict, sizeof(req->result_message) - 1);
		req->result_message[sizeof(req->result_message) - 1] = '\0';
		return;
	}

	const char *name = nullptr;
	const char *message = nullptr;
	const char *persona_str = nullptr;
	const char *talking_head = nullptr;
	const char *voice_file = nullptr;
	int team = -1;
	int insert_index = -1;  // -1 means append to end

	if (input) {
		json_t *v;

		v = json_object_get(input, "name");
		if (v && json_is_string(v))
			name = json_string_value(v);

		v = json_object_get(input, "message");
		if (v && json_is_string(v))
			message = json_string_value(v);

		v = json_object_get(input, "persona");
		if (v && json_is_string(v))
			persona_str = json_string_value(v);

		v = json_object_get(input, "talking_head");
		if (v && json_is_string(v))
			talking_head = json_string_value(v);

		v = json_object_get(input, "voice_file");
		if (v && json_is_string(v))
			voice_file = json_string_value(v);

		v = json_object_get(input, "team");
		if (v && json_is_number(v))
			team = (int)json_number_value(v);

		v = json_object_get(input, "index");
		if (v && json_is_number(v))
			insert_index = (int)json_number_value(v);
	}

	if (!name || !name[0]) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message), "Missing required parameter: name");
		return;
	}
	if (!message || !message[0]) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message), "Missing required parameter: message");
		return;
	}
	if (strlen(name) >= NAME_LENGTH) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Name too long (max %d characters)", NAME_LENGTH - 1);
		return;
	}
	if (strlen(message) >= MESSAGE_LENGTH) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Message too long (max %d characters)", MESSAGE_LENGTH - 1);
		return;
	}

	// Check for duplicate name
	for (int i = 0; i < Num_messages; i++) {
		if (!stricmp(Messages[i].name, name)) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"A message with name '%s' already exists", name);
			return;
		}
	}

	// Look up persona by name
	int persona_index = -1;
	if (persona_str) {
		persona_index = message_persona_name_lookup(persona_str);
		if (persona_index < 0) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"Unknown persona: %s", persona_str);
			return;
		}
	}

	// Validate and resolve insert index
	int target_index;
	if (insert_index < 0) {
		// Default: append to end
		target_index = Num_messages;
	} else {
		// Caller specifies a mission-relative index (0 = first mission message)
		target_index = Num_builtin_messages + insert_index;
		if (target_index < Num_builtin_messages || target_index > Num_messages) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"Invalid index %d: must be 0 to %d (number of mission messages)",
				insert_index, Num_messages - Num_builtin_messages);
			return;
		}
	}

	// Ensure the Messages vector has room
	if (Num_messages >= (int)Messages.size())
		Messages.resize(Num_messages + 1);

	// Shift messages after the insertion point
	for (int i = Num_messages; i > target_index; i--)
		Messages[i] = Messages[i - 1];

	MMessage &msg = Messages[target_index];
	memset(&msg, 0, sizeof(MMessage));
	strcpy_s(msg.name, name);
	strcpy_s(msg.message, message);
	msg.persona_index = persona_index;
	msg.multi_team = team;
	msg.avi_info.name = talking_head ? strdup(talking_head) : nullptr;
	msg.wave_info.name = voice_file ? strdup(voice_file) : nullptr;
	Num_messages++;

	set_modified();
	if (FREDDoc_ptr) {
		char desc[128];
		snprintf(desc, sizeof(desc), "MCP: create message %s", name);
		FREDDoc_ptr->autosave(desc);
	}

	// Return the created message
	json_t *data = json_object();
	json_object_set_new(data, "name", json_string(name));
	json_object_set_new(data, "message", json_string(message));
	json_object_set_new(data, "index", json_integer(target_index - Num_builtin_messages));
	if (persona_str)
		json_object_set_new(data, "persona", json_string(persona_str));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_update_message(json_t *input, McpToolRequest *req)
{
	const char *conflict = check_dialog_conflict_for_messages();
	if (conflict) {
		req->success = false;
		strncpy(req->result_message, conflict, sizeof(req->result_message) - 1);
		req->result_message[sizeof(req->result_message) - 1] = '\0';
		return;
	}

	const char *name = nullptr;
	if (input) {
		json_t *v = json_object_get(input, "name");
		if (v && json_is_string(v))
			name = json_string_value(v);
	}
	if (!name || !name[0]) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message), "Missing required parameter: name");
		return;
	}

	// Find the message (mission-specific only)
	int idx = -1;
	for (int i = Num_builtin_messages; i < Num_messages; i++) {
		if (!stricmp(Messages[i].name, name)) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message), "Message not found: %s", name);
		return;
	}

	bool changed = false;

	// Update message text
	json_t *v = json_object_get(input, "message");
	if (v && json_is_string(v)) {
		const char *new_msg = json_string_value(v);
		if (strlen(new_msg) >= MESSAGE_LENGTH) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"Message too long (max %d characters)", MESSAGE_LENGTH - 1);
			return;
		}
		if (strcmp(Messages[idx].message, new_msg) != 0) {
			strcpy_s(Messages[idx].message, new_msg);
			changed = true;
		}
	}

	// Update persona
	v = json_object_get(input, "persona");
	if (v && json_is_string(v)) {
		const char *persona_str = json_string_value(v);
		int persona_index = message_persona_name_lookup(persona_str);
		if (persona_index < 0) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"Unknown persona: %s", persona_str);
			return;
		}
		if (Messages[idx].persona_index != persona_index) {
			Messages[idx].persona_index = persona_index;
			changed = true;
		}
	}

	// Update talking head
	v = json_object_get(input, "talking_head");
	if (v && json_is_string(v)) {
		const char *new_head = json_string_value(v);
		if (Messages[idx].avi_info.name)
			free(Messages[idx].avi_info.name);
		Messages[idx].avi_info.name = (new_head[0] != '\0') ? strdup(new_head) : nullptr;
		changed = true;
	}

	// Update voice file
	v = json_object_get(input, "voice_file");
	if (v && json_is_string(v)) {
		const char *new_voice = json_string_value(v);
		if (Messages[idx].wave_info.name)
			free(Messages[idx].wave_info.name);
		Messages[idx].wave_info.name = (new_voice[0] != '\0') ? strdup(new_voice) : nullptr;
		changed = true;
	}

	// Update team
	v = json_object_get(input, "team");
	if (v && json_is_number(v)) {
		int new_team = (int)json_number_value(v);
		if (Messages[idx].multi_team != new_team) {
			Messages[idx].multi_team = new_team;
			changed = true;
		}
	}

	// Update name (must be last — invalidates `name` pointer and updates SEXP refs)
	v = json_object_get(input, "new_name");
	if (v && json_is_string(v)) {
		const char *new_name = json_string_value(v);
		if (strlen(new_name) >= NAME_LENGTH) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"New name too long (max %d characters)", NAME_LENGTH - 1);
			return;
		}
		if (stricmp(Messages[idx].name, new_name) != 0) {
			// Check for duplicate
			for (int i = 0; i < Num_messages; i++) {
				if (i != idx && !stricmp(Messages[i].name, new_name)) {
					req->success = false;
					snprintf(req->result_message, sizeof(req->result_message),
						"A message with name '%s' already exists", new_name);
					return;
				}
			}

			// Update SEXP references before changing the name
			update_sexp_references(Messages[idx].name, new_name, OPF_MESSAGE);
			update_sexp_references(Messages[idx].name, new_name, OPF_MESSAGE_OR_STRING);
			strcpy_s(Messages[idx].name, new_name);
			changed = true;
		}
	}

	if (changed) {
		set_modified();
		if (FREDDoc_ptr) {
			char desc[128];
			snprintf(desc, sizeof(desc), "MCP: update message %s", Messages[idx].name);
			FREDDoc_ptr->autosave(desc);
		}
	}

	// Return the updated message
	json_t *data = json_object();
	json_object_set_new(data, "name", json_string(Messages[idx].name));
	json_object_set_new(data, "message", json_string(Messages[idx].message));
	const char *persona = persona_name_from_index(Messages[idx].persona_index);
	if (persona)
		json_object_set_new(data, "persona", json_string(persona));
	json_object_set_new(data, "team", json_integer(Messages[idx].multi_team));
	if (Messages[idx].avi_info.name)
		json_object_set_new(data, "talking_head", json_string(Messages[idx].avi_info.name));
	if (Messages[idx].wave_info.name)
		json_object_set_new(data, "voice_file", json_string(Messages[idx].wave_info.name));

	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_delete_message(json_t *input, McpToolRequest *req)
{
	const char *conflict = check_dialog_conflict_for_messages();
	if (conflict) {
		req->success = false;
		strncpy(req->result_message, conflict, sizeof(req->result_message) - 1);
		req->result_message[sizeof(req->result_message) - 1] = '\0';
		return;
	}

	const char *name = nullptr;
	bool force = false;

	if (input) {
		json_t *v = json_object_get(input, "name");
		if (v && json_is_string(v))
			name = json_string_value(v);

		v = json_object_get(input, "force");
		if (v && json_is_boolean(v))
			force = json_is_true(v);
	}
	if (!name || !name[0]) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message), "Missing required parameter: name");
		return;
	}

	// Find the message (mission-specific only)
	int idx = -1;
	for (int i = Num_builtin_messages; i < Num_messages; i++) {
		if (!stricmp(Messages[i].name, name)) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message), "Message not found: %s", name);
		return;
	}

	// Check for SEXP references unless force is set
	if (!force) {
		int node;
		auto ref = query_referenced_in_sexp(sexp_ref_type::SHIP, Messages[idx].name, node);
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
	for (int i = idx; i < Num_messages - 1; i++)
		Messages[i] = Messages[i + 1];
	Num_messages--;

	set_modified();
	if (FREDDoc_ptr) {
		char desc[128];
		snprintf(desc, sizeof(desc), "MCP: delete message %s", name);
		FREDDoc_ptr->autosave(desc);
	}

	snprintf(req->result_message, sizeof(req->result_message), "Deleted message: %s", name);
	req->success = true;
}

static void handle_move_message(json_t *input, McpToolRequest *req)
{
	const char *conflict = check_dialog_conflict_for_messages();
	if (conflict) {
		req->success = false;
		strncpy(req->result_message, conflict, sizeof(req->result_message) - 1);
		req->result_message[sizeof(req->result_message) - 1] = '\0';
		return;
	}

	int from_index = -1;
	int to_index = -1;

	if (input) {
		json_t *v;

		v = json_object_get(input, "from_index");
		if (v && json_is_number(v))
			from_index = (int)json_number_value(v);

		v = json_object_get(input, "to_index");
		if (v && json_is_number(v))
			to_index = (int)json_number_value(v);
	}

	int num_mission_msgs = Num_messages - Num_builtin_messages;

	if (from_index < 0 || from_index >= num_mission_msgs) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Invalid from_index %d: must be 0 to %d", from_index, num_mission_msgs - 1);
		return;
	}
	if (to_index < 0 || to_index >= num_mission_msgs) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Invalid to_index %d: must be 0 to %d", to_index, num_mission_msgs - 1);
		return;
	}

	if (from_index == to_index) {
		// No-op: already at the target position
		int abs_idx = Num_builtin_messages + from_index;
		json_t *data = json_object();
		json_object_set_new(data, "name", json_string(Messages[abs_idx].name));
		json_object_set_new(data, "index", json_integer(from_index));
		req->result_json = make_json_tool_result(data);
		req->success = true;
		return;
	}

	int abs_from = Num_builtin_messages + from_index;
	int abs_to = Num_builtin_messages + to_index;

	// Save the message being moved
	MMessage temp = Messages[abs_from];

	// Shift to close the gap at abs_from, then open a slot at abs_to
	if (abs_from < abs_to) {
		for (int i = abs_from; i < abs_to; i++)
			Messages[i] = Messages[i + 1];
	} else {
		for (int i = abs_from; i > abs_to; i--)
			Messages[i] = Messages[i - 1];
	}

	Messages[abs_to] = temp;

	set_modified();
	if (FREDDoc_ptr) {
		char desc[128];
		snprintf(desc, sizeof(desc), "MCP: move message %s from %d to %d", temp.name, from_index, to_index);
		FREDDoc_ptr->autosave(desc);
	}

	json_t *data = json_object();
	json_object_set_new(data, "name", json_string(Messages[abs_to].name));
	json_object_set_new(data, "index", json_integer(to_index));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_swap_messages(json_t *input, McpToolRequest *req)
{
	const char *conflict = check_dialog_conflict_for_messages();
	if (conflict) {
		req->success = false;
		strncpy(req->result_message, conflict, sizeof(req->result_message) - 1);
		req->result_message[sizeof(req->result_message) - 1] = '\0';
		return;
	}

	int index_a = -1;
	int index_b = -1;

	if (input) {
		json_t *v;

		v = json_object_get(input, "index_a");
		if (v && json_is_number(v))
			index_a = (int)json_number_value(v);

		v = json_object_get(input, "index_b");
		if (v && json_is_number(v))
			index_b = (int)json_number_value(v);
	}

	int num_mission_msgs = Num_messages - Num_builtin_messages;

	if (index_a < 0 || index_a >= num_mission_msgs) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Invalid index_a %d: must be 0 to %d", index_a, num_mission_msgs - 1);
		return;
	}
	if (index_b < 0 || index_b >= num_mission_msgs) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Invalid index_b %d: must be 0 to %d", index_b, num_mission_msgs - 1);
		return;
	}

	int abs_a = Num_builtin_messages + index_a;
	int abs_b = Num_builtin_messages + index_b;

	if (index_a != index_b) {
		MMessage temp = Messages[abs_a];
		Messages[abs_a] = Messages[abs_b];
		Messages[abs_b] = temp;

		set_modified();
		if (FREDDoc_ptr) {
			char desc[128];
			snprintf(desc, sizeof(desc), "MCP: swap messages %s and %s",
				Messages[abs_a].name, Messages[abs_b].name);
			FREDDoc_ptr->autosave(desc);
		}
	}

	json_t *data = json_object();
	json_t *a_obj = json_object();
	json_object_set_new(a_obj, "name", json_string(Messages[abs_a].name));
	json_object_set_new(a_obj, "index", json_integer(index_a));
	json_t *b_obj = json_object();
	json_object_set_new(b_obj, "name", json_string(Messages[abs_b].name));
	json_object_set_new(b_obj, "index", json_integer(index_b));
	json_object_set_new(data, "a", a_obj);
	json_object_set_new(data, "b", b_obj);
	req->result_json = make_json_tool_result(data);
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
		add_string_prop(props, "source",
			"Which messages to list: \"mission\" (default) for mission-specific messages, "
			"or \"builtin\" for built-in engine messages from messages.tbl");
		register_tool(tools, "list_messages",
			"List messages. By default lists mission-specific messages. "
			"Use source=\"builtin\" to list built-in engine messages instead. "
			"Returns each message's name, text, and persona.",
			props);
	}

	// get_message
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the message to retrieve");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "get_message",
			"Get full details of a message by name, including text, persona, "
			"talking head animation, voice file, and team assignment.",
			props, req);
	}

	// create_message
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Unique name for the message");
		add_string_prop(props, "message", "The message text displayed in-game");
		add_string_prop(props, "persona",
			"Name of the persona who delivers this message (e.g. \"Wingman 1\")");
		add_string_prop(props, "talking_head", "Filename for the talking head animation");
		add_string_prop(props, "voice_file", "Filename for the voice audio");
		add_integer_prop(props, "team", "Multiplayer team filter (-1 for all teams)");
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
		add_string_prop(props, "new_name", "New name for the message (updates SEXP references)");
		add_string_prop(props, "message", "New message text");
		add_string_prop(props, "persona",
			"Name of the persona who delivers this message (e.g. \"Wingman 1\")");
		add_string_prop(props, "talking_head", "Filename for the talking head animation (empty string to clear)");
		add_string_prop(props, "voice_file", "Filename for the voice audio (empty string to clear)");
		add_integer_prop(props, "team", "Multiplayer team filter (-1 for all teams)");
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
	} else {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Unknown mission tool: %s", tool_name);
	}
}
