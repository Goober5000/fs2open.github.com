#include "stdafx.h"
#include "mcp_loadout.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcpserver.h"

#include <jansson.h>
#include <cstring>
#include <cstdlib>

#include "management.h"          // generate_ship_usage_list / generate_weaponry_usage_list

#include "mission/missionparse.h"
#include "parse/sexp.h"
#include "ship/ship.h"
#include "weapon/weapon.h"

// ---------------------------------------------------------------------------
// Team loadout tools
//
// Expose the Team Loadout editor's data (Team_data[MAX_TVT_TEAMS]) over MCP.
// Each team owns two pools of entries -- ships and weapons -- where an entry's
// class is either a literal class name or the name of a string SEXP variable,
// and its count is either a literal or the name of a number SEXP variable.
//
// Count semantics (mirrors FRED's in-memory convention): for entries with a
// literal class and literal count, the weapon count means "extras beyond what
// the starting wings already carry" -- FREDDoc subtracts wing-carried weapons
// after load and mission save adds them back (unless do_not_validate is set).
// Variable-based entries are never padded; their counts are absolute.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Dialog conflict guard
// ---------------------------------------------------------------------------

static bool validate_dialog_for_loadout(SCP_string &error_msg)
{
	return validate_single_dialog("team loadout", "player start", error_msg);
}

// ---------------------------------------------------------------------------
// FRED dialog ranges (mirrored from playerstarteditor.cpp)
// ---------------------------------------------------------------------------

static constexpr int LOADOUT_COUNT_MIN = 0;
static constexpr int LOADOUT_COUNT_MAX = 9999;

// ---------------------------------------------------------------------------
// Team resolution (same pattern as get_debriefing_for_team)
// ---------------------------------------------------------------------------

// Resolve optional "team" parameter to a team index.
// Defaults to Team 1. Rejects "none". Returns -1 with sink error on failure.
static int resolve_loadout_team(json_t *input, McpErrorSink &sink)
{
	auto team_str = get_optional_string(input, "team", sink);
	if (sink.has_error()) return -1;

	if (team_str) {
		if (!check_string_enum(team_str, team_enum_values, "team", sink))
			return -1;
		if (reject_team_none(team_str, "team loadout", sink)) return -1;
		return team_index_from_name(team_str);
	}

	return 0;	// default to Team 1
}

// ---------------------------------------------------------------------------
// Wing usage
// ---------------------------------------------------------------------------

// Per-team ship/weapon usage from the starting wings, matching how the Team
// Loadout editor computes its "used in wings" readouts: TVT missions count
// each team's TVT wings; other missions count the starting wings for Team 1
// only (Team 2 has no starting wings outside TVT).
static void compute_team_usage(int team, SCP_vector<int> &ship_usage, SCP_vector<int> &weapon_usage)
{
	ship_usage.assign(MAX_SHIP_CLASSES, 0);
	weapon_usage.assign(MAX_WEAPON_TYPES, 0);

	if (The_mission.game_type & MISSION_TYPE_MULTI_TEAMS) {
		for (int j = 0; j < MAX_TVT_WINGS_PER_TEAM; j++) {
			generate_ship_usage_list(ship_usage.data(), TVT_wings[(team * MAX_TVT_WINGS_PER_TEAM) + j]);
			generate_weaponry_usage_list(weapon_usage.data(), TVT_wings[(team * MAX_TVT_WINGS_PER_TEAM) + j]);
		}
	} else if (team == 0) {
		for (int j = 0; j < MAX_STARTING_WINGS; j++) {
			generate_ship_usage_list(ship_usage.data(), Starting_wings[j]);
			generate_weaponry_usage_list(weapon_usage.data(), Starting_wings[j]);
		}
	}
}

// ---------------------------------------------------------------------------
// JSON builders
// ---------------------------------------------------------------------------

