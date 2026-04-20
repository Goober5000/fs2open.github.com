#include "stdafx.h"
#include "mcp_sexp.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "mcp_app.h"
#include "mcp_mission_tools.h"
#include "mcp_sexp_forest.h"
#include "mcp_reference_tools.h"
#include "sexp_tree.h"

#include <jansson.h>
#include <algorithm>
#include <climits>
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
#include "parse/sexp/sexp_lookup.h"
#include "freddoc.h"
#include "fred.h"
#include "eventeditor.h"
#include "mainfrm.h"

#define PLACEHOLDER_STRING "<placeholder>"

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
// SEXP formula helpers
// ---------------------------------------------------------------------------

bool check_sexp_formula(int node, sexp_opr_t expected_return_type, McpErrorSink &sink)
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
		sink.set_error("Formula node %d (\"%s\") has return type %s, but this entity requires %s",
			node, Sexp_nodes[node].text,
			get_opr_type_name(actual_return_type),
			get_opr_type_name(expected_return_type));
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

enum class entity_specific_tag { NONE, TEAM_1, TEAM_2, ARRIVAL_CUE, DEPARTURE_CUE };
static entity_specific_tag enum_from_tag(const char *str)
{
	if (!stricmp(str, "none")) return entity_specific_tag::NONE;
	if (!stricmp(str, "team_1")) return entity_specific_tag::TEAM_1;
	if (!stricmp(str, "team_2")) return entity_specific_tag::TEAM_2;
	if (!stricmp(str, "arrival_cue")) return entity_specific_tag::ARRIVAL_CUE;
	if (!stricmp(str, "departure_cue")) return entity_specific_tag::DEPARTURE_CUE;
	Warning(LOCATION, "Invalid tag string %s", str);
	return entity_specific_tag::NONE;
}
static const char *enum_to_tag(entity_specific_tag tag)
{
	switch (tag)
	{
		case entity_specific_tag::TEAM_1: return "team_1";
		case entity_specific_tag::TEAM_2: return "team_2";
		case entity_specific_tag::ARRIVAL_CUE: return "arrival_cue";
		case entity_specific_tag::DEPARTURE_CUE: return "departure_cue";
		default: return "none";
	}
}
#if (MAX_TVT_TEAMS) != 2
#error FormulaRootInfo must be updated with another way to distinguish teams!
#endif

struct FormulaRootInfo
{
	int root;                                       // root of the tree (walked up via parent chain)
	bool attached;                                  // root is a mission formula
	sexp_opr_t opr_type;                            // OPR_NULL (events) or OPR_BOOL (all others); only meaningful if attached
	const char *attached_type;					    // if attached, the entity type that holds the formula
	std::variant<const char *, int> attached_id;    // if attached, the entity name or index that holds the formula
	entity_specific_tag attached_tag;               // if attached, additional relevant information about the formula holder

	static bool valid_tag(const char *entity_type, entity_specific_tag tag) {
		if (!stricmp(entity_type, "ship") || !stricmp(entity_type, "wing")) {
			return tag == entity_specific_tag::ARRIVAL_CUE || tag == entity_specific_tag::DEPARTURE_CUE;
		}
		if (!stricmp(entity_type, "briefing_stage") || !stricmp(entity_type, "debriefing_stage")) {
			return tag == entity_specific_tag::TEAM_1 || tag == entity_specific_tag::TEAM_2;
		}
		return tag == entity_specific_tag::NONE;
	}

	SCP_string to_string() const
	{
		SCP_string str(attached_type);
		str += " ";
		if (std::holds_alternative<const char*>(attached_id)) {
			str += std::get<const char*>(attached_id);
		} else {
			str += std::to_string(std::get<int>(attached_id));
		}
		switch (attached_tag) {
		case entity_specific_tag::ARRIVAL_CUE:
		case entity_specific_tag::DEPARTURE_CUE:
			str = " for " + str;
			str = enum_to_tag(attached_tag) + str;
			break;
		case entity_specific_tag::TEAM_1:
		case entity_specific_tag::TEAM_2:
			str += ", ";
			str += enum_to_tag(attached_tag);
			break;
		default:
			break;
		}
		return str;
	}
};

// Entity-type values that can hold formulas.  Matches the entities scanned
// by find_formula_root_and_type.
static const SCP_vector<const char *> formula_entity_type_values = {
	"cutscene", "fiction_viewer_stage", "briefing_stage", "debriefing_stage",
	"ship", "wing", "event", "goal"
};
static const SCP_vector<const char *> formula_entity_tag_values = {
	"arrival_cue", "departure_cue", "team_1", "team_2"
};

// Build a FormulaRootInfo from user-supplied entity coordinates.
// Returns the current formula root index via out_current_root, or -1 on error.
static FormulaRootInfo build_formula_root_info_for_entity(
	const char *entity_type, const char *entity_id, const char *entity_tag,
	int &out_current_root, McpErrorSink &sink)
{
	FormulaRootInfo info = { -1, true, OPR_BOOL, entity_type, 0, entity_specific_tag::NONE };
	out_current_root = -1;

	// Convert entity_tag string to enum and apply defaults for entities that
	// have multiple formula slots
	entity_specific_tag tag = entity_tag ? enum_from_tag(entity_tag) : entity_specific_tag::NONE;
	if (tag == entity_specific_tag::NONE) {
		if (!stricmp(entity_type, "ship") || !stricmp(entity_type, "wing"))
			tag = entity_specific_tag::ARRIVAL_CUE;
		else if (!stricmp(entity_type, "briefing_stage") || !stricmp(entity_type, "debriefing_stage"))
			tag = entity_specific_tag::TEAM_1;
	}
	if (!FormulaRootInfo::valid_tag(entity_type, tag)) {
		sink.set_error("Tag '%s' is not valid for entity type '%s'", entity_tag, entity_type);
		return info;
	}

	if (!stricmp(entity_type, "cutscene")) {
		int idx = atoi(entity_id);
		if (idx < 0 || idx >= (int)The_mission.cutscenes.size()) {
			sink.set_error("Cutscene index %d is out of range (0..%d)", idx, (int)The_mission.cutscenes.size() - 1);
			return info;
		}
		info.attached_id = idx;
		out_current_root = The_mission.cutscenes[idx].formula;
	} else if (!stricmp(entity_type, "fiction_viewer_stage")) {
		int idx = atoi(entity_id);
		if (idx < 0 || idx >= (int)Fiction_viewer_stages.size()) {
			sink.set_error("Fiction viewer stage index %d is out of range (0..%d)", idx, (int)Fiction_viewer_stages.size() - 1);
			return info;
		}
		info.attached_id = idx;
		out_current_root = Fiction_viewer_stages[idx].formula;
	} else if (!stricmp(entity_type, "briefing_stage")) {
		int idx = atoi(entity_id);
		int t = (tag == entity_specific_tag::TEAM_2) ? 1 : 0;
		if (idx < 0 || idx >= Briefings[t].num_stages) {
			sink.set_error("Briefing stage index %d is out of range for team %d (0..%d)", idx, t + 1, Briefings[t].num_stages - 1);
			return info;
		}
		info.attached_id = idx;
		info.attached_tag = tag;
		out_current_root = Briefings[t].stages[idx].formula;
	} else if (!stricmp(entity_type, "debriefing_stage")) {
		int idx = atoi(entity_id);
		int t = (tag == entity_specific_tag::TEAM_2) ? 1 : 0;
		if (idx < 0 || idx >= Debriefings[t].num_stages) {
			sink.set_error("Debriefing stage index %d is out of range for team %d (0..%d)", idx, t + 1, Debriefings[t].num_stages - 1);
			return info;
		}
		info.attached_id = idx;
		info.attached_tag = tag;
		out_current_root = Debriefings[t].stages[idx].formula;
	} else if (!stricmp(entity_type, "ship")) {
		int ship_idx = ship_name_lookup(entity_id, 1);
		if (ship_idx < 0) {
			set_not_found_error(sink, "Ship", entity_id);
			return info;
		}
		info.attached_id = Ships[ship_idx].ship_name;
		info.attached_tag = tag;
		if (tag == entity_specific_tag::DEPARTURE_CUE)
			out_current_root = Ships[ship_idx].departure_cue;
		else
			out_current_root = Ships[ship_idx].arrival_cue;
	} else if (!stricmp(entity_type, "wing")) {
		int wing_idx = wing_name_lookup(entity_id);
		if (wing_idx < 0) {
			set_not_found_error(sink, "Wing", entity_id);
			return info;
		}
		info.attached_id = Wings[wing_idx].name;
		info.attached_tag = tag;
		if (tag == entity_specific_tag::DEPARTURE_CUE)
			out_current_root = Wings[wing_idx].departure_cue;
		else
			out_current_root = Wings[wing_idx].arrival_cue;
	} else if (!stricmp(entity_type, "event")) {
		int evt_idx = mission_event_lookup(entity_id);
		if (evt_idx < 0) {
			set_not_found_error(sink, "Event", entity_id);
			return info;
		}
		info.opr_type = OPR_NULL;
		info.attached_id = Mission_events[evt_idx].name.c_str();
		out_current_root = Mission_events[evt_idx].formula;
	} else if (!stricmp(entity_type, "goal")) {
		int goal_idx = mission_goal_lookup(entity_id);
		if (goal_idx < 0) {
			set_not_found_error(sink, "Goal", entity_id);
			return info;
		}
		info.attached_id = Mission_goals[goal_idx].name.c_str();
		out_current_root = Mission_goals[goal_idx].formula;
	} else {
		sink.set_error("Unknown entity type '%s'", entity_type);
		return info;
	}

	info.root = out_current_root;
	return info;
}

