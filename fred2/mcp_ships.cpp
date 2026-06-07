#include "stdafx.h"
#include "mcp_ships.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcp_sexp.h"
#include "mcp_sexp_forest.h"
#include "mcpserver.h"

#include <jansson.h>
#include <climits>
#include <cstring>
#include <optional>

#include "globalincs/utility.h"

#include "ai/ai.h"
#include "iff_defs/iff_defs.h"
#include "math/floating.h"            // fl2ir
#include "mission/missionmessage.h"   // Personas
#include "mission/missionparse.h"
#include "missioneditor/common.h"     // target_to_anchor, anchor_to_target, stuff_special_arrival_anchor_name
#include "object/object.h"
#include "parse/parselo.h"
#include "parse/sexp.h"
#include "ship/anchor_t.h"
#include "ship/ship.h"
#include "weapon/weapon.h"            // Weapon_info, weapon_info_lookup, Weapon::Info_Flags

#include "fred.h"                     // Ship_editor_dialog
#include "management.h"               // create_ship, delete_ship, rename_ship, invalidate_references, etc.

// Engine helpers used to resolve flag names and apply them across the
// object/ship/parse/AI flag enums.  Same pattern as scripting/api/objs/ship.cpp.
extern bool sexp_check_flag_arrays(const char *flag_name,
	Object::Object_Flags &object_flag, Ship::Ship_Flags &ship_flag,
	Mission::Parse_Object_Flags &parse_obj_flag, AI::AI_Flags &ai_flag);
extern void sexp_alter_ship_flag_helper(object_ship_wing_point_team &oswpt,
	bool future_ships, Object::Object_Flags object_flag, Ship::Ship_Flags ship_flag,
	Mission::Parse_Object_Flags parse_obj_flag, AI::AI_Flags ai_flag, bool set_flag);

// ---------------------------------------------------------------------------
// Dialog conflict guard
// ---------------------------------------------------------------------------

static bool validate_dialog_for_ships(SCP_string &error_msg)
{
	return validate_single_dialog("ships", "ship", error_msg);
}

// ---------------------------------------------------------------------------
// Ship JSON serialization
// ---------------------------------------------------------------------------

static json_t *build_ship_flags_array(int ship_idx);

static json_t *build_ship_json(int ship_idx, bool include_details)
{
	const ship &shipp = Ships[ship_idx];
	const object &objp = Objects[shipp.objnum];

	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_safe_string(shipp.ship_name));
	json_object_set_new(obj, "ship_class",
		json_safe_string(Ship_info[shipp.ship_info_index].name));
	json_object_set_new(obj, "team",
		json_safe_string(Iff_info[shipp.team].iff_name));
	json_object_set_new(obj, "is_player_start", json_boolean(objp.type == OBJ_START));

	if (shipp.wingnum >= 0) {
		json_object_set_new(obj, "in_wing", json_true());
		json_object_set_new(obj, "wing", json_safe_string(Wings[shipp.wingnum].name));
	} else {
		json_object_set_new(obj, "in_wing", json_false());
	}

	// Spatial
	json_object_set_new(obj, "position", build_vec3d_json(objp.pos));
	json_object_set_new(obj, "orientation", build_matrix_json(objp.orient));

	if (!include_details)
		return obj;

	// Initial state at mission start (percent of class max).  In FRED edit mode
	// the engine stores these as 0-100 percents directly in the runtime fields
	// (see code/ship/ship.cpp Fred_running branches) so that missionsave can
	// emit them as "+Initial Hull/Shields/Velocity:" without conversion.
	// initial_shield_percent is omitted when the no-shields flag is set (the
	// stored value is inert), but updates remain permitted so callers can
	// pre-stage a value for when the flag is later turned off.
	json_object_set_new(obj, "initial_hull_percent",   json_integer((int)objp.hull_strength));
	if (!objp.flags[Object::Object_Flags::No_shields])
		json_object_set_new(obj, "initial_shield_percent", json_integer((int)objp.shield_quadrant[0]));
	json_object_set_new(obj, "initial_speed_percent",  json_integer((int)objp.phys_info.speed));

	// Identity
	if (shipp.has_display_name())
		json_object_set_new(obj, "display_name", json_safe_string(shipp.display_name.c_str()));
	json_object_set_new(obj, "ai_class", json_safe_string(Ai_class_names[shipp.weapons.ai_class]));
	if (shipp.cargo1 != 0 && Cargo_names[(int)shipp.cargo1] != nullptr)
		json_object_set_new(obj, "cargo", json_safe_string(Cargo_names[(int)shipp.cargo1]));
	if (shipp.hotkey >= 0)
		json_object_set_new(obj, "hotkey", json_integer(shipp.hotkey));
	if (Personas.in_bounds(shipp.persona_index))
		json_object_set_new(obj, "persona", json_safe_string(Personas[shipp.persona_index].name));
	if (*Fred_alt_names[ship_idx])
		json_object_set_new(obj, "alt_class_name", json_safe_string(Fred_alt_names[ship_idx]));
	if (*Fred_callsigns[ship_idx])
		json_object_set_new(obj, "callsign", json_safe_string(Fred_callsigns[ship_idx]));

	// Scoring
	json_object_set_new(obj, "score", json_integer(shipp.score));
	json_object_set_new(obj, "assist_score_fraction", json_real(shipp.assist_score_pct));

	// Arrival
	json_object_set_new(obj, "arrival_location",
		json_safe_string(Arrival_location_names[static_cast<int>(shipp.arrival_location)]));
	auto arr_tgt = anchor_to_name(shipp.arrival_anchor);
	if (!arr_tgt.empty())
		json_object_set_new(obj, "arrival_target", json_safe_string(arr_tgt.c_str()));
	json_object_set_new(obj, "arrival_distance", json_integer(shipp.arrival_distance));
	json_object_set_new(obj, "arrival_delay", json_integer(shipp.arrival_delay));
	json_object_set_new(obj, "arrival_cue", json_integer(shipp.arrival_cue));

	// Departure
	json_object_set_new(obj, "departure_location",
		json_safe_string(Departure_location_names[static_cast<int>(shipp.departure_location)]));
	auto dep_tgt = anchor_to_name(shipp.departure_anchor);
	if (!dep_tgt.empty())
		json_object_set_new(obj, "departure_target", json_safe_string(dep_tgt.c_str()));
	json_object_set_new(obj, "departure_delay", json_integer(shipp.departure_delay));
	json_object_set_new(obj, "departure_cue", json_integer(shipp.departure_cue));

	// Flags (cross-enum: object/ship/parse-object/AI; see list_ship_flags)
	json_object_set_new(obj, "ship_flags", build_ship_flags_array(ship_idx));

	// Flag-paired scalars: emitted only when the matching flag is set, mirroring
	// the FRED ship-flags dialog's "checkbox + spinner" model.  The scalars are
	// independently stored in the ship/AI struct (qtFRED writes them
	// unconditionally) -- the flag governs interpretation, not storage.
	if (shipp.flags[Ship::Ship_Flags::Escort])
		json_object_set_new(obj, "escort_priority", json_integer(shipp.escort_priority));
	if (Ai_info[shipp.ai_index].ai_flags[AI::AI_Flags::Kamikaze])
		json_object_set_new(obj, "kamikaze_damage",
			json_integer(Ai_info[shipp.ai_index].kamikaze_damage));
	// Kill_before_mission has no parse-flag counterpart and isn't in ship_flags;
	// the scalar field carries both presence and value.
	if (shipp.flags[Ship::Ship_Flags::Kill_before_mission])
		json_object_set_new(obj, "destroy_before_mission_seconds",
			json_integer(shipp.final_death_time));

	return obj;
}

// ---------------------------------------------------------------------------
// Display name helper - "<none>" / matches ship name => clear the flag
// ---------------------------------------------------------------------------

static void apply_display_name(ship &shipp, const char *display_name)
{
	// A blank display name is a valid display name.  To remove a display
	// name, pass "<none>" or a string matching the ship's regular name.
	if (!stricmp(display_name, "<none>") || !strcmp(display_name, shipp.ship_name)) {
		shipp.display_name = "";
		shipp.flags.remove(Ship::Ship_Flags::Has_display_name);
	} else {
		shipp.display_name = display_name;
		shipp.flags.set(Ship::Ship_Flags::Has_display_name);
	}
}

// ---------------------------------------------------------------------------
// Cargo helper - mirrors the editor's auto-add logic
// ---------------------------------------------------------------------------

static bool resolve_or_add_cargo(const char *cargo_name, char &out_index, McpErrorSink &sink)
{
	if (!cargo_name || !*cargo_name) {
		out_index = 0;	// "Nothing"
		return true;
	}

	// Cast to size_t to disambiguate between Management.h's CString-based
	// string_lookup(const CString&, char*[], int) and parselo.h's templated
	// string_lookup(const char*, const T&, size_t, ...) -- with int->size_t
	// explicit, the template wins by exact match.
	int z = string_lookup(cargo_name, Cargo_names, (size_t)Num_cargo);
	if (z >= 0) {
		out_index = (char)z;
		return true;
	}

	if (Num_cargo >= MAX_CARGO) {
		sink.set_error("Cannot add cargo '%s': maximum number of cargo names (%d) already in use.",
			cargo_name, MAX_CARGO);
		return false;
	}

	z = Num_cargo++;
	strcpy(Cargo_names[z], cargo_name);
	out_index = (char)z;
	return true;
}

// ---------------------------------------------------------------------------
// Alt-name / callsign helpers (shared by create + update)
// ---------------------------------------------------------------------------