static json_t *build_team_loadout_json(int team)
{
	const team_data &td = Team_data[team];

	SCP_vector<int> ship_usage, weapon_usage;
	compute_team_usage(team, ship_usage, weapon_usage);

	json_t *obj = json_object();
	json_object_set_new(obj, "team", json_string(team_name_from_index(team)));
	json_object_set_new(obj, "do_not_validate", json_boolean(td.do_not_validate));

	// ship pool
	json_t *ships = json_array();
	for (int i = 0; i < td.num_ship_choices; i++) {
		json_t *e = json_object();
		if (strlen(td.ship_list_variables[i]) > 0) {
			json_object_set_new(e, "class_variable", json_safe_string(td.ship_list_variables[i]));
		} else if (td.ship_list[i] >= 0 && td.ship_list[i] < ship_info_size()) {
			json_object_set_new(e, "ship_class", json_safe_string(Ship_info[td.ship_list[i]].name));
			json_object_set_new(e, "used_in_wings", json_integer(ship_usage[td.ship_list[i]]));
		}
		if (strlen(td.ship_count_variables[i]) > 0)
			json_object_set_new(e, "count_variable", json_safe_string(td.ship_count_variables[i]));
		else
			json_object_set_new(e, "count", json_integer(td.ship_count[i]));
		json_array_append_new(ships, e);
	}
	json_object_set_new(obj, "ships", ships);

	// weapon pool
	json_t *weapons = json_array();
	for (int i = 0; i < td.num_weapon_choices; i++) {
		json_t *e = json_object();
		if (strlen(td.weaponry_pool_variable[i]) > 0) {
			json_object_set_new(e, "class_variable", json_safe_string(td.weaponry_pool_variable[i]));
		} else if (td.weaponry_pool[i] >= 0 && td.weaponry_pool[i] < weapon_info_size()) {
			json_object_set_new(e, "weapon_class", json_safe_string(Weapon_info[td.weaponry_pool[i]].name));
			json_object_set_new(e, "used_in_wings", json_integer(weapon_usage[td.weaponry_pool[i]]));
			json_object_set_new(e, "required", json_boolean(td.weapon_required[td.weaponry_pool[i]]));
		}
		if (strlen(td.weaponry_amount_variable[i]) > 0)
			json_object_set_new(e, "count_variable", json_safe_string(td.weaponry_amount_variable[i]));
		else
			json_object_set_new(e, "count", json_integer(td.weaponry_count[i]));
		json_array_append_new(weapons, e);
	}
	json_object_set_new(obj, "weapons", weapons);

	// Informational: classes carried by this team's starting wings but absent
	// from the static pool.  Missing weapons are auto-added at save time unless
	// do_not_validate is set; missing ships trip the mission error checker.
	json_t *missing_ships = json_array();
	for (int j = 0; j < ship_info_size(); j++) {
		if (ship_usage[j] <= 0)
			continue;
		bool in_pool = false;
		for (int i = 0; i < td.num_ship_choices; i++) {
			if (strlen(td.ship_list_variables[i]) == 0 && td.ship_list[i] == j) {
				in_pool = true;
				break;
			}
		}
		if (!in_pool) {
			json_t *e = json_object();
			json_object_set_new(e, "ship_class", json_safe_string(Ship_info[j].name));
			json_object_set_new(e, "used_in_wings", json_integer(ship_usage[j]));
			json_array_append_new(missing_ships, e);
		}
	}
	if (json_array_size(missing_ships) > 0)
		json_object_set_new(obj, "wing_ships_not_in_pool", missing_ships);
	else
		json_decref(missing_ships);

	json_t *missing_weapons = json_array();
	for (int j = 0; j < weapon_info_size(); j++) {
		if (weapon_usage[j] <= 0)
			continue;
		bool in_pool = false;
		for (int i = 0; i < td.num_weapon_choices; i++) {
			if (strlen(td.weaponry_pool_variable[i]) == 0 && td.weaponry_pool[i] == j) {
				in_pool = true;
				break;
			}
		}
		if (!in_pool) {
			json_t *e = json_object();
			json_object_set_new(e, "weapon_class", json_safe_string(Weapon_info[j].name));
			json_object_set_new(e, "used_in_wings", json_integer(weapon_usage[j]));
			json_array_append_new(missing_weapons, e);
		}
	}
	if (json_array_size(missing_weapons) > 0)
		json_object_set_new(obj, "wing_weapons_not_in_pool", missing_weapons);
	else
		json_decref(missing_weapons);

	return obj;
}

// ---------------------------------------------------------------------------
// Entry parsing
// ---------------------------------------------------------------------------

struct PendingLoadoutEntry {
	int class_idx = -1;			// Ship_info/Weapon_info index; valid when class_var is empty
	SCP_string class_var;		// string SEXP variable name, or empty for a static class
	int count = 0;				// literal count, or the cached value of count_var
	SCP_string count_var;		// number SEXP variable name, or empty for a literal count
	bool required = false;		// weapons only
};

// Per-pool differences between the ship and weapon variants.
struct LoadoutEntrySpec {
	const char *class_param;			// "ship_class" / "weapon_class"
	const char *class_label;			// for error messages
	const char *pool_label;				// "ship pool" / "weapon pool", for error messages
	const char *flag_requirement;		// for error messages
	int (*lookup)(const char *);		// class name -> index, or -1
	bool (*player_flagged)(int);		// eligibility flag check
	int max_entries;					// size of the Team_data arrays
};

static bool ship_player_flagged(int idx)
{
	return Ship_info[idx].flags[Ship::Info_Flags::Player_ship];
}

static bool weapon_player_flagged(int idx)
{
	return Weapon_info[idx].wi_flags[Weapon::Info_Flags::Player_allowed];
}

static const LoadoutEntrySpec Ship_entry_spec = {
	"ship_class", "ship class", "ship pool", "flagged as a player ship",
	ship_info_lookup, ship_player_flagged, MAX_SHIP_CLASSES
};