// Walk up from any node to find its tree root, then check if that root is
// attached to a mission entity and determine the expected return type.
static FormulaRootInfo find_formula_root_and_type(int node)
{
	// Walk to root
	int root = find_sexp_root(node);

	// Check against all mission entities

	// Mission cutscenes (OPR_BOOL)
	for (int i = 0; i < (int)The_mission.cutscenes.size(); i++) {
		if (The_mission.cutscenes[i].formula == root)
			return { root, true, OPR_BOOL, "cutscene", i, entity_specific_tag::NONE };
	}

	// Fiction viewer stages (OPR_BOOL)
	for (int i = 0; i < (int)Fiction_viewer_stages.size(); i++) {
		if (Fiction_viewer_stages[i].formula == root)
			return { root, true, OPR_BOOL, "fiction_viewer_stage", i, entity_specific_tag::NONE };
	}

	// Briefing stages (OPR_BOOL)
	for (int t = 0; t < MAX_TVT_TEAMS; t++) {
		for (int s = 0; s < Briefings[t].num_stages; s++) {
			if (Briefings[t].stages[s].formula == root)
				return { root, true, OPR_BOOL, "briefing_stage", s, (t == 0) ? entity_specific_tag::TEAM_1 : entity_specific_tag::TEAM_2 };
		}
	}

	// Debriefing stages (OPR_BOOL)
	for (int t = 0; t < MAX_TVT_TEAMS; t++) {
		for (int s = 0; s < Debriefings[t].num_stages; s++) {
			if (Debriefings[t].stages[s].formula == root)
				return { root, true, OPR_BOOL, "debriefing_stage", s, (t == 0) ? entity_specific_tag::TEAM_1 : entity_specific_tag::TEAM_2 };
		}
	}

	// Ship arrival/departure cues (OPR_BOOL)
	for (auto objp : list_range(&obj_used_list)) {
		if (objp->type == OBJ_SHIP || objp->type == OBJ_START) {
			auto shipp = &Ships[objp->instance];
			if (shipp->arrival_cue == root)
				return { root, true, OPR_BOOL, "ship", shipp->ship_name, entity_specific_tag::ARRIVAL_CUE };
			if (shipp->departure_cue == root)
				return { root, true, OPR_BOOL, "ship", shipp->ship_name, entity_specific_tag::DEPARTURE_CUE };
		}
	}

	// Wing arrival/departure cues (OPR_BOOL)
	for (int i = 0; i < Num_wings; i++) {
		if (Wings[i].arrival_cue == root)
			return { root, true, OPR_BOOL, "wing", Wings[i].name, entity_specific_tag::ARRIVAL_CUE };
		if (Wings[i].departure_cue == root)
			return { root, true, OPR_BOOL, "wing", Wings[i].name, entity_specific_tag::DEPARTURE_CUE };
	}

	// Events (OPR_NULL)
	for (const auto &evt : Mission_events) {
		if (evt.formula == root)
			return { root, true, OPR_NULL, "event", evt.name.c_str(), entity_specific_tag::NONE };
	}

	// Goals (OPR_BOOL)
	for (const auto &goal : Mission_goals) {
		if (goal.formula == root)
			return { root, true, OPR_BOOL, "goal", goal.name.c_str(), entity_specific_tag::NONE };
	}

	return { root, false, OPR_NULL, nullptr, 0, entity_specific_tag::NONE };
}

// Set a mission entity formula to new_root, using the entity identification
// from a previously computed FormulaRootInfo.
static void set_formula(const FormulaRootInfo &info, int new_root)
{
	Assertion(info.attached, "set_formula called on unattached formula!");

	const char *type = info.attached_type;
	int index = std::holds_alternative<int>(info.attached_id) ? std::get<int>(info.attached_id) : -1;
	const char *name = std::holds_alternative<const char *>(info.attached_id) ? std::get<const char *>(info.attached_id) : nullptr;

	if (!stricmp(type, "cutscene")) {
		The_mission.cutscenes[index].formula = new_root;
	} else if (!stricmp(type, "fiction_viewer_stage")) {
		Fiction_viewer_stages[index].formula = new_root;
	} else if (!stricmp(type, "briefing_stage")) {
		int t = (info.attached_tag == entity_specific_tag::TEAM_1) ? 0 : 1;
		Briefings[t].stages[index].formula = new_root;
	} else if (!stricmp(type, "debriefing_stage")) {
		int t = (info.attached_tag == entity_specific_tag::TEAM_1) ? 0 : 1;
		Debriefings[t].stages[index].formula = new_root;
	} else if (!stricmp(type, "ship")) {
		int ship_idx = ship_name_lookup(name, 1);
		Assertion(ship_idx >= 0, "set_formula: ship '%s' not found!", name);
		if (info.attached_tag == entity_specific_tag::ARRIVAL_CUE)
			Ships[ship_idx].arrival_cue = new_root;
		else
			Ships[ship_idx].departure_cue = new_root;
	} else if (!stricmp(type, "wing")) {
		int wing_idx = wing_name_lookup(name);
		Assertion(wing_idx >= 0, "set_formula: wing '%s' not found!", name);
		if (info.attached_tag == entity_specific_tag::ARRIVAL_CUE)
			Wings[wing_idx].arrival_cue = new_root;
		else
			Wings[wing_idx].departure_cue = new_root;
	} else if (!stricmp(type, "event")) {
		int evt_idx = mission_event_lookup(name);
		Assertion(evt_idx >= 0, "set_formula: event '%s' not found!", name);
		Mission_events[evt_idx].formula = new_root;
	} else if (!stricmp(type, "goal")) {
		int goal_idx = mission_goal_lookup(name);
		Assertion(goal_idx >= 0, "set_formula: goal '%s' not found!", name);
		Mission_goals[goal_idx].formula = new_root;
	} else {
		Assertion(0, "set_formula: unknown entity type '%s'!", type);
	}
}

