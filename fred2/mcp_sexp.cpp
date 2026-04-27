#include "stdafx.h"
#include "mcp_sexp.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "mcp_app.h"
#include "mcp_mission_tools.h"
#include "mcp_sexp_forest.h"
#include "mcp_reference_tools.h"
#include "mcp_utils.h"
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
			opr_to_name(actual_return_type),
			opr_to_name(expected_return_type));
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
		if (idx < 1 || idx > (int)The_mission.cutscenes.size()) {
			sink.set_error("Cutscene index %d is out of range (1..%d)", idx, (int)The_mission.cutscenes.size());
			return info;
		}
		info.attached_id = idx;
		out_current_root = The_mission.cutscenes[idx - 1].formula;
	} else if (!stricmp(entity_type, "fiction_viewer_stage")) {
		int idx = atoi(entity_id);
		if (idx < 1 || idx > (int)Fiction_viewer_stages.size()) {
			sink.set_error("Fiction viewer stage index %d is out of range (1..%d)", idx, (int)Fiction_viewer_stages.size());
			return info;
		}
		info.attached_id = idx;
		out_current_root = Fiction_viewer_stages[idx - 1].formula;
	} else if (!stricmp(entity_type, "briefing_stage")) {
		int idx = atoi(entity_id);
		int t = (tag == entity_specific_tag::TEAM_2) ? 1 : 0;
		if (idx < 1 || idx > Briefings[t].num_stages) {
			sink.set_error("Briefing stage index %d is out of range for team %d (1..%d)", idx, t + 1, Briefings[t].num_stages);
			return info;
		}
		info.attached_id = idx;
		info.attached_tag = tag;
		out_current_root = Briefings[t].stages[idx - 1].formula;
	} else if (!stricmp(entity_type, "debriefing_stage")) {
		int idx = atoi(entity_id);
		int t = (tag == entity_specific_tag::TEAM_2) ? 1 : 0;
		if (idx < 1 || idx > Debriefings[t].num_stages) {
			sink.set_error("Debriefing stage index %d is out of range for team %d (1..%d)", idx, t + 1, Debriefings[t].num_stages);
			return info;
		}
		info.attached_id = idx;
		info.attached_tag = tag;
		out_current_root = Debriefings[t].stages[idx - 1].formula;
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

	// Mission cutscenes (OPR_BOOL) — attached_id is 1-based for client consumption
	for (int i = 0; i < (int)The_mission.cutscenes.size(); i++) {
		if (The_mission.cutscenes[i].formula == root)
			return { root, true, OPR_BOOL, "cutscene", i + 1, entity_specific_tag::NONE };
	}

	// Fiction viewer stages (OPR_BOOL)
	for (int i = 0; i < (int)Fiction_viewer_stages.size(); i++) {
		if (Fiction_viewer_stages[i].formula == root)
			return { root, true, OPR_BOOL, "fiction_viewer_stage", i + 1, entity_specific_tag::NONE };
	}

	// Briefing stages (OPR_BOOL)
	for (int t = 0; t < MAX_TVT_TEAMS; t++) {
		for (int s = 0; s < Briefings[t].num_stages; s++) {
			if (Briefings[t].stages[s].formula == root)
				return { root, true, OPR_BOOL, "briefing_stage", s + 1, (t == 0) ? entity_specific_tag::TEAM_1 : entity_specific_tag::TEAM_2 };
		}
	}

	// Debriefing stages (OPR_BOOL)
	for (int t = 0; t < MAX_TVT_TEAMS; t++) {
		for (int s = 0; s < Debriefings[t].num_stages; s++) {
			if (Debriefings[t].stages[s].formula == root)
				return { root, true, OPR_BOOL, "debriefing_stage", s + 1, (t == 0) ? entity_specific_tag::TEAM_1 : entity_specific_tag::TEAM_2 };
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

	// Stage indices are stored 1-based in attached_id; subtract 1 for array access.
	if (!stricmp(type, "cutscene")) {
		The_mission.cutscenes[index - 1].formula = new_root;
	} else if (!stricmp(type, "fiction_viewer_stage")) {
		Fiction_viewer_stages[index - 1].formula = new_root;
	} else if (!stricmp(type, "briefing_stage")) {
		int t = (info.attached_tag == entity_specific_tag::TEAM_1) ? 0 : 1;
		Briefings[t].stages[index - 1].formula = new_root;
	} else if (!stricmp(type, "debriefing_stage")) {
		int t = (info.attached_tag == entity_specific_tag::TEAM_1) ? 0 : 1;
		Debriefings[t].stages[index - 1].formula = new_root;
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
			json_string(opr_to_name(info.opr_type)));
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

	SCP_string text;
	convert_sexp_to_string(text, n, SEXP_SAVE_MODE);

	json_t *result = make_tool_result(text.c_str());
	json_t *sc = json_object();
	json_object_set_new(sc, "text", json_string(text.c_str()));
	json_object_set_new(result, "structuredContent", sc);

	req->result_json = result;
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

// Schema-enum strings derived from get_sexp_value_type().  Used by
// find_sexp_text (the only tool today that filters on value_type) and
// kept in lockstep with that function — if a new value_type case is
// added, the schema enum here must be extended too.
static const SCP_vector<const char *> sexp_value_type_values = {
	"operator", "numeric_literal", "numeric_variable",
	"string_literal", "string_variable",
	"container_name", "container_data"
};

// Roles searchable by find_sexp_text.  Excludes "list_wrapper" because
// list-wrapper nodes are structural — they hold no user-meaningful text
// — and find_sexp_text skips them unconditionally.
static const SCP_vector<const char *> sexp_searchable_role_values = { "operator", "argument" };

#define MAX_SEXP_WALK_ENTRIES 500

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
		json_object_set_new(obj, "return_type", json_string(opr_to_name(ret)));
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

static void collect_walk_entries(int n, SCP_vector<walk_entry> &entries, int depth, int max_depth = -1, int max_entries = -1)
{
	if (n < 0 || n >= Num_sexp_nodes)
		return;
	if (Sexp_nodes[n].type == SEXP_NOT_USED)
		return;
	if (max_depth >= 0 && depth > max_depth)
		return;
	if (max_entries >= 0 && entries.size() >= i2sz(max_entries))
		return;

	entries.push_back({n, depth});
	collect_walk_entries(Sexp_nodes[n].first, entries, depth + 1, max_depth, max_entries);
	collect_walk_entries(Sexp_nodes[n].rest, entries, depth, max_depth, max_entries);
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
	collect_walk_entries(n, entries, 0, max_depth, MAX_SEXP_WALK_ENTRIES);

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
// SEXP text search
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> find_match_mode_values = { "substring", "exact", "fuzzy" };

static bool node_text_matches(int n, const char *needle, bool exact)
{
	const char *hay = Sexp_nodes[n].text;
	if (exact)
		return stricmp(hay, needle) == 0;
	return stristr(hay, needle) != nullptr;
}

static void handle_find_sexp_text(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	auto needle = get_required_string(input, "text", sink, true);
	if (!needle) return;

	// The '@' prefix is a SEXP display convention for variable references
	// (sexp_to_text emits "@var", text_to_sexp accepts it).  Stored node text
	// is bare, so a naive client passing "@var" would otherwise find nothing.
	// Strip a single leading '@', if it exists.
	if (needle[0] == '@') needle++;
	if (!needle[0]) {
		sink.set_error("Required parameter must not be empty after stripping leading '@': text");
		return;
	}

	const char *match_mode_str = get_optional_string(input, "match_mode", sink);
	if (sink.has_error()) return;
	bool exact = false;
	bool fuzzy = false;
	if (match_mode_str) {
		if (!check_string_enum(match_mode_str, find_match_mode_values, "match_mode", sink))
			return;
		exact = (stricmp(match_mode_str, "exact") == 0);
		fuzzy = (stricmp(match_mode_str, "fuzzy") == 0);
	}

	const char *role_filter = get_optional_string(input, "role", sink);
	if (sink.has_error()) return;
	if (role_filter && !check_string_enum(role_filter, sexp_searchable_role_values, "role", sink))
		return;

	const char *value_type_filter = get_optional_string(input, "value_type", sink);
	if (sink.has_error()) return;
	if (value_type_filter && !check_string_enum(value_type_filter, sexp_value_type_values, "value_type", sink))
		return;

	auto root_node_opt = get_optional_integer(input, "root_node", sink);
	if (sink.has_error()) return;

	// Build the candidate list: either every live node (mission-wide) or the
	// subtree rooted at the supplied node.
	SCP_vector<walk_entry> candidates;
	if (root_node_opt.has_value()) {
		int root = *root_node_opt;
		if (!check_int_range(root, 0, Num_sexp_nodes - 1, "root_node", sink)) return;
		if (Sexp_nodes[root].type == SEXP_NOT_USED) {
			sink.set_error("root_node %d is not in use", root);
			return;
		}
		collect_walk_entries(root, candidates, 0);
	} else {
		candidates.reserve(Num_sexp_nodes);
		for (int i = 0; i < Num_sexp_nodes; i++) {
			if (Sexp_nodes[i].type != SEXP_NOT_USED)
				candidates.push_back({ i, -1 });
		}
	}

	// list_wrapper nodes are structural — their text is not user-facing —
	// and the role/value_type filters apply identically to all match modes.
	auto passes_filters = [&](int n) -> bool {
		if (SEXP_NODE_TYPE(n) == SEXP_LIST)
			return false;
		if (role_filter) {
			const char *role = get_sexp_role(n);
			if (!role || strcmp(role, role_filter) != 0)
				return false;
		}
		if (value_type_filter) {
			const char *vt = get_sexp_value_type(n);
			if (!vt || strcmp(vt, value_type_filter) != 0)
				return false;
		}
		return true;
	};

	bool truncated = false;
	SCP_vector<int> result_nodes;

	if (fuzzy) {
		// Fuzzy must score every candidate before truncating, since the best
		// matches may sit at the tail of the candidate list.  Compute
		// max_text_length over post-filter candidates so the cost threshold
		// scales with the actual data (matches the convention in
		// mcp_reference_tools); fall back to TOKEN_LENGTH-1 if the candidate
		// set is empty or contains only zero-length text.
		SCP_string needle_str(needle);
		size_t max_text_length = 0;
		for (const auto &c : candidates) {
			if (!passes_filters(c.node)) continue;
			size_t len = strlen(Sexp_nodes[c.node].text);
			if (len > max_text_length) max_text_length = len;
		}
		if (max_text_length == 0)
			max_text_length = TOKEN_LENGTH - 1;

		auto fuzzy_matches = fuzzy_search_and_sort(candidates.size(), needle,
			[&](size_t i) -> size_t {
				int n = candidates[i].node;
				if (!passes_filters(n)) return SCP_string::npos;
				return fuzzy_match_cost(Sexp_nodes[n].text, needle_str, max_text_length);
			});

		if (fuzzy_matches.size() > MAX_SEXP_WALK_ENTRIES) {
			fuzzy_matches.resize(MAX_SEXP_WALK_ENTRIES);
			truncated = true;
		}
		result_nodes.reserve(fuzzy_matches.size());
		for (const auto &m : fuzzy_matches)
			result_nodes.push_back(candidates[m.first].node);
	} else {
		// Substring/exact preserve traversal order, so we can early-break at
		// the cap without losing any meaningful results.
		for (const auto &c : candidates) {
			int n = c.node;
			if (!passes_filters(n)) continue;
			if (!node_text_matches(n, needle, exact)) continue;
			if (result_nodes.size() >= MAX_SEXP_WALK_ENTRIES) {
				truncated = true;
				break;
			}
			result_nodes.push_back(n);
		}
	}

	json_t *arr = json_array();
	for (int n : result_nodes)
		json_array_append_new(arr, build_sexp_node_json(n));

	json_t *result = json_object();
	json_object_set_new(result, "nodes", arr);
	if (truncated)
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

// Set the predecessor link to point at `node`.  pred_list is the wrapper whose
// .first should reference `node`; pred_ante is the sibling whose .rest should
// reference `node`.  Exactly one of the two should be >= 0 (matching how
// find_sexp_list / find_sexp_antecedent identify a node's predecessor).  This
// is the inverse of splice_replace_node and is also handy for rolling back a
// predecessor update.
static void splice_set_predecessor(int pred_list, int pred_ante, int node)
{
	if (pred_list >= 0)
		Sexp_nodes[pred_list].first = node;
	else if (pred_ante >= 0)
		Sexp_nodes[pred_ante].rest = node;
}

// Wraps n in a list wrapper.  Returns the wrapper node.
static int wrap_node(int n, int rest = -1)
{
	int wrapper_n = alloc_sexp("", SEXP_LIST, SEXP_ATOM_LIST, n, rest);

	// Note that alloc_sexp has already set the parents on n's rest chain,
	// as well as the parent of the wrapped node (unless the wrapped node
	// is a singleton)

	return wrapper_n;
}

// Unwraps the node enclosed in wrapper_n, and returns it.
static int unwrap_node(int wrapper_n, bool free_wrapper)
{
	int n = Sexp_nodes[wrapper_n].first;

	// Locked singletons' parents are already -1, but we should be
	// in the habit of not modifying any of their fields
	if (n != Locked_sexp_true && n != Locked_sexp_false)
		Sexp_nodes[n].parent = -1;

	// Set the parents on n's rest chain to -1 to match the parser's convention for
	// arg chains directly under a top-level operator atom (no wrapper).
	for (int sibling = Sexp_nodes[n].rest; sibling != -1; sibling = Sexp_nodes[sibling].rest)
		Sexp_nodes[sibling].parent = -1;

	if (free_wrapper)
		free_one_sexp(wrapper_n);
	else
		Sexp_nodes[wrapper_n].first = -1;

	return n;
}

// Make `node` a free-standing root, separating it from any supertree.  If
// `wrapper_or_node` differs from `node`, it is the list wrapper currently
// enclosing `node` and gets unwrapped (and freed) in the process; otherwise
// `node`'s parent/rest fields are cleared directly.  Pass node twice when
// there is no wrapper.
static void make_free_standing(int node, int wrapper_or_node)
{
	if (node != wrapper_or_node) {
		unwrap_node(wrapper_or_node, true);
	} else {
		Sexp_nodes[node].rest = -1;
		Sexp_nodes[node].parent = -1;
	}
}

// Normalize a subtree that just became free-standing.  If its root is a LIST
// wrapper, unwrap it so the new root is a bare operator atom (or the data atom
// inside) -- matching the parser's convention that top-level trees are not
// wrapped in a redundant list.  Updates `node` to the (possibly new) root
// index and increments `freed_count` when an unwrap occurs.  Returns true if
// an unwrap was performed, false otherwise.
static bool normalize_free_standing_root(int &node, int &freed_count)
{
	if (node < 0 || node == Locked_sexp_true || node == Locked_sexp_false)
		return false;
	if (SEXP_NODE_TYPE(node) != SEXP_LIST)
		return false;
	if (Sexp_nodes[node].first < 0)
		return false;	// defensive: empty wrapper shouldn't exist
	node = unwrap_node(node, true);
	freed_count++;
	return true;
}

// If node is an operator atom that is the first child of a list wrapper,
// return the wrapper index; otherwise return node unchanged.
static int retarget_to_list_wrapper(int node)
{
	// Locked singletons may or may not be wrapped, but since they aren't
	// unique, it's not possible to determine which wrapper we may need
	if (node == Locked_sexp_true || node == Locked_sexp_false)
		return node;

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
// SEXP reference resolution
// ---------------------------------------------------------------------------

struct GeneralSEXPReference {
	enum class Mode { Entity, Node };
	Mode mode;
	int node = -1;
	int original_node = -1;
	FormulaRootInfo entity_info = {};
	int entity_current_root = -1;
};

// Construct a Mode::Node GeneralSEXPReference pointing at the given node.
static GeneralSEXPReference make_node_ref(int node)
{
	GeneralSEXPReference ref;
	ref.mode = GeneralSEXPReference::Mode::Node;
	ref.node = node;
	ref.original_node = node;
	return ref;
}

static bool parse_general_sexp_reference(json_t *input, const char *field_prefix, GeneralSEXPReference &out, McpErrorSink &sink)
{
	SCP_string field_prefix_scp_str;
	if (field_prefix && field_prefix[0]) {
		field_prefix_scp_str += field_prefix;
	}
	auto node_scp_str = field_prefix_scp_str + "node";
	auto node_str = node_scp_str.c_str();
	auto arg_idx_scp_str = field_prefix_scp_str + "argument_index";
	auto arg_idx_str = arg_idx_scp_str.c_str();
	auto entity_type_scp_str = field_prefix_scp_str + "entity_type";
	auto entity_type_str = entity_type_scp_str.c_str();
	auto entity_id_scp_str = field_prefix_scp_str + "entity_id";
	auto entity_id_str = entity_id_scp_str.c_str();
	auto entity_tag_scp_str = field_prefix_scp_str + "entity_tag";
	auto entity_tag_str = entity_tag_scp_str.c_str();

	auto node_opt = get_optional_integer(input, node_str, sink);
	auto arg_idx_opt = get_optional_integer(input, arg_idx_str, sink);
	auto entity_type = get_optional_string(input, entity_type_str, sink);
	auto entity_id = get_optional_string(input, entity_id_str, sink);
	auto entity_tag = get_optional_string(input, entity_tag_str, sink);
	if (sink.has_error()) return false;

	bool have_node = node_opt.has_value();
	bool have_entity = (entity_type != nullptr);

	if (have_node == have_entity) {
		sink.set_error("Exactly one of '%s' or '%s' must be provided", node_str, entity_type_str);
		return false;
	}
	if (arg_idx_opt.has_value() && !have_node) {
		sink.set_error("'%s' can only be used with '%s', not with entity mode", arg_idx_str, node_str);
		return false;
	}
	if (entity_tag && have_node) {
		sink.set_error("'%s' can only be used with '%s', not with node mode", entity_tag_str, entity_type_str);
		return false;
	}
	if (entity_id && have_node) {
		sink.set_error("'%s' can only be used with '%s', not with node mode", entity_id_str, entity_type_str);
		return false;
	}

	if (have_entity) {
		// --- Entity mode ---
		if (!entity_id) {
			sink.set_error("'%s' is required when '%s' is provided", entity_id_str, entity_type_str);
			return false;
		}
		if (!check_string_enum(entity_type, formula_entity_type_values, entity_type_str, sink))
			return false;
		if (entity_tag) {
			if (!check_string_enum(entity_tag, formula_entity_tag_values, entity_tag_str, sink))
				return false;
		}
		out.mode = GeneralSEXPReference::Mode::Entity;
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
	int node = *node_opt;
	if (!check_int_range(node, 0, Num_sexp_nodes - 1, node_str, sink))
		return false;
	if (Sexp_nodes[node].type == SEXP_NOT_USED) {
		sink.set_error("%s %d is not in use", node_str, node);
		return false;
	}
	out.original_node = node;

	if (!arg_idx_opt.has_value()) {
		// No argument index — direct node addressing.
		// Reject locked singletons since they can't be uniquely addressed.
		if (node == Locked_sexp_true || node == Locked_sexp_false) {
			sink.set_error("%s %d is a shared singleton (locked %s) and cannot be "
				"uniquely addressed. Pass the parent operator as %s and set "
				"%s to the singleton's position in its argument list.",
				node_str, node, (node == Locked_sexp_true) ? "true" : "false", node_str, arg_idx_str);
			return false;
		}
		out.mode = GeneralSEXPReference::Mode::Node;
		out.node = node;
		return true;
	}

	// --- Node + argument index ---
	int arg_idx = *arg_idx_opt;
	if (arg_idx < 1) {
		sink.set_error("%s must be positive (got %d); argument positions are 1-based", arg_idx_str, arg_idx);
		return false;
	}

	// Retarget operator atom to its list wrapper if applicable
	int retargeted = retarget_to_list_wrapper(node);

	// Must be an operator atom or list wrapper
	int op_atom;
	if (SEXP_NODE_TYPE(retargeted) == SEXP_LIST) {
		op_atom = Sexp_nodes[retargeted].first;
		if (op_atom < 0 || Sexp_nodes[op_atom].subtype != SEXP_ATOM_OPERATOR) {
			sink.set_error("%s requires %s to be an operator or its list wrapper", arg_idx_str, node_str);
			return false;
		}
	} else if (SEXP_NODE_TYPE(retargeted) == SEXP_ATOM && Sexp_nodes[retargeted].subtype == SEXP_ATOM_OPERATOR) {
		op_atom = retargeted;
	} else {
		sink.set_error("%s requires %s to be an operator or its list wrapper", arg_idx_str, node_str);
		return false;
	}

	// Walk the argument chain from the operator atom.
	// arg_idx is 1-based; the first argument is at arg_idx == 1.
	int arg = Sexp_nodes[op_atom].rest;
	int count = 0;
	for (int i = 1; i < arg_idx; i++) {
		if (arg < 0)
			break;
		arg = Sexp_nodes[arg].rest;
		count++;
	}

	if (arg < 0) {
		sink.set_error("Operator '%s' has %d argument(s); %s=%d is out of range",
			Sexp_nodes[op_atom].text, count, arg_idx_str, arg_idx);
		return false;
	}

	out.mode = GeneralSEXPReference::Mode::Node;
	out.node = arg;
	return true;
}

// ---------------------------------------------------------------------------
// detach_sexp_node handler (run on main thread)
// ---------------------------------------------------------------------------

struct detach_result {
	int detached_node = -1;
	int freed_count = 0;
	int replacement = -1;
	int pred_list = -1;
	int pred_ante = -1;
};

static detach_result handle_detach_sexp_node(const GeneralSEXPReference &general_ref, bool shrink, bool do_delete, McpErrorSink &sink, bool suppress_mark_modified = false);

static json_t *build_detach_response_json(const detach_result &result, bool do_delete)
{
	json_t *result_json = json_object();
	json_object_set_new(result_json, "detached_node", json_integer(result.detached_node));
	if (!do_delete)
		json_object_set_new(result_json, "detached_node_data", build_sexp_node_json(result.detached_node));
	json_object_set_new(result_json, "deleted", (do_delete && result.freed_count > 0) ? json_true() : json_false());
	json_object_set_new(result_json, "freed_count", json_integer(result.freed_count));
	if (result.replacement >= 0) {
		json_object_set_new(result_json, "replacement_node", json_integer(result.replacement));
		json_object_set_new(result_json, "replacement_node_data", build_sexp_node_json(result.replacement));
	} else {
		json_object_set_new(result_json, "replacement_node", json_null());
	}
	return result_json;
}

static void handle_detach_sexp_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	GeneralSEXPReference general_ref;
	if (!parse_general_sexp_reference(input, nullptr, general_ref, sink))
		return;

	if (general_ref.mode == GeneralSEXPReference::Mode::Entity && general_ref.entity_current_root < 0) {
		sink.set_error("Entity (%s) has no formula to detach.", general_ref.entity_info.to_string().c_str());
		return;
	}

	auto shrink_opt = get_optional_bool(input, "shrink", sink);
	bool shrink = shrink_opt.has_value() && *shrink_opt;
	auto delete_opt = get_optional_bool(input, "delete", sink);
	bool do_delete = delete_opt.has_value() && *delete_opt;

	auto result = handle_detach_sexp_node(general_ref, shrink, do_delete, sink);
	if (sink.has_error()) return;

	req->result_json = make_json_tool_result(build_detach_response_json(result, do_delete));
	req->success = true;
}

static detach_result handle_detach_sexp_node(const GeneralSEXPReference &general_ref, bool shrink, bool do_delete, McpErrorSink &sink, bool suppress_mark_modified)
{
	int n = general_ref.node;
	int original_n = n;

	if (Sexp_nodes[n].type == SEXP_NOT_USED) {
		sink.set_error("Node %d is not in use", n);
		return {};
	}

	// If the client targeted an operator atom inside a list wrapper,
	// retarget to the wrapper so the entire sub-expression is detached.
	// Skip for entity mode which targets the formula root directly.
	if (general_ref.mode == GeneralSEXPReference::Mode::Node)
		n = retarget_to_list_wrapper(n);

	// Determine context: walk to tree root and check mission attachment
	FormulaRootInfo info = (general_ref.mode == GeneralSEXPReference::Mode::Entity) ? general_ref.entity_info : find_formula_root_and_type(n);
	bool is_root = (n == info.root);
	bool is_attached = info.attached;

	int replacement = -1;
	int freed_count = 0;

	// in case we need to reverse the detach after it was successful
	int pred_list = -1;
	int pred_ante = -1;

	if (is_root && is_attached) {
		// Case A: Root of a mission-attached formula -- replace with default
		if (info.opr_type == OPR_NULL) {
			replacement = parse_sexp_text("( do-nothing )", "detach_sexp_node");
			if (replacement < 0) {
				sink.set_error("Failed to create replacement formula");
				return {};
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
			free_sexp2(replacement);
			return {};
		}

		// Set the entity formula to the replacement
		set_formula(info, replacement);

		// Syntax check the replacement
		int bad_node = -1;
		int syntax_result = check_sexp_syntax(replacement, static_cast<int>(info.opr_type), 1, &bad_node);
		if (syntax_result != SEXP_CHECK_NO_ERROR) {
			// Rollback
			set_formula(info, n);
			free_sexp2(replacement);
			sink.set_error("Detachment would cause syntax error: %s (error code %d, bad node %d)",
				sexp_error_message(syntax_result), syntax_result, bad_node);
			return {};
		}
		// no rollback: it's a modification

		// Detach the old tree and optionally free it
		freed_count = release_subtree(n, do_delete);

		if (!suppress_mark_modified)
			mark_modified("MCP: replace SEXP formula for %s", info.to_string().c_str());

	} else if (is_root) {
		// Case B: Root of a free-standing tree
		freed_count = release_subtree(n, do_delete);

		// we may not have actually done anything
		if (!do_delete && n == original_n) {
			sink.set_error("Node %d is already the root of a detached tree; there is nothing to do. Pass delete=true to free it, or pass an inner node to detach a sub-tree.", n);
			return {};
		}

	} else {
		// Cases C & D: Embedded node -- splice a replacement into the link chain.
		int rest = Sexp_nodes[n].rest;

		// Locate the predecessor before splicing so we can reliably restore
		// it on rollback.  splice_replace_node can't find the old position if
		// the forward splice replaced n with -1 (shrink mode, last sibling).
		pred_list = find_sexp_list(n);                                      // wrapper whose .first == n
		pred_ante = (pred_list < 0) ? find_sexp_antecedent(n) : -1;         // sibling whose .rest == n

		if (shrink) {
			// Shrink mode: link the predecessor directly to the next sibling,
			// shifting subsequent arguments up by one position.
			replacement = -1;
			splice_replace_node(n, rest);
		} else {
			// Default mode: insert a placeholder to preserve argument positions.
			replacement = alloc_sexp(PLACEHOLDER_STRING, SEXP_ATOM, SEXP_ATOM_STRING, -1, rest);
			Sexp_nodes[replacement].parent = Sexp_nodes[n].parent;
			splice_replace_node(n, replacement);
		}

		// Syntax check for mission-attached trees (Case C).
		if (!check_attached_syntax_or_rollback(info, is_attached, [&]() {
				splice_set_predecessor(pred_list, pred_ante, n);
				if (replacement >= 0) {
					Sexp_nodes[replacement].rest = -1;	// don't walk into the live tree
					Sexp_nodes[replacement].parent = -1;
					free_sexp2(replacement);
				}
			}, "Detachment", sink))
			return {};
		if (is_attached && !suppress_mark_modified)
			mark_modified("MCP: detach SEXP node in %s", info.to_string().c_str());

		// Commit: detach and optionally free the subtree
		Sexp_nodes[n].rest = -1;	// don't walk into the live tree
		Sexp_nodes[n].parent = -1;
		if (do_delete)
			freed_count = free_sexp2(n);
	}

	if (is_root && is_attached)
		mcp_sexp_forest_mark_dirty({ replacement });	// case A: mark the new formula, not the old (now detached/freed) root
	else
		mcp_sexp_forest_mark_dirty({ info.root });		// cases B/C/D: mark the tree that was modified

	// If preserved, the detached node is the root of a new free-standing tree.
	// Normalize the root so it is not a redundant list wrapper (matching the
	// parser's convention that top-level trees are not wrapped).
	int new_root = n;
	if (!do_delete) {
		normalize_free_standing_root(new_root, freed_count);
		mcp_sexp_forest_mark_dirty({ new_root });
	}

	// Return response.  detached_node is the root of the resulting free-standing
	// tree after any normalization.  When do_delete is true the tree is gone,
	// so we echo the node the user passed in.
	return {
		do_delete ? original_n : new_root,
		freed_count,
		replacement,
		pred_list,
		pred_ante
	};
}

// ---------------------------------------------------------------------------
// attach_sexp_node handler (run on main thread)
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> attach_position_values = { "replace", "before", "after" };
enum class attach_position { INVALID, REPLACE, BEFORE, AFTER };

static attach_position parse_attach_position(const char *position_param, const char *position_str, McpErrorSink &sink)
{
	if (position_str) {
		if (!check_string_enum(position_str, attach_position_values, position_param, sink))
			return attach_position::INVALID;
		if (!stricmp(position_str, "replace")) return attach_position::REPLACE;
		else if (!stricmp(position_str, "before")) return attach_position::BEFORE;
		else if (!stricmp(position_str, "after")) return attach_position::AFTER;
		else {
			sink.set_error("internal error: '%s' value '%s' passed check_string_enum but is not handled", position_param, position_str);
			return attach_position::INVALID;
		}
	}
	return attach_position::REPLACE;
}

static const char *attach_position_to_string(attach_position position)
{
	return (position == attach_position::REPLACE) ? "replace"
		: (position == attach_position::BEFORE) ? "before"
		: (position == attach_position::AFTER) ? "after"
		: "invalid";
}

// Shared logic for replacing a mission entity's formula with a new source node.
// Validates source, sets the formula, runs a syntax check (rolling back on failure),
// handles the displaced subtree, marks modified, and marks the forest dirty.
// Returns true on success; on failure, populates sink and returns false.
static bool attach_as_entity_formula(
	int source, const FormulaRootInfo &info, int current_root,
	bool delete_displaced, int &displaced, int &freed_count,
	McpErrorSink &sink, bool suppress_mark_modified = false)
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
	if (!delete_displaced)
		normalize_free_standing_root(displaced, freed_count);

	if (!suppress_mark_modified)
		mark_modified("MCP: attach SEXP formula for %s", info.to_string().c_str());

	mcp_sexp_forest_mark_dirty({ source, displaced });
	return true;
}

struct attach_result {
	int displaced = -1;
	int freed_count = 0;
	int pred_list = -1;
	int pred_ante = -1;
};

static attach_result handle_attach_sexp_node(int source, const GeneralSEXPReference &general_target, attach_position position, bool delete_displaced, McpErrorSink &sink, bool suppress_mark_modified = false);

static json_t *build_attach_response_json(int source, const GeneralSEXPReference &general_target,
	attach_position position, bool delete_displaced, const attach_result &result)
{
	json_t *result_json = json_object();
	json_object_set_new(result_json, "source_node", json_integer(source));
	json_object_set_new(result_json, "source_node_data", build_sexp_node_json(source));
	const char *pos_str;
	if (general_target.mode == GeneralSEXPReference::Mode::Entity)
		pos_str = "entity_formula";
	else
		pos_str = attach_position_to_string(position);
	json_object_set_new(result_json, "position", json_string(pos_str));

	if (result.displaced >= 0) {
		json_object_set_new(result_json, "displaced_node", json_integer(result.displaced));
		if (!delete_displaced && result.displaced != Locked_sexp_true && result.displaced != Locked_sexp_false)
			json_object_set_new(result_json, "displaced_node_data", build_sexp_node_json(result.displaced));
	} else {
		json_object_set_new(result_json, "displaced_node", json_null());
	}

	json_object_set_new(result_json, "deleted_displaced", (delete_displaced && result.freed_count > 0) ? json_true() : json_false());
	json_object_set_new(result_json, "freed_count", json_integer(result.freed_count));
	return result_json;
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

	GeneralSEXPReference general_target;
	if (!parse_general_sexp_reference(input, "target_", general_target, sink))
		return;

	auto position_str = get_optional_string(input, "position", sink);
	auto delete_displaced_opt = get_optional_bool(input, "delete_displaced", sink);
	if (sink.has_error()) return;

	if (position_str && general_target.mode != GeneralSEXPReference::Mode::Node) {
		sink.set_error("'position' can only be used with 'target_node', not with entity mode");
		return;
	}

	bool delete_displaced = delete_displaced_opt.has_value() && *delete_displaced_opt;

	// Determine position enum
	attach_position position = parse_attach_position("position", position_str, sink);
	if (position == attach_position::INVALID) return;

	auto result = handle_attach_sexp_node(source, general_target, position, delete_displaced, sink);
	if (sink.has_error()) return;

	req->result_json = make_json_tool_result(build_attach_response_json(source, general_target, position, delete_displaced, result));
	req->success = true;
}

static attach_result handle_attach_sexp_node(int source, const GeneralSEXPReference &general_target, attach_position position, bool delete_displaced, McpErrorSink &sink, bool suppress_mark_modified)
{
	bool have_entity = (general_target.mode == GeneralSEXPReference::Mode::Entity);

	// --- Validate source ---
	if (Sexp_nodes[source].type == SEXP_NOT_USED) {
		sink.set_error("Source node %d is not in use", source);
		return {};
	}
	// Locked singletons (true/false) are shared nodes whose fields must not
	// be modified.  They are still valid attach sources: entity-formula mode
	// just stores the node index, and node-relative mode auto-wraps them in
	// a SEXP_LIST whose fields are modified instead.  Skip the root and
	// attached checks since singletons are intentionally shared.
	if (source != Locked_sexp_true && source != Locked_sexp_false) {
		FormulaRootInfo src_info = find_formula_root_and_type(source);
		if (src_info.root != source) {
			sink.set_error("Source node %d is not a free-standing root. Use detach_sexp_node first to detach it from its current tree.", source);
			return {};
		}
		if (src_info.attached) {
			sink.set_error("Source node %d is currently attached to %s. Use detach_sexp_node first to detach it.",
				source, src_info.attached_type);
			return {};
		}
	}

	int displaced = -1;
	int freed_count = 0;

	// in case we need to reverse the detach after it was successful
	int pred_list = -1;
	int pred_ante = -1;

	if (have_entity) {
		// ---------------------------------------------------------------
		// Case A': Entity formula mode
		// ---------------------------------------------------------------

		if (!attach_as_entity_formula(source, general_target.entity_info, general_target.entity_current_root,
				delete_displaced, displaced, freed_count, sink, suppress_mark_modified))
			return {};

	} else {
		// ---------------------------------------------------------------
		// Node-relative mode
		// ---------------------------------------------------------------
		int target = general_target.node;

		if (target == source) {
			sink.set_error("Source and target cannot be the same node (%d)", source);
			return {};
		}

		// Retarget: if target is an operator atom inside a list wrapper,
		// operate on the wrapper instead.  Skip when the resolver already
		// descended via target_argument_index.
		int original_target = general_target.original_node;
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
			return {};
		}

		if (is_root && !is_attached) {
			// Case B': free-standing root — nothing to attach to
			sink.set_error("Target node %d is a free-standing root; to attach source as a formula, "
				"use target_entity_type/target_entity_id. To replace content inside a tree, "
				"target an embedded node.", target);
			return {};
		}

		if (is_root && is_attached && position != attach_position::REPLACE) {
			// Case A' via target_node with insert mode — formula roots have no sibling chain
			sink.set_error("Cannot insert before/after a formula root. Use position='replace' to replace the formula.");
			return {};
		}

		// If the source is an operator atom that will be spliced into a rest
		// chain, wrap it in a SEXP_LIST node first.  check_sexp_syntax expects
		// list wrappers (or data atoms) in rest chains, and the splice would
		// overwrite source.rest, severing any argument chain.
		// Entity-formula mode and root-replace mode don't need this because
		// those paths store the operator atom directly as the formula root.
		int effective_source = source;
		bool need_wrap = (SEXP_NODE_TYPE(source) == SEXP_ATOM
			&& Sexp_nodes[source].subtype == SEXP_ATOM_OPERATOR
			&& !(is_root && is_attached));  // root-replace delegates to entity logic
		if (need_wrap) {
			effective_source = wrap_node(source);
		}

		if (position == attach_position::REPLACE) {
			// -------------------------------------------------------
			// Replace mode
			// -------------------------------------------------------

			if (is_root && is_attached) {
				// Replacing a formula root via target_node: delegate to entity logic.
				if (!attach_as_entity_formula(source, info, info.root, delete_displaced, displaced, freed_count, sink, suppress_mark_modified))
					return {};

			} else {
				// Embedded replace (Cases C'/D')
				pred_list = find_sexp_list(target);
				pred_ante = (pred_list < 0) ? find_sexp_antecedent(target) : -1;

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
						splice_set_predecessor(pred_list, pred_ante, target);
						make_free_standing(source, effective_source);
					}, "Attachment", sink))
					return {};
				if (is_attached && !suppress_mark_modified)
					mark_modified("MCP: attach SEXP node in %s", info.to_string().c_str());

				// Handle displaced subtree
				freed_count = release_subtree(displaced, delete_displaced);
				if (!delete_displaced)
					normalize_free_standing_root(displaced, freed_count);

				mcp_sexp_forest_mark_dirty({ info.root });
				mcp_sexp_forest_unmark_dirty({ source });
				if (!delete_displaced && displaced >= 0)
					mcp_sexp_forest_mark_dirty({ displaced });
			}

		} else if (position == attach_position::BEFORE) {
			// -------------------------------------------------------
			// Insert before
			// -------------------------------------------------------

			if (is_root) {
				sink.set_error("Cannot insert before a tree root; use position='replace', or target an embedded node.");
				return {};
			}

			pred_list = find_sexp_list(target);
			pred_ante = (pred_list < 0) ? find_sexp_antecedent(target) : -1;

			Sexp_nodes[effective_source].parent = Sexp_nodes[target].parent;
			Sexp_nodes[effective_source].rest = target;

			if (pred_list >= 0)
				Sexp_nodes[pred_list].first = effective_source;
			else if (pred_ante >= 0)
				Sexp_nodes[pred_ante].rest = effective_source;

			if (!check_attached_syntax_or_rollback(info, is_attached, [&]() {
					splice_set_predecessor(pred_list, pred_ante, target);
					make_free_standing(source, effective_source);
				}, "Insertion", sink))
				return {};
			if (is_attached && !suppress_mark_modified)
				mark_modified("MCP: insert SEXP node in %s", info.to_string().c_str());

			mcp_sexp_forest_mark_dirty({ info.root });
			mcp_sexp_forest_unmark_dirty({ source });

		} else {
			// -------------------------------------------------------
			// Insert after
			// -------------------------------------------------------
			if (is_root) {
				sink.set_error("Cannot insert after a tree root; use position='replace', or target an embedded node.");
				return {};
			}

			int old_target_rest = Sexp_nodes[target].rest;

			Sexp_nodes[effective_source].parent = Sexp_nodes[target].parent;
			Sexp_nodes[effective_source].rest = old_target_rest;
			Sexp_nodes[target].rest = effective_source;

			if (!check_attached_syntax_or_rollback(info, is_attached, [&]() {
					Sexp_nodes[target].rest = old_target_rest;
					make_free_standing(source, effective_source);
				}, "Insertion", sink))
				return {};
			if (is_attached && !suppress_mark_modified)
				mark_modified("MCP: insert SEXP node in %s", info.to_string().c_str());

			mcp_sexp_forest_mark_dirty({ info.root });
			mcp_sexp_forest_unmark_dirty({ source });
		}
	}

	return {
		displaced,
		freed_count,
		pred_list,
		pred_ante
	};
}

// ---------------------------------------------------------------------------
// move_sexp_node and swap_sexp_nodes handlers (run on main thread)
//
// Composed from handle_detach_sexp_node + handle_attach_sexp_node.  Atomicity
// is achieved via rollback_detach, which inverts a successful detach so that
// composed operations can be aborted cleanly when a later step fails.
// ---------------------------------------------------------------------------

// Construct a copy of orig with its addressed node set to new_node.  For
// entity-mode references, also updates entity_current_root so the result
// addresses the entity's current formula at new_node; entity_info, attached
// metadata, etc. are preserved.  For node-mode references, returns a fresh
// node ref (the original mode-specific fields don't apply).  Used wherever
// a composer needs to point an existing reference at a different slot --
// e.g. after a detach to re-address the placeholder or default that took
// the original's place.
static GeneralSEXPReference rebind_ref(
	const GeneralSEXPReference &orig, int new_node)
{
	if (orig.mode == GeneralSEXPReference::Mode::Entity) {
		GeneralSEXPReference dest = orig;
		dest.entity_current_root = new_node;
		dest.node = new_node;
		dest.original_node = new_node;
		return dest;
	}
	return make_node_ref(new_node);
}

// Reverse a successful detach.  All four detach cases (entity-formula root,
// free-standing root, embedded shrink=false, embedded shrink=true) are
// recoverable using the data captured in detach_result.  Errors during
// rollback are written to the supplied sink so the caller can compose a
// combined "operation failed AND rollback failed" message.
static bool rollback_detach(const GeneralSEXPReference &orig_ref,
	const detach_result &det, bool shrink, McpErrorSink &rollback_sink)
{
	if (orig_ref.mode == GeneralSEXPReference::Mode::Entity) {
		// Case A: entity-formula root.  Re-install the detached subtree as the
		// entity's formula, freeing the default that detach inserted.  A
		// successful rollback restores the pre-call state, so suppress the
		// inner mark_modified -- no autosave entry is warranted.
		GeneralSEXPReference dest = rebind_ref(orig_ref, det.replacement);
		handle_attach_sexp_node(det.detached_node, dest,
			attach_position::REPLACE, /*delete_displaced=*/true, rollback_sink,
			/*suppress_mark_modified=*/true);
		return !rollback_sink.has_error();
	}

	if (!shrink && det.replacement >= 0) {
		// Case C/D shrink=false: re-install by replacing the placeholder.
		GeneralSEXPReference dest = rebind_ref(orig_ref, det.replacement);
		handle_attach_sexp_node(det.detached_node, dest,
			attach_position::REPLACE, /*delete_displaced=*/true, rollback_sink,
			/*suppress_mark_modified=*/true);
		return !rollback_sink.has_error();
	}

	if (shrink && (det.pred_list >= 0 || det.pred_ante >= 0)) {
		// Case D shrink=true: direct splice using det.pred_list / det.pred_ante.
		// The post-syntax-check commit cleared n.parent and n.rest, so we
		// reconstruct them from the predecessor.
		int n = det.detached_node;
		int current_link = (det.pred_list >= 0)
			? Sexp_nodes[det.pred_list].first
			: Sexp_nodes[det.pred_ante].rest;
		Sexp_nodes[n].rest = current_link;
		Sexp_nodes[n].parent = (det.pred_list >= 0)
			? det.pred_list
			: Sexp_nodes[det.pred_ante].parent;
		splice_set_predecessor(det.pred_list, det.pred_ante, n);
		mcp_sexp_forest_mark_dirty({ find_formula_root_and_type(n).root });
		return true;
	}

	// Case B (free-standing root): nothing meaningful was committed.
	return true;
}

// Compose the user-visible error after a composed-operation step failed.
// step_err is the json_t error from a sub-operation (attach or detach).  If
// orphan_nodes is empty, the sub-operation's error text is forwarded to the
// user sink verbatim; otherwise a combined message is reported listing each
// SEXP subtree that remains free-standing because some part of the rollback
// failed, so the client can recover them.
static void report_composed_error(McpErrorSink &user_sink, json_t *step_err,
	const SCP_vector<int> &orphan_nodes, const char *step_label)
{
	if (orphan_nodes.empty()) {
		const char *err_text = extract_tool_result_text(step_err);
		user_sink.set_error("%s", err_text ? err_text : "operation failed");
		return;
	}

	if (orphan_nodes.size() == 1) {
		user_sink.set_error("%s failed AND rollback failed; "
			"a SEXP subtree remains free-standing at node %d. "
			"Use sexp_to_text or walk_sexp_tree to inspect it; "
			"use attach_sexp_node to re-integrate it.",
			step_label, orphan_nodes[0]);
		return;
	}

	SCP_string node_list;
	for (size_t i = 0; i < orphan_nodes.size(); i++) {
		if (i > 0)
			node_list += (i + 1 == orphan_nodes.size()) ? " and " : ", ";
		node_list += std::to_string(orphan_nodes[i]);
	}
	user_sink.set_error("%s failed AND rollback failed; "
		"%zu SEXP subtrees remain free-standing at nodes %s. "
		"Use sexp_to_text or walk_sexp_tree to inspect them; "
		"use attach_sexp_node to re-integrate them.",
		step_label, orphan_nodes.size(), node_list.c_str());
}

static void handle_move_sexp_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	GeneralSEXPReference src_ref;
	if (!parse_general_sexp_reference(input, "source_", src_ref, sink)) return;
	GeneralSEXPReference tgt_ref;
	if (!parse_general_sexp_reference(input, "target_", tgt_ref, sink)) return;

	if (src_ref.mode == GeneralSEXPReference::Mode::Entity && src_ref.entity_current_root < 0) {
		sink.set_error("Source entity (%s) has no formula to move; use attach_sexp_node to install one first.", src_ref.entity_info.to_string().c_str());
		return;
	}

	auto position_str = get_optional_string(input, "position", sink);
	auto shrink_opt = get_optional_bool(input, "shrink", sink);
	auto delete_displaced_opt = get_optional_bool(input, "delete_displaced", sink);
	if (sink.has_error()) return;

	if (position_str && tgt_ref.mode != GeneralSEXPReference::Mode::Node) {
		sink.set_error("'position' can only be used with 'target_node', not with target entity mode");
		return;
	}

	bool shrink = shrink_opt.has_value() && *shrink_opt;
	bool delete_displaced = delete_displaced_opt.has_value() && *delete_displaced_opt;

	attach_position position = parse_attach_position("position", position_str, sink);
	if (position == attach_position::INVALID) return;

	// Compute effective slot indices by retargeting operator atoms to their
	// list wrappers (the same retarget detach does internally).  This makes
	// the same-node guard catch the case where the user addresses a slot via
	// the bare operator atom on one side and via the wrapper on the other.
	int src_eff = (src_ref.mode == GeneralSEXPReference::Mode::Node)
		? retarget_to_list_wrapper(src_ref.node) : src_ref.node;
	int tgt_eff = (tgt_ref.mode == GeneralSEXPReference::Mode::Node)
		? retarget_to_list_wrapper(tgt_ref.node) : tgt_ref.node;

	// Same-slot no-op: trivially succeed without mutating state, autosaving,
	// or returning a flag.  Mirrors the convention of the per-entity move/swap
	// tools in mcp_mission_tools.cpp.
	if (src_eff == tgt_eff) {
		req->result_json = make_json_tool_result(json_object());
		req->success = true;
		return;
	}

	// Free-standing source guard: a free-standing root has no "location" to
	// move from -- detaching it would be a no-op, and the user should call
	// attach_sexp_node directly.  Entity mode is by definition mission-attached
	// and was already validated above.
	if (src_ref.mode == GeneralSEXPReference::Mode::Node) {
		FormulaRootInfo src_info = find_formula_root_and_type(src_eff);
		if (src_info.root == src_eff && !src_info.attached) {
			sink.set_error("source_node %d is a free-standing root; "
				"use attach_sexp_node directly to install it at the target.",
				src_ref.node);
			return;
		}
	}

	// Step 1: detach the source.  Suppress the inner mark_modified -- the
	// composer emits one high-level entry on success.
	auto det = handle_detach_sexp_node(src_ref, shrink, /*do_delete=*/false, sink, /*suppress_mark_modified=*/true);
	if (sink.has_error()) return;

	// Step 2: attach the detached subtree at the target.  Capture errors in a
	// local sink so that on failure we can roll back the detach before
	// surfacing the error to the user.
	json_t *attach_err = nullptr;
	McpErrorSink attach_sink(&attach_err);
	auto att = handle_attach_sexp_node(det.detached_node, tgt_ref, position,
		delete_displaced, attach_sink, /*suppress_mark_modified=*/true);
	if (attach_sink.has_error()) {
		McpErrorSink null_sink;	// dev/null
		bool restored = rollback_detach(src_ref, det, shrink, null_sink);
		SCP_vector<int> orphans;
		if (!restored) orphans.push_back(det.detached_node);
		// If rollback succeeded, net state matches pre-call -- no autosave.
		// If it failed, the mission is in a partial state; record one entry
		// so the user can recover via the autosave history.
		if (!restored)
			mark_modified("MCP: move SEXP subtree (partial -- rollback failed)");
		report_composed_error(sink, attach_err, orphans, "Attach");
		if (attach_err) json_decref(attach_err);
		return;
	}

	// Step 3: build the combined response.
	json_t *result_json = json_object();
	json_object_set_new(result_json, "moved_node", json_integer(det.detached_node));
	json_object_set_new(result_json, "detached", build_detach_response_json(det, /*do_delete=*/false));
	json_object_set_new(result_json, "attached",
		build_attach_response_json(det.detached_node, tgt_ref, position, delete_displaced, att));

	// One high-level entry for the composed operation, in lieu of the inner
	// detach + attach autosaves we suppressed.
	mark_modified("MCP: move SEXP subtree from node %d to node %d", src_eff, tgt_eff);

	req->result_json = make_json_tool_result(result_json);
	req->success = true;
}

static void handle_swap_sexp_nodes(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	GeneralSEXPReference src_ref;
	if (!parse_general_sexp_reference(input, "source_", src_ref, sink)) return;
	GeneralSEXPReference tgt_ref;
	if (!parse_general_sexp_reference(input, "target_", tgt_ref, sink)) return;

	if (src_ref.mode == GeneralSEXPReference::Mode::Entity && src_ref.entity_current_root < 0) {
		sink.set_error("Source entity (%s) has no formula to swap; use attach_sexp_node to install one first.", src_ref.entity_info.to_string().c_str());
		return;
	}
	if (tgt_ref.mode == GeneralSEXPReference::Mode::Entity && tgt_ref.entity_current_root < 0) {
		sink.set_error("Target entity (%s) has no formula to swap; use attach_sexp_node to install one first.", tgt_ref.entity_info.to_string().c_str());
		return;
	}

	// Compute effective slot indices by retargeting operator atoms to their
	// list wrappers (the same retarget detach does internally).  This makes
	// the same-node guard catch the case where the user addresses a slot via
	// the bare operator atom on one side and via the wrapper on the other,
	// and is also what the containment guard below operates on.
	int src_eff = (src_ref.mode == GeneralSEXPReference::Mode::Node)
		? retarget_to_list_wrapper(src_ref.node) : src_ref.node;
	int tgt_eff = (tgt_ref.mode == GeneralSEXPReference::Mode::Node)
		? retarget_to_list_wrapper(tgt_ref.node) : tgt_ref.node;

	// Same-slot no-op: trivially succeed without mutating state, autosaving,
	// or returning a flag.  Mirrors the convention of the per-entity move/swap
	// tools in mcp_mission_tools.cpp.
	if (src_eff == tgt_eff) {
		req->result_json = make_json_tool_result(json_object());
		req->success = true;
		return;
	}

	// Containment guard: swap requires the two subtrees to be disjoint.  If
	// one operand is inside the other's subtree, swap can't be cleanly
	// implemented as two detaches + two attaches because each detach disturbs
	// the other's slot.  Reject up-front before any mutation.
	{
		// Walk up from descendant via find_parent_operator, which handles
		// both the embedded case (.parent links) and the free-standing case
		// (reverse-iteration via .antecedent) -- a plain .parent walk would
		// miss the latter because of the unwrap convention.  If ancestor is
		// a list wrapper, the operator atom we're matching against is its
		// .first; otherwise ancestor itself.
		auto contains = [](int ancestor, int descendant) {
			if (descendant < 0 || ancestor < 0) return false;
			int target_op = (SEXP_NODE_TYPE(ancestor) == SEXP_LIST)
				? Sexp_nodes[ancestor].first : ancestor;
			for (int cur = descendant; cur >= 0; cur = find_parent_operator(cur)) {
				if (cur == target_op) return true;
			}
			return false;
		};

		if (contains(src_eff, tgt_eff)) {
			sink.set_error("Cannot swap: target (node %d) is inside source's subtree "
				"(rooted at node %d). Swap requires disjoint operands.",
				tgt_eff, src_eff);
			return;
		}
		if (contains(tgt_eff, src_eff)) {
			sink.set_error("Cannot swap: source (node %d) is inside target's subtree "
				"(rooted at node %d). Swap requires disjoint operands.",
				src_eff, tgt_eff);
			return;
		}
	}

	// Step 1: detach source.  Suppress the inner mark_modified -- the
	// composer emits one high-level entry on success.
	auto det_a = handle_detach_sexp_node(src_ref, /*shrink=*/false, /*do_delete=*/false, sink, /*suppress_mark_modified=*/true);
	if (sink.has_error()) return;

	// Step 2: detach target.  On failure, undo step 1.
	json_t *det_b_err = nullptr;
	McpErrorSink det_b_sink(&det_b_err);
	auto det_b = handle_detach_sexp_node(tgt_ref, /*shrink=*/false, /*do_delete=*/false, det_b_sink, /*suppress_mark_modified=*/true);
	if (det_b_sink.has_error()) {
		McpErrorSink null_sink;	// dev/null
		bool a_restored = rollback_detach(src_ref, det_a, /*shrink=*/false, null_sink);
		SCP_vector<int> orphans;
		if (!a_restored) orphans.push_back(det_a.detached_node);
		if (!a_restored)
			mark_modified("MCP: swap SEXP subtrees (partial -- rollback failed)");
		report_composed_error(sink, det_b_err, orphans, "Target detach");
		if (det_b_err) json_decref(det_b_err);
		return;
	}

	// Step 3: build attach destinations.  Each side's vacated slot is either
	// the entity itself (entity-mode -- the placeholder is the inserted default
	// formula that we will displace) or the placeholder spliced in by detach.
	GeneralSEXPReference dest_for_a = rebind_ref(tgt_ref, det_b.replacement);  // a goes to b's vacated slot
	GeneralSEXPReference dest_for_b = rebind_ref(src_ref, det_a.replacement);  // b goes to a's vacated slot

	// Step 4: attach a at b's slot.
	json_t *att_a_err = nullptr;
	McpErrorSink att_a_sink(&att_a_err);
	auto att_a = handle_attach_sexp_node(det_a.detached_node, dest_for_a,
		attach_position::REPLACE, /*delete_displaced=*/true, att_a_sink,
		/*suppress_mark_modified=*/true);
	if (att_a_sink.has_error()) {
		McpErrorSink null_sink_1;	// dev/null
		McpErrorSink null_sink_2;	// dev/null
		bool b_restored = rollback_detach(tgt_ref, det_b, /*shrink=*/false, null_sink_1);
		bool a_restored = rollback_detach(src_ref, det_a, /*shrink=*/false, null_sink_2);
		SCP_vector<int> orphans;
		if (!b_restored) orphans.push_back(det_b.detached_node);
		if (!a_restored) orphans.push_back(det_a.detached_node);
		if (!b_restored || !a_restored)
			mark_modified("MCP: swap SEXP subtrees (partial -- rollback failed)");
		report_composed_error(sink, att_a_err, orphans, "First attach");
		if (att_a_err) json_decref(att_a_err);
		return;
	}

	// Step 5: attach b at a's slot.
	json_t *att_b_err = nullptr;
	McpErrorSink att_b_sink(&att_b_err);
	auto att_b = handle_attach_sexp_node(det_b.detached_node, dest_for_b,
		attach_position::REPLACE, /*delete_displaced=*/true, att_b_sink,
		/*suppress_mark_modified=*/true);
	if (att_b_sink.has_error()) {
		// Reachability note: when tgt_ref is entity mode, this branch is
		// unreachable.  Both step 4 and step 5 run check_sexp_formula against
		// an entity OPR (OPR_NULL or OPR_BOOL); since a originated at src and
		// b originated at tgt, step-4 success implies step-5 success.  Any
		// OPR mismatch fails step 4 first.  The rollback below is therefore
		// only exercised when tgt_ref is node mode -- but it still has to
		// handle src_ref being either node or entity.

		// Undo step 4: detach a from b's slot so it's free-standing again.
		// After att_a, det_a.detached_node is embedded at b's old slot.
		// undo_a uses its own private sink so the bool return is independent
		// of any later rollback failure.
		McpErrorSink undo_sink;	// dev/null
		auto undo_a = handle_detach_sexp_node(make_node_ref(det_a.detached_node),
			/*shrink=*/false, /*do_delete=*/false, undo_sink, /*suppress_mark_modified=*/true);
		bool a_extracted = !undo_sink.has_error();

		// If undo_a succeeded, both detached subtrees are free-standing again
		// and we can attempt to put each back where it came from.  If it
		// failed, det_a is still embedded at b's old slot (tree state is
		// wedged) and we report only det_b as a free-standing orphan.
		bool b_restored = false;
		bool a_restored = false;
		if (a_extracted) {
			McpErrorSink null_sink_1;	// dev/null
			McpErrorSink null_sink_2;	// dev/null
			detach_result det_b_prime = det_b;
			det_b_prime.replacement = undo_a.replacement;
			b_restored = rollback_detach(tgt_ref, det_b_prime, /*shrink=*/false, null_sink_1);
			a_restored = rollback_detach(src_ref, det_a, /*shrink=*/false, null_sink_2);
		}

		SCP_vector<int> orphans;
		if (!a_extracted) {
			orphans.push_back(det_b.detached_node);
		} else {
			if (!b_restored) orphans.push_back(det_b.detached_node);
			if (!a_restored) orphans.push_back(det_a.detached_node);
		}
		if (!orphans.empty())
			mark_modified("MCP: swap SEXP subtrees (partial -- rollback failed)");
		report_composed_error(sink, att_b_err, orphans, "Second attach");
		if (att_b_err) json_decref(att_b_err);
		return;
	}

	// Step 6: build the combined response.
	json_t *result = json_object();
	json_object_set_new(result, "first_attach",
		build_attach_response_json(det_a.detached_node, dest_for_a,
			attach_position::REPLACE, /*delete_displaced=*/true, att_a));
	json_object_set_new(result, "second_attach",
		build_attach_response_json(det_b.detached_node, dest_for_b,
			attach_position::REPLACE, /*delete_displaced=*/true, att_b));

	// One high-level entry for the composed operation, in lieu of the inner
	// detach + attach autosaves we suppressed.
	mark_modified("MCP: swap SEXP subtrees at nodes %d and %d", src_eff, tgt_eff);

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
		return wrap_node(locked_node, next);
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
		// Always wrap to avoid corrupting existing tree linkage
		return wrap_node(idx, next);
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

	json_t *warnings = nullptr;
	json_t *args = json_object_get(input, "operator_arguments");
	bool has_args = (args && json_is_array(args) && json_array_size(args) > 0);

	// Locked singletons (true/false operators) should not have anything linked
	if (has_args && (op_node == Locked_sexp_true || op_node == Locked_sexp_false)) {
		sink.set_error("Operator '%s' does not accept arguments", op_name);
		return;
	}

	// Parse arguments (if any)
	if (has_args) {
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
						i, result.type_str, opf_to_name(expected_opf), op_name);
					break;
				}
			}

			// creating this argument succeeded
			next = result.arg_node;
		}

		// there might have been an error in the argument loop
		if (sink.has_error()) {
			// Before freeing, unwrap wrapped nodes so free_sexp2
			// doesn't recurse into referenced existing trees
			int walk = (result.arg_node != -1) ? result.arg_node : next;
			while (walk != -1) {
				if (Sexp_nodes[walk].subtype == SEXP_ATOM_LIST
					&& Sexp_nodes[walk].first != Locked_sexp_true
					&& Sexp_nodes[walk].first != Locked_sexp_false
				) {
					unwrap_node(walk, false);	// don't free the wrapper so that the arg chain remains intact - we need to free the arg chain later
				}
				walk = Sexp_nodes[walk].rest;
			}

			// Free the argument chain and operator
			if (result.arg_node != -1)			// frees the arg and anything linked after it in the chain
				free_sexp2(result.arg_node);
			else if (next != -1)				// arg was never created, so just free the chain
				free_sexp2(next);
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
		Sexp_nodes[op_node].rest = next;
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

	json_object_set_new(obj, "variable_flags",
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
	// Validate that value is a valid integer (optional leading sign, then digits)
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
		if ((Sexp_nodes[i].type != SEXP_NOT_USED) && (Sexp_nodes[i].type & SEXP_FLAG_VARIABLE) && !strcmp(Sexp_nodes[i].text, old_name)) {
			strcpy_s(Sexp_nodes[i].text, new_name);
		}
	}
}