static const LoadoutEntrySpec Weapon_entry_spec = {
	"weapon_class", "weapon class", "weapon pool", "flagged as player-allowed",
	weapon_info_lookup, weapon_player_flagged, MAX_WEAPON_TYPES
};

// SEXP variables are referenced by name in loadout entries; accept the @name
// spelling used in SEXP text too.
static const char *strip_leading_at(const char *name)
{
	return (name && name[0] == '@') ? name + 1 : name;
}

// Look up a SEXP variable by name and require the given type flag.
// Returns the variable index, or -1 with a sink error.
static int lookup_loadout_variable(const char *name, int required_type, const char *type_label,
	const char *ctx, McpErrorSink &sink)
{
	name = strip_leading_at(name);
	int idx = get_index_sexp_variable_name(name);
	if (idx < 0) {
		sink.set_error("%s: SEXP variable not found: %s", ctx, name);
		return -1;
	}
	if (!(Sexp_variables[idx].type & required_type)) {
		sink.set_error("%s: SEXP variable '%s' must be a %s variable", ctx, name, type_label);
		return -1;
	}
	return idx;
}

// Parse and validate one pool array ("ships" or "weapons") into pending entries.
static bool parse_loadout_entries(json_t *arr, const LoadoutEntrySpec &spec, const char *param_name,
	bool allow_required, SCP_vector<PendingLoadoutEntry> &out, McpErrorSink &sink)
{
	if (!json_is_array(arr)) {
		sink.set_error("Parameter '%s' must be an array", param_name);
		return false;
	}
	if ((int)json_array_size(arr) > spec.max_entries) {
		sink.set_error("Parameter '%s' must have at most %d entries", param_name, spec.max_entries);
		return false;
	}

	SCP_vector<int> seen_classes, seen_class_vars;
	char ctx[64];

	size_t idx;
	json_t *elem;
	json_array_foreach(arr, idx, elem) {
		sprintf(ctx, "%s[" SIZE_T_ARG "]", param_name, idx);

		if (!json_is_object(elem)) {
			sink.set_error("%s must be an object", ctx);
			return false;
		}

		PendingLoadoutEntry entry;

		// class: literal name XOR string variable
		auto class_str = get_optional_string(elem, spec.class_param, sink, NAME_LENGTH - 1);
		if (sink.has_error()) return false;
		auto class_var_str = get_optional_string(elem, "class_variable", sink, TOKEN_LENGTH - 1);
		if (sink.has_error()) return false;

		if ((class_str != nullptr) == (class_var_str != nullptr)) {
			sink.set_error("%s must have exactly one of '%s' or 'class_variable'", ctx, spec.class_param);
			return false;
		}

		if (class_str) {
			entry.class_idx = spec.lookup(class_str);
			if (entry.class_idx < 0) {
				sink.set_error("%s: %s not found: %s", ctx, spec.class_label, class_str);
				return false;
			}
			if (!spec.player_flagged(entry.class_idx)) {
				sink.set_error("%s: %s '%s' cannot appear in the loadout because it is not %s",
					ctx, spec.class_label, class_str, spec.flag_requirement);
				return false;
			}
			for (int seen : seen_classes) {
				if (seen == entry.class_idx) {
					sink.set_error("%s: duplicate %s '%s'", ctx, spec.class_label, class_str);
					return false;
				}
			}
			seen_classes.push_back(entry.class_idx);
		} else {
			int var_idx = lookup_loadout_variable(class_var_str, SEXP_VARIABLE_STRING, "string", ctx, sink);
			if (var_idx < 0) return false;
			if (spec.lookup(Sexp_variables[var_idx].text) < 0) {
				sink.set_error("%s: SEXP variable '%s' currently holds '%s', which is not a valid %s",
					ctx, Sexp_variables[var_idx].variable_name, Sexp_variables[var_idx].text, spec.class_label);
				return false;
			}
			for (int seen : seen_class_vars) {
				if (seen == var_idx) {
					sink.set_error("%s: duplicate class_variable '%s'", ctx, Sexp_variables[var_idx].variable_name);
					return false;
				}
			}
			seen_class_vars.push_back(var_idx);
			entry.class_var = Sexp_variables[var_idx].variable_name;	// canonical spelling
		}

		// count: literal XOR number variable
		bool has_count = json_object_get(elem, "count") != nullptr;
		auto count_var_str = get_optional_string(elem, "count_variable", sink, TOKEN_LENGTH - 1);
		if (sink.has_error()) return false;

		if (has_count == (count_var_str != nullptr)) {
			sink.set_error("%s must have exactly one of 'count' or 'count_variable'", ctx);
			return false;
		}

		if (has_count) {
			auto count_opt = get_required_integer(elem, "count", sink);
			if (!count_opt.has_value()) return false;
			char count_param[80];
			sprintf(count_param, "%s.count", ctx);
			if (!check_int_range(*count_opt, LOADOUT_COUNT_MIN, LOADOUT_COUNT_MAX, count_param, sink))
				return false;
			entry.count = *count_opt;
		} else {
			int var_idx = lookup_loadout_variable(count_var_str, SEXP_VARIABLE_NUMBER, "number", ctx, sink);
			if (var_idx < 0) return false;
			entry.count_var = Sexp_variables[var_idx].variable_name;
			entry.count = atoi(Sexp_variables[var_idx].text);	// cached value, same as the dialog's OnOK
		}

		// required (weapons only): mirrors the dialog, which only offers the
		// flag for weapons present in the static pool with a positive count
		if (allow_required) {
			auto required_opt = get_optional_bool(elem, "required", sink);
			if (sink.has_error()) return false;
			if (required_opt.has_value() && *required_opt) {
				if (!entry.class_var.empty()) {
					sink.set_error("%s: only static weapon classes can be required for the mission "
						"(this entry's class comes from a variable)", ctx);
					return false;
				}
				if (entry.count < 1) {
					sink.set_error("%s: a required weapon must have a count of at least 1", ctx);
					return false;
				}
				entry.required = true;
			}
		}

		out.push_back(std::move(entry));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Applying pending entries to Team_data
// ---------------------------------------------------------------------------

static void apply_ship_entries(team_data &td, const SCP_vector<PendingLoadoutEntry> &entries)
{
	for (int i = 0; i < (int)entries.size(); i++) {
		const auto &e = entries[i];
		if (!e.class_var.empty()) {
			strcpy_s(td.ship_list_variables[i], e.class_var.c_str());
			td.ship_list[i] = -1;
		} else {
			td.ship_list[i] = e.class_idx;
			td.ship_list_variables[i][0] = '\0';
		}
		td.ship_count[i] = e.count;
		strcpy_s(td.ship_count_variables[i], e.count_var.c_str());
	}
	td.num_ship_choices = (int)entries.size();
}

static void apply_weapon_entries(team_data &td, const SCP_vector<PendingLoadoutEntry> &entries)
{
	for (int j = 0; j < MAX_WEAPON_TYPES; j++)
		td.weapon_required[j] = false;

	for (int i = 0; i < (int)entries.size(); i++) {
		const auto &e = entries[i];
		if (!e.class_var.empty()) {
			strcpy_s(td.weaponry_pool_variable[i], e.class_var.c_str());
			td.weaponry_pool[i] = -1;
		} else {
			td.weaponry_pool[i] = e.class_idx;
			td.weaponry_pool_variable[i][0] = '\0';
			if (e.required)
				td.weapon_required[e.class_idx] = true;
		}
		td.weaponry_count[i] = e.count;
		strcpy_s(td.weaponry_amount_variable[i], e.count_var.c_str());
	}
	td.num_weapon_choices = (int)entries.size();
}

// Remove one entry from a team's ship pool, shifting later entries down.
static void remove_ship_entry(team_data &td, int idx)
{
	for (int j = idx; j < td.num_ship_choices - 1; j++) {
		td.ship_list[j] = td.ship_list[j + 1];
		strcpy_s(td.ship_list_variables[j], td.ship_list_variables[j + 1]);
		td.ship_count[j] = td.ship_count[j + 1];
		strcpy_s(td.ship_count_variables[j], td.ship_count_variables[j + 1]);
	}
	td.num_ship_choices--;
}

// Remove one entry from a team's weapon pool, shifting later entries down.
static void remove_weapon_entry(team_data &td, int idx)
{
	for (int j = idx; j < td.num_weapon_choices - 1; j++) {
		td.weaponry_pool[j] = td.weaponry_pool[j + 1];
		strcpy_s(td.weaponry_pool_variable[j], td.weaponry_pool_variable[j + 1]);
		td.weaponry_count[j] = td.weaponry_count[j + 1];
		strcpy_s(td.weaponry_amount_variable[j], td.weaponry_amount_variable[j + 1]);
	}
	td.num_weapon_choices--;
}

// ---------------------------------------------------------------------------
// Tool handlers
// ---------------------------------------------------------------------------

static void handle_get_team_loadout(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_loadout, sink)) return;

	json_t *obj = json_object();
	json_object_set_new(obj, "num_teams", json_integer(Num_teams));
	json_t *teams = json_array();
	for (int i = 0; i < MAX_TVT_TEAMS; i++)
		json_array_append_new(teams, build_team_loadout_json(i));
	json_object_set_new(obj, "teams", teams);

	req->result_json = make_json_tool_result(obj);
	req->success = true;
}