static void handle_get_sexp_formula_info(json_t *input, McpToolRequest *req)
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

	FormulaRootInfo info = find_formula_root_and_type(n);

	json_t *result = json_object();
	json_object_set_new(result, "node", json_integer(n));
	json_object_set_new(result, "root", json_integer(info.root));
	json_object_set_new(result, "attached", info.attached ? json_true() : json_false());

	if (info.attached) {
		json_object_set_new(result, "return_type",
			json_string(get_opr_type_name(info.opr_type)));
		json_object_set_new(result, "entity_type", json_string(info.attached_type));

		if (std::holds_alternative<const char *>(info.attached_id))
			json_object_set_new(result, "entity_id",
				json_string(std::get<const char *>(info.attached_id)));
		else
			json_object_set_new(result, "entity_id",
				json_integer(std::get<int>(info.attached_id)));

		if (info.attached_tag != entity_specific_tag::NONE)
			json_object_set_new(result, "entity_tag", json_string(enum_to_tag(info.attached_tag)));
	}

	req->result_json = make_json_tool_result(result);
	req->success = true;
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
	const char *param_name, int &out_flags, McpErrorSink &sink)
{
	auto lookup_fn = [&](const char *str)->int {
		return find_item_with_string(entries, count, &flag_entry::name, str);
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

	convert_sexp_to_string(req->result_message, n, SEXP_SAVE_MODE);
	req->success = true;
}

// ---------------------------------------------------------------------------
// SEXP node navigation
// ---------------------------------------------------------------------------

static const char *get_sexp_kind(int n)
{
	switch (SEXP_NODE_TYPE(n)) {
		case SEXP_LIST: return "list";
		case SEXP_ATOM: return "atom";
		default:        return "not_used";
	}
}

static const char *get_sexp_role(int n)
{
	// role: high-level classification of what this node represents
	if (SEXP_NODE_TYPE(n) == SEXP_LIST)
		return "list_wrapper";
	else if (Sexp_nodes[n].subtype == SEXP_ATOM_OPERATOR)
		return "operator";
	else
		return "argument";
}

static const char *get_sexp_value_type(int n)
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

static constexpr size_t MAX_SEXP_WALK_ENTRIES = 500;

static json_t *build_sexp_node_json(int n)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "node", json_integer(n));
	json_object_set_new(obj, "value", json_string(Sexp_nodes[n].text));

	json_object_set_new(obj, "kind", json_string(get_sexp_kind(n)));
	json_object_set_new(obj, "role", json_string(get_sexp_role(n)));
	const char *node_type = get_sexp_value_type(n);
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
	if (Sexp_nodes[n].type == SEXP_NOT_USED) {
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
	if (n < 0 || n >= Num_sexp_nodes || entries.size() >= MAX_SEXP_WALK_ENTRIES)
		return;
	if (Sexp_nodes[n].type == SEXP_NOT_USED)
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
	if (Sexp_nodes[n].type == SEXP_NOT_USED) {
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
	if (entries.size() >= MAX_SEXP_WALK_ENTRIES)
		json_object_set_new(result, "truncated", json_true());

	req->result_json = make_json_tool_result(result);
	req->success = true;
}

// ---------------------------------------------------------------------------
// SEXP text parsing
// ---------------------------------------------------------------------------

// Parse SEXP text and return the root node index, or -1 on failure.
// Saves and restores the global parse state so it can be called from anywhere.
// Any errors encountered will be stored in the Parse_errors vector.
int parse_sexp_text(const char *text, const char *source)
{
	// Save global parse state (Mp, Current_filename, Warning_count, Error_count)
	pause_parse();

	SCP_string buf(text);
	Mp = buf.data();
	strcpy_s(Current_filename, source);

	// Enable error collection so error_display() doesn't show modal dialogs
	Parse_collect_errors = true;
	Parse_errors.clear();

	int n = get_sexp_main();

	// Restore global parse state
	Parse_collect_errors = false;
	unpause_parse();

	if (!Parse_errors.empty()) {
		// Free any partially-allocated SEXP nodes
		if (n >= 0)
			free_sexp2(n);
		return -1;
	}

	return n;
}

static void handle_text_to_sexp(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;
	auto text = get_required_string(input, "text", sink, true);
	if (!text)
		return;

	int n = parse_sexp_text(text, "text_to_sexp");

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
// SEXP node manipulation
// ---------------------------------------------------------------------------

static bool is_placeholder_node(int node)
{
	return node >= 0
		&& Sexp_nodes[node].type == SEXP_ATOM
		&& Sexp_nodes[node].subtype == SEXP_ATOM_STRING
		&& !stricmp(Sexp_nodes[node].text, PLACEHOLDER_STRING);
}

// Replace one node with another in the tree's link structure.
// Uses find_sexp_list and find_sexp_antecedent to locate the node
// that references old_node, even when parent == -1.
static void splice_replace_node(int old_node, int new_node)
{
	// Check if old_node is the first child of a list wrapper
	int ref = find_sexp_list(old_node);
	if (ref >= 0) {
		Sexp_nodes[ref].first = new_node;
		return;
	}

	// Check if old_node is in a rest chain
	ref = find_sexp_antecedent(old_node);
	if (ref >= 0) {
		Sexp_nodes[ref].rest = new_node;
		return;
	}
}

// Restore the predecessor link that was modified by splice_replace_node or a
// manual predecessor update.  pred_list is the wrapper whose .first was
// changed; pred_ante is the sibling whose .rest was changed.
static void restore_predecessor(int pred_list, int pred_ante, int original_node)
{
	if (pred_list >= 0)
		Sexp_nodes[pred_list].first = original_node;
	else if (pred_ante >= 0)
		Sexp_nodes[pred_ante].rest = original_node;
}

// Undo the operator-atom wrapping performed during attach.  If wrapped_source
// is true, frees the wrapper and clears the flag.  Otherwise, resets the
// effective_source (which equals the unwrapped source) back to free-standing.
static void undo_source_wrap(int source, int effective_source, bool &wrapped_source)
{
	if (wrapped_source) {
		// Match the wrap side: don't touch locked singletons' parent field.
		if (source != Locked_sexp_true && source != Locked_sexp_false)
			Sexp_nodes[source].parent = -1;
		free_one_sexp(effective_source);
		wrapped_source = false;
	} else {
		Sexp_nodes[effective_source].rest = -1;
		Sexp_nodes[effective_source].parent = -1;
	}
}

// If node is an operator atom that is the first child of a list wrapper,
// return the wrapper index; otherwise return node unchanged.
static int retarget_to_list_wrapper(int node)
{
	if (SEXP_NODE_TYPE(node) == SEXP_ATOM
		&& Sexp_nodes[node].subtype == SEXP_ATOM_OPERATOR) {
		int list = find_sexp_list(node);
		if (list >= 0)
			return list;
	}
	return node;
}

// Release a displaced subtree: free it if do_free is true, otherwise detach
// it by clearing its parent.  Skips locked singletons and negative indices.
// Returns the number of nodes freed (0 if preserved or skipped).
static int release_subtree(int node, bool do_free)
{
	if (node < 0 || node == Locked_sexp_true || node == Locked_sexp_false)
		return 0;
	if (do_free)
		return free_sexp2(node);
	Sexp_nodes[node].parent = -1;
	return 0;
}

// Run check_sexp_syntax on an attached tree after a splice.  If the tree is
// not attached, returns true immediately (no check needed).  On syntax
// failure, calls rollback_fn and sets an error on sink, then returns false.
// On success, returns true; the caller is responsible for calling
// mark_modified on the true path.
static bool check_attached_syntax_or_rollback(
	const FormulaRootInfo &info, bool is_attached,
	const std::function<void()> &rollback_fn,
	const char *action_verb,
	McpErrorSink &sink)
{
	if (!is_attached)
		return true;

	int bad_node = -1;
	int syntax_result = check_sexp_syntax(
		info.root, static_cast<int>(info.opr_type), 1, &bad_node);
	if (syntax_result != SEXP_CHECK_NO_ERROR) {
		rollback_fn();
		sink.set_error(
			"%s would cause syntax error in formula root %d: %s "
			"(error code %d, bad node %d)",
			action_verb, info.root,
			sexp_error_message(syntax_result), syntax_result, bad_node);
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Shared target resolution
// ---------------------------------------------------------------------------

struct ResolvedTarget {
	enum class Mode { Entity, Node };
	Mode mode;
	int node = -1;
	int original_node = -1;
	FormulaRootInfo entity_info = {};
	int entity_current_root = -1;
};

static bool resolve_target(json_t *input, ResolvedTarget &out, McpErrorSink &sink)
{
	auto target_opt = get_optional_integer(input, "target_node", sink);
	auto arg_idx_opt = get_optional_integer(input, "target_argument_index", sink);
	auto entity_type = get_optional_string(input, "target_entity_type", sink);
	auto entity_id = get_optional_string(input, "target_entity_id", sink);
	auto entity_tag = get_optional_string(input, "entity_tag", sink);
	if (sink.has_error()) return false;

	bool have_target_node = target_opt.has_value();
	bool have_entity = (entity_type != nullptr);

	if (have_target_node == have_entity) {
		sink.set_error("Exactly one of 'target_node' or 'target_entity_type' must be provided");
		return false;
	}

	if (arg_idx_opt.has_value() && !have_target_node) {
		sink.set_error("'target_argument_index' can only be used with 'target_node', not with entity mode");
		return false;
	}

	if (have_entity) {
		// --- Entity mode ---
		if (!entity_id) {
			sink.set_error("'target_entity_id' is required when 'target_entity_type' is provided");
			return false;
		}
		if (!check_string_enum(entity_type, formula_entity_type_values, "target_entity_type", sink))
			return false;
		if (entity_tag) {
			if (!check_string_enum(entity_tag, formula_entity_tag_values, "entity_tag", sink))
				return false;
		}
		out.mode = ResolvedTarget::Mode::Entity;
		out.entity_info = build_formula_root_info_for_entity(entity_type, entity_id, entity_tag,
			out.entity_current_root, sink);
		if (sink.has_error()) return false;
		if (out.entity_current_root >= Num_sexp_nodes) {
			sink.set_error("Entity formula root index %d is out of range", out.entity_current_root);
			return false;
		}
		out.node = out.entity_current_root;
		out.original_node = out.entity_current_root;
		return true;
	}

	// --- Node mode ---
	int target = *target_opt;
	if (!check_int_range(target, 0, Num_sexp_nodes - 1, "target_node", sink))
		return false;
	if (Sexp_nodes[target].type == SEXP_NOT_USED) {
		sink.set_error("Target node %d is not in use", target);
		return false;
	}
	out.original_node = target;

	if (!arg_idx_opt.has_value()) {
		// No argument index — direct node addressing.
		// Reject locked singletons since they can't be uniquely addressed.
		if (target == Locked_sexp_true || target == Locked_sexp_false) {
			sink.set_error("Target node %d is a shared singleton (locked %s) and cannot be "
				"uniquely addressed. Pass the parent operator as target_node and set "
				"target_argument_index to the singleton's position in its argument list.",
				target, (target == Locked_sexp_true) ? "true" : "false");
			return false;
		}
		out.mode = ResolvedTarget::Mode::Node;
		out.node = target;
		return true;
	}

	// --- Node + argument index ---
	int arg_idx = *arg_idx_opt;
	if (arg_idx < 0) {
		sink.set_error("target_argument_index must be non-negative, got %d", arg_idx);
		return false;
	}

	// Retarget operator atom to its list wrapper if applicable
	int resolved = retarget_to_list_wrapper(target);

	// Must be an operator atom or list wrapper
	int op_atom;
	if (SEXP_NODE_TYPE(resolved) == SEXP_LIST) {
		op_atom = Sexp_nodes[resolved].first;
		if (op_atom < 0 || Sexp_nodes[op_atom].subtype != SEXP_ATOM_OPERATOR) {
			sink.set_error("target_argument_index requires target_node to be an operator or its list wrapper");
			return false;
		}
	} else if (SEXP_NODE_TYPE(resolved) == SEXP_ATOM && Sexp_nodes[resolved].subtype == SEXP_ATOM_OPERATOR) {
		op_atom = resolved;
	} else {
		sink.set_error("target_argument_index requires target_node to be an operator or its list wrapper");
		return false;
	}

	// Walk the argument chain from the operator atom
	int arg = Sexp_nodes[op_atom].rest;
	int count = 0;
	for (int i = 0; i < arg_idx; i++) {
		if (arg < 0)
			break;
		arg = Sexp_nodes[arg].rest;
		count++;
	}

	if (arg < 0) {
		sink.set_error("Operator '%s' has %d argument(s); target_argument_index=%d is out of range",
			Sexp_nodes[op_atom].text, count, arg_idx);
		return false;
	}

	out.mode = ResolvedTarget::Mode::Node;
	out.node = arg;
	return true;
}

// ---------------------------------------------------------------------------
// detach_sexp_node handler (run on main thread)
// ---------------------------------------------------------------------------

static json_t *handle_detach_sexp_node(int n, bool shrink, bool do_delete,
	const FormulaRootInfo *pre_resolved, McpErrorSink &sink);

static void handle_detach_sexp_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	ResolvedTarget target;
	if (!resolve_target(input, target, sink))
		return;

	if (target.mode == ResolvedTarget::Mode::Entity && target.entity_current_root < 0) {
		sink.set_error("Entity has no formula to detach (formula index %d)", target.entity_current_root);
		return;
	}

	auto shrink_opt = get_optional_bool(input, "shrink", sink);
	bool shrink = shrink_opt.has_value() && *shrink_opt;
	auto delete_opt = get_optional_bool(input, "delete", sink);
	bool do_delete = delete_opt.has_value() && *delete_opt;

	const FormulaRootInfo *pre = nullptr;
	if (target.mode == ResolvedTarget::Mode::Entity)
		pre = &target.entity_info;

	auto result_json = handle_detach_sexp_node(target.node, shrink, do_delete, pre, sink);
	if (sink.has_error()) return;

	req->result_json = result_json;
	req->success = true;
}

static json_t *handle_detach_sexp_node(int n, bool shrink, bool do_delete,
	const FormulaRootInfo *pre_resolved, McpErrorSink &sink)
{
	int original_n = n;

	if (Sexp_nodes[n].type == SEXP_NOT_USED) {
		sink.set_error("Node %d is not in use", n);
		return nullptr;
	}

	// If the client targeted an operator atom inside a list wrapper,
	// retarget to the wrapper so the entire sub-expression is detached.
	// Skip when pre_resolved (entity mode targets the formula root directly).
	if (!pre_resolved)
		n = retarget_to_list_wrapper(n);

	// Determine context: walk to tree root and check mission attachment
	FormulaRootInfo info = pre_resolved ? *pre_resolved : find_formula_root_and_type(n);
	bool is_root = (n == info.root);
	bool is_attached = info.attached;

	int replacement = -1;
	int freed_count = 0;

	if (is_root && is_attached) {
		// Case A: Root of a mission-attached formula -- replace with default
		if (info.opr_type == OPR_NULL) {
			replacement = parse_sexp_text("( do-nothing )", "detach_sexp_node");
			if (replacement < 0) {
				sink.set_error("Failed to create replacement formula");
				return nullptr;
			}
		} else {
			replacement = Locked_sexp_true;
		}

		// If the replacement is the same node as the current formula, there
		// is nothing to do (e.g. an OPR_BOOL entity already set to true).
		if (replacement == n) {
			sink.set_error("Entity formula is already the default (%s); "
				"there is nothing to detach",
				Sexp_nodes[n].text);
			if (replacement != Locked_sexp_true && replacement != Locked_sexp_false)
				free_sexp2(replacement);
			return nullptr;
		}

		// Set the entity formula to the replacement
		set_formula(info, replacement);

		// Syntax check the replacement
		int bad_node = -1;
		int syntax_result = check_sexp_syntax(replacement, static_cast<int>(info.opr_type), 1, &bad_node);
		if (syntax_result != SEXP_CHECK_NO_ERROR) {
			// Rollback
			set_formula(info, n);
			if (replacement != Locked_sexp_true && replacement != Locked_sexp_false)
				free_sexp2(replacement);
			sink.set_error("Detachment would cause syntax error: %s (error code %d, bad node %d)",
				sexp_error_message(syntax_result), syntax_result, bad_node);
			return nullptr;
		}

		// no rollback: it's a modification
		mark_modified("MCP: replace SEXP formula for %s", info.to_string().c_str());

		// Detach the old tree and optionally free it
		freed_count = release_subtree(n, do_delete);

	} else if (is_root) {
		// Case B: Root of a free-standing tree
		freed_count = release_subtree(n, do_delete);

		// we may not have actually done anything
		if (!do_delete && n == original_n) {
			sink.set_error("Node %d is already the root of a detached tree; there is nothing to do. Pass delete=true to free it, or pass an inner node to detach a sub-tree.", n);
			return nullptr;
		}

	} else {
		// Cases C & D: Embedded node -- splice a replacement into the link chain.
		int target_rest = Sexp_nodes[n].rest;

		// Locate the predecessor before splicing so we can reliably restore
		// it on rollback.  splice_replace_node can't find the old position if
		// the forward splice replaced n with -1 (shrink mode, last sibling).
		int pred_list = find_sexp_list(n);                                      // wrapper whose .first == n
		int pred_ante = (pred_list < 0) ? find_sexp_antecedent(n) : -1;         // sibling whose .rest == n

		if (shrink) {
			// Shrink mode: link the predecessor directly to the next sibling,
			// shifting subsequent arguments up by one position.
			replacement = -1;
			splice_replace_node(n, target_rest);
		} else {
			// Default mode: insert a placeholder to preserve argument positions.
			replacement = alloc_sexp(PLACEHOLDER_STRING, SEXP_ATOM, SEXP_ATOM_STRING, -1, target_rest);
			Sexp_nodes[replacement].parent = Sexp_nodes[n].parent;
			splice_replace_node(n, replacement);
		}

		// Syntax check for mission-attached trees (Case C).
		if (!check_attached_syntax_or_rollback(info, is_attached, [&]() {
				restore_predecessor(pred_list, pred_ante, n);
				if (replacement >= 0) {
					Sexp_nodes[replacement].rest = -1;	// don't walk into the live tree
					Sexp_nodes[replacement].parent = -1;
					free_sexp2(replacement);
				}
			}, "Detachment", sink))
			return nullptr;
		if (is_attached)
			mark_modified("MCP: detach SEXP node in %s", info.to_string().c_str());

		// Commit: detach and optionally free the target subtree
		Sexp_nodes[n].rest = -1;	// don't walk into the live tree
		Sexp_nodes[n].parent = -1;
		if (do_delete)
			freed_count = free_sexp2(n);
	}

	if (is_root && is_attached)
		mcp_sexp_forest_mark_dirty({ replacement });	// case A: mark the new formula, not the old (now detached/freed) root
	else
		mcp_sexp_forest_mark_dirty({ info.root });		// cases B/C/D: mark the tree that was modified

	// If preserved, the detached node is the root of a new free-standing tree
	if (!do_delete) {
		// but if the new free-standing tree is wrapped, unwrap it
		if (n != original_n) {
			free_one_sexp(n);
			freed_count++;
			Sexp_nodes[original_n].parent = -1;
			// Reparent siblings in original_n's rest chain: alloc_sexp had
			// set them to parent = wrapper, which we just freed.  Point them
			// at the operator atom (the new root of the detached subtree).
			for (int sib = Sexp_nodes[original_n].rest; sib != -1; sib = Sexp_nodes[sib].rest)
				Sexp_nodes[sib].parent = original_n;
		}
		mcp_sexp_forest_mark_dirty({ original_n });
	}

	// Build response.  Always report the node the user passed in, regardless
	// of whether we retargeted internally to a list wrapper.
	json_t *result = json_object();
	json_object_set_new(result, "detached_node", json_integer(original_n));
	if (!do_delete)
		json_object_set_new(result, "detached_node_data", build_sexp_node_json(original_n));
	json_object_set_new(result, "deleted", (do_delete && freed_count > 0) ? json_true() : json_false());
	json_object_set_new(result, "unwrapped", (n != original_n) ? json_true() : json_false());
	json_object_set_new(result, "freed_count", json_integer(freed_count));
	if (replacement >= 0) {
		json_object_set_new(result, "replacement_node", json_integer(replacement));
		json_object_set_new(result, "replacement_node_data", build_sexp_node_json(replacement));
	}
	else
		json_object_set_new(result, "replacement_node", json_null());
	return make_json_tool_result(result);
}

// ---------------------------------------------------------------------------
// attach_sexp_node handler (run on main thread)
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> attach_position_values = { "replace", "before", "after" };
enum class attach_position { REPLACE, BEFORE, AFTER };

// Shared logic for replacing a mission entity's formula with a new source node.
// Validates source, sets the formula, runs a syntax check (rolling back on failure),
// handles the displaced subtree, marks modified, and marks the forest dirty.
// Returns true on success; on failure, populates sink and returns false.
static bool attach_as_entity_formula(
	int source, const FormulaRootInfo &info, int current_root,
	bool delete_displaced, int &displaced, int &freed_count,
	McpErrorSink &sink)
{
	// Validate source against entity's expected return type
	if (!check_sexp_formula(source, info.opr_type, sink))
		return false;

	displaced = current_root;

	// Commit: set the entity formula to source
	set_formula(info, source);

	// Syntax check (defensive, should not fail since check_sexp_formula passed)
	int bad_node = -1;
	int syntax_result = check_sexp_syntax(source, static_cast<int>(info.opr_type), 1, &bad_node);
	if (syntax_result != SEXP_CHECK_NO_ERROR) {
		// Rollback
		set_formula(info, current_root);
		sink.set_error("Attachment would cause syntax error: %s (error code %d, bad node %d)",
			sexp_error_message(syntax_result), syntax_result, bad_node);
		return false;
	}

	// Handle the displaced formula
	freed_count = release_subtree(displaced, delete_displaced);

	// mark_modified
	mark_modified("MCP: attach SEXP formula for %s", info.to_string().c_str());

	mcp_sexp_forest_mark_dirty({ source, displaced });
	return true;
}

static void handle_attach_sexp_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	// --- Parse parameters ---
	auto source_opt = get_required_integer(input, "source_node", sink);
	if (!source_opt.has_value()) return;
	int source = *source_opt;
	if (!check_int_range(source, 0, Num_sexp_nodes - 1, "source_node", sink))
		return;

	ResolvedTarget resolved;
	if (!resolve_target(input, resolved, sink))
		return;

	auto position_str = get_optional_string(input, "position", sink);
	auto delete_displaced_opt = get_optional_bool(input, "delete_displaced", sink);
	if (sink.has_error()) return;

	bool delete_displaced = delete_displaced_opt.has_value() && *delete_displaced_opt;

	// Determine position enum
	attach_position position = attach_position::REPLACE;
	if (position_str) {
		if (!check_string_enum(position_str, attach_position_values, "position", sink))
			return;
		if (!stricmp(position_str, "before")) position = attach_position::BEFORE;
		else if (!stricmp(position_str, "after")) position = attach_position::AFTER;
	}

	bool have_entity = (resolved.mode == ResolvedTarget::Mode::Entity);

	// --- Validate source ---
	if (Sexp_nodes[source].type == SEXP_NOT_USED) {
		sink.set_error("Source node %d is not in use", source);
		return;
	}
	bool source_is_locked = (source == Locked_sexp_true || source == Locked_sexp_false);
	// Locked singletons (true/false) are shared nodes whose fields must not
	// be modified.  They are still valid attach sources: entity-formula mode
	// just stores the node index, and node-relative mode auto-wraps them in
	// a SEXP_LIST whose fields are modified instead.  Skip the root and
	// attached checks since singletons are intentionally shared.
	if (!source_is_locked) {
		FormulaRootInfo src_info = find_formula_root_and_type(source);
		if (src_info.root != source) {
			sink.set_error("Source node %d is not a free-standing root. Use detach_sexp_node first to detach it from its current tree.", source);
			return;
		}
		if (src_info.attached) {
			sink.set_error("Source node %d is currently attached to %s. Use detach_sexp_node first to detach it.",
				source, src_info.attached_type);
			return;
		}
	}

	int original_source = source;
	int displaced = -1;
	int freed_count = 0;

	if (have_entity) {
		// ---------------------------------------------------------------
		// Case A': Entity formula mode
		// ---------------------------------------------------------------

		if (!attach_as_entity_formula(source, resolved.entity_info, resolved.entity_current_root,
				delete_displaced, displaced, freed_count, sink))
			return;

	} else {
		// ---------------------------------------------------------------
		// Node-relative mode
		// ---------------------------------------------------------------
		int target = resolved.node;

		if (target == source) {
			sink.set_error("Source and target cannot be the same node (%d)", source);
			return;
		}

		// Retarget: if target is an operator atom inside a list wrapper,
		// operate on the wrapper instead.  Skip when the resolver already
		// descended via target_argument_index.
		int original_target = resolved.original_node;
		if (target == original_target)
			target = retarget_to_list_wrapper(target);

		// Determine context
		FormulaRootInfo info = find_formula_root_and_type(target);
		bool is_root = (target == info.root);
		bool is_attached = info.attached;

		// Guard against cycles: if target is inside source's own tree,
		// splicing source into target's position would create a cycle.
		if (info.root == source) {
			sink.set_error("Target node %d is inside source node %d's tree; "
				"attaching would create a cycle", target, source);
			return;
		}

		if (is_root && !is_attached) {
			// Case B': free-standing root — nothing to attach to
			sink.set_error("Target node %d is a free-standing root; to attach source as a formula, "
				"use target_entity_type/target_entity_id. To replace content inside a tree, "
				"target an embedded node.", target);
			return;
		}

		if (is_root && is_attached && position != attach_position::REPLACE) {
			// Case A' via target_node with insert mode — formula roots have no sibling chain
			sink.set_error("Cannot insert before/after a formula root. Use position='replace' to replace the formula.");
			return;
		}

		// If the source is an operator atom that will be spliced into a rest
		// chain, wrap it in a SEXP_LIST node first.  check_sexp_syntax expects
		// list wrappers (or data atoms) in rest chains, and the splice would
		// overwrite source.rest, severing any argument chain.
		// Entity-formula mode and root-replace mode don't need this because
		// those paths store the operator atom directly as the formula root.
		int effective_source = source;
		bool wrapped_source = false;
		bool need_wrap = (SEXP_NODE_TYPE(source) == SEXP_ATOM
			&& Sexp_nodes[source].subtype == SEXP_ATOM_OPERATOR
			&& !(is_root && is_attached));  // root-replace delegates to entity logic
		if (need_wrap) {
			effective_source = alloc_sexp("", SEXP_LIST, SEXP_ATOM_LIST, source, -1);
			// Don't set parent on locked singletons — they're shared and their
			// parent must stay -1 (matching how the parser handles them).
			if (!source_is_locked)
				Sexp_nodes[source].parent = effective_source;
			wrapped_source = true;
		}

		if (position == attach_position::REPLACE) {
			// -------------------------------------------------------
			// Replace mode
			// -------------------------------------------------------

			if (is_root && is_attached) {
				// Replacing a formula root via target_node: delegate to entity logic.
				if (!attach_as_entity_formula(source, info, info.root, delete_displaced, displaced, freed_count, sink))
					return;

			} else {
				// Embedded replace (Cases C'/D')
				int pred_list = find_sexp_list(target);
				int pred_ante = (pred_list < 0) ? find_sexp_antecedent(target) : -1;

				int target_parent = Sexp_nodes[target].parent;
				int target_rest = Sexp_nodes[target].rest;

				// Splice effective_source into target's position
				Sexp_nodes[effective_source].parent = target_parent;
				Sexp_nodes[effective_source].rest = target_rest;
				splice_replace_node(target, effective_source);

				// Detach the displaced target
				Sexp_nodes[target].rest = -1;
				Sexp_nodes[target].parent = -1;
				displaced = target;

				// Syntax check for attached trees (Case C')
				if (!check_attached_syntax_or_rollback(info, is_attached, [&]() {
						Sexp_nodes[target].rest = target_rest;
						Sexp_nodes[target].parent = target_parent;
						restore_predecessor(pred_list, pred_ante, target);
						undo_source_wrap(source, effective_source, wrapped_source);
					}, "Attachment", sink))
					return;
				if (is_attached)
					mark_modified("MCP: attach SEXP node in %s", info.to_string().c_str());

				// Handle displaced subtree
				freed_count = release_subtree(displaced, delete_displaced);

				mcp_sexp_forest_mark_dirty({ info.root });
				mcp_sexp_forest_unmark_dirty_root(source);
				if (!delete_displaced && displaced >= 0)
					mcp_sexp_forest_mark_dirty({ displaced });
			}

		} else if (position == attach_position::BEFORE) {
			// -------------------------------------------------------
			// Insert before
			// -------------------------------------------------------

			if (is_root) {
				sink.set_error("Cannot insert before a tree root; use position='replace', or target an embedded node.");
				return;
			}

			int pred_list = find_sexp_list(target);
			int pred_ante = (pred_list < 0) ? find_sexp_antecedent(target) : -1;

			Sexp_nodes[effective_source].parent = Sexp_nodes[target].parent;
			Sexp_nodes[effective_source].rest = target;

			if (pred_list >= 0)
				Sexp_nodes[pred_list].first = effective_source;
			else if (pred_ante >= 0)
				Sexp_nodes[pred_ante].rest = effective_source;

			if (!check_attached_syntax_or_rollback(info, is_attached, [&]() {
					restore_predecessor(pred_list, pred_ante, target);
					undo_source_wrap(source, effective_source, wrapped_source);
				}, "Insertion", sink))
				return;
			if (is_attached)
				mark_modified("MCP: insert SEXP node in %s", info.to_string().c_str());

			mcp_sexp_forest_mark_dirty({ info.root });
			mcp_sexp_forest_unmark_dirty_root(source);

		} else {
			// -------------------------------------------------------
			// Insert after
			// -------------------------------------------------------
			if (is_root) {
				sink.set_error("Cannot insert after a tree root; use position='replace', or target an embedded node.");
				return;
			}

			int old_target_rest = Sexp_nodes[target].rest;

			Sexp_nodes[effective_source].parent = Sexp_nodes[target].parent;
			Sexp_nodes[effective_source].rest = old_target_rest;
			Sexp_nodes[target].rest = effective_source;

			if (!check_attached_syntax_or_rollback(info, is_attached, [&]() {
					Sexp_nodes[target].rest = old_target_rest;
					undo_source_wrap(source, effective_source, wrapped_source);
				}, "Insertion", sink))
				return;
			if (is_attached)
				mark_modified("MCP: insert SEXP node in %s", info.to_string().c_str());

			mcp_sexp_forest_mark_dirty({ info.root });
			mcp_sexp_forest_unmark_dirty_root(source);
		}
	}

	// Build response
	json_t *result = json_object();
	json_object_set_new(result, "source_node", json_integer(original_source));
	json_object_set_new(result, "source_node_data", build_sexp_node_json(original_source));
	const char *pos_str = have_entity ? "entity_formula"
		: (position == attach_position::REPLACE) ? "replace"
		: (position == attach_position::BEFORE)  ? "before"
		: "after";
	json_object_set_new(result, "position", json_string(pos_str));

	if (displaced >= 0) {
		json_object_set_new(result, "displaced_node", json_integer(displaced));
		if (!delete_displaced && displaced != Locked_sexp_true && displaced != Locked_sexp_false)
			json_object_set_new(result, "displaced_node_data", build_sexp_node_json(displaced));
	} else {
		json_object_set_new(result, "displaced_node", json_null());
	}

	json_object_set_new(result, "deleted_displaced", (delete_displaced && freed_count > 0) ? json_true() : json_false());
	json_object_set_new(result, "freed_count", json_integer(freed_count));

	req->result_json = make_json_tool_result(result);
	req->success = true;
}

// ---------------------------------------------------------------------------
// create_sexp_node handler (run on main thread)
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> sexp_role_values = { "list_wrapper", "operator", "argument" };
static const SCP_vector<const char *> sexp_arg_type_values = { "number", "string", "boolean", "node" };
static const SCP_vector<const char *> sexp_mutable_arg_type_values = { "number", "string", "boolean" };

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
static int create_sexp_arg_node(const char *type_str, const char *value_str, int next, int arg_index, McpErrorSink &sink)
{
	// Variable handling for number/string types
	if ((!stricmp(type_str, "number") || !stricmp(type_str, "string")) && value_str[0] == '@') {
		const char *var_name = value_str + 1;
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
		if (!can_construe_as_integer(value_str)) {
			sink.set_error("Argument %d: value \"%s\" is not a valid number", arg_index, value_str);
			return -1;
		}
		return alloc_sexp(value_str, SEXP_ATOM, SEXP_ATOM_NUMBER, -1, next);
	}

	if (!stricmp(type_str, "string")) {
		return alloc_sexp(value_str, SEXP_ATOM, SEXP_ATOM_STRING, -1, next);
	}

	if (!stricmp(type_str, "boolean")) {
		int locked_node;
		if (!stricmp(value_str, "true"))
			locked_node = Locked_sexp_true;
		else if (!stricmp(value_str, "false"))
			locked_node = Locked_sexp_false;
		else {
			sink.set_error("Boolean argument %d must be \"true\" or \"false\", got \"%s\"", arg_index, value_str);
			return -1;
		}
		return alloc_sexp("", SEXP_LIST, SEXP_ATOM_LIST, locked_node, next);
	}

	if (!stricmp(type_str, "node")) {
		char *endptr;
		long idx_long = strtol(value_str, &endptr, 10);
		int idx = static_cast<int>(idx_long);
		if (*endptr != '\0' || endptr == value_str) {
			sink.set_error("Node argument %d: value must be an integer node index, got \"%s\"", arg_index, value_str);
			return -1;
		}
		// -1 is a placeholder meaning "empty slot, to be filled later"
		if (idx == -1) {
			return alloc_sexp(PLACEHOLDER_STRING, SEXP_ATOM, SEXP_ATOM_STRING, -1, next);
		}
		if (idx < 0 || idx >= Num_sexp_nodes) {
			sink.set_error("Node argument %d: node index %d is out of range (0-%d)", arg_index, idx, Num_sexp_nodes - 1);
			return -1;
		}
		if (Sexp_nodes[idx].type == SEXP_NOT_USED) {
			sink.set_error("Node argument %d: node %d is not in use", arg_index, idx);
			return -1;
		}
		if (find_sexp_root(idx) != idx) {
			sink.set_error("Node argument %d: node %d is not the root of its own tree", arg_index, idx);
			return -1;
		}
		// Always wrap in SEXP_LIST to avoid corrupting existing tree linkage
		return alloc_sexp("", SEXP_LIST, SEXP_ATOM_LIST, idx, next);
	}

	// Should not reach here if type was validated
	sink.set_error("Unknown argument type \"%s\" in argument %d", type_str, arg_index);
	return -1;
}

struct create_arg_result {
	int arg_node = -1;
	bool do_type_checking = false;
	const char *type_str = nullptr;
	const char *value_str = nullptr;
};
static create_arg_result create_sexp_arg_node(json_t *input, bool allow_node_type, int next, int arg_index, McpErrorSink &sink)
{
	auto type_str = get_required_string(input, "argument_type", sink, true);
	if (!type_str) return {};
	if (!check_string_enum(type_str, allow_node_type ? sexp_arg_type_values : sexp_mutable_arg_type_values, "argument_type", sink)) return {};

	auto value_str = get_required_string(input, "argument_value", sink, true, TOKEN_LENGTH - 1);
	if (!value_str) return {};

	bool do_type_checking = true;
	// A node value of -1 is a placeholder that bypasses type checking
	if (   (!stricmp(type_str, "node")   && !strcmp(value_str, "-1"))
		|| (!stricmp(type_str, "string") && !stricmp(value_str, PLACEHOLDER_STRING))
	)
		do_type_checking = false;

	int arg_node = create_sexp_arg_node(type_str, value_str, next, arg_index, sink);
	if (arg_node < 0 && !sink.has_error())
		sink.set_error("Unable to create SEXP node!");

	return { arg_node, do_type_checking, type_str, value_str };
}

static void handle_create_sexp_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	auto role = get_required_string(input, "role", sink, true);
	if (!role) return;
	if (!check_string_enum(role, sexp_role_values, "role", sink)) return;

	if (!stricmp(role, "list_wrapper")) {
		sink.set_error("Creating a node with the 'list_wrapper' role is not supported");
		return;
	}

	create_arg_result result;

	// --- argument role: create a standalone argument node ---
	if (!stricmp(role, "argument")) {
		result = create_sexp_arg_node(input, false, -1, 0, sink);
		if (sink.has_error()) return;

		mcp_sexp_forest_mark_dirty({ result.arg_node });

		req->result_json = make_json_tool_result(build_sexp_node_json(result.arg_node));
		req->success = true;
		return;
	}

	// --- operator role: create an operator with arguments ---
	auto op_name = get_required_string(input, "operator_name", sink, true, TOKEN_LENGTH - 1);
	if (!op_name) return;

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
				result = {};
				sink.set_error("Argument %d is not an object", i);
				break;
			} else {
				result = create_sexp_arg_node(arg, true, next, i, sink);
				if (sink.has_error()) break;
			}

			// Type-check this argument against the operator's expected type
			int expected_opf = query_operator_argument_type(op_idx, i);
			if (!result.do_type_checking) {
				// Placeholder nodes skip type checking entirely
			} else if (expected_opf == OPF_NONE) {
				// Argument index exceeds operator's expected count; warn but allow
				if (!warnings) warnings = json_array();
				SCP_string wmsg;
				sprintf(wmsg, "Argument %d exceeds expected argument count for '%s'; type not checked", i, op_name);
				json_array_append_new(warnings, json_string(wmsg.c_str()));
			} else {
				bool is_variable = (!stricmp(result.type_str, "number") || !stricmp(result.type_str, "string")) && result.value_str[0] == '@';
				int node_opr = -1;

				if (!stricmp(result.type_str, "node")) {
					// Determine the referenced operator's return type
					char *endptr;
					long node_idx = strtol(result.value_str, &endptr, 10);
					if (*endptr == '\0' && endptr != result.value_str && node_idx >= 0 && node_idx < Num_sexp_nodes) {
						int ref_node = (int)node_idx;
						// If this is a list wrapper, the operator is the first child
						if (Sexp_nodes[ref_node].subtype == SEXP_ATOM_LIST && Sexp_nodes[ref_node].first >= 0)
							ref_node = Sexp_nodes[ref_node].first;
						int ref_op = get_operator_index(ref_node);
						if (ref_op >= 0)
							node_opr = query_operator_return_type(ref_op);
					}
				}

				if (!is_arg_type_compatible(expected_opf, result.type_str, is_variable, node_opr)) {
					sink.set_error("Argument %d: type '%s' is not compatible with expected type '%s' for operator '%s'",
						i, result.type_str, opf_to_string(expected_opf), op_name);
					break;
				}
			}

			// creating this argument succeeded
			next = result.arg_node;
		}

		// there might have been an error in the argument loop
		if (sink.has_error()) {
			// Before freeing, sever first pointers on node-type wrappers so
			// free_sexp2 doesn't recurse into referenced existing trees
			int walk = (result.arg_node != -1) ? result.arg_node : next;
			while (walk != -1) {
				if (Sexp_nodes[walk].subtype == SEXP_ATOM_LIST
					&& Sexp_nodes[walk].first != Locked_sexp_true
					&& Sexp_nodes[walk].first != Locked_sexp_false
				)
					Sexp_nodes[walk].first = -1;
				walk = Sexp_nodes[walk].rest;
			}

			// Free the argument chain and operator
			if (result.arg_node != -1)			// frees the arg and anything linked after it in the chain
				free_sexp2(result.arg_node);
			else if (next != -1)				// arg was never created, so just free the chain
				free_sexp2(next);
			if (!op_is_locked)
				free_sexp2(op_node);

			if (warnings) json_decref(warnings);
			return;
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
// update_sexp_node handler (run on main thread)
// ---------------------------------------------------------------------------

static void handle_update_sexp_node(json_t *input, McpToolRequest *req)
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

	if (n == Locked_sexp_true || n == Locked_sexp_false) {
		sink.set_error("A locked boolean operator cannot be updated directly.  Instead, update the list wrapper node that contains it,"
			" or attach a new boolean in its place.");
		return;
	}

	auto role = get_sexp_role(n);
	bool is_boolean_wrapper = false;

	if (!stricmp(role, "list_wrapper")) {
		// Special case: boolean wrappers can be toggled
		int first = Sexp_nodes[n].first;
		if (first == Locked_sexp_true || first == Locked_sexp_false) {
			is_boolean_wrapper = true;
		} else {
			sink.set_error("Updating a list_wrapper node is not supported");
			return;
		}
	}

	// Save original state for rollback
	char saved_text[TOKEN_LENGTH];
	int saved_type = Sexp_nodes[n].type;
	int saved_subtype = Sexp_nodes[n].subtype;
	int saved_first = Sexp_nodes[n].first;
	strcpy_s(saved_text, Sexp_nodes[n].text);

	if (!stricmp(role, "operator")) {
		// --- Operator update ---
		auto op_name = get_required_string(input, "operator_name", sink, true, TOKEN_LENGTH - 1);
		if (!op_name) return;

		int op_idx = get_operator_index(op_name);
		if (op_idx < 0) {
			sink.set_error("Unknown SEXP operator: '%s'", op_name);
			return;
		}

		strcpy_s(Sexp_nodes[n].text, op_name);
	} else {
		// --- Argument update (atom or boolean wrapper) ---
		auto type_str = get_required_string(input, "argument_type", sink, true);
		if (!type_str) return;
		if (!check_string_enum(type_str, sexp_mutable_arg_type_values, "argument_type", sink)) return;

		auto value_str = get_required_string(input, "argument_value", sink, true, TOKEN_LENGTH - 1);
		if (!value_str) return;

		if (!stricmp(type_str, "boolean")) {
			if (!is_boolean_wrapper) {
				sink.set_error("Cannot change a non-boolean argument to boolean");
				return;
			}

			if (!stricmp(value_str, "true"))
				Sexp_nodes[n].first = Locked_sexp_true;
			else if (!stricmp(value_str, "false"))
				Sexp_nodes[n].first = Locked_sexp_false;
			else {
				sink.set_error("Boolean value must be \"true\" or \"false\", got \"%s\"", value_str);
				return;
			}
		} else {
			// number or string
			if (is_boolean_wrapper) {
				sink.set_error("Cannot change a boolean argument to %s", type_str);
				return;
			}

			int new_subtype = !stricmp(type_str, "number") ? SEXP_ATOM_NUMBER : SEXP_ATOM_STRING;

			if (value_str[0] == '@') {
				const char *var_name = value_str + 1;
				int var_idx = get_index_sexp_variable_name(var_name);
				if (var_idx < 0) {
					sink.set_error("Unknown SEXP variable '%s'", var_name);
					return;
				}
				Sexp_nodes[n].type = SEXP_ATOM | SEXP_FLAG_VARIABLE;
				Sexp_nodes[n].subtype = new_subtype;
				strcpy_s(Sexp_nodes[n].text, Sexp_variables[var_idx].variable_name);
			} else {
				if (new_subtype == SEXP_ATOM_NUMBER && !can_construe_as_integer(value_str)) {
					sink.set_error("Value \"%s\" is not a valid number", value_str);
					return;
				}
				Sexp_nodes[n].type = SEXP_ATOM;
				Sexp_nodes[n].subtype = new_subtype;
				strcpy_s(Sexp_nodes[n].text, value_str);
			}
		}
	}

	// Syntax check for mission-attached formulas
	auto info = find_formula_root_and_type(n);
	if (info.attached) {
		int bad_node = -1;
		int syntax_result = check_sexp_syntax(info.root, static_cast<int>(info.opr_type), 1, &bad_node);
		if (syntax_result != SEXP_CHECK_NO_ERROR) {
			// Rollback
			strcpy_s(Sexp_nodes[n].text, saved_text);
			Sexp_nodes[n].type = saved_type;
			Sexp_nodes[n].subtype = saved_subtype;
			Sexp_nodes[n].first = saved_first;

			sink.set_error("Update would cause syntax error in formula root %d: %s (error code %d, bad node %d)",
				info.root, sexp_error_message(syntax_result), syntax_result, bad_node);
			return;
		}

		// no rollback: it's a modification
		mark_modified("MCP: replace SEXP formula for %s", info.to_string().c_str());
	}

	mcp_sexp_forest_mark_dirty({ info.root });

	req->result_json = make_json_tool_result(build_sexp_node_json(n));
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
		json_object_set_new(obj, "variable_type", json_string("number"));
	else
		json_object_set_new(obj, "variable_type", json_string("string"));

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

	auto default_value = get_required_string(input, "default_value", sink, false, TOKEN_LENGTH - 1);
	if (!default_value) return;

	auto type_str = get_required_string(input, "variable_type", sink, true);
	if (!type_str) return;
	if (!check_string_enum(type_str, sexp_var_type_values, "variable_type", sink)) return;

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
		if (!parse_flags_array(*flags_arr, sexp_var_flag_entries, sexp_var_flag_entries_count, "flags", flag_bits, sink))
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
	auto new_name = get_optional_string(input, "new_name", sink);
	auto default_value = get_optional_string(input, "default_value", sink, TOKEN_LENGTH - 1);
	auto type_str = get_optional_string(input, "variable_type", sink);
	if (sink.has_error()) return;

	if (new_name) {
		if (!validate_sexp_variable_name(new_name, sink, idx)) return;
	}

	int type_bits;
	if (type_str) {
		if (!check_string_enum(type_str, sexp_var_type_values, "variable_type", sink)) return;
		type_bits = !stricmp(type_str, "number") ? SEXP_VARIABLE_NUMBER : SEXP_VARIABLE_STRING;
	} else {
		type_bits = old_type & (SEXP_VARIABLE_NUMBER | SEXP_VARIABLE_STRING);
	}

	int flag_bits;
	auto flags_arr = get_optional_string_array(input, "flags", sink);
	if (!flags_arr.has_value() && json_object_get(input, "flags"))
		return;  // non-string element error already reported by sink
	if (flags_arr.has_value()) {
		if (!parse_flags_array(*flags_arr, sexp_var_flag_entries, sexp_var_flag_entries_count, "flags", flag_bits, sink))
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
// Registration
// ---------------------------------------------------------------------------

void mcp_register_sexp_tools(json_t *tools)
{
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

	// get_sexp_formula_info
	{
		json_t *props = json_object();
		add_integer_prop(props, "node", "Node index of the SEXP node to query");
		json_t *req = json_array();
		json_array_append_new(req, json_string("node"));
		register_tool(tools, "get_sexp_formula_info",
			"Get information about which mission entity (if any) owns the formula "
			"tree containing the given SEXP node. Returns the tree root, whether it "
			"is attached to a mission entity, and if so, the entity type, identifier, "
			"and expected return type.",
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
			"of node descriptors with kind, value, value_type, role "
			"(operator/argument/list_wrapper), child/sibling indices, and "
			"walk_first/walk_rest indices into the array for easy traversal. "
			"In FreeSpace SEXP trees, the top-level operator is a bare atom node, while "
			"operators deeper in the tree are wrapped in list nodes.",
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
			"Parse SEXP text into a node tree. Returns the index of the root node. "
			"In FreeSpace SEXP trees, the top-level operator is a bare atom node, while "
			"operators deeper in the tree are wrapped in list nodes. The caller is "
			"responsible for attaching the tree to a mission entity's formula, attaching the tree to "
			"another tree, or deleting the tree. Also returns the round-tripped "
			"text, any parsing errors encountered, and the first (if any) syntax error.",
			props, req);
	}

	// detach_sexp_node
	{
		json_t *props = json_object();
		add_integer_prop(props, "target_node",
			"Node index of the SEXP node to detach. "
			"Mutually exclusive with target_entity_type. If the target is an operator "
			"inside a list wrapper, the wrapper is automatically targeted instead. "
			"Shared locked singletons (true/false) cannot be targeted directly; use "
			"target_argument_index with the parent operator instead.");
		add_integer_prop(props, "target_argument_index",
			"0-based index into the argument list of the operator at target_node. "
			"When provided, target_node must be an operator (or its list wrapper) and "
			"the detach operates on that argument instead of the node itself. Required "
			"when target_node resolves to a shared locked singleton (true/false).");
		add_string_enum_prop(props, "target_entity_type",
			"Entity type whose formula to detach and replace with a default. "
			"Mutually exclusive with target_node. Requires target_entity_id.",
			formula_entity_type_values);
		add_string_prop(props, "target_entity_id",
			"Entity name or index. Required when target_entity_type is set. "
			"For ships/wings: the ship or wing name. For events/goals: the event or goal "
			"name. For cutscenes/fiction viewer/briefing/debriefing stages: the 0-based stage index.");
		add_string_enum_prop(props, "entity_tag",
			"Disambiguation tag for entities that have multiple formula slots. "
			"For ships/wings: 'arrival_cue' (default) or 'departure_cue'. "
			"For briefing/debriefing stages: 'team_1' (default) or 'team_2'.",
			formula_entity_tag_values);
		add_bool_prop(props, "shrink",
			"If true, remove the node and shift subsequent siblings up by one "
			"position instead of inserting a " PLACEHOLDER_STRING ". Defaults to false.");
		add_bool_prop(props, "delete",
			"If true, free the detached node and its subtree. If false (the default), "
			"the detached node is preserved and returned in the response.");
		register_tool(tools, "detach_sexp_node",
			"Detach a SEXP node from its tree. The target can be specified by node index "
			"(target_node), by node index plus argument position (target_node + "
			"target_argument_index), or by entity coordinates (target_entity_type + "
			"target_entity_id). If the target is an operator inside a list wrapper, the "
			"wrapper is automatically detached as well. If the node is the root of a "
			"mission entity's formula (or entity mode is used), it is replaced with an "
			"appropriate default (e.g. do-nothing for action formulas, true for boolean "
			"formulas). If the node is embedded within a tree, it is replaced with "
			PLACEHOLDER_STRING ", unless 'shrink' is true, in which case subsequent "
			"siblings shift up by one position. By default, the detached node is "
			"preserved and returned; set 'delete' to true to free it. The response "
			"includes detached_node (int index), detached_node_data (full node object when "
			"the node was not deleted), unwrapped (bool, true if an enclosing list wrapper "
			"was automatically removed), replacement_node (int or null), and replacement_node_data "
			"(full node object when a replacement was inserted). For mission-attached "
			"trees, a syntax check is performed after modification; if the check fails, "
			"the operation is rolled back. Shared locked singleton nodes (true/false) cannot "
			"be targeted directly; use target_argument_index with the parent operator.",
			props, nullptr);
	}

	// attach_sexp_node
	{
		json_t *props = json_object();
		add_integer_prop(props, "source_node",
			"Node index of the free-standing root to attach. Must have parent == -1 "
			"and not be attached to any mission entity.");
		add_integer_prop(props, "target_node",
			"Node index of an existing tree node to position the source relative to. "
			"Mutually exclusive with target_entity_type. If the target is an operator "
			"inside a list wrapper, the wrapper is automatically targeted instead. "
			"Shared locked singletons (true/false) cannot be targeted directly; use "
			"target_argument_index with the parent operator instead.");
		add_integer_prop(props, "target_argument_index",
			"0-based index into the argument list of the operator at target_node. "
			"When provided, target_node must be an operator (or its list wrapper) and "
			"the attach operates on that argument instead of the node itself. Required "
			"when target_node resolves to a shared locked singleton (true/false).");
		add_string_enum_prop(props, "target_entity_type",
			"Entity type whose formula the source will become. Mutually exclusive with "
			"target_node. Requires target_entity_id.",
			formula_entity_type_values);
		add_string_prop(props, "target_entity_id",
			"Entity name or index. Required when target_entity_type is set. "
			"For ships/wings: the ship or wing name. For events/goals: the event or goal "
			"name. For cutscenes/fiction viewer/briefing/debriefing stages: the 0-based stage index.");
		add_string_enum_prop(props, "entity_tag",
			"Disambiguation tag for entities that have multiple formula slots. "
			"For ships/wings: 'arrival_cue' (default) or 'departure_cue'. "
			"For briefing/debriefing stages: 'team_1' (default) or 'team_2'.",
			formula_entity_tag_values);
		add_string_enum_prop(props, "position",
			"How to position the source relative to the target node. "
			"'replace' (default) replaces the target; 'before' inserts before the target; "
			"'after' inserts after the target. Only used with target_node.",
			attach_position_values);
		add_bool_prop(props, "delete_displaced",
			"If true, free the displaced subtree when replacing. If false (the default), "
			"the displaced subtree is preserved as a free-standing root and returned in "
			"the response. Only meaningful for replace mode and entity mode.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("source_node"));
		register_tool(tools, "attach_sexp_node",
			"Attach a free-standing SEXP subtree to a position in another tree or as a "
			"mission entity's formula. The source must be a free-standing root (parent == -1, "
			"not attached to any entity); use detach_sexp_node first if needed. "
			"The target can be specified by node index (target_node), by node index plus "
			"argument position (target_node + target_argument_index), or by entity "
			"coordinates (target_entity_type + target_entity_id). "
			"In node-relative mode (target_node), position can be 'replace' (default, swaps "
			"the target with the source), 'before' (inserts source before target), or 'after' "
			"(inserts source after target). In entity mode (target_entity_type + target_entity_id), "
			"the source replaces the entity's current formula. For mission-attached trees, a "
			"syntax check is performed after modification; if the check fails, the operation "
			"is rolled back. Shared locked singleton nodes (true/false) cannot be targeted "
			"directly; use target_argument_index with the parent operator. The response "
			"includes source_node (int), source_node_data (full node object), position (string), "
			"displaced_node (int or null), displaced_node_data (full node object when preserved), "
			"deleted_displaced (bool), and freed_count (int).",
			props, req);
	}

	// create_sexp_node
	{
		json_t *props = json_object();
		add_string_enum_prop(props, "role",
			"Role of the node to create. 'operator' creates an operator node "
			"(requires the 'operator_name' parameter). 'argument' creates a standalone "
			"argument node (requires 'argument_type' and 'argument_value' parameters). "
			"The 'list_wrapper' role is not supported.",
			sexp_role_values);
		add_string_prop(props, "operator_name",
			"Name of the SEXP operator (e.g. \"when\", \"is-destroyed-delay\"). "
			"Required when role is 'operator'.");

		json_t *arg_props = json_object();
		add_string_enum_prop(arg_props, "argument_type",
			"Argument type",
			sexp_arg_type_values);
		add_string_prop(arg_props, "argument_value",
			"Argument value. For number/string: literal value (prefix with @ "
			"for SEXP variable). For boolean: \"true\" or \"false\". "
			"For node: a node index, or \"-1\" as a placeholder. "
			"A node value of -1 or a string value of " PLACEHOLDER_STRING
			" bypasses type checking, serves as a placeholder, and can be "
			"used in any argument position as an empty slot to fill later.");
		json_t *arg_req = json_array();
		json_array_append_new(arg_req, json_string("argument_type"));
		json_array_append_new(arg_req, json_string("argument_value"));
		add_object_array_prop(props, "operator_arguments",
			"List of arguments for the operator. Each argument has an 'argument_type' "
			"(number, string, boolean, node) and an 'argument_value'. "
			"Only used when role is 'operator'.",
			arg_props, arg_req);

		add_string_enum_prop(props, "argument_type",
			"Argument type for standalone argument creation. "
			"Required when role is 'argument'.",
			sexp_mutable_arg_type_values);
		add_string_prop(props, "argument_value",
			"Argument value for standalone argument creation. "
			"For number/string: literal value (prefix with @ for SEXP variable). "
			"For boolean: \"true\" or \"false\". "
			"Required when role is 'argument'.");

		json_t *req = json_array();
		json_array_append_new(req, json_string("role"));
		register_tool(tools, "create_sexp_node",
			"Create a SEXP node. When role is 'operator', creates an operator node "
			"with optional arguments, suitable for assigning as a mission entity's "
			"formula. When role is 'argument', creates a standalone argument node "
			"(number, string, or boolean). Does not enforce argument count or check syntax.",
			props, req);
	}

	// update_sexp_node
	{
		json_t *props = json_object();
		add_integer_prop(props, "node", "Node index of the SEXP node to update");
		add_string_prop(props, "operator_name",
			"New operator name. Required when updating an operator node.");
		add_string_enum_prop(props, "argument_type",
			"New argument type. Required when updating an argument node. "
			"Boolean is only valid for boolean wrapper nodes.",
			sexp_mutable_arg_type_values);
		add_string_prop(props, "argument_value",
			"New argument value. For number/string: literal value (prefix with @ "
			"for SEXP variable). For boolean: \"true\" or \"false\". "
			"Required when updating an argument node.");

		json_t *req = json_array();
		json_array_append_new(req, json_string("node"));
		register_tool(tools, "update_sexp_node",
			"Update a SEXP node in place. Operators can be changed to other operators; "
			"arguments can be changed to other argument types (number/string) or between "
			"literal and variable. Boolean arguments can be toggled between true and false. "
			"Node reference wrappers and non-boolean list wrappers cannot be updated with "
			"this tool. If the node belongs to a mission formula and the change would cause "
			"a syntax error, the edit is rolled back.",
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
		add_string_enum_prop(props, "variable_type",
			"Data type of the variable",
			sexp_var_type_values);
		add_string_array_prop(props, "flags",
			"Persistence and network flags. save_on_mission_progress and save_on_mission_close "
			"are mutually exclusive.",
			flag_names);
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("default_value"));
		json_array_append_new(req, json_string("variable_type"));
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
		add_string_enum_prop(props, "variable_type",
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
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_sexp_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "sexp_to_text") == 0) {
		handle_sexp_to_text(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "get_sexp_node") == 0) {
		handle_get_sexp_node(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "get_sexp_formula_info") == 0) {
		handle_get_sexp_formula_info(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "walk_sexp_tree") == 0) {
		handle_walk_sexp_tree(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "text_to_sexp") == 0) {
		handle_text_to_sexp(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "detach_sexp_node") == 0) {
		handle_detach_sexp_node(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "attach_sexp_node") == 0) {
		handle_attach_sexp_node(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "create_sexp_node") == 0) {
		handle_create_sexp_node(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "update_sexp_node") == 0) {
		handle_update_sexp_node(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "list_sexp_variables") == 0) {
		handle_list_sexp_variables(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "get_sexp_variable") == 0) {
		handle_get_sexp_variable(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "create_sexp_variable") == 0) {
		handle_create_sexp_variable(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "update_sexp_variable") == 0) {
		handle_update_sexp_variable(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "delete_sexp_variable") == 0) {
		handle_delete_sexp_variable(input_json, req);
		return true;
	}
	return false;
}