enum class apply_type { ALT_NAME, CALLSIGN };
static void apply_helper(int ship_idx, const char *new_text, apply_type type)
{
	auto apply_array = (type == apply_type::ALT_NAME) ? Fred_alt_names : Fred_callsigns;
	auto lookup_func = (type == apply_type::ALT_NAME) ? mission_parse_lookup_alt : mission_parse_lookup_callsign;
	auto remove_func = (type == apply_type::ALT_NAME) ? mission_parse_remove_alt : mission_parse_remove_callsign;
	auto add_func =    (type == apply_type::ALT_NAME) ? mission_parse_add_alt : mission_parse_add_callsign;

	char *slot = apply_array[ship_idx];

	// Treat empty / "<none>" / "none" as clearing.
	bool clearing = !new_text || !*new_text
		|| !stricmp(new_text, "<none>") || !stricmp(new_text, "none");

	if (clearing) {
		if (*slot) {
			// Remove from the mission-wide pool only if no other ship still uses it.
			bool used = false;
			for (int i = 0; i < MAX_SHIPS; ++i) {
				if (i != ship_idx && !strcmp(apply_array[i], slot)) {
					used = true;
					break;
				}
			}
			if (!used)
				remove_func(slot);
			*slot = '\0';
		}
		return;
	}

	// No change?
	if (!strcmp(slot, new_text))
		return;

	// If the old text was in use elsewhere too, leave it in the pool; otherwise drop it.
	if (*slot) {
		bool used = false;
		for (int i = 0; i < MAX_SHIPS; ++i) {
			if (i != ship_idx && !strcmp(apply_array[i], slot)) {
				used = true;
				break;
			}
		}
		if (!used)
			remove_func(slot);
	}

	// Add the new text (skip if already in the pool, matching the editor's behavior).
	if (lookup_func(new_text) < 0)
		add_func(new_text);
	strcpy_s(apply_array[ship_idx], new_text);
}

// ---------------------------------------------------------------------------
// Ship flags (cross-enum dispatch via the engine's sexp_check_flag_arrays /
// sexp_alter_ship_flag_helper -- same helpers used by Ship:getFlag / setFlag).
// ---------------------------------------------------------------------------

// don't include these in the valid flags for MCP for various reasons:
//   player-start: edits go through is_player_start (manages Player_starts and OBJ_START)
//   immobile:     deprecated, callers should set don't-change-position + don't-change-orientation
//   locked:       deprecated, callers should set ship-locked + weapons-locked
bool mcp_ship_flag_excluded(const Mission::Parse_Object_Flags &candidate)
{
	return candidate == Mission::Parse_Object_Flags::OF_Player_start || candidate == Mission::Parse_Object_Flags::OF_Immobile || candidate == Mission::Parse_Object_Flags::SF_Locked;
}

// Resolve a flag name to the runtime enum(s) sexp_alter_ship_flag_helper can apply.
//
// sexp_check_flag_arrays handles names in the engine's SEXP runtime tables directly.
// Names that only resolve to a parse-only flag (e.g. "kamikaze", "ignore-count",
// "reinforcement") would otherwise route through the helper's OSWPT_TYPE_PARSE_OBJECT
// branch and no-op on live FRED-created ships.  For those, we project the parse flag
// onto its corresponding runtime enum(s) via resolve_parse_flags.
//
// Returns true if at least one enum was resolved.
static bool resolve_ship_flag_name(const char *name,
	Object::Object_Flags &object_flag, Ship::Ship_Flags &ship_flag,
	Mission::Parse_Object_Flags &parse_obj_flag, AI::AI_Flags &ai_flag)
{
	object_flag = Object::Object_Flags::NUM_VALUES;
	ship_flag = Ship::Ship_Flags::NUM_VALUES;
	parse_obj_flag = Mission::Parse_Object_Flags::NUM_VALUES;
	ai_flag = AI::AI_Flags::NUM_VALUES;

	sexp_check_flag_arrays(name, object_flag, ship_flag, parse_obj_flag, ai_flag);

	// Honor the MCP exclusion list regardless of which enum tables also matched.
	// (Object_flag_names contains "immobile" too, so the projection-block guard
	// alone wouldn't catch it.)
	if (parse_obj_flag != Mission::Parse_Object_Flags::NUM_VALUES && mcp_ship_flag_excluded(parse_obj_flag)) {
		object_flag    = Object::Object_Flags::NUM_VALUES;
		ship_flag      = Ship::Ship_Flags::NUM_VALUES;
		parse_obj_flag = Mission::Parse_Object_Flags::NUM_VALUES;
		ai_flag        = AI::AI_Flags::NUM_VALUES;
		return false;
	}

	// Runtime enum directly, or OF_No_collide which the helper special-cases.
	if (object_flag != Object::Object_Flags::NUM_VALUES ||
		ship_flag != Ship::Ship_Flags::NUM_VALUES ||
		ai_flag != AI::AI_Flags::NUM_VALUES ||
		parse_obj_flag == Mission::Parse_Object_Flags::OF_No_collide)
		return true;

	// Project a parse-only flag onto its runtime enum via resolve_parse_flags.
	// sexp_alter_ship_flag_helper takes one of each enum, so only single-bit
	// projections are accepted; multi-bit projections (no parse flag currently
	// has one outside the mcp_ship_flag_excluded list) would be rejected here.
	if (parse_obj_flag != Mission::Parse_Object_Flags::NUM_VALUES) {
		flagset<Mission::Parse_Object_Flags> pf;
		flagset<Object::Object_Flags> of;
		flagset<Ship::Ship_Flags> sf;
		flagset<AI::AI_Flags> af;
		pf.set(parse_obj_flag);
		resolve_parse_flags(pf, of, sf, af);

		int total = 0;
		for (size_t i = 0; i < (size_t)Object::Object_Flags::NUM_VALUES; i++) {
			auto f = (Object::Object_Flags)i;
			if (of[f]) { object_flag = f; total++; }
		}
		for (size_t i = 0; i < (size_t)Ship::Ship_Flags::NUM_VALUES; i++) {
			auto f = (Ship::Ship_Flags)i;
			if (sf[f]) { ship_flag = f; total++; }
		}
		for (size_t i = 0; i < (size_t)AI::AI_Flags::NUM_VALUES; i++) {
			auto f = (AI::AI_Flags)i;
			if (af[f]) { ai_flag = f; total++; }
		}

		if (total == 1)
			return true;

		// Multi-bit projection or no projection at all.
		object_flag = Object::Object_Flags::NUM_VALUES;
		ship_flag   = Ship::Ship_Flags::NUM_VALUES;
		ai_flag     = AI::AI_Flags::NUM_VALUES;
	}

	return false;
}

// Return the array of currently-set flag names on the given ship.  Iterates
// Parse_object_flags so the GET surface matches list_ship_flags;
// inverse_resolve_parse_flags handles the runtime-bit -> parse-flag projection.
static json_t *build_ship_flags_array(int ship_idx)
{
	const ship &shipp = Ships[ship_idx];
	const object &objp = Objects[shipp.objnum];
	const ai_info &aip = Ai_info[shipp.ai_index];

	flagset<Mission::Parse_Object_Flags> parse_flags;
	inverse_resolve_parse_flags(parse_flags, objp.flags, shipp.flags, aip.ai_flags);

	json_t *arr = json_array();
	for (size_t i = 0; i < Num_parse_object_flags; i++) {
		if (Parse_object_flags[i].in_use && parse_flags[Parse_object_flags[i].def] && !mcp_ship_flag_excluded(Parse_object_flags[i].def))
			json_array_append_new(arr, json_string(Parse_object_flags[i].name));
	}
	return arr;
}

// Validate-only pass: confirm flags_obj is an object of {name: bool} where each
// name resolves via resolve_ship_flag_name.  Used pre-creation in create_ship so
// a bad flag name aborts before any mutation.
static bool validate_ship_flags_only(json_t *flags_obj, McpErrorSink &sink)
{
	if (!flags_obj)
		return true;
	if (!json_is_object(flags_obj)) {
		sink.set_error("Parameter 'ship_flags' must be an object of {flag_name: bool}");
		return false;
	}
	const char *key;
	json_t *val;
	json_object_foreach(flags_obj, key, val) {
		if (!json_is_boolean(val)) {
			sink.set_error("ship_flags.%s must be a boolean", key);
			return false;
		}
		auto object_flag = Object::Object_Flags::NUM_VALUES;
		auto ship_flag = Ship::Ship_Flags::NUM_VALUES;
		auto parse_obj_flag = Mission::Parse_Object_Flags::NUM_VALUES;
		auto ai_flag = AI::AI_Flags::NUM_VALUES;
		if (!resolve_ship_flag_name(key, object_flag, ship_flag, parse_obj_flag, ai_flag)) {
			sink.set_error("Unknown ship flag '%s' (use list_ship_flags to see valid names)", key);
			return false;
		}
	}
	return true;
}