static void handle_update_team_loadout(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_loadout, sink)) return;

	int team = resolve_loadout_team(input, sink);
	if (team < 0) return;

	auto do_not_validate = get_optional_bool(input, "do_not_validate", sink);
	if (sink.has_error()) return;

	json_t *ships_in = input ? json_object_get(input, "ships") : nullptr;
	json_t *weapons_in = input ? json_object_get(input, "weapons") : nullptr;

	// --- Phase 1: validate everything (no mutations yet) ---
	SCP_vector<PendingLoadoutEntry> pending_ships, pending_weapons;
	if (ships_in && !parse_loadout_entries(ships_in, Ship_entry_spec, "ships", false, pending_ships, sink))
		return;
	if (weapons_in && !parse_loadout_entries(weapons_in, Weapon_entry_spec, "weapons", true, pending_weapons, sink))
		return;

	// --- Phase 2: apply ---
	team_data &td = Team_data[team];
	bool changed = false;

	if (ships_in) {
		apply_ship_entries(td, pending_ships);
		changed = true;
	}
	if (weapons_in) {
		apply_weapon_entries(td, pending_weapons);
		changed = true;
	}
	if (do_not_validate.has_value() && td.do_not_validate != *do_not_validate) {
		td.do_not_validate = *do_not_validate;
		changed = true;
	}

	if (changed)
		mark_modified("MCP: update team loadout for %s", team_name_from_index(team));

	req->result_json = make_json_tool_result(build_team_loadout_json(team));
	req->success = true;
}

