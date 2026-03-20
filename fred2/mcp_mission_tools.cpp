#include "stdafx.h"
#include "mcp_mission_tools.h"
#include "mcpserver.h"
#include "mcp_json.h"
#include "mcp_array_utils.h"

#include <jansson.h>
#include <cstdarg>
#include <cstring>
#include <functional>

#include "globalincs/utility.h"

#include "mission/missionmessage.h"
#include "mission/missiongoals.h"
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

static const char *check_dialog_conflict_for_events()
{
	if (Event_editor_dlg && Event_editor_dlg->IsWindowVisible())
		return "Cannot modify events while the Event Editor is open. "
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
static json_t *build_message_json(const MMessage &msg, bool include_details = false)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(msg.name));
	json_object_set_new(obj, "message", json_string(msg.message));

	const char *persona = persona_name_from_index(msg.persona_index);
	if (persona)
		json_object_set_new(obj, "persona", json_string(persona));

	if (include_details) {
		json_object_set_new(obj, "team", json_integer(msg.multi_team));
		set_optional_string(obj, "talking_head", msg.avi_info.name);
		set_optional_string(obj, "voice_file", msg.wave_info.name);
	}

	return obj;
}

static void handle_list_messages(json_t *input, McpToolRequest *req)
{
	// Determine range based on "source" parameter
	const char *source = get_optional_string(input, "source");

	int start, end;
	if (source && !stricmp(source, "builtin")) {
		start = 0;
		end = Num_builtin_messages;
	} else {
		// Default: mission messages — check for conflicting dialogs
		if (set_conflict_error(req, check_dialog_conflict_for_messages)) return;
		start = Num_builtin_messages;
		end = Num_messages;
	}

	json_t *arr = json_array();
	for (int i = start; i < end; i++)
		json_array_append_new(arr, build_message_json(Messages[i]));

	req->result_json = make_list_result("messages", arr);
	req->success = true;
}

static void handle_get_message(json_t *input, McpToolRequest *req)
{
	const char *name = require_string_param(input, "name", req);
	if (!name) return;

	// Find the message
	int idx = find_item_with_string(Messages, &MMessage::name, name);
	if (idx < 0) {
		set_not_found_error(req, "Message", name);
		return;
	}

	// Check for conflicting dialogs if this is a mission-specific message
	if (idx >= Num_builtin_messages) {
		if (set_conflict_error(req, check_dialog_conflict_for_messages)) return;
	}

	req->result_json = make_json_tool_result(build_message_json(Messages[idx], true));
	req->success = true;
}