// Validate and apply a partial ship_flags update.  flags_obj may be null (no-op).
static bool apply_ship_flags_partial(int ship_idx, json_t *flags_obj, McpErrorSink &sink)
{
	if (!flags_obj)
		return true;
	if (!validate_ship_flags_only(flags_obj, sink))
		return false;

	object_ship_wing_point_team oswpt(&Ships[ship_idx]);

	const char *key;
	json_t *val;
	json_object_foreach(flags_obj, key, val) {
		auto object_flag = Object::Object_Flags::NUM_VALUES;
		auto ship_flag = Ship::Ship_Flags::NUM_VALUES;
		auto parse_obj_flag = Mission::Parse_Object_Flags::NUM_VALUES;
		auto ai_flag = AI::AI_Flags::NUM_VALUES;
		resolve_ship_flag_name(key, object_flag, ship_flag, parse_obj_flag, ai_flag);
		sexp_alter_ship_flag_helper(oswpt, true, object_flag, ship_flag,
			parse_obj_flag, ai_flag, json_boolean_value(val));
	}
	return true;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void handle_list_ships(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	json_t *arr = json_array();
	for (auto objp : list_range(&obj_used_list)) {
		if (objp->type != OBJ_SHIP && objp->type != OBJ_START)
			continue;
		json_array_append_new(arr, build_ship_json(objp->instance, false));
	}

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_ship(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	req->result_json = make_json_tool_result(build_ship_json(ship_idx, true));
	req->success = true;
}

static void handle_create_ship(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	// Required
	auto name = get_required_string(input, "name", sink, true, NAME_LENGTH - 1);
	if (!name) return;

	auto class_name = get_required_string(input, "ship_class", sink, true);
	if (!class_name) return;

	auto pos = get_required_vec3d(input, "position", sink);
	if (!pos.has_value()) return;

	auto orient = get_required_matrix(input, "orientation", sink);
	if (!orient.has_value()) return;

	// Optional
	auto display_name      = get_optional_string(input, "display_name", sink, NAME_LENGTH - 1);
	auto team_str          = get_optional_string(input, "team", sink);
	auto ai_class_str      = get_optional_string(input, "ai_class", sink);
	auto cargo_str         = get_optional_string(input, "cargo", sink, NAME_LENGTH - 1);
	auto hotkey            = get_optional_integer(input, "hotkey", sink);
	auto persona_str       = get_optional_string(input, "persona", sink);
	auto alt_name          = get_optional_string(input, "alt_class_name", sink, NAME_LENGTH - 1);
	auto callsign          = get_optional_string(input, "callsign", sink, NAME_LENGTH - 1);
	auto is_player         = get_optional_bool(input, "is_player_start", sink);
	auto score             = get_optional_integer(input, "score", sink);
	auto assist_pct        = get_optional_float(input, "assist_score_fraction", sink);

	auto arrival_loc_str   = get_optional_string(input, "arrival_location", sink);
	auto arrival_tgt_str   = get_optional_string(input, "arrival_target", sink);
	auto arrival_distance  = get_optional_integer(input, "arrival_distance", sink);
	auto arrival_delay     = get_optional_integer(input, "arrival_delay", sink);
	auto arrival_cue       = get_optional_integer(input, "arrival_cue", sink);

	auto departure_loc_str = get_optional_string(input, "departure_location", sink);
	auto departure_tgt_str = get_optional_string(input, "departure_target", sink);
	auto departure_delay   = get_optional_integer(input, "departure_delay", sink);
	auto departure_cue     = get_optional_integer(input, "departure_cue", sink);

	auto escort_priority   = get_optional_integer(input, "escort_priority", sink);
	auto kamikaze_damage   = get_optional_integer(input, "kamikaze_damage", sink);

	bool destroy_before_mission_is_explicit_null = is_parameter_present_and_null(input, "destroy_before_mission_seconds");
	auto destroy_before_mission = destroy_before_mission_is_explicit_null ? std::nullopt : get_optional_integer(input, "destroy_before_mission_seconds", sink);

	auto initial_hull   = get_optional_integer(input, "initial_hull_percent", sink);
	auto initial_shield = get_optional_integer(input, "initial_shield_percent", sink);
	auto initial_speed  = get_optional_integer(input, "initial_speed_percent", sink);

	json_t *ship_flags_in  = json_object_get(input, "ship_flags");

	if (sink.has_error()) return;

	// Range-check the flag-paired scalars.
	if (escort_priority.has_value() && !check_int_range(*escort_priority, 0, INT_MAX, "escort_priority", sink))
		return;
	if (kamikaze_damage.has_value() && !check_int_range(*kamikaze_damage, 0, INT_MAX, "kamikaze_damage", sink))
		return;
	if (destroy_before_mission.has_value() && !check_int_range(*destroy_before_mission, 0, INT_MAX, "destroy_before_mission_seconds", sink))
		return;
	if (initial_hull.has_value()   && !check_int_range(*initial_hull,   0, 100, "initial_hull_percent",   sink)) return;
	if (initial_shield.has_value() && !check_int_range(*initial_shield, 0, 100, "initial_shield_percent", sink)) return;
	if (initial_speed.has_value()  && !check_int_range(*initial_speed,  0, 100, "initial_speed_percent",  sink)) return;

	// Validate ship_flags up-front so a bad flag name aborts before mutation.
	if (!validate_ship_flags_only(ship_flags_in, sink)) return;

	// Resolve ship class
	int class_idx = check_lookup(class_name, ship_info_lookup, "ship_class", sink);
	if (class_idx < 0) return;

	// Validate name does not collide with any existing object
	if (!check_object_rename("ship", name, sink)) return;

	// Resolve optional enum/lookup fields up-front so we can fail before creation
	int team_idx = -1;
	if (team_str) {
		team_idx = check_lookup(team_str, iff_lookup, "team", sink);
		if (team_idx < 0) return;
	}

	int ai_class_idx = -1;
	if (ai_class_str) {
		ai_class_idx = check_lookup(ai_class_str,
			[](const char *s) { return string_lookup(s, Ai_class_names, (size_t)Num_ai_classes); },
			"ai_class", sink);
		if (ai_class_idx < 0) return;
	}

	int persona_idx = -1;
	if (persona_str && *persona_str) {
		persona_idx = check_lookup(persona_str, message_persona_name_lookup, "persona", sink);
		if (persona_idx < 0) return;
	}

	if (hotkey.has_value() && !check_int_range(*hotkey, 0, 9, "hotkey", sink))
		return;

	int arr_loc = -1;
	if (arrival_loc_str) {
		arr_loc = check_lookup(arrival_loc_str, arrival_location_enum_values, "arrival_location", sink);
		if (arr_loc < 0) return;
	}

	int dep_loc = -1;
	if (departure_loc_str) {
		dep_loc = check_lookup(departure_loc_str, departure_location_enum_values, "departure_location", sink);
		if (dep_loc < 0) return;
	}

	anchor_t arr_anchor = anchor_t::invalid();
	if (arrival_tgt_str && !resolve_target_name_to_anchor(arrival_tgt_str, arr_anchor, sink))
		return;

	anchor_t dep_anchor = anchor_t::invalid();
	if (departure_tgt_str && !resolve_target_name_to_anchor(departure_tgt_str, dep_anchor, sink))
		return;

	if (arrival_cue.has_value() && !check_sexp_formula(*arrival_cue, OPR_BOOL, sink))
		return;
	if (departure_cue.has_value() && !check_sexp_formula(*departure_cue, OPR_BOOL, sink))
		return;

	// Pre-validate cargo: refuse if it's a brand-new name and the pool is full,
	// so we don't leave a half-configured ship behind.
	if (cargo_str && *cargo_str) {
		if (string_lookup(cargo_str, Cargo_names, (size_t)Num_cargo) < 0 && Num_cargo >= MAX_CARGO) {
			sink.set_error("Cannot add cargo '%s': maximum number of cargo names (%d) already in use.",
				cargo_str, MAX_CARGO);
			return;
		}
	}

	// Create the ship
	matrix m = *orient;
	vec3d p = *pos;
	int obj = create_ship(&m, &p, class_idx);
	if (obj < 0) {
		sink.set_error("create_ship failed (engine returned -1)");
		return;
	}

	int ship_idx = Objects[obj].instance;
	ship &shipp = Ships[ship_idx];

	// Rename to user-provided name (create_ship gave it a default).
	if (stricmp(shipp.ship_name, name) != 0) {
		rename_ship(ship_idx, name);
		mcp_sexp_forest_mark_dirty();
	}

	if (display_name)
		apply_display_name(shipp, display_name);

	if (team_idx >= 0)
		shipp.team = team_idx;
	if (ai_class_idx >= 0)
		shipp.weapons.ai_class = ai_class_idx;

	if (cargo_str) {
		char cargo_idx = 0;
		// Pre-validated above; failure here would indicate a race that the MCP single-threaded model rules out.
		(void)resolve_or_add_cargo(cargo_str, cargo_idx, sink);
		shipp.cargo1 = cargo_idx;
	}

	if (hotkey.has_value())
		shipp.hotkey = *hotkey;

	if (persona_str)
		shipp.persona_index = persona_idx;

	if (alt_name)
		apply_helper(ship_idx, alt_name, apply_type::ALT_NAME);
	if (callsign)
		apply_helper(ship_idx, callsign, apply_type::CALLSIGN);

	if (score.has_value())
		shipp.score = *score;
	if (assist_pct.has_value()) {
		shipp.assist_score_pct = std::clamp((*assist_pct), 0.0f, 1.0f);
	}

	if (arr_loc >= 0)
		shipp.arrival_location = static_cast<ArrivalLocation>(arr_loc);
	if (arrival_tgt_str)
		shipp.arrival_anchor = arr_anchor;
	if (arrival_distance.has_value())
		shipp.arrival_distance = *arrival_distance;
	if (arrival_delay.has_value())
		shipp.arrival_delay = *arrival_delay;
	if (arrival_cue.has_value())
		replace_cue(shipp.arrival_cue, *arrival_cue);

	if (dep_loc >= 0)
		shipp.departure_location = static_cast<DepartureLocation>(dep_loc);
	if (departure_tgt_str)
		shipp.departure_anchor = dep_anchor;
	if (departure_delay.has_value())
		shipp.departure_delay = *departure_delay;
	if (departure_cue.has_value())
		replace_cue(shipp.departure_cue, *departure_cue);

	// Player-start toggle
	if (is_player.has_value() && *is_player) {
		Objects[obj].type = OBJ_START;
		Player_starts++;
		if (Player_start_shipnum < 0)
			Player_start_shipnum = ship_idx;
	}

	// Apply ship_flags (already validated above).
	apply_ship_flags_partial(ship_idx, ship_flags_in, sink);

	// Flag-paired scalars: written unconditionally (qtFRED pattern); the matching
	// flag in ship_flags governs whether the value is meaningful at runtime.
	if (escort_priority.has_value())
		shipp.escort_priority = *escort_priority;
	if (kamikaze_damage.has_value())
		Ai_info[shipp.ai_index].kamikaze_damage = *kamikaze_damage;

	// destroy_before_mission_seconds: scalar IS the flag.  Non-null number sets
	// both; null clears the flag (final_death_time is preserved as residual).
	if (destroy_before_mission_is_explicit_null) {
		shipp.flags.remove(Ship::Ship_Flags::Kill_before_mission);
	} else if (destroy_before_mission.has_value()) {
		shipp.flags.set(Ship::Ship_Flags::Kill_before_mission);
		shipp.final_death_time = *destroy_before_mission;
	}

	// Initial state at mission start.  In FRED edit mode the engine stores
	// these as 0-100 percents directly in the runtime object fields.
	if (initial_hull.has_value())
		Objects[obj].hull_strength = (float)*initial_hull;
	if (initial_shield.has_value())
		Objects[obj].shield_quadrant[0] = (float)*initial_shield;
	if (initial_speed.has_value())
		Objects[obj].phys_info.speed = (float)*initial_speed;

	obj_merge_created_list();
	mark_modified("MCP: create ship %s", name);

	req->result_json = make_json_tool_result(build_ship_json(ship_idx, true));
	req->success = true;
}

static void handle_update_ship(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	auto new_name          = get_optional_string(input, "new_name", sink, NAME_LENGTH - 1);
	auto display_name      = get_optional_string(input, "display_name", sink, NAME_LENGTH - 1);
	auto class_name        = get_optional_string(input, "ship_class", sink);
	auto team_str          = get_optional_string(input, "team", sink);
	auto ai_class_str      = get_optional_string(input, "ai_class", sink);
	auto cargo_str         = get_optional_string(input, "cargo", sink, NAME_LENGTH - 1);
	auto hotkey            = get_optional_integer(input, "hotkey", sink);
	auto persona_str       = get_optional_string(input, "persona", sink);
	auto alt_name          = get_optional_string(input, "alt_class_name", sink, NAME_LENGTH - 1);
	auto callsign          = get_optional_string(input, "callsign", sink, NAME_LENGTH - 1);
	auto is_player         = get_optional_bool(input, "is_player_start", sink);
	auto score             = get_optional_integer(input, "score", sink);
	auto assist_pct        = get_optional_float(input, "assist_score_fraction", sink);
	auto new_pos           = get_optional_vec3d(input, "position", sink);
	auto new_orient        = get_optional_matrix(input, "orientation", sink);

	auto arrival_loc_str   = get_optional_string(input, "arrival_location", sink);
	auto arrival_tgt_str   = get_optional_string(input, "arrival_target", sink);
	auto arrival_distance  = get_optional_integer(input, "arrival_distance", sink);
	auto arrival_delay     = get_optional_integer(input, "arrival_delay", sink);
	auto arrival_cue       = get_optional_integer(input, "arrival_cue", sink);

	auto departure_loc_str = get_optional_string(input, "departure_location", sink);
	auto departure_tgt_str = get_optional_string(input, "departure_target", sink);
	auto departure_delay   = get_optional_integer(input, "departure_delay", sink);
	auto departure_cue     = get_optional_integer(input, "departure_cue", sink);

	auto escort_priority   = get_optional_integer(input, "escort_priority", sink);
	auto kamikaze_damage   = get_optional_integer(input, "kamikaze_damage", sink);

	bool destroy_before_mission_is_explicit_null = is_parameter_present_and_null(input, "destroy_before_mission_seconds");
	auto destroy_before_mission = destroy_before_mission_is_explicit_null ? std::nullopt : get_optional_integer(input, "destroy_before_mission_seconds", sink);

	auto initial_hull   = get_optional_integer(input, "initial_hull_percent", sink);
	auto initial_shield = get_optional_integer(input, "initial_shield_percent", sink);
	auto initial_speed  = get_optional_integer(input, "initial_speed_percent", sink);

	json_t *ship_flags_in  = json_object_get(input, "ship_flags");

	if (sink.has_error()) return;

	// Range-check the flag-paired scalars.
	if (escort_priority.has_value() && !check_int_range(*escort_priority, 0, INT_MAX, "escort_priority", sink))
		return;
	if (kamikaze_damage.has_value() && !check_int_range(*kamikaze_damage, 0, INT_MAX, "kamikaze_damage", sink))
		return;
	if (destroy_before_mission.has_value() && !check_int_range(*destroy_before_mission, 0, INT_MAX, "destroy_before_mission_seconds", sink))
		return;
	if (initial_hull.has_value()   && !check_int_range(*initial_hull,   0, 100, "initial_hull_percent",   sink)) return;
	if (initial_shield.has_value() && !check_int_range(*initial_shield, 0, 100, "initial_shield_percent", sink)) return;
	if (initial_speed.has_value()  && !check_int_range(*initial_speed,  0, 100, "initial_speed_percent",  sink)) return;

	// Validate ship_flags up-front so a bad flag name aborts before mutation.
	if (!validate_ship_flags_only(ship_flags_in, sink)) return;

	ship &shipp = Ships[ship_idx];
	object &objp = Objects[shipp.objnum];
	bool in_wing = (shipp.wingnum >= 0);
	SCP_vector<SCP_string> wing_ignored;

	// Rename
	if (new_name && stricmp(shipp.ship_name, new_name) != 0) {
		if (!check_object_rename("ship", new_name, sink, ship_idx)) return;
		rename_ship(ship_idx, new_name);
		mcp_sexp_forest_mark_dirty();
	}

	if (display_name)
		apply_display_name(shipp, display_name);

	// Ship class
	if (class_name) {
		int new_class = check_lookup(class_name, ship_info_lookup, "ship_class", sink);
		if (new_class < 0) return;
		if (new_class != shipp.ship_info_index)
			change_ship_type(ship_idx, new_class, 0);
	}

	if (team_str) {
		int team_idx = check_lookup(team_str, iff_lookup, "team", sink);
		if (team_idx < 0) return;
		shipp.team = team_idx;
	}

	if (ai_class_str) {
		int ai_class_idx = check_lookup(ai_class_str,
			[](const char *s) { return string_lookup(s, Ai_class_names, (size_t)Num_ai_classes); },
			"ai_class", sink);
		if (ai_class_idx < 0) return;
		shipp.weapons.ai_class = ai_class_idx;
	}

	if (cargo_str) {
		char cargo_idx = 0;
		if (!resolve_or_add_cargo(cargo_str, cargo_idx, sink)) return;
		shipp.cargo1 = cargo_idx;
	}

	if (hotkey.has_value()) {
		if (!check_int_range(*hotkey, 0, 9, "hotkey", sink)) return;
		shipp.hotkey = *hotkey;
	}

	if (persona_str) {
		if (!*persona_str) {
			shipp.persona_index = -1;
		} else {
			int p = check_lookup(persona_str, message_persona_name_lookup, "persona", sink);
			if (p < 0) return;
			shipp.persona_index = p;
		}
	}

	if (alt_name) apply_helper(ship_idx, alt_name, apply_type::ALT_NAME);
	if (callsign) apply_helper(ship_idx, callsign, apply_type::CALLSIGN);

	if (score.has_value())
		shipp.score = *score;
	if (assist_pct.has_value()) {
		shipp.assist_score_pct = std::clamp(*assist_pct, 0.0f, 1.0f);
	}

	if (new_pos.has_value())
		objp.pos = *new_pos;
	if (new_orient.has_value())
		objp.orient = *new_orient;

	// Arrival group (gated by wing membership)
	auto skip_arr_dep = [&](const char *field) {
		wing_ignored.push_back(field);
	};

	if (arrival_loc_str) {
		if (in_wing) {
			skip_arr_dep("arrival_location");
		} else {
			int loc = check_lookup(arrival_loc_str, arrival_location_enum_values, "arrival_location", sink);
			if (loc < 0) return;
			shipp.arrival_location = static_cast<ArrivalLocation>(loc);
		}
	}
	if (arrival_tgt_str) {
		if (in_wing) {
			skip_arr_dep("arrival_target");
		} else {
			anchor_t a = anchor_t::invalid();
			if (!resolve_target_name_to_anchor(arrival_tgt_str, a, sink)) return;
			shipp.arrival_anchor = a;
		}
	}
	if (arrival_distance.has_value()) {
		if (in_wing) skip_arr_dep("arrival_distance");
		else shipp.arrival_distance = *arrival_distance;
	}
	if (arrival_delay.has_value()) {
		if (in_wing) skip_arr_dep("arrival_delay");
		else shipp.arrival_delay = *arrival_delay;
	}
	if (arrival_cue.has_value()) {
		if (in_wing) {
			skip_arr_dep("arrival_cue");
		} else {
			if (!check_sexp_formula(*arrival_cue, OPR_BOOL, sink)) return;
			replace_cue(shipp.arrival_cue, *arrival_cue);
		}
	}

	// Departure group (gated by wing membership)
	if (departure_loc_str) {
		if (in_wing) {
			skip_arr_dep("departure_location");
		} else {
			int loc = check_lookup(departure_loc_str, departure_location_enum_values, "departure_location", sink);
			if (loc < 0) return;
			shipp.departure_location = static_cast<DepartureLocation>(loc);
		}
	}
	if (departure_tgt_str) {
		if (in_wing) {
			skip_arr_dep("departure_target");
		} else {
			anchor_t a = anchor_t::invalid();
			if (!resolve_target_name_to_anchor(departure_tgt_str, a, sink)) return;
			shipp.departure_anchor = a;
		}
	}
	if (departure_delay.has_value()) {
		if (in_wing) skip_arr_dep("departure_delay");
		else shipp.departure_delay = *departure_delay;
	}
	if (departure_cue.has_value()) {
		if (in_wing) {
			skip_arr_dep("departure_cue");
		} else {
			if (!check_sexp_formula(*departure_cue, OPR_BOOL, sink)) return;
			replace_cue(shipp.departure_cue, *departure_cue);
		}
	}

	// Player-start toggle
	if (is_player.has_value()) {
		bool currently_player = (objp.type == OBJ_START);
		if (*is_player && !currently_player) {
			objp.type = OBJ_START;
			Player_starts++;
			if (Player_start_shipnum < 0)
				Player_start_shipnum = ship_idx;
		} else if (!*is_player && currently_player) {
			if (Player_starts < 2) {
				sink.set_error("Cannot remove player-start flag from '%s': at least one player start must exist.", shipp.ship_name);
				return;
			}
			objp.type = OBJ_SHIP;
			Player_starts--;
			// If this was the single-player start reference, hand it off to another player-start ship.
			if (Player_start_shipnum == ship_idx) {
				Player_start_shipnum = -1;
				for (auto p : list_range(&obj_used_list)) {
					if (p->type == OBJ_START) {
						Player_start_shipnum = p->instance;
						break;
					}
				}
			}
		}
	}

	// Apply ship_flags (already validated above).
	apply_ship_flags_partial(ship_idx, ship_flags_in, sink);

	// Flag-paired scalars: written unconditionally (qtFRED pattern); the matching
	// flag in ship_flags governs whether the value is meaningful at runtime.
	if (escort_priority.has_value())
		shipp.escort_priority = *escort_priority;
	if (kamikaze_damage.has_value())
		Ai_info[shipp.ai_index].kamikaze_damage = *kamikaze_damage;

	// destroy_before_mission_seconds: scalar IS the flag.  Non-null number sets
	// both; null clears the flag (final_death_time is preserved as residual).
	if (destroy_before_mission_is_explicit_null) {
		shipp.flags.remove(Ship::Ship_Flags::Kill_before_mission);
	} else if (destroy_before_mission.has_value()) {
		shipp.flags.set(Ship::Ship_Flags::Kill_before_mission);
		shipp.final_death_time = *destroy_before_mission;
	}

	// Initial state at mission start.  In FRED edit mode the engine stores
	// these as 0-100 percents directly in the runtime object fields.
	if (initial_hull.has_value())
		objp.hull_strength = (float)*initial_hull;
	if (initial_shield.has_value())
		objp.shield_quadrant[0] = (float)*initial_shield;
	if (initial_speed.has_value())
		objp.phys_info.speed = (float)*initial_speed;

	mark_modified("MCP: update ship %s", shipp.ship_name);

	// Build response
	json_t *result = build_ship_json(ship_idx, true);
	if (!wing_ignored.empty()) {
		json_t *skipped = json_array();
		for (const auto &s : wing_ignored)
			json_array_append_new(skipped, json_string(s.c_str()));
		json_object_set_new(result, "wing_ignored_fields", skipped);
		json_object_set_new(result, "wing_ignored_reason",
			json_string("Ship is a wing member; arrival/departure fields are owned by the wing."));
	}
	req->result_json = make_json_tool_result(result);
	req->success = true;
}

static void handle_delete_ship(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	const ship &shipp = Ships[ship_idx];
	const object &objp = Objects[shipp.objnum];
	bool is_player = (objp.type == OBJ_START);

	// Pre-check: refuse if this is the last player start
	if (is_player && Player_starts < 2) {
		sink.set_error("Cannot delete '%s': at least one player start must exist.", name);
		return;
	}

	// Pre-check SEXP references (and reject unless force=true)
	if (!check_and_report_sexp_refs(sexp_ref_type::SHIP, "Ship", name, force, sink))
		return;

	// Also pre-check AI goal references; common_object_delete's reference_handler
	// would otherwise pop a modal MessageBox for these even after SEXP refs are clean.
	bool forced = force.has_value() && *force;
	if (!forced) {
		auto ai_ref = query_referenced_in_ai_goals(sexp_ref_type::SHIP, name);
		if (ai_ref.second != sexp_src::NONE) {
			sink.set_error("Ship '%s' is referenced in an AI goal. "
				"Use force=true to delete anyway (references will be invalidated).", name);
			return;
		}
	} else {
		// Invalidate both SEXP and AI goal refs so reference_handler stays silent.
		invalidate_references(name, sexp_ref_type::SHIP);
		mcp_sexp_forest_mark_dirty();
	}

	if (delete_ship(ship_idx) != 0) {
		sink.set_error("delete_ship for '%s' failed (engine returned non-zero).", name);
		return;
	}

	mark_modified("MCP: delete ship %s", name);

	sprintf(req->result_message, "Deleted ship: %s", name);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Move / swap
// ---------------------------------------------------------------------------

// Sentinel matching the engine convention in code/scripting/api/libs/mission.cpp.
static constexpr int COUNT_SHIPS = -1000;

// Modeled on object_subclass_at_index in code/scripting/api/libs/mission.cpp.
// Walks Ships[] in slot order, skips empty slots, and returns
// the internal Ships[] slot of the index-th occupied ship (1-based).  When
// index == COUNT_SHIPS, returns the total count instead; returns -1 if the
// requested index doesn't exist.
static int ship_slot_at_public_index(int index)
{
	int count = 0;
	for (int i = 0; i < MAX_SHIPS; ++i) {
		if (Ships[i].objnum < 0)
			continue;
		++count;
		if (count == index)
			return i;
	}
	return (index == COUNT_SHIPS) ? count : -1;
}

static int ship_slot_count()
{
	return ship_slot_at_public_index(COUNT_SHIPS);
}

static FredShipSlotConfig make_ship_slot_config()
{
	FredShipSlotConfig scfg;
	scfg.fred_alt_names = Fred_alt_names;
	scfg.fred_callsigns = Fred_callsigns;
	scfg.cur_ship = &cur_ship;
	return scfg;
}

static MoveSwapConfig make_ship_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "ship";
	cfg.count = ship_slot_count();
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_ships;
	cfg.get_name = [](int i) {
		return SCP_string(Ships[ship_slot_at_public_index(i)].ship_name);
	};
	cfg.do_move = [](int from, int to) {
		// Walk the moving element via adjacent swaps.  After each swap the
		// sparse positions of *other* occupied slots are unchanged, so
		// ship_slot_at_public_index remains correct for the next step.
		auto scfg = make_ship_slot_config();
		int step = (from < to) ? 1 : -1;
		for (int pos = from; pos != to; pos += step) {
			int a = ship_slot_at_public_index(pos);
			int b = ship_slot_at_public_index(pos + step);
			swap_ship_slots(a, b, scfg);
		}
	};
	cfg.do_swap = [](int a, int b) {
		int sa = ship_slot_at_public_index(a);
		int sb = ship_slot_at_public_index(b);
		swap_ship_slots(sa, sb, make_ship_slot_config());
	};
	return cfg;
}

static void handle_move_ship(json_t *input, McpToolRequest *req)
{
	auto cfg = make_ship_move_swap_config();
	handle_generic_move(input, req, cfg);
}

static void handle_swap_ships(json_t *input, McpToolRequest *req)
{
	auto cfg = make_ship_move_swap_config();
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Weapon bank tools
// ---------------------------------------------------------------------------

static const SCP_vector<const char*> bank_type_enum_values = { "primary", "secondary" };

// Matches the FRED Weapons-dialog listbox label for the ship's main weapon
// set, used to distinguish it from named turret subsystems.
static const char *PILOT_BANK_SET_NAME = "Pilot";

// Resolve a subsystem name to its ship_weapon and ship_subsys.  The pilot's
// own weapons are returned when subsystem_name is null or "Pilot"
// (case-insensitive); any other value must name a SUBSYSTEM_TURRET on the
// ship.  Returns false and writes to sink on error.
static bool resolve_weapon_bank_set(
	ship &shipp,
	const char *subsystem_name,
	ship_weapon *&out_swp,
	ship_subsys *&out_subsys,
	McpErrorSink &sink)
{
	if (subsystem_name == nullptr || !stricmp(subsystem_name, PILOT_BANK_SET_NAME)) {
		out_swp = &shipp.weapons;
		out_subsys = nullptr;
		return true;
	}

	ship_subsys *ss = ship_get_subsys(&shipp, subsystem_name);
	if (ss == nullptr) {
		sink.set_error("Subsystem '%s' not found on ship '%s'.",
			subsystem_name, shipp.ship_name);
		return false;
	}
	if (ss->system_info == nullptr || ss->system_info->type != SUBSYSTEM_TURRET) {
		sink.set_error("Subsystem '%s' on ship '%s' is not a turret; weapon banks "
			"are only defined for the pilot and turret subsystems.",
			subsystem_name, shipp.ship_name);
		return false;
	}

	out_swp = &ss->weapons;
	out_subsys = ss;
	return true;
}

// Per-bank accessors that absorb the is_primary ternary.  References so callers
// can read or assign through them.
static int& bank_weapon_ref(ship_weapon *swp, bool is_primary, int idx)
{
	return is_primary ? swp->primary_bank_weapons[idx] : swp->secondary_bank_weapons[idx];
}

static int& bank_ammo_ref(ship_weapon *swp, bool is_primary, int idx)
{
	return is_primary ? swp->primary_bank_ammo[idx] : swp->secondary_bank_ammo[idx];
}

static int bank_count(const ship_weapon *swp, bool is_primary)
{
	return is_primary ? swp->num_primary_banks : swp->num_secondary_banks;
}

// Dispatch to the correct engine max-ammo helper.  Pilot helpers use
// std::lround (round-to-nearest); turret helpers use truncation -- an engine
// inconsistency we mirror so the reported max matches what the game clamps to.
static int compute_max_ammo(
	int ship_class, ship_weapon *swp, ship_subsys *subsys,
	bool is_primary, int bank_zero_based, int weapon_idx)
{
	if (subsys == nullptr) {
		if (is_primary)
			return get_max_ammo_count_for_primary_bank(ship_class, bank_zero_based, weapon_idx);
		return get_max_ammo_count_for_bank(ship_class, bank_zero_based, weapon_idx);
	}
	if (is_primary)
		return get_max_ammo_count_for_primary_turret_bank(swp, bank_zero_based, weapon_idx);
	return get_max_ammo_count_for_turret_bank(swp, bank_zero_based, weapon_idx);
}

// FRED stores secondary/ballistic ammo as 0-100 percent of the per-bank max
// (the dialog converts on every read/write -- see weaponeditordlg.cpp:307/371).
// Both directions clamp to 0 when the max is 0 (energy/beam/cleared bank).
static int ammo_percent_to_count(int percent, int max_count)
{
	if (max_count <= 0)
		return 0;
	return fl2ir((float)percent * (float)max_count / 100.0f);
}

static int ammo_count_to_percent(int count, int max_count)
{
	if (max_count <= 0)
		return 0;
	return fl2ir((float)count * 100.0f / (float)max_count);
}

// Build { bank, weapon_class, ammo_count } for one bank slot.
static json_t *build_bank_json(
	int ship_class, ship_weapon *swp, ship_subsys *subsys,
	bool is_primary, int bank_zero_based)
{
	int weapon_idx = bank_weapon_ref(swp, is_primary, bank_zero_based);

	json_t *obj = json_object();
	json_object_set_new(obj, "bank", json_integer(bank_zero_based + 1));

	if (weapon_idx < 0 || weapon_idx >= weapon_info_size()) {
		json_object_set_new(obj, "weapon_class", json_string("<none>"));
		json_object_set_new(obj, "ammo_count", json_integer(0));
		return obj;
	}

	json_object_set_new(obj, "weapon_class", json_safe_string(Weapon_info[weapon_idx].name));

	int max_ammo = compute_max_ammo(ship_class, swp, subsys, is_primary, bank_zero_based, weapon_idx);
	int stored_percent = bank_ammo_ref(swp, is_primary, bank_zero_based);
	json_object_set_new(obj, "ammo_count",
		json_integer(ammo_percent_to_count(stored_percent, max_ammo)));

	return obj;
}

// Build { primary_banks: [...], secondary_banks: [...] } for one set.
// Only banks within num_*_banks are reported.
static json_t *build_bank_set_json(int ship_class, ship_weapon *swp, ship_subsys *subsys)
{
	json_t *obj = json_object();

	json_t *primaries = json_array();
	for (int b = 0; b < swp->num_primary_banks; ++b)
		json_array_append_new(primaries, build_bank_json(ship_class, swp, subsys, true, b));
	json_object_set_new(obj, "primary_banks", primaries);

	json_t *secondaries = json_array();
	for (int b = 0; b < swp->num_secondary_banks; ++b)
		json_array_append_new(secondaries, build_bank_json(ship_class, swp, subsys, false, b));
	json_object_set_new(obj, "secondary_banks", secondaries);

	return obj;
}

// Primary slots reject secondary weapons; secondary slots require them.
// Mirrors the dialog's dropdown filter (weaponeditordlg.cpp:226-274).
static bool weapon_matches_bank_type(int weapon_idx, bool is_primary, McpErrorSink &sink)
{
	const weapon_info &wip = Weapon_info[weapon_idx];
	if (is_primary && wip.is_secondary()) {
		sink.set_error("Weapon '%s' is a secondary weapon and cannot be loaded into a primary bank.",
			wip.name);
		return false;
	}
	if (!is_primary && !wip.is_secondary()) {
		sink.set_error("Weapon '%s' is a primary weapon and cannot be loaded into a secondary bank.",
			wip.name);
		return false;
	}
	return true;
}

// Editor-selectability check matching the selectable_in_editor field on
// list_weapon_classes.  Overridable with force=true.
static bool weapon_selectable_in_editor(int weapon_idx, McpErrorSink &sink)
{
	const weapon_info &wip = Weapon_info[weapon_idx];
	bool no_fred = wip.wi_flags[Weapon::Info_Flags::No_fred];
	bool child = wip.wi_flags[Weapon::Info_Flags::Child];
	if (!no_fred && !child)
		return true;

	const char *flag_label;
	if (no_fred && child)
		flag_label = "No_fred and Child";
	else if (no_fred)
		flag_label = "No_fred";
	else
		flag_label = "Child";

	sink.set_error("Weapon '%s' is not selectable in the editor (%s flag set). "
		"Pass force=true to override.", wip.name, flag_label);
	return false;
}

// Pilot banks only: enforce the ship-class allowed-weapons list and per-bank
// restrictions.  Dogfight missions use DOGFIGHT_WEAPON; everything else uses
// REGULAR_WEAPON.  Turret loadouts are governed by the model rather than the
// ship class, so callers must skip this check for turret banks.
static bool weapon_allowed_for_pilot_bank(
	int ship_class, int weapon_idx, bool is_primary, int bank_zero_based, McpErrorSink &sink)
{
	const ship_info &sip = Ship_info[ship_class];
	int mode_flag = IS_MISSION_MULTI_DOGFIGHT ? DOGFIGHT_WEAPON : REGULAR_WEAPON;
	int bank_global_index = is_primary
		? bank_zero_based
		: MAX_SHIP_PRIMARY_BANKS + bank_zero_based;

	auto weapon_present_in = [weapon_idx, mode_flag](const auto &allowed) {
		for (const auto &wf : allowed.weapon_and_flags) {
			if (wf.first == weapon_idx && (wf.second & mode_flag))
				return true;
		}
		return false;
	};

	bool per_bank_active = (bank_global_index < (int)sip.restricted_loadout_flag.size())
		&& (sip.restricted_loadout_flag[bank_global_index] & mode_flag);

	bool allowed;
	const char *scope_label;
	if (per_bank_active) {
		allowed = (bank_global_index < (int)sip.allowed_bank_restricted_weapons.size())
			&& weapon_present_in(sip.allowed_bank_restricted_weapons[bank_global_index]);
		scope_label = "this bank's";
	} else if (!sip.allowed_weapons.weapon_and_flags.empty()) {
		allowed = weapon_present_in(sip.allowed_weapons);
		scope_label = "the ship class's";
	} else {
		return true;
	}

	if (allowed)
		return true;

	sink.set_error("Weapon '%s' is not in %s allowed-weapons list for ship class '%s'. "
		"Pass force=true to override.",
		Weapon_info[weapon_idx].name, scope_label, sip.name);
	return false;
}

// Common arg parsing for get_ship_weapon_bank, update_ship_weapon_bank, and
// get_max_ammo_for_bank.  On success, the caller receives a resolved bank
// target with the 1-based "bank" arg converted to a 0-based index and bounds
// already checked against num_*_banks.
static bool resolve_bank_target(
	json_t *input, McpErrorSink &sink,
	ship *&out_shipp,
	ship_weapon *&out_swp, ship_subsys *&out_subsys,
	bool &out_is_primary, int &out_bank_zero_based)
{
	auto name = get_required_string(input, "name", sink, true);
	if (!name) return false;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return false;

	auto bank_type = get_required_string(input, "bank_type", sink, true);
	if (!bank_type) return false;
	if (!check_string_enum(bank_type, bank_type_enum_values, "bank_type", sink))
		return false;
	bool is_primary = !stricmp(bank_type, "primary");

	auto bank_1based = get_required_integer(input, "bank", sink);
	if (!bank_1based) return false;

	auto subsystem = get_optional_string(input, "subsystem", sink);
	if (sink.has_error()) return false;

	ship &shipp = Ships[ship_idx];
	ship_weapon *swp = nullptr;
	ship_subsys *subsys = nullptr;
	if (!resolve_weapon_bank_set(shipp, subsystem, swp, subsys, sink))
		return false;

	int num_banks = bank_count(swp, is_primary);
	if (num_banks <= 0) {
		sink.set_error("%s on ship '%s' has no %s banks.",
			subsys ? subsys->system_info->subobj_name : "Pilot weapon set",
			shipp.ship_name,
			is_primary ? "primary" : "secondary");
		return false;
	}
	if (*bank_1based < 1 || *bank_1based > num_banks) {
		sink.set_error("bank %d out of range; %s has %d %s bank%s.",
			*bank_1based,
			subsys ? "this turret" : "the pilot set",
			num_banks,
			is_primary ? "primary" : "secondary",
			num_banks == 1 ? "" : "s");
		return false;
	}

	out_shipp = &shipp;
	out_swp = swp;
	out_subsys = subsys;
	out_is_primary = is_primary;
	out_bank_zero_based = *bank_1based - 1;
	return true;
}

static void handle_get_ship_weapons(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	ship &shipp = Ships[ship_idx];
	int ship_class = shipp.ship_info_index;

	json_t *result = json_object();
	json_object_set_new(result, "ship", json_safe_string(shipp.ship_name));
	json_object_set_new(result, "pilot",
		build_bank_set_json(ship_class, &shipp.weapons, nullptr));

	json_t *turrets = json_array();
	for (ship_subsys *ss = GET_FIRST(&shipp.subsys_list);
		ss != END_OF_LIST(&shipp.subsys_list);
		ss = GET_NEXT(ss))
	{
		if (ss->system_info == nullptr || ss->system_info->type != SUBSYSTEM_TURRET)
			continue;
		json_t *t = build_bank_set_json(ship_class, &ss->weapons, ss);
		json_object_set_new(t, "subsystem", json_safe_string(ss->system_info->subobj_name));
		json_array_append_new(turrets, t);
	}
	json_object_set_new(result, "turrets", turrets);

	req->result_json = make_json_tool_result(result);
	req->success = true;
}

static void handle_get_ship_weapon_bank(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	ship *shipp;
	ship_weapon *swp;
	ship_subsys *subsys;
	bool is_primary;
	int bank_zb;
	if (!resolve_bank_target(input, sink, shipp, swp, subsys, is_primary, bank_zb))
		return;

	req->result_json = make_json_tool_result(
		build_bank_json(shipp->ship_info_index, swp, subsys, is_primary, bank_zb));
	req->success = true;
}

static void handle_update_ship_weapon_bank(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	ship *shipp;
	ship_weapon *swp;
	ship_subsys *subsys;
	bool is_primary;
	int bank_zb;
	if (!resolve_bank_target(input, sink, shipp, swp, subsys, is_primary, bank_zb))
		return;

	auto weapon_class_arg = get_optional_string(input, "weapon_class", sink);
	auto ammo_count_arg = get_optional_integer(input, "ammo_count", sink);
	auto force_arg = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;
	bool force = force_arg.value_or(false);

	// Resolve the target weapon: -1 means "<none>" (clear).
	int existing_weapon_idx = bank_weapon_ref(swp, is_primary, bank_zb);
	int new_weapon_idx = existing_weapon_idx;
	bool weapon_specified = (weapon_class_arg != nullptr);
	bool clearing = false;

	if (weapon_specified) {
		if (!stricmp(weapon_class_arg, "<none>")) {
			new_weapon_idx = -1;
			clearing = true;
		} else {
			new_weapon_idx = check_lookup(weapon_class_arg, weapon_info_lookup, "weapon_class", sink);
			if (new_weapon_idx < 0) return;
			if (!weapon_matches_bank_type(new_weapon_idx, is_primary, sink))
				return;
			if (!force && !weapon_selectable_in_editor(new_weapon_idx, sink))
				return;
			if (!force && subsys == nullptr
				&& !weapon_allowed_for_pilot_bank(shipp->ship_info_index, new_weapon_idx,
					is_primary, bank_zb, sink))
				return;
		}
	}

	bool weapon_changing = weapon_specified && (new_weapon_idx != existing_weapon_idx);
	bool ammo_specified = ammo_count_arg.has_value();

	// Compute the post-update max with the resolved weapon.
	int new_max_ammo = (new_weapon_idx >= 0)
		? compute_max_ammo(shipp->ship_info_index, swp, subsys,
			is_primary, bank_zb, new_weapon_idx)
		: 0;
	int new_ammo_count = 0;

	if (clearing) {
		if (ammo_specified && *ammo_count_arg != 0) {
			sink.set_error("ammo_count must be 0 when weapon_class is \"<none>\".");
			return;
		}
		new_ammo_count = 0;
	} else if (ammo_specified) {
		if (new_max_ammo <= 0) {
			if (*ammo_count_arg != 0) {
				sink.set_error("Weapon '%s' has no ammo (energy/beam or non-ballistic primary); "
					"ammo_count must be 0.",
					new_weapon_idx >= 0 ? Weapon_info[new_weapon_idx].name : "<none>");
				return;
			}
			new_ammo_count = 0;
		} else {
			if (!check_int_range(*ammo_count_arg, 0, new_max_ammo, "ammo_count", sink))
				return;
			new_ammo_count = *ammo_count_arg;
		}
	} else if (weapon_changing) {
		// Weapon changing without explicit ammo -- preserve the previous
		// percentage against the new weapon's max (mirrors the FRED dialog,
		// see weaponeditordlg.cpp change_selection()).
		int existing_percent = bank_ammo_ref(swp, is_primary, bank_zb);
		new_ammo_count = ammo_percent_to_count(existing_percent, new_max_ammo);
	}

	bool any_change = weapon_changing || clearing || ammo_specified;

	if (any_change) {
		if (weapon_changing || clearing)
			bank_weapon_ref(swp, is_primary, bank_zb) = new_weapon_idx;
		bank_ammo_ref(swp, is_primary, bank_zb) = ammo_count_to_percent(new_ammo_count, new_max_ammo);

		mark_modified("MCP: update ship %s weapon bank (%s %s %d)",
			shipp->ship_name,
			subsys ? subsys->system_info->subobj_name : PILOT_BANK_SET_NAME,
			is_primary ? "primary" : "secondary",
			bank_zb + 1);
	}

	req->result_json = make_json_tool_result(
		build_bank_json(shipp->ship_info_index, swp, subsys, is_primary, bank_zb));
	req->success = true;
}

static void handle_get_max_ammo_for_bank(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	ship *shipp;
	ship_weapon *swp;
	ship_subsys *subsys;
	bool is_primary;
	int bank_zb;
	if (!resolve_bank_target(input, sink, shipp, swp, subsys, is_primary, bank_zb))
		return;

	auto weapon_class = get_required_string(input, "weapon_class", sink, true);
	if (!weapon_class) return;

	int weapon_idx = check_lookup(weapon_class, weapon_info_lookup, "weapon_class", sink);
	if (weapon_idx < 0) return;
	if (!weapon_matches_bank_type(weapon_idx, is_primary, sink))
		return;

	int max_ammo = compute_max_ammo(shipp->ship_info_index, swp, subsys,
		is_primary, bank_zb, weapon_idx);

	json_t *result = json_object();
	json_object_set_new(result, "max_ammo_count", json_integer(max_ammo));
	req->result_json = make_json_tool_result(result);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_ship_tools(json_t *tools)
{
	// list_ships
	register_tool(tools, "list_ships",
		"List all ships in the mission (including player-start ships) in mission iteration order. "
		"Each entry has the ship's name, index, class, team (IFF), player-start flag, wing membership, and position.",
		json_object());

	// get_ship
	register_tool_with_required_string(tools, "get_ship",
		"Get full details of a ship by name. Returns identity, scoring, position/orientation, arrival/departure settings "
		"(including SEXP cue node IDs you can edit via the SEXP tools), and wing membership. If the ship is in a wing, "
		"the wing owns the arrival/departure settings — the ship's own values are returned for reference but are not authoritative.",
		"name", "Name of the ship to retrieve");

	// create_ship
	{
		json_t *props = json_object();
		add_string_prop(props, "name",
			"Unique name for the ship (must not collide with any other ship, wing, waypoint, or jump node).");
		add_string_prop(props, "ship_class",
			"Ship class name (e.g. \"GTF Ulysses\"). Use list_ship_classes to discover valid names.");
		add_vec3d_prop(props, "position", "World position where the ship is placed.");
		add_matrix_prop(props, "orientation",
			"Orientation matrix (rvec/uvec/fvec) for the ship at placement.");
		add_string_prop(props, "display_name",
			"Optional display name shown to the player (if different from `name`). Pass \"<none>\" or "
			"a string matching the ship's `name` to clear; a blank string is a valid display name and "
			"will be stored as-is.");
		add_string_prop(props, "team",
			"IFF team name (e.g. \"Friendly\", \"Hostile\"). Defaults to the ship class's species default IFF.");
		add_string_prop(props, "ai_class",
			"AI class name (e.g. \"General\", \"Captain\"). Defaults to the ship class's default AI.");
		add_string_prop(props, "cargo",
			"Cargo name. Auto-added to the mission cargo pool if new (subject to MAX_CARGO).");
		add_integer_prop(props, "hotkey",
			"Hotkey assignment 0-9, or omit for no hotkey.");
		add_string_prop(props, "persona",
			"Persona name. Empty string clears.");
		add_string_prop(props, "alt_class_name",
			"Alternate ship class name (display alias). Empty/\"<none>\" clears.");
		add_string_prop(props, "callsign",
			"Pilot callsign. Empty/\"<none>\" clears.");
		add_bool_prop(props, "is_player_start",
			"If true, mark this ship as a player starting point. Default false.");
		add_integer_prop(props, "score",
			"Points awarded to the player for destroying this ship.");
		add_number_prop(props, "assist_score_fraction",
			"Fraction (0.0-1.0) of the score awarded for an assist.");
		add_string_enum_prop(props, "arrival_location",
			"How the ship arrives. Default \"Hyperspace\".", arrival_location_enum_values);
		add_string_prop(props, "arrival_target",
			"Ship name or special anchor (e.g. \"<any friendly>\") referenced by the arrival_location.");
		add_integer_prop(props, "arrival_distance",
			"Distance offset (meters) from the arrival target.");
		add_integer_prop(props, "arrival_delay",
			"Seconds to wait after the arrival cue evaluates true before the ship actually arrives.");
		add_integer_prop(props, "arrival_cue",
			"SEXP node ID of the boolean formula that gates arrival. Defaults to a locked \"true\" cue. "
			"Use text_to_sexp / create_sexp_node to build cues.");
		add_string_enum_prop(props, "departure_location",
			"How the ship departs. Default \"Hyperspace\".", departure_location_enum_values);
		add_string_prop(props, "departure_target",
			"Ship name (typically one with a docking bay when departure_location is \"Docking Bay\").");
		add_integer_prop(props, "departure_delay",
			"Seconds to wait after the departure cue evaluates true before the ship actually departs.");
		add_integer_prop(props, "departure_cue",
			"SEXP node ID of the boolean formula that gates departure. Defaults to a locked \"false\" cue.");
		add_bool_map_prop(props, "ship_flags",
			"Partial map of {flag_name: bool} for this ship's flags. Names come from list_ship_flags and "
			"span the engine's object, ship, parse-object, and AI flag enums (same surface as the Lua "
			"Ship:setFlag API and the alter-ship-flag SEXP). Only listed keys are touched.");
		add_integer_prop(props, "escort_priority",
			"Escort priority. Paired with the \"escort\" entry in ship_flags: the priority is stored "
			"unconditionally but only matters at runtime when \"escort\" is set. Non-negative.");
		add_integer_prop(props, "kamikaze_damage",
			"Damage dealt by a kamikaze collision. Paired with the \"kamikaze\" entry in ship_flags: "
			"stored unconditionally but only matters at runtime when \"kamikaze\" is set. "
			"FRED defaults this to min(1000, 200 + hull/4) on ship creation. Non-negative.");
		add_integer_prop(props, "destroy_before_mission_seconds",
			"Seconds before mission start at which the ship is pre-destroyed (sets Kill_before_mission). "
			"This scalar IS the flag: a non-negative integer enables the behavior; null clears it. "
			"There is no \"destroy-before-mission\" entry in ship_flags. Omit the field entirely to leave unchanged.");
		add_integer_prop(props, "initial_hull_percent",
			"Initial hull strength at mission start, as a percent of class max (0-100). FRED defaults to 100.");
		add_integer_prop(props, "initial_shield_percent",
			"Initial shield strength at mission start, as a percent of class max (0-100). FRED defaults to 100. "
			"If a ship does not have shields, this field does not appear in a GET response but can still be written.");
		add_integer_prop(props, "initial_speed_percent",
			"Initial speed at mission start, as a percent of class max (0-100). FRED defaults to 33.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("ship_class"));
		json_array_append_new(req, json_string("position"));
		json_array_append_new(req, json_string("orientation"));
		register_tool(tools, "create_ship",
			"Create a new ship in the mission. The ship is placed in no wing (use a wing tool to add it to a wing later). "
			"Arrival/departure cues default to locked-true and locked-false respectively.",
			props, req);
	}

	// update_ship
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship to update.");
		add_string_prop(props, "new_name",
			"New name for the ship. SEXP references, AI goals, texture replacements, and reinforcement entries "
			"are updated automatically. Per FRED convention, renaming may reset the display name.");
		add_string_prop(props, "display_name",
			"Display name shown to the player. Pass \"<none>\" or a string matching the ship's `name` "
			"to clear; a blank string is a valid display name and will be stored as-is.");
		add_string_prop(props, "ship_class",
			"Change the ship's class. Triggers a model/subsystem swap via change_ship_type.");
		add_string_prop(props, "team", "IFF team name.");
		add_string_prop(props, "ai_class", "AI class name.");
		add_string_prop(props, "cargo", "Cargo name (auto-added if new).");
		add_integer_prop(props, "hotkey", "Hotkey 0-9.");
		add_string_prop(props, "persona", "Persona name. Empty string clears.");
		add_string_prop(props, "alt_class_name", "Alternate ship class name. Empty/\"<none>\" clears.");
		add_string_prop(props, "callsign", "Pilot callsign. Empty/\"<none>\" clears.");
		add_bool_prop(props, "is_player_start",
			"Toggles whether this is a player ship or not. Refused if turning off would leave zero player ships.");
		add_integer_prop(props, "score", "Score awarded for destroying this ship.");
		add_number_prop(props, "assist_score_fraction", "Assist score fraction 0.0-1.0.");
		add_vec3d_prop(props, "position", "New world position.");
		add_matrix_prop(props, "orientation", "New orientation matrix.");
		add_string_enum_prop(props, "arrival_location",
			"Arrival mode (ignored if ship is in a wing).", arrival_location_enum_values);
		add_string_prop(props, "arrival_target",
			"Ship name or special anchor (ignored if ship is in a wing).");
		add_integer_prop(props, "arrival_distance", "Arrival distance (ignored if ship is in a wing).");
		add_integer_prop(props, "arrival_delay", "Arrival delay (ignored if ship is in a wing).");
		add_integer_prop(props, "arrival_cue",
			"SEXP node ID for the arrival cue (ignored if ship is in a wing). The previous cue tree is freed.");
		add_string_enum_prop(props, "departure_location",
			"Departure mode (ignored if ship is in a wing).", departure_location_enum_values);
		add_string_prop(props, "departure_target",
			"Departure target (ignored if ship is in a wing).");
		add_integer_prop(props, "departure_delay", "Departure delay (ignored if ship is in a wing).");
		add_integer_prop(props, "departure_cue",
			"SEXP node ID for the departure cue (ignored if ship is in a wing). The previous cue tree is freed.");
		add_bool_map_prop(props, "ship_flags",
			"Partial map of {flag_name: bool} for this ship's flags. Names come from list_ship_flags and "
			"span the engine's object, ship, parse-object, and AI flag enums (same surface as the Lua "
			"Ship:setFlag API and the alter-ship-flag SEXP). Only listed keys are touched.");
		add_integer_prop(props, "escort_priority",
			"Escort priority. Paired with the \"escort\" entry in ship_flags: the priority is stored "
			"unconditionally but only matters at runtime when \"escort\" is set. Non-negative.");
		add_integer_prop(props, "kamikaze_damage",
			"Damage dealt by a kamikaze collision. Paired with the \"kamikaze\" entry in ship_flags: "
			"stored unconditionally but only matters at runtime when \"kamikaze\" is set. Non-negative.");
		add_integer_prop(props, "destroy_before_mission_seconds",
			"Seconds before mission start at which the ship is pre-destroyed (sets Kill_before_mission). "
			"This scalar IS the flag: a non-negative integer enables the behavior; null clears it. "
			"There is no \"destroy-before-mission\" entry in ship_flags. Omit the field entirely to leave unchanged.");
		add_integer_prop(props, "initial_hull_percent",
			"Initial hull strength at mission start, as a percent of class max (0-100).");
		add_integer_prop(props, "initial_shield_percent",
			"Initial shield strength at mission start, as a percent of class max (0-100). "
			"If a ship does not have shields, this field does not appear in a GET response but can still be written.");
		add_integer_prop(props, "initial_speed_percent",
			"Initial speed at mission start, as a percent of class max (0-100).");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_ship",
			"Update properties of an existing ship. Only specified fields are changed. "
			"If the ship is in a wing, arrival/departure fields are silently ignored and the response lists which were skipped.",
			props, req);
	}

	// delete_ship
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship to delete.");
		add_bool_prop(props, "force",
			"If true, delete even when the ship is referenced in SEXPs or AI goals "
			"(references are invalidated). Default false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "delete_ship",
			"Delete a ship from the mission. Refuses if the ship is the last player start, "
			"or if it is referenced in SEXPs unless force=true. Wing membership and docking are cleaned up automatically.",
			props, req);
	}

	// move_ship
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"1-based index of the ship to move");
		add_integer_prop(props, "to_index",
			"1-based destination index");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_ship",
			"Move a ship from one list position to another.  Indices are 1-based.",
			props, req);
	}

	// swap_ships
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first ship");
		add_integer_prop(props, "index_b",
			"1-based index of the second ship");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_ships",
			"Swap two ships at the given list positions.  Indices are 1-based.",
			props, req);
	}

	// get_ship_weapons
	register_tool_with_required_string(tools, "get_ship_weapons",
		"Get every weapon bank assignment on a ship -- the pilot weapon set plus every turret "
		"subsystem.  Each bank entry is { bank, weapon_class, ammo_count } where bank is 1-based, "
		"weapon_class \"<none>\" means an empty slot, and ammo_count is an absolute count (0 for "
		"energy primaries and beams).  Only banks within the ship/turret's num_*_banks are reported.",
		"name", "Name of the ship");

	// get_ship_weapon_bank
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship.");
		add_string_prop(props, "subsystem",
			"Subsystem name.  Default \"Pilot\" (the ship's main weapon set).  Any other value "
			"must name a turret subsystem on the ship.");
		add_string_enum_prop(props, "bank_type",
			"Which set of banks to read.", bank_type_enum_values);
		add_integer_prop(props, "bank",
			"1-based bank index within the selected bank_type.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("bank_type"));
		json_array_append_new(req, json_string("bank"));
		register_tool(tools, "get_ship_weapon_bank",
			"Get the weapon class and ammo count for a single weapon bank (pilot or turret).",
			props, req);
	}

	// update_ship_weapon_bank
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship.");
		add_string_prop(props, "subsystem",
			"Subsystem name.  Default \"Pilot\".  Any other value must name a turret subsystem.");
		add_string_enum_prop(props, "bank_type",
			"Which set of banks to update.", bank_type_enum_values);
		add_integer_prop(props, "bank",
			"1-based bank index within the selected bank_type.");
		add_string_prop(props, "weapon_class",
			"Weapon to load.  Pass \"<none>\" to clear the bank.  Must match the bank's type "
			"(primary slots reject secondary weapons and vice versa).  Omit to leave the weapon "
			"unchanged.");
		add_integer_prop(props, "ammo_count",
			"Absolute ammo count for ballistic primaries and secondaries.  Must be 0 for energy "
			"primaries, beams, and cleared banks.  Range-checked against the per-bank-and-weapon "
			"maximum (see get_max_ammo_for_bank).  Omit to leave ammo unchanged -- if the weapon "
			"is changing, the previous percentage is preserved against the new weapon's max.");
		add_bool_prop(props, "force",
			"If true, bypass the editor-selectability check (No_fred / Child) and the ship-class "
			"allowed-weapons check for pilot banks.  Default false.  Does not bypass structural "
			"rules (bank range, weapon/bank type match, ammo bounds).");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("bank_type"));
		json_array_append_new(req, json_string("bank"));
		register_tool(tools, "update_ship_weapon_bank",
			"Set the weapon and/or ammo count for a single weapon bank.  Either weapon_class or "
			"ammo_count may be omitted to leave that field unchanged.",
			props, req);
	}

	// get_max_ammo_for_bank
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship.");
		add_string_prop(props, "subsystem",
			"Subsystem name.  Default \"Pilot\".  Any other value must name a turret subsystem.");
		add_string_enum_prop(props, "bank_type",
			"Which set of banks to compute for.", bank_type_enum_values);
		add_integer_prop(props, "bank",
			"1-based bank index within the selected bank_type.");
		add_string_prop(props, "weapon_class",
			"Weapon to compute the maximum for.  Returns 0 for energy primaries and beams "
			"(only ballistic weapons have ammo).");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("bank_type"));
		json_array_append_new(req, json_string("bank"));
		json_array_append_new(req, json_string("weapon_class"));
		register_tool(tools, "get_max_ammo_for_bank",
			"Compute the maximum ammo count for a specific (ship, bank, weapon) combination.  "
			"Mirrors the engine's per-bank-and-weapon clamp.",
			props, req);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_ship_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_ships") == 0) {
		handle_list_ships(input_json, req);
	} else if (strcmp(tool_name, "get_ship") == 0) {
		handle_get_ship(input_json, req);
	} else if (strcmp(tool_name, "create_ship") == 0) {
		handle_create_ship(input_json, req);
	} else if (strcmp(tool_name, "update_ship") == 0) {
		handle_update_ship(input_json, req);
	} else if (strcmp(tool_name, "delete_ship") == 0) {
		handle_delete_ship(input_json, req);
	} else if (strcmp(tool_name, "move_ship") == 0) {
		handle_move_ship(input_json, req);
	} else if (strcmp(tool_name, "swap_ships") == 0) {
		handle_swap_ships(input_json, req);
	} else if (strcmp(tool_name, "get_ship_weapons") == 0) {
		handle_get_ship_weapons(input_json, req);
	} else if (strcmp(tool_name, "get_ship_weapon_bank") == 0) {
		handle_get_ship_weapon_bank(input_json, req);
	} else if (strcmp(tool_name, "update_ship_weapon_bank") == 0) {
		handle_update_ship_weapon_bank(input_json, req);
	} else if (strcmp(tool_name, "get_max_ammo_for_bank") == 0) {
		handle_get_max_ammo_for_bank(input_json, req);
	} else {
		return false;
	}
	return true;
}