// A uniform view over the ship or weapon parallel arrays in team_data, so the
// upsert handler can be written once for both pools.
struct LoadoutPoolView {
	int *class_arr;
	char (*class_var_arr)[TOKEN_LENGTH];
	int *count_arr;
	char (*count_var_arr)[TOKEN_LENGTH];
	int *num_choices;
};

static LoadoutPoolView ship_pool_view(team_data &td)
{
	return { td.ship_list, td.ship_list_variables, td.ship_count, td.ship_count_variables, &td.num_ship_choices };
}

static LoadoutPoolView weapon_pool_view(team_data &td)
{
	return { td.weaponry_pool, td.weaponry_pool_variable, td.weaponry_count, td.weaponry_amount_variable, &td.num_weapon_choices };
}

// set_team_loadout_ship / set_team_loadout_weapon: upsert or remove a single
// pool entry, keyed by its static class or class-variable name.  Follows the
// set_reinforcement pattern: enable=false removes; otherwise the entry is
// created (count required) or partially updated (only provided fields change).
static void handle_set_team_loadout_entry(json_t *input, McpToolRequest *req,
	const LoadoutEntrySpec &spec, bool is_weapon)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_loadout, sink)) return;

	int team = resolve_loadout_team(input, sink);
	if (team < 0) return;

	team_data &td = Team_data[team];
	LoadoutPoolView pool = is_weapon ? weapon_pool_view(td) : ship_pool_view(td);

	// --- Phase 1: parse and validate everything (no mutations yet) ---

	// key: static class XOR class variable
	auto class_str = get_optional_string(input, spec.class_param, sink, NAME_LENGTH - 1);
	if (sink.has_error()) return;
	auto class_var_str = get_optional_string(input, "class_variable", sink, TOKEN_LENGTH - 1);
	if (sink.has_error()) return;

	if ((class_str != nullptr) == (class_var_str != nullptr)) {
		sink.set_error("Exactly one of '%s' or 'class_variable' is required", spec.class_param);
		return;
	}

	int class_idx = -1;
	const char *class_var_name = nullptr;	// canonical spelling
	int class_var_idx = -1;
	if (class_str) {
		class_idx = spec.lookup(class_str);
		if (class_idx < 0) {
			sink.set_error("%s not found: %s", spec.class_label, class_str);
			return;
		}
	} else {
		class_var_idx = lookup_loadout_variable(class_var_str, SEXP_VARIABLE_STRING, "string",
			"class_variable", sink);
		if (class_var_idx < 0) return;
		class_var_name = Sexp_variables[class_var_idx].variable_name;
	}

	auto enable = get_optional_bool(input, "enable", sink);
	if (sink.has_error()) return;
	bool removing = enable.has_value() && !*enable;

	// Eligibility only matters when the entry will remain in the pool, so an
	// ineligible class that somehow got into a mission can still be removed.
	if (!removing) {
		if (class_str) {
			if (!spec.player_flagged(class_idx)) {
				sink.set_error("%s '%s' cannot appear in the loadout because it is not %s",
					spec.class_label, class_str, spec.flag_requirement);
				return;
			}
		} else if (spec.lookup(Sexp_variables[class_var_idx].text) < 0) {
			sink.set_error("SEXP variable '%s' currently holds '%s', which is not a valid %s",
				class_var_name, Sexp_variables[class_var_idx].text, spec.class_label);
			return;
		}
	}

	// find the existing entry for this key
	int found = -1;
	for (int i = 0; i < *pool.num_choices; i++) {
		if (class_var_name) {
			if (!strcmp(pool.class_var_arr[i], class_var_name)) {
				found = i;
				break;
			}
		} else if (pool.class_var_arr[i][0] == '\0' && pool.class_arr[i] == class_idx) {
			found = i;
			break;
		}
	}

	if (removing) {
		if (found < 0) {
			set_not_found_error(sink, "Loadout entry", class_str ? class_str : class_var_name);
			return;
		}
		if (is_weapon && pool.class_var_arr[found][0] == '\0' && pool.class_arr[found] >= 0)
			td.weapon_required[pool.class_arr[found]] = false;
		if (is_weapon)
			remove_weapon_entry(td, found);
		else
			remove_ship_entry(td, found);

		mark_modified("MCP: remove %s loadout entry for %s", spec.class_label, team_name_from_index(team));
		req->result_json = make_json_tool_result(build_team_loadout_json(team));
		req->success = true;
		return;
	}

	// count: literal XOR number variable; required on create, optional on update
	bool has_count = input && json_object_get(input, "count") != nullptr;
	auto count_var_str = get_optional_string(input, "count_variable", sink, TOKEN_LENGTH - 1);
	if (sink.has_error()) return;
	if (has_count && count_var_str) {
		sink.set_error("Provide at most one of 'count' or 'count_variable'");
		return;
	}

	bool count_touched = false;
	int new_count = 0;
	const char *new_count_var = "";
	if (has_count) {
		auto count_opt = get_required_integer(input, "count", sink);
		if (!count_opt.has_value()) return;
		if (!check_int_range(*count_opt, LOADOUT_COUNT_MIN, LOADOUT_COUNT_MAX, "count", sink)) return;
		new_count = *count_opt;
		count_touched = true;
	} else if (count_var_str) {
		int var_idx = lookup_loadout_variable(count_var_str, SEXP_VARIABLE_NUMBER, "number",
			"count_variable", sink);
		if (var_idx < 0) return;
		new_count_var = Sexp_variables[var_idx].variable_name;
		new_count = atoi(Sexp_variables[var_idx].text);	// cached value, same as the dialog's OnOK
		count_touched = true;
	}

	if (found < 0) {
		if (!count_touched) {
			sink.set_error("Creating a new loadout entry requires 'count' or 'count_variable'");
			return;
		}
		if (*pool.num_choices >= spec.max_entries) {
			sink.set_error("Cannot add more than %d %s entries", spec.max_entries, spec.pool_label);
			return;
		}
	}

	// required (weapons only): same constraints as update_team_loadout
	std::optional<bool> required_opt;
	if (is_weapon) {
		required_opt = get_optional_bool(input, "required", sink);
		if (sink.has_error()) return;
		if (required_opt.has_value() && *required_opt) {
			if (class_var_name) {
				sink.set_error("Only static weapon classes can be required for the mission "
					"(this entry's class comes from a variable)");
				return;
			}
			int effective_count = count_touched ? new_count : pool.count_arr[found];
			if (effective_count < 1) {
				sink.set_error("A required weapon must have a count of at least 1");
				return;
			}
		}
	}

	// --- Phase 2: apply ---
	bool changed = false;
	if (found < 0) {
		int slot = (*pool.num_choices)++;
		if (class_var_name) {
			strcpy_s(pool.class_var_arr[slot], class_var_name);
			pool.class_arr[slot] = -1;
		} else {
			pool.class_arr[slot] = class_idx;
			pool.class_var_arr[slot][0] = '\0';
		}
		pool.count_arr[slot] = new_count;
		strcpy_s(pool.count_var_arr[slot], new_count_var);
		changed = true;
	} else if (count_touched &&
		(strcmp(pool.count_var_arr[found], new_count_var) != 0 || pool.count_arr[found] != new_count)) {
		pool.count_arr[found] = new_count;
		strcpy_s(pool.count_var_arr[found], new_count_var);
		changed = true;
	}

	if (is_weapon && required_opt.has_value() && class_idx >= 0
		&& td.weapon_required[class_idx] != *required_opt) {
		td.weapon_required[class_idx] = *required_opt;
		changed = true;
	}

	if (changed)
		mark_modified("MCP: set %s loadout entry for %s", spec.class_label, team_name_from_index(team));

	req->result_json = make_json_tool_result(build_team_loadout_json(team));
	req->success = true;
}