static void handle_create_message(json_t *input, McpToolRequest *req)
{
	if (set_conflict_error(req, check_dialog_conflict_for_messages)) return;

	const char *name = nullptr;
	const char *message = nullptr;
	const char *persona_str = nullptr;
	const char *talking_head = nullptr;
	const char *voice_file = nullptr;
	int team = -1;
	int insert_index = -1;  // -1 means append to end

	name    = require_string_param(input, "name", req);
	if (!name) return;
	message = require_string_param(input, "message", req);
	if (!message) return;

	persona_str  = get_optional_string(input, "persona");
	talking_head = get_optional_string(input, "talking_head");
	voice_file   = get_optional_string(input, "voice_file");
	get_optional_integer(input, "team", &team);
	get_optional_integer(input, "index", &insert_index);
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
	if (find_item_with_string(Messages, &MMessage::name, name) >= 0) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"A message with name '%s' already exists", name);
		return;
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

	// Insert a slot at the target position
	array_insert_slot(Messages, Num_messages, target_index);

	MMessage &msg = Messages[target_index];
	memset(&msg, 0, sizeof(MMessage));
	strcpy_s(msg.name, name);
	strcpy_s(msg.message, message);
	msg.persona_index = persona_index;
	msg.multi_team = team;
	msg.avi_info.name = talking_head ? strdup(talking_head) : nullptr;
	msg.wave_info.name = voice_file ? strdup(voice_file) : nullptr;

	mark_modified("MCP: create message %s", name);

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
	if (set_conflict_error(req, check_dialog_conflict_for_messages)) return;

	const char *name = require_string_param(input, "name", req);
	if (!name) return;

	// Find the message (mission-specific only)
	int idx = find_item_with_string(Messages, &MMessage::name, name, Num_builtin_messages);
	if (idx < 0) {
		set_not_found_error(req, "Message", name);
		return;
	}

	bool changed = false;

	// Update message text
	const char *new_msg = get_optional_string(input, "message");
	if (new_msg) {
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
	const char *persona_str = get_optional_string(input, "persona");
	if (persona_str) {
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
	const char *new_head = get_optional_string(input, "talking_head");
	if (new_head) {
		if (Messages[idx].avi_info.name)
			free(Messages[idx].avi_info.name);
		Messages[idx].avi_info.name = (new_head[0] != '\0') ? strdup(new_head) : nullptr;
		changed = true;
	}

	// Update voice file
	const char *new_voice = get_optional_string(input, "voice_file");
	if (new_voice) {
		if (Messages[idx].wave_info.name)
			free(Messages[idx].wave_info.name);
		Messages[idx].wave_info.name = (new_voice[0] != '\0') ? strdup(new_voice) : nullptr;
		changed = true;
	}

	// Update team
	int new_team;
	if (get_optional_integer(input, "team", &new_team)) {
		if (Messages[idx].multi_team != new_team) {
			Messages[idx].multi_team = new_team;
			changed = true;
		}
	}

	// Update name (must be last — invalidates `name` pointer and updates SEXP refs)
	const char *new_name = get_optional_string(input, "new_name");
	if (new_name) {
		if (strlen(new_name) >= NAME_LENGTH) {
			req->success = false;
			snprintf(req->result_message, sizeof(req->result_message),
				"New name too long (max %d characters)", NAME_LENGTH - 1);
			return;
		}
		if (stricmp(Messages[idx].name, new_name) != 0) {
			// Check for duplicate
			if (find_item_with_string(Messages, &MMessage::name, new_name) >= 0) {
				req->success = false;
				snprintf(req->result_message, sizeof(req->result_message),
					"A message with name '%s' already exists", new_name);
				return;
			}

			// Update SEXP references before changing the name
			update_sexp_references(Messages[idx].name, new_name, OPF_MESSAGE);
			update_sexp_references(Messages[idx].name, new_name, OPF_MESSAGE_OR_STRING);
			strcpy_s(Messages[idx].name, new_name);
			changed = true;
		}
	}

	if (changed) {
		mark_modified("MCP: update message %s", Messages[idx].name);
	}

	// Return the updated message
	req->result_json = make_json_tool_result(build_message_json(Messages[idx], true));
	req->success = true;
}

static void handle_delete_message(json_t *input, McpToolRequest *req)
{
	if (set_conflict_error(req, check_dialog_conflict_for_messages)) return;

	bool force = false;
	get_optional_bool(input, "force", &force);

	const char *name = require_string_param(input, "name", req);
	if (!name) return;

	// Find the message (mission-specific only)
	int idx = find_item_with_string(Messages, &MMessage::name, name, Num_builtin_messages);
	if (idx < 0) {
		set_not_found_error(req, "Message", name);
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

// ---------------------------------------------------------------------------
// Generic move/swap handlers
// ---------------------------------------------------------------------------

// Configuration for entity-specific move/swap behavior.
// Lambdas encapsulate offsets, annotation updates, and array access.
struct MoveSwapConfig {
	const char *entity_name;
	int count;
	std::function<const char *()> check_conflict;
	std::function<const char *(int)> get_name;
	std::function<void(int, int)> do_move;
	std::function<void(int, int)> do_swap;
};

static void handle_generic_move(json_t *input, McpToolRequest *req, const MoveSwapConfig &cfg)
{
	if (set_conflict_error(req, cfg.check_conflict)) return;

	int from_index = -1;
	int to_index = -1;
	get_optional_integer(input, "from_index", &from_index);
	get_optional_integer(input, "to_index", &to_index);

	if (from_index < 0 || from_index >= cfg.count) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Invalid from_index %d: must be 0 to %d", from_index, cfg.count - 1);
		return;
	}
	if (to_index < 0 || to_index >= cfg.count) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Invalid to_index %d: must be 0 to %d", to_index, cfg.count - 1);
		return;
	}

	if (from_index == to_index) {
		json_t *data = json_object();
		json_object_set_new(data, "name", json_string(cfg.get_name(from_index)));
		json_object_set_new(data, "index", json_integer(from_index));
		req->result_json = make_json_tool_result(data);
		req->success = true;
		return;
	}

	cfg.do_move(from_index, to_index);

	mark_modified("MCP: move %s %s from %d to %d", cfg.entity_name, cfg.get_name(to_index), from_index, to_index);

	json_t *data = json_object();
	json_object_set_new(data, "name", json_string(cfg.get_name(to_index)));
	json_object_set_new(data, "index", json_integer(to_index));
	req->result_json = make_json_tool_result(data);
	req->success = true;
}

static void handle_generic_swap(json_t *input, McpToolRequest *req, const MoveSwapConfig &cfg)
{
	if (set_conflict_error(req, cfg.check_conflict)) return;

	int index_a = -1;
	int index_b = -1;
	get_optional_integer(input, "index_a", &index_a);
	get_optional_integer(input, "index_b", &index_b);

	if (index_a < 0 || index_a >= cfg.count) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Invalid index_a %d: must be 0 to %d", index_a, cfg.count - 1);
		return;
	}
	if (index_b < 0 || index_b >= cfg.count) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Invalid index_b %d: must be 0 to %d", index_b, cfg.count - 1);
		return;
	}

	if (index_a != index_b) {
		cfg.do_swap(index_a, index_b);

		mark_modified("MCP: swap %ss %s and %s", cfg.entity_name, cfg.get_name(index_a), cfg.get_name(index_b));
	}

	json_t *data = json_object();
	json_t *a_obj = json_object();
	json_object_set_new(a_obj, "name", json_string(cfg.get_name(index_a)));
	json_object_set_new(a_obj, "index", json_integer(index_a));
	json_t *b_obj = json_object();
	json_object_set_new(b_obj, "name", json_string(cfg.get_name(index_b)));
	json_object_set_new(b_obj, "index", json_integer(index_b));
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
	cfg.check_conflict = check_dialog_conflict_for_messages;
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
	cfg.check_conflict = check_dialog_conflict_for_events;
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
	"move_event",
	"swap_events",
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
	} else if (strcmp(tool_name, "move_event") == 0) {
		handle_move_event(input_json, req);
	} else if (strcmp(tool_name, "swap_events") == 0) {
		handle_swap_events(input_json, req);
	} else {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Unknown mission tool: %s", tool_name);
	}
}
