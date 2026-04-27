#include "stdafx.h"
#include "mcp_events.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "mcp_array_utils.h"
#include "mcp_sexp.h"
#include "mcp_sexp_forest.h"

#include <jansson.h>
#include <algorithm>
#include <cstring>

#include "globalincs/utility.h"

#include "mission/missiongoals.h"
#include "parse/parselo.h"
#include "parse/sexp.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_events(SCP_string &error_msg)
{
	return validate_single_dialog("events", "event", error_msg);
}

// ---------------------------------------------------------------------------
// Enum helpers
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> event_unit_enum_values = { "milliseconds", "seconds" };

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
	json_object_set_new(obj, "name", json_safe_string(evt.name.c_str()));
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
		json_object_set_new(obj, "team", json_safe_string(team_name_from_index(evt.team)));

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

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int idx = mission_event_lookup(name);
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

	auto name = get_required_string(input, "name", sink, true, NAME_LENGTH - 1);
	if (!name) return;

	// Check for duplicate name
	if (mission_event_lookup(name) >= 0) {
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

	auto units = get_optional_string(input, "chain_and_interval_units", sink);
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
	auto team_str = get_optional_string(input, "team", sink);
	int multi_team = -1;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", sink))
			return;
		multi_team = team_index_from_name(team_str);
	}

	auto objective_text = get_optional_string(input, "objective_text", sink, MULTITEXT_LENGTH - 1);
	auto objective_key_text = get_optional_string(input, "objective_key_text", sink, MULTITEXT_LENGTH - 1);
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
		// Use the parser so the default formula has the same node structure
		// as any formula created via text_to_sexp or loaded from a mission file
		int default_formula = parse_sexp_text("( when ( true ) ( do-nothing ) )", "create_event");
		if (default_formula < 0) {
			sink.set_error("Failed to create default event formula");
			return;
		}
		formula = default_formula;
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

	int idx = mission_event_lookup(name);
	if (idx < 0) {
		set_not_found_error(sink,"Event", name);
		return;
	}

	mission_event &evt = Mission_events[idx];

	// Validate new_name
	auto new_name      = get_optional_string(input, "new_name", sink, NAME_LENGTH - 1);
	if (new_name) {
		if (!check_general_rename(new_name, evt.name.c_str(),
			[](const char *n) { return mission_event_lookup(n) >= 0; },
			"Event", sink)) return;
	}

	// Extract optional fields
	auto formula       = get_optional_integer(input, "formula", sink);
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_NULL, sink)) return;
	auto is_chained    = get_optional_bool(input, "is_chained", sink);
	auto repeat_count  = get_optional_integer(input, "repeat_count", sink);
	auto trigger_count = get_optional_integer(input, "trigger_count", sink);
	auto interval      = get_optional_integer(input, "interval", sink);

	auto units = get_optional_string(input, "chain_and_interval_units", sink);
	if (units && !check_string_enum(units, event_unit_enum_values, "chain_and_interval_units", sink))
		return;

	auto score         = get_optional_integer(input, "score", sink);
	auto chain_delay   = get_optional_integer(input, "chain_delay", sink);

	if (!chain_delay.has_value() && is_chained.has_value()) {
		chain_delay = *is_chained ? 0 : -1;
	}

	// Team
	auto team_str = get_optional_string(input, "team", sink);
	std::optional<int> new_team = std::nullopt;
	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", sink))
			return;
		new_team = team_index_from_name(team_str);
	}

	auto objective_text     = get_optional_string(input, "objective_text", sink, MULTITEXT_LENGTH - 1);
	auto objective_key_text = get_optional_string(input, "objective_key_text", sink, MULTITEXT_LENGTH - 1);
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
		mcp_sexp_forest_mark_dirty();
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

	int idx = mission_event_lookup(name);
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
	mcp_sexp_forest_mark_dirty();

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
// Move/swap config and handlers
// ---------------------------------------------------------------------------

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
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_event_tools(json_t *tools)
{
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
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_event_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_events") == 0) {
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
	} else {
		return false;
	}
	return true;
}