static void handle_set_team_loadout_ship(json_t *input, McpToolRequest *req)
{
	handle_set_team_loadout_entry(input, req, Ship_entry_spec, false);
}

static void handle_set_team_loadout_weapon(json_t *input, McpToolRequest *req)
{
	handle_set_team_loadout_entry(input, req, Weapon_entry_spec, true);
}

// ---------------------------------------------------------------------------
// Loadout variable-reference helpers (see header)
// ---------------------------------------------------------------------------

int mcp_count_loadout_variable_refs(const char *var_name)
{
	if (!var_name || !var_name[0])
		return 0;

	int count = 0;
	for (int i = 0; i < MAX_TVT_TEAMS; i++) {
		const team_data &td = Team_data[i];
		for (int idx = 0; idx < td.num_ship_choices; idx++) {
			if (!strcmp(td.ship_list_variables[idx], var_name))
				count++;
			if (!strcmp(td.ship_count_variables[idx], var_name))
				count++;
		}
		for (int idx = 0; idx < td.num_weapon_choices; idx++) {
			if (!strcmp(td.weaponry_pool_variable[idx], var_name))
				count++;
			if (!strcmp(td.weaponry_amount_variable[idx], var_name))
				count++;
		}
	}
	return count;
}

int mcp_rename_loadout_variable_refs(const char *old_name, const char *new_name)
{
	if (!old_name || !old_name[0])
		return 0;

	int count = 0;
	for (int i = 0; i < MAX_TVT_TEAMS; i++) {
		team_data &td = Team_data[i];
		for (int idx = 0; idx < td.num_ship_choices; idx++) {
			if (!strcmp(td.ship_list_variables[idx], old_name)) {
				strcpy_s(td.ship_list_variables[idx], new_name);
				count++;
			}
			if (!strcmp(td.ship_count_variables[idx], old_name)) {
				strcpy_s(td.ship_count_variables[idx], new_name);
				count++;
			}
		}
		for (int idx = 0; idx < td.num_weapon_choices; idx++) {
			if (!strcmp(td.weaponry_pool_variable[idx], old_name)) {
				strcpy_s(td.weaponry_pool_variable[idx], new_name);
				count++;
			}
			if (!strcmp(td.weaponry_amount_variable[idx], old_name)) {
				strcpy_s(td.weaponry_amount_variable[idx], new_name);
				count++;
			}
		}
	}
	return count;
}