static int reset_sexp_node_variable_references(const char *var_name, int var_type)
{
	int count = 0;

	for (int i = 0; i < Num_sexp_nodes; i++) {
		if ((Sexp_nodes[i].type != SEXP_NOT_USED) && (Sexp_nodes[i].type & SEXP_FLAG_VARIABLE) && !strcmp(Sexp_nodes[i].text, var_name)) {
			Sexp_nodes[i].type &= ~SEXP_FLAG_VARIABLE;
			strcpy_s(Sexp_nodes[i].text, PLACEHOLDER_STRING);
			count++;
		}
	}
	return count;
}

static bool is_variable_referenced_in_sexp_nodes(const char *var_name)
{
	for (int i = 0; i < Num_sexp_nodes; i++) {
		if ((Sexp_nodes[i].type != SEXP_NOT_USED) && (Sexp_nodes[i].type & SEXP_FLAG_VARIABLE) && !strcmp(Sexp_nodes[i].text, var_name))
			return true;
	}
	return false;
}

static void handle_list_sexp_variables(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_sexp_nodes, sink)) return;

	// Optional variable_type filter
	auto type_str = get_optional_string(input, "variable_type", sink);
	if (sink.has_error()) return;
	if (type_str && !check_string_enum(type_str, sexp_var_type_values, "variable_type", sink))
		return;
	int type_bits = 0;
	if (type_str) {
		type_bits = (stricmp(type_str, "number") == 0) ? SEXP_VARIABLE_NUMBER : SEXP_VARIABLE_STRING;
	}

	// Optional variable_flags filter (match-ALL)
	int required_flags = 0;
	auto flags_arr = get_optional_string_array(input, "variable_flags", sink);
	if (!flags_arr.has_value() && json_object_get(input, "variable_flags"))
		return;  // non-string element error already reported by sink
	if (flags_arr.has_value()) {
		if (!parse_flags_array(*flags_arr, sexp_var_flag_entries, sexp_var_flag_entries_count,
				"variable_flags", required_flags, sink))
			return;
		if (!validate_sexp_variable_flags(required_flags, sink)) return;
	}

	json_t *arr = json_array();
	for (int i = 0; i < MAX_SEXP_VARIABLES; i++) {
		const auto &v = Sexp_variables[i];
		if (!(v.type & SEXP_VARIABLE_SET)) continue;
		if (type_bits && !(v.type & type_bits)) continue;
		if (required_flags && (v.type & required_flags) != required_flags) continue;
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
	auto flags_arr = get_optional_string_array(input, "variable_flags", sink);
	if (!flags_arr.has_value() && json_object_get(input, "variable_flags"))
		return;  // non-string element error already reported by sink
	if (flags_arr.has_value()) {
		if (!parse_flags_array(*flags_arr, sexp_var_flag_entries, sexp_var_flag_entries_count, "variable_flags", flag_bits, sink))
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
	auto flags_arr = get_optional_string_array(input, "variable_flags", sink);
	if (!flags_arr.has_value() && json_object_get(input, "variable_flags"))
		return;  // non-string element error already reported by sink
	if (flags_arr.has_value()) {
		if (!parse_flags_array(*flags_arr, sexp_var_flag_entries, sexp_var_flag_entries_count, "variable_flags", flag_bits, sink))
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

// Schema descriptor for a "general SEXP reference" parameter group.  Used by
// add_general_sexp_ref_props to emit the five props (<prefix>node,
// <prefix>argument_index, <prefix>entity_type, <prefix>entity_id,
// <prefix>entity_tag) consistently across the four tools that share this
// reference shape (detach_sexp_node, attach_sexp_node, move_sexp_node,
// swap_sexp_nodes).  All boilerplate text -- mutual-exclusion notes, list-
// wrapper retarget warnings, locked-singleton workaround, per-entity-type
// id/tag guidance -- lives in the helper, so callers only supply the bits
// that genuinely vary by tool.
struct GeneralSexpRefDesc {
	// First sentence of the <prefix>node description (no trailing period).
	// E.g. "Node index of the SEXP node to move".
	const char *node_intro;
	// Past-tense verb dropped into "...the wrapper is automatically <verb>
	// instead."  E.g. "moved", "swapped", "detached", "targeted".
	const char *retarget_verb;
	// Verb dropped into "...the <verb> operates on that argument instead of
	// the operator itself."  E.g. "move", "swap", "detach", "attach".
	const char *arg_action_verb;
	// First sentence of the <prefix>entity_type description (no trailing
	// period).  E.g. "Entity type whose formula to move", "Entity type for
	// the first formula in the swap".
	const char *entity_type_role;
	// Optional follow-on sentence appended after the mut-ex/requires text in
	// the <prefix>entity_type description.  E.g. for detach / move source:
	// "The entity's formula is replaced with a default."  Pass nullptr when
	// the tool has no such side-effect to advertise.
	const char *entity_type_extra;
};

static void add_general_sexp_ref_props(json_t *props, const char *prefix,
	const GeneralSexpRefDesc &d)
{
	SCP_string p = prefix ? prefix : "";

	// <prefix>node
	SCP_string node_desc = d.node_intro;
	node_desc += ". Mutually exclusive with " + p + "entity_type. ";
	node_desc += "If the node is an operator inside a list wrapper, the wrapper is automatically ";
	node_desc += d.retarget_verb;
	node_desc += " instead. ";
	node_desc += "Shared locked singletons (true/false) cannot be targeted directly; use ";
	node_desc += p + "argument_index with the parent operator instead.";
	add_integer_prop(props, (p + "node").c_str(), node_desc.c_str());

	// <prefix>argument_index
	SCP_string arg_desc = "1-based index into the argument list of the operator at " + p + "node. ";
	arg_desc += "When provided, " + p + "node must be an operator (or its list wrapper) and the ";
	arg_desc += d.arg_action_verb;
	arg_desc += " operates on that argument instead of the operator itself.";
	add_integer_prop(props, (p + "argument_index").c_str(), arg_desc.c_str());

	// <prefix>entity_type
	SCP_string et_desc = d.entity_type_role;
	et_desc += ". Mutually exclusive with " + p + "node. Requires " + p + "entity_id.";
	if (d.entity_type_extra && d.entity_type_extra[0]) {
		et_desc += " ";
		et_desc += d.entity_type_extra;
	}
	add_string_enum_prop(props, (p + "entity_type").c_str(), et_desc.c_str(),
		formula_entity_type_values);

	// <prefix>entity_id
	SCP_string id_desc = "Entity name or index. Required when " + p + "entity_type is set. ";
	id_desc += "For ships/wings: the ship or wing name. ";
	id_desc += "For events/goals: the event or goal name. ";
	id_desc += "For cutscenes/fiction viewer/briefing/debriefing stages: the 1-based stage index.";
	add_string_prop(props, (p + "entity_id").c_str(), id_desc.c_str());

	// <prefix>entity_tag
	add_string_enum_prop(props, (p + "entity_tag").c_str(),
		"Disambiguation tag for entities that have multiple formula slots. "
		"For ships/wings: 'arrival_cue' (default) or 'departure_cue'. "
		"For briefing/debriefing stages: 'team_1' (default) or 'team_2'.",
		formula_entity_tag_values);
}

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
		add_integer_prop(props, "depth",
			"Maximum recursion depth. 0 returns only the root node; N returns the root "
			"plus N levels of descendants. Omit or pass a negative value for unlimited depth.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("node"));
		register_tool(tools, "walk_sexp_tree",
			"Walk the SEXP subtree rooted at the given node. Returns a flat array "
			"of node descriptors with kind, value, value_type, role "
			"(operator/argument/list_wrapper), child/sibling indices, and "
			"walk_first/walk_rest indices into the array for easy traversal. "
			"In FreeSpace SEXP trees, the top-level operator is a bare atom node, while "
			"operators deeper in the tree are wrapped in list nodes. "
			"The result is capped at " SCP_TOKEN_TO_STR(MAX_SEXP_WALK_ENTRIES) " nodes; if the cap is reached the response "
			"includes 'truncated': true. To inspect a larger tree, call again with a "
			"deeper sub-node as the root, or use the 'depth' parameter to limit the walk.",
			props, req);
	}

	// find_sexp_text
	{
		json_t *props = json_object();
		add_string_prop(props, "text",
			"Text to search for in SEXP node values.  Must be non-empty.  "
			"A leading '@' is stripped before matching (the '@' prefix is a "
			"SEXP display convention for variable references and is never "
			"stored in node text).");
		add_string_enum_prop(props, "match_mode",
			"How to compare 'text' against each node's value.  'substring' (default) "
			"matches if 'text' appears anywhere in the node's value.  'exact' matches "
			"only when the node's value equals 'text'.  'fuzzy' allows typos and "
			"missing characters via a Levenshtein-style cost; fuzzy results are "
			"returned ordered by relevance (best match first), unlike substring and "
			"exact which preserve traversal order.  All three modes are case-insensitive.",
			find_match_mode_values);
		add_integer_prop(props, "root_node",
			"Optional root node index.  When provided, the search is restricted to "
			"the subtree rooted at this node.  When omitted, every live SEXP node in "
			"the mission is searched.");
		add_string_enum_prop(props, "role",
			"Optional role filter.  When provided, only nodes with this role are "
			"considered.  list_wrapper nodes are structural and are never returned "
			"regardless of this filter.",
			sexp_searchable_role_values);
		add_string_enum_prop(props, "value_type",
			"Optional value-type filter.  When provided, only nodes whose value_type "
			"matches are considered.  Mirrors the value_type field returned by "
			"get_sexp_node and walk_sexp_tree.",
			sexp_value_type_values);
		json_t *req = json_array();
		json_array_append_new(req, json_string("text"));
		register_tool(tools, "find_sexp_text",
			"Find SEXP nodes whose text matches a given query.  Useful for locating "
			"every reference to an entity name (a ship, a wing, a variable, a literal) "
			"across the mission, or within a single tree.  By default the search is "
			"mission-wide; pass root_node to restrict to one tree.  Matching is "
			"case-insensitive; choose substring (default) or exact via match_mode.  "
			"Optional role and value_type filters narrow the candidate set; "
			"list_wrapper nodes are always excluded.  The response is "
			"{nodes: [<node_object>...], truncated?: true}, where each node_object "
			"matches the shape returned by get_sexp_node.  The result is capped at "
			SCP_TOKEN_TO_STR(MAX_SEXP_WALK_ENTRIES) " nodes; if the cap is reached the "
			"response includes 'truncated': true and the caller should narrow the "
			"query (use a more specific text or add filters).",
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
		add_general_sexp_ref_props(props, "", {
			"Node index of the SEXP node to detach",                       // node_intro
			"detached",                                                     // retarget_verb
			"detach",                                                       // arg_action_verb
			"Entity type whose formula to detach",                          // entity_type_role
			"The entity's formula is replaced with a default.",             // entity_type_extra
		});
		add_bool_prop(props, "shrink",
			"If true, remove the node and shift subsequent siblings up by one "
			"position instead of inserting a " PLACEHOLDER_STRING ". Defaults to false.");
		add_bool_prop(props, "delete",
			"If true, free the detached node and its subtree. If false (the default), "
			"the detached node is preserved and returned in the response.");
		register_tool(tools, "detach_sexp_node",
			"Detach a SEXP node from its tree. The target can be specified by node index "
			"(node), by node index plus argument position (node + "
			"argument_index), or by entity coordinates (entity_type + "
			"entity_id). If the target is an operator inside a list wrapper, the "
			"wrapper is automatically detached as well. If the node is the root of a "
			"mission entity's formula (or entity mode is used), it is replaced with an "
			"appropriate default (e.g. do-nothing for action formulas, true for boolean "
			"formulas). If the node is embedded within a tree, it is replaced with "
			PLACEHOLDER_STRING ", unless 'shrink' is true, in which case subsequent "
			"siblings shift up by one position. By default, the detached node is "
			"preserved and returned; set 'delete' to true to free it. The response "
			"includes detached_node (int index), detached_node_data (full node object when "
			"the node was not deleted), replacement_node (int or null), replacement_node_data "
			"(full node object when a replacement was inserted), deleted (bool, true only when "
			"'delete' was requested and at least one node was actually freed), and freed_count "
			"(int, total number of nodes freed during the operation). Note - freed_count may be "
			"non-zero even when 'delete' is false: if the detached subtree's root is a "
			"redundant list wrapper, it is unwrapped so the new free-standing root is the "
			"bare operator atom, and the wrapper counts toward freed_count. For mission-attached "
			"trees, a syntax check is performed after modification; if the check fails, "
			"the operation is rolled back. Shared locked singleton nodes (true/false) cannot "
			"be targeted directly; use argument_index with the parent operator.",
			props, nullptr,
			merge_schema_extras(
				build_branch_required_fields("oneOf", { {"node"}, {"entity_type", "entity_id"} }),
				build_dependencies_extras({
					{"argument_index", {"node"}},
					{"entity_id", {"entity_type"}},
					{"entity_tag", {"entity_type"}},
				})));
	}

	// attach_sexp_node
	{
		json_t *props = json_object();
		add_integer_prop(props, "source_node",
			"Node index of the free-standing root to attach. Must have parent == -1 "
			"and not be attached to any mission entity.");
		add_general_sexp_ref_props(props, "target_", {
			"Node index of an existing tree node to position the source relative to", // node_intro
			"targeted",                                                                // retarget_verb
			"attach",                                                                  // arg_action_verb
			"Entity type whose formula the source will become",                        // entity_type_role
			nullptr,                                                                   // entity_type_extra
		});
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
			"deleted_displaced (bool, true only when 'delete_displaced' was requested and at least "
			"one node was actually freed; a locked singleton displaced from an entity formula "
			"cannot be freed and will report false), and freed_count (int, total number of "
			"nodes freed during the operation). Note - freed_count may be non-zero even when "
			"'delete_displaced' is false: if the displaced subtree's root is a redundant list "
			"wrapper, it is unwrapped so the preserved free-standing root is the bare operator "
			"atom, and the wrapper counts toward freed_count.",
			props, req,
			merge_schema_extras(
				build_branch_required_fields("oneOf", { {"target_node"}, {"target_entity_type", "target_entity_id"} }),
				build_dependencies_extras({
					{"target_argument_index", {"target_node"}},
					{"target_entity_id", {"target_entity_type"}},
					{"target_entity_tag", {"target_entity_type"}},
					{"position", {"target_node"}},
				})));
	}

	// move_sexp_node
	{
		json_t *props = json_object();
		add_general_sexp_ref_props(props, "source_", {
			"Node index of the SEXP node to move",                          // node_intro
			"moved",                                                         // retarget_verb
			"move",                                                          // arg_action_verb
			"Entity type whose formula to move",                             // entity_type_role
			"The entity's formula is replaced with a default.",              // entity_type_extra
		});
		add_general_sexp_ref_props(props, "target_", {
			"Node index of an existing tree node to position the source relative to", // node_intro
			"targeted",                                                                // retarget_verb
			"move",                                                                    // arg_action_verb
			"Entity type whose formula the source will become",                        // entity_type_role
			nullptr,                                                                   // entity_type_extra
		});

		add_string_enum_prop(props, "position",
			"How to position the source relative to the target node. "
			"'replace' (default) replaces the target; 'before' inserts before the target; "
			"'after' inserts after the target. Only used with target_node.",
			attach_position_values);
		add_bool_prop(props, "shrink",
			"If true, when removing the source from a sibling chain, shift subsequent "
			"siblings up by one position instead of leaving a " PLACEHOLDER_STRING ". "
			"Defaults to false.");
		add_bool_prop(props, "delete_displaced",
			"If true, free the displaced subtree at the target slot when in 'replace' "
			"mode or entity target mode. If false (the default), the displaced subtree "
			"is preserved as a free-standing root and reported in the 'attached' "
			"sub-object's 'displaced_node' field. No effect for 'before'/'after' "
			"positions, which never displace anything.");

		register_tool(tools, "move_sexp_node",
			"Move a SEXP subtree from one location to another in a single atomic operation. "
			"Composes detach_sexp_node + attach_sexp_node internally: the source is detached "
			"into a free-standing tree, then attached at the target.  If the attach fails "
			"(e.g. due to a syntax check), the detach is rolled back so the original tree is "
			"left unchanged. The source can be specified by node index (source_node), by "
			"node index plus argument position (source_node + source_argument_index), or by "
			"entity coordinates (source_entity_type + source_entity_id).  The target uses "
			"the same three forms with target_ prefixes.  In node-relative target mode, "
			"position can be 'replace' (default), 'before', or 'after' -- same semantics as "
			"attach_sexp_node.  In entity target mode, the source replaces the entity's "
			"current formula. If source and target resolve to the same slot, the call is a "
			"trivial no-op (success with an empty response, no autosave). Otherwise the "
			"response includes 'moved_node' (int, the absolute index of the relocated "
			"subtree's root) and the full 'detached' and 'attached' sub-objects (see "
			"detach_sexp_node and attach_sexp_node for their fields). By default, anything "
			"already at the target slot is preserved as a free-standing root and reported in "
			"the response; pass delete_displaced=true to free it instead.",
			props, /*required=*/nullptr,
			merge_schema_extras(
				build_branch_required_fields_allof("oneOf", {
					{ {"source_node"}, {"source_entity_type", "source_entity_id"} },
					{ {"target_node"}, {"target_entity_type", "target_entity_id"} },
				}),
				build_dependencies_extras({
					{"source_argument_index", {"source_node"}},
					{"source_entity_id",      {"source_entity_type"}},
					{"source_entity_tag",     {"source_entity_type"}},
					{"target_argument_index", {"target_node"}},
					{"target_entity_id",      {"target_entity_type"}},
					{"target_entity_tag",     {"target_entity_type"}},
					{"position",              {"target_node"}},
				})));
	}

	// swap_sexp_nodes
	{
		json_t *props = json_object();
		add_general_sexp_ref_props(props, "source_", {
			"Node index of the first SEXP node to swap",      // node_intro
			"swapped",                                         // retarget_verb
			"swap",                                            // arg_action_verb
			"Entity type for the first formula in the swap",   // entity_type_role
			nullptr,                                           // entity_type_extra
		});
		add_general_sexp_ref_props(props, "target_", {
			"Node index of the second SEXP node to swap",      // node_intro
			"swapped",                                         // retarget_verb
			"swap",                                            // arg_action_verb
			"Entity type for the second formula in the swap",  // entity_type_role
			nullptr,                                           // entity_type_extra
		});

		register_tool(tools, "swap_sexp_nodes",
			"Exchange two SEXP subtrees in place, preserving each other's structural "
			"position. Composes detach_sexp_node + attach_sexp_node internally and is "
			"atomic: if any sub-step fails, all earlier steps are rolled back. Each "
			"endpoint can be specified by node index (source_node/target_node), by "
			"node index plus argument position, or by entity coordinates "
			"(entity_type + entity_id). All four mode combinations are supported "
			"(node/node, entity/entity, node/entity, entity/node). If source and "
			"target resolve to the same slot, the call is a trivial no-op (success "
			"with an empty response, no autosave). Otherwise the response includes "
			"'first_attach' and 'second_attach' (see attach_sexp_node for their fields).  "
			"'first_attach' reports the source attached at the target's vacated slot; "
			"'second_attach' reports the target attached at the source's vacated slot.",
			props, /*required=*/nullptr,
			merge_schema_extras(
				build_branch_required_fields_allof("oneOf", {
					{ {"source_node"}, {"source_entity_type", "source_entity_id"} },
					{ {"target_node"}, {"target_entity_type", "target_entity_id"} },
				}),
				build_dependencies_extras({
					{"source_argument_index", {"source_node"}},
					{"source_entity_id",      {"source_entity_type"}},
					{"source_entity_tag",     {"source_entity_type"}},
					{"target_argument_index", {"target_node"}},
					{"target_entity_id",      {"target_entity_type"}},
					{"target_entity_tag",     {"target_entity_type"}},
				})));
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
			props, req,
			build_conditional_required_fields("role", {
				{"operator", {"operator_name"}},
				{"argument", {"argument_type", "argument_value"}},
			}));
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
	{
		auto flag_names = flags_to_list(sexp_var_flag_entries, sexp_var_flag_entries_count);
		json_t *props = json_object();
		add_string_enum_prop(props, "variable_type",
			"Filter to variables of this type only.",
			sexp_var_type_values);
		add_string_array_prop(props, "variable_flags",
			"Filter to variables that have ALL of the specified flags set.",
			flag_names);
		register_tool(tools, "list_sexp_variables",
			"List all SEXP variables defined in this mission. "
			"Returns each variable's name, default value, type (number or string), and flags. "
			"Optionally filter by variable_type and/or variable_flags.",
			props);
	}

	// get_sexp_variable
	register_tool_with_required_string(tools, "get_sexp_variable",
		"Get full details of a SEXP variable by name, including default value, "
		"type (number or string), and flags.",
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
		add_string_array_prop(props, "variable_flags",
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
		add_string_array_prop(props, "variable_flags",
			"New persistence and network flags. Specify the complete set you want — "
			"this replaces all existing flags rather than merging. "
			"save_on_mission_progress and save_on_mission_close are mutually exclusive.",
			flag_names);
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_sexp_variable",
			"Update a SEXP variable's properties. Omitted fields are unchanged; "
			"provided fields replace existing values (variable_flags replaces the full flag set, not a delta). "
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
	if (strcmp(tool_name, "find_sexp_text") == 0) {
		handle_find_sexp_text(input_json, req);
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
	if (strcmp(tool_name, "move_sexp_node") == 0) {
		handle_move_sexp_node(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "swap_sexp_nodes") == 0) {
		handle_swap_sexp_nodes(input_json, req);
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