int mcp_clear_loadout_variable_refs(const char *var_name)
{
	if (!var_name || !var_name[0])
		return 0;

	int count = 0;
	for (int i = 0; i < MAX_TVT_TEAMS; i++) {
		team_data &td = Team_data[i];
		for (int idx = td.num_ship_choices - 1; idx >= 0; idx--) {
			if (!strcmp(td.ship_list_variables[idx], var_name)) {
				count++;
				if (!strcmp(td.ship_count_variables[idx], var_name))
					count++;	// both references die with the entry
				remove_ship_entry(td, idx);
			} else if (!strcmp(td.ship_count_variables[idx], var_name)) {
				td.ship_count_variables[idx][0] = '\0';	// falls back to the cached literal count
				count++;
			}
		}
		for (int idx = td.num_weapon_choices - 1; idx >= 0; idx--) {
			if (!strcmp(td.weaponry_pool_variable[idx], var_name)) {
				count++;
				if (!strcmp(td.weaponry_amount_variable[idx], var_name))
					count++;
				remove_weapon_entry(td, idx);
			} else if (!strcmp(td.weaponry_amount_variable[idx], var_name)) {
				td.weaponry_amount_variable[idx][0] = '\0';
				count++;
			}
		}
	}
	return count;
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_loadout_tools(json_t *tools)
{
	static const char *loadout_team_desc =
		"Which team's loadout to operate on (\"Team 1\" or \"Team 2\"). "
		"Defaults to \"Team 1\". \"none\" is not valid for loadouts. "
		"Team 2 is only meaningful in team-versus-team missions but may be edited at any time.";

	// get_team_loadout
	register_tool(tools, "get_team_loadout",
		"Returns the Team Loadout data for all teams: for each team, the ship pool and weapon pool "
		"(entries have either a literal class or a string-variable class, and either a literal count "
		"or a number-variable count), required weapons, the do_not_validate flag, and how many of "
		"each pooled class the team's starting wings already use. For entries with a literal class "
		"and count, the count means extras available beyond what the starting wings carry; "
		"variable-based counts are absolute. Any wing-carried classes missing from the pool are "
		"reported under wing_ships_not_in_pool / wing_weapons_not_in_pool.",
		json_object());

	// update_team_loadout
	{
		json_t *props = json_object();
		add_string_enum_prop(props, "team", loadout_team_desc, team_enum_values);
		add_bool_prop(props, "do_not_validate",
			"If true, the loadout is saved exactly as entered: FRED will not pad the weapon pool "
			"with weapons carried by the starting wings.");

		// ships
		{
			json_t *item_props = json_object();
			add_string_prop(item_props, "ship_class",
				"Ship class name (must be flagged as a player ship). Mutually exclusive with class_variable.");
			add_string_prop(item_props, "class_variable",
				"Name of a string SEXP variable whose value is a ship class name. "
				"Mutually exclusive with ship_class. Use list_sexp_variables to see variables.");
			add_integer_prop(item_props, "count",
				"Number of ships of this class available beyond those used by starting wings (0-9999). "
				"Mutually exclusive with count_variable.");
			add_string_prop(item_props, "count_variable",
				"Name of a number SEXP variable providing the count. Mutually exclusive with count.");
			add_object_array_prop(props, "ships",
				"Ship pool entries. REPLACES the team's entire ship pool (including variable-based "
				"entries); use get_team_loadout first and include any entries you want to keep. "
				"Pass [] to clear the pool.",
				item_props);
		}

		// weapons
		{
			json_t *item_props = json_object();
			add_string_prop(item_props, "weapon_class",
				"Weapon class name (must be flagged as player-allowed). Mutually exclusive with class_variable.");
			add_string_prop(item_props, "class_variable",
				"Name of a string SEXP variable whose value is a weapon class name. "
				"Mutually exclusive with weapon_class. Use list_sexp_variables to see variables.");
			add_integer_prop(item_props, "count",
				"Number of weapons of this class available beyond those carried by starting wings (0-9999). "
				"Mutually exclusive with count_variable.");
			add_string_prop(item_props, "count_variable",
				"Name of a number SEXP variable providing the count. Mutually exclusive with count.");
			add_bool_prop(item_props, "required",
				"If true, the player must take at least one of this weapon into the mission. "
				"Only valid for a static weapon_class with a count of at least 1.");
			add_object_array_prop(props, "weapons",
				"Weapon pool entries. REPLACES the team's entire weapon pool (including variable-based "
				"entries) and its required-weapon flags; use get_team_loadout first and include any "
				"entries you want to keep. Pass [] to clear the pool.",
				item_props);
		}

		register_tool(tools, "update_team_loadout",
			"Update a team's loadout (Team Loadout editor). All parameters are optional; the ships "
			"and weapons arrays each REPLACE that entire pool when provided, while omitted parameters "
			"are left unchanged. Returns the team's full updated loadout.",
			props);
	}

	// set_team_loadout_ship
	{
		json_t *props = json_object();
		add_string_enum_prop(props, "team", loadout_team_desc, team_enum_values);
		add_string_prop(props, "ship_class",
			"Ship class of the entry to create, update, or remove (must be flagged as a player ship). "
			"Mutually exclusive with class_variable.");
		add_string_prop(props, "class_variable",
			"Name of a string SEXP variable identifying the entry (its value must be a ship class name). "
			"Mutually exclusive with ship_class.");
		add_integer_prop(props, "count",
			"Number of ships of this class available beyond those used by starting wings (0-9999). "
			"Required when creating a new entry (unless count_variable is given); optional on update. "
			"Mutually exclusive with count_variable.");
		add_string_prop(props, "count_variable",
			"Name of a number SEXP variable providing the count. Mutually exclusive with count.");
		add_bool_prop(props, "enable",
			"Pass false to remove the entry from the pool (other parameters are ignored). Defaults to true.");
		register_tool(tools, "set_team_loadout_ship",
			"Create, update, or remove a single ship pool entry in a team's loadout, keyed by "
			"ship_class or class_variable. Existing entries are partially updated (only provided "
			"fields change); enable=false removes the entry. Use update_team_loadout to replace "
			"the whole pool at once. Returns the team's full updated loadout.",
			props);
	}

	// set_team_loadout_weapon
	{
		json_t *props = json_object();
		add_string_enum_prop(props, "team", loadout_team_desc, team_enum_values);
		add_string_prop(props, "weapon_class",
			"Weapon class of the entry to create, update, or remove (must be flagged as player-allowed). "
			"Mutually exclusive with class_variable.");
		add_string_prop(props, "class_variable",
			"Name of a string SEXP variable identifying the entry (its value must be a weapon class name). "
			"Mutually exclusive with weapon_class.");
		add_integer_prop(props, "count",
			"Number of weapons of this class available beyond those carried by starting wings (0-9999). "
			"Required when creating a new entry (unless count_variable is given); optional on update. "
			"Mutually exclusive with count_variable.");
		add_string_prop(props, "count_variable",
			"Name of a number SEXP variable providing the count. Mutually exclusive with count.");
		add_bool_prop(props, "required",
			"If true, the player must take at least one of this weapon into the mission. "
			"Only valid for a static weapon_class with a count of at least 1. "
			"Removing the entry clears the flag.");
		add_bool_prop(props, "enable",
			"Pass false to remove the entry from the pool (other parameters are ignored). Defaults to true.");
		register_tool(tools, "set_team_loadout_weapon",
			"Create, update, or remove a single weapon pool entry in a team's loadout, keyed by "
			"weapon_class or class_variable. Existing entries are partially updated (only provided "
			"fields change); enable=false removes the entry and clears its required flag. Use "
			"update_team_loadout to replace the whole pool at once. Returns the team's full "
			"updated loadout.",
			props);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_loadout_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "get_team_loadout") == 0) {
		handle_get_team_loadout(input_json, req);
	} else if (strcmp(tool_name, "update_team_loadout") == 0) {
		handle_update_team_loadout(input_json, req);
	} else if (strcmp(tool_name, "set_team_loadout_ship") == 0) {
		handle_set_team_loadout_ship(input_json, req);
	} else if (strcmp(tool_name, "set_team_loadout_weapon") == 0) {
		handle_set_team_loadout_weapon(input_json, req);
	} else {
		return false;
	}
	return true;
}
