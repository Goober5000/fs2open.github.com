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

#include "model/model.h"              // model_find_dock_name_index, model_get_dock_*
#include "object/objectdock.h"        // dock_*, object_is_docked, dock_function_info

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

// FRED initial-status helpers used to bring a freshly-docked chain to a
// consistent state.  Defined at file scope (non-static) in initialstatus.cpp.
extern void initial_status_unmark_dock_handled_flag(object *objp, dock_function_info *infop);
extern void initial_status_mark_dock_leader_helper(object *objp, dock_function_info *infop);

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
	object &objp = Objects[shipp.objnum];

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

	// Initially-docked state -- emitted only when the ship is actually docked.
	// is_dock_leader is meaningful only in that context; absence signals "not docked."
	if (object_is_docked(&objp)) {
		json_object_set_new(obj, "is_dock_leader",
			json_boolean(shipp.flags[Ship::Ship_Flags::Dock_leader]));

		json_t *docks = json_array();
		const int this_model = Ship_info[shipp.ship_info_index].model_num;
		for (dock_instance *d = objp.dock_list; d != nullptr; d = d->next) {
			object *partner = d->docked_objp;
			const ship &partner_shipp = Ships[partner->instance];
			const int partner_model = Ship_info[partner_shipp.ship_info_index].model_num;

			json_t *entry = json_object();
			json_object_set_new(entry, "ship", json_safe_string(partner_shipp.ship_name));
			json_object_set_new(entry, "my_dockpoint",
				json_safe_string(model_get_dock_name(this_model, d->dockpoint_used)));
			const int partner_point = dock_find_dockpoint_used_by_object(partner, &objp);
			json_object_set_new(entry, "other_dockpoint",
				json_safe_string(model_get_dock_name(partner_model, partner_point)));
			json_array_append_new(docks, entry);
		}
		json_object_set_new(obj, "docked_to", docks);
	}

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
	// Treat null, empty, and "<none>" all as clear-to-Nothing; the latter
	// matches the display_name / weapon_class / alt_class_name / callsign
	// convention used elsewhere in the ship tools.
	if (!cargo_name || !*cargo_name || !stricmp(cargo_name, "<none>")) {
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
//   player-start:  edits go through is_player_start (manages Player_starts and OBJ_START)
//   reinforcement: edits go through reinforcement tools (to keep the vector in sync)
//   immobile:      deprecated, callers should set don't-change-position + don't-change-orientation
//   locked:        deprecated, callers should set ship-locked + weapons-locked
bool mcp_ship_flag_excluded(const Mission::Parse_Object_Flags &candidate)
{
	return candidate == Mission::Parse_Object_Flags::OF_Player_start || candidate == Mission::Parse_Object_Flags::SF_Reinforcement
		|| candidate == Mission::Parse_Object_Flags::OF_Immobile || candidate == Mission::Parse_Object_Flags::SF_Locked;
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
// Docking tools -- mirror initial_status::dock / initial_status::undock from
// fred2/initialstatus.cpp.  Spatial separation on undock is unconditionally
// skipped; callers can issue update_ship with a new position if needed.
// ---------------------------------------------------------------------------

// Walks the docked chain rooted at objp and returns whichever ship in the
// chain holds the Dock_leader flag, or nullptr if none does.
// dock_find_dock_leader_helper is a single-object check; dock_evaluate_all_docked_objects
// fans it across the chain and short-circuits via early_return_condition.
static object *find_dock_leader_obj(object *objp)
{
	dock_function_info dfi;
	dock_evaluate_all_docked_objects(objp, &dfi, dock_find_dock_leader_helper);
	return dfi.maintained_variables.objp_value;
}

static const char *find_dock_leader_name(object *objp)
{
	object *leader_objp = find_dock_leader_obj(objp);
	return leader_objp != nullptr ? Ships[leader_objp->instance].ship_name : nullptr;
}

// Mirrors the arrival-cue restoration + dock-leader clear that
// initial_status::undock performs on each ship after a pair separates.
static void undock_followup(ship &s, object *objp)
{
	if (s.arrival_cue == Locked_sexp_false && s.wingnum < 0)
		s.arrival_cue = Locked_sexp_true;
	if (!object_is_docked(objp))
		s.flags.remove(Ship::Ship_Flags::Dock_leader);
}

static void handle_dock_ships(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto docker_name      = get_required_string(input, "docker", sink, true);
	auto dockee_name      = get_required_string(input, "dockee", sink, true);
	auto docker_point     = get_required_string(input, "docker_point", sink, true);
	auto dockee_point     = get_required_string(input, "dockee_point", sink, true);
	if (!docker_name || !dockee_name || !docker_point || !dockee_point) return;

	if (!stricmp(docker_name, dockee_name)) {
		sink.set_error("docker and dockee must be different ships.");
		return;
	}

	int docker_idx_ship = lookup_ship(docker_name, sink);
	if (docker_idx_ship < 0) return;
	int dockee_idx_ship = lookup_ship(dockee_name, sink);
	if (dockee_idx_ship < 0) return;

	ship &docker_shipp = Ships[docker_idx_ship];
	ship &dockee_shipp = Ships[dockee_idx_ship];
	object *docker_objp = &Objects[docker_shipp.objnum];
	object *dockee_objp = &Objects[dockee_shipp.objnum];
	const int docker_model = Ship_info[docker_shipp.ship_info_index].model_num;
	const int dockee_model = Ship_info[dockee_shipp.ship_info_index].model_num;

	int docker_pt = model_find_dock_name_index(docker_model, docker_point);
	if (docker_pt < 0) {
		sink.set_error("Dockpoint '%s' not found on '%s' (use list_ship_class_dockpoints).",
			docker_point, docker_name);
		return;
	}
	int dockee_pt = model_find_dock_name_index(dockee_model, dockee_point);
	if (dockee_pt < 0) {
		sink.set_error("Dockpoint '%s' not found on '%s' (use list_ship_class_dockpoints).",
			dockee_point, dockee_name);
		return;
	}

	// Type compatibility
	const int docker_type = model_get_dock_index_type(docker_model, docker_pt);
	const int dockee_type = model_get_dock_index_type(dockee_model, dockee_pt);
	if ((docker_type & dockee_type) == 0) {
		sink.set_error("Incompatible dockpoint types: '%s' on '%s' and '%s' on '%s' do not share a type.",
			docker_point, docker_name, dockee_point, dockee_name);
		return;
	}

	// Occupancy
	if (dock_find_object_at_dockpoint(docker_objp, docker_pt) != nullptr) {
		sink.set_error("Dockpoint '%s' on '%s' is already occupied.", docker_point, docker_name);
		return;
	}
	if (dock_find_object_at_dockpoint(dockee_objp, dockee_pt) != nullptr) {
		sink.set_error("Dockpoint '%s' on '%s' is already occupied.", dockee_point, dockee_name);
		return;
	}

	// Already directly docked?
	if (dock_check_find_direct_docked_object(docker_objp, dockee_objp)) {
		sink.set_error("'%s' and '%s' are already docked to each other.", docker_name, dockee_name);
		return;
	}

	// Mutation: mirrors initial_status::dock.  The MCP convention is "docker
	// moves, dockee stays", so the dockee is the anchor: it's the engine's
	// "dockee" param (so dock_orient_and_approach moves the docker into
	// position), and it's the chain-walk anchor for unmark/move/leader.
	dock_function_info dfi;
	ai_dock_with_object(docker_objp, docker_pt, dockee_objp, dockee_pt, AIDO_DOCK_NOW);
	dock_evaluate_all_docked_objects(dockee_objp, &dfi, initial_status_unmark_dock_handled_flag);
	dock_move_docked_objects(dockee_objp);
	dfi.parameter_variables.bool_value = true;	// suppress "setting to false for initial docking purposes" message box
	dock_evaluate_all_docked_objects(dockee_objp, &dfi, initial_status_mark_dock_leader_helper);
	if (dfi.maintained_variables.int_value == 0)
		dockee_shipp.flags.set(Ship::Ship_Flags::Dock_leader);

	// Leader-resolution may have rewritten an arrival_cue to Locked_sexp_false.
	mcp_sexp_forest_mark_dirty();
	mark_modified("MCP: dock %s -> %s", docker_name, dockee_name);

	json_t *result = json_object();
	json_object_set_new(result, "docker", json_safe_string(docker_name));
	json_object_set_new(result, "dockee", json_safe_string(dockee_name));
	json_object_set_new(result, "docker_point", json_safe_string(docker_point));
	json_object_set_new(result, "dockee_point", json_safe_string(dockee_point));
	const char *leader = find_dock_leader_name(dockee_objp);
	if (leader != nullptr)
		json_object_set_new(result, "dock_leader", json_safe_string(leader));
	req->result_json = make_json_tool_result(result);
	req->success = true;
}

static void handle_undock_ships(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto ship_name = get_required_string(input, "ship", sink, true);
	if (!ship_name) return;

	auto other_name_opt = get_optional_string(input, "other_ship", sink);
	if (sink.has_error()) return;

	int ship_idx = lookup_ship(ship_name, sink);
	if (ship_idx < 0) return;

	ship &this_shipp = Ships[ship_idx];
	object *this_objp = &Objects[this_shipp.objnum];

	object *other_objp;
	if (other_name_opt) {
		int other_idx = lookup_ship(other_name_opt, sink);
		if (other_idx < 0) return;
		other_objp = &Objects[Ships[other_idx].objnum];
		if (!dock_check_find_direct_docked_object(this_objp, other_objp)) {
			sink.set_error("'%s' is not directly docked to '%s'.", ship_name, other_name_opt);
			return;
		}
	} else {
		int n = dock_count_direct_docked_objects(this_objp);
		if (n == 0) {
			sink.set_error("'%s' is not docked to anything.", ship_name);
			return;
		}
		if (n > 1) {
			sink.set_error("'%s' is docked to %d ships; specify 'other_ship' to disambiguate.",
				ship_name, n);
			return;
		}
		other_objp = dock_get_first_docked_object(this_objp);
	}

	ship &other_shipp = Ships[other_objp->instance];
	const SCP_string other_name(other_shipp.ship_name);

	ai_do_objects_undocked_stuff(this_objp, other_objp);
	undock_followup(this_shipp, this_objp);
	undock_followup(other_shipp, other_objp);

	mcp_sexp_forest_mark_dirty();
	mark_modified("MCP: undock %s from %s", ship_name, other_name.c_str());

	json_t *result = json_object();
	json_object_set_new(result, "ship", json_safe_string(ship_name));
	json_object_set_new(result, "other_ship", json_safe_string(other_name.c_str()));

	// Enumerate any ships that "ship" remains directly docked to after this call,
	// so the caller can tell whether further undock_ships calls (or undock_all_ships)
	// are needed without a follow-up get_ship.
	json_t *still_docked = json_array();
	for (dock_instance *d = this_objp->dock_list; d != nullptr; d = d->next)
		json_array_append_new(still_docked,
			json_safe_string(Ships[d->docked_objp->instance].ship_name));
	json_object_set_new(result, "still_docked_to", still_docked);

	req->result_json = make_json_tool_result(result);
	req->success = true;
}

static void handle_undock_all_ships(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto ship_name = get_required_string(input, "ship", sink, true);
	if (!ship_name) return;

	int ship_idx = lookup_ship(ship_name, sink);
	if (ship_idx < 0) return;

	ship &this_shipp = Ships[ship_idx];
	object *this_objp = &Objects[this_shipp.objnum];

	SCP_vector<SCP_string> former_partners;
	while (object_is_docked(this_objp)) {
		object *partner = dock_get_first_docked_object(this_objp);
		ship &partner_shipp = Ships[partner->instance];
		former_partners.emplace_back(partner_shipp.ship_name);
		ai_do_objects_undocked_stuff(this_objp, partner);
		undock_followup(partner_shipp, partner);
	}
	undock_followup(this_shipp, this_objp);

	mcp_sexp_forest_mark_dirty();
	if (!former_partners.empty())
		mark_modified("MCP: undock_all %s", ship_name);

	json_t *result = json_object();
	json_object_set_new(result, "ship", json_safe_string(ship_name));
	json_t *partners = json_array();
	for (const auto &p : former_partners)
		json_array_append_new(partners, json_safe_string(p.c_str()));
	json_object_set_new(result, "formerly_docked_to", partners);
	req->result_json = make_json_tool_result(result);
	req->success = true;
}

// Chain-walk helper for list_docked_group.  Stashes its vector pointer in
// the dock_function_info's vecp_value slot (typed vec3d* but unused for our
// purposes) via reinterpret_cast -- mirrors how initialstatus.cpp's leader
// helper repurposes objp_value to carry state through the chain walker.
static void collect_ship_name_helper(object *objp, dock_function_info *infop)
{
	auto *names = reinterpret_cast<SCP_vector<SCP_string> *>(infop->maintained_variables.vecp_value);
	names->emplace_back(Ships[objp->instance].ship_name);
}

static void handle_list_docked_group(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto ship_name = get_required_string(input, "ship", sink, true);
	if (!ship_name) return;

	int ship_idx = lookup_ship(ship_name, sink);
	if (ship_idx < 0) return;

	ship &shipp = Ships[ship_idx];
	object *objp = &Objects[shipp.objnum];

	SCP_vector<SCP_string> names;
	if (!object_is_docked(objp)) {
		// Solitary ship: report a single-element group with null leader.
		names.emplace_back(shipp.ship_name);
	} else {
		dock_function_info dfi;
		dfi.maintained_variables.vecp_value = reinterpret_cast<vec3d *>(&names);
		dock_evaluate_all_docked_objects(objp, &dfi, collect_ship_name_helper);
	}

	const char *leader = object_is_docked(objp) ? find_dock_leader_name(objp) : nullptr;

	json_t *result = json_object();
	json_object_set_new(result, "ship", json_safe_string(ship_name));
	if (leader != nullptr)
		json_object_set_new(result, "dock_leader", json_safe_string(leader));
	else
		json_object_set_new(result, "dock_leader", json_null());
	json_t *ships_arr = json_array();
	for (const auto &n : names)
		json_array_append_new(ships_arr, json_safe_string(n.c_str()));
	json_object_set_new(result, "ships", ships_arr);
	req->result_json = make_json_tool_result(result);
	req->success = true;
}

static void handle_set_dock_leader(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto ship_name = get_required_string(input, "ship", sink, true);
	if (!ship_name) return;

	int ship_idx = lookup_ship(ship_name, sink);
	if (ship_idx < 0) return;

	ship &shipp = Ships[ship_idx];
	object *objp = &Objects[shipp.objnum];

	if (!object_is_docked(objp)) {
		sink.set_error("'%s' is not docked; the dock-leader flag is meaningless on a solitary ship.",
			ship_name);
		return;
	}

	if (shipp.wingnum >= 0) {
		sink.set_error("Cannot set a wing-member ship as dock leader; its effective arrival cue "
			"is the wing's, not its own. Remove '%s' from its wing first.", ship_name);
		return;
	}

	object *current_leader_objp = find_dock_leader_obj(objp);
	ship *current_leader_shipp = current_leader_objp != nullptr
		? &Ships[current_leader_objp->instance] : nullptr;
	const char *previous_leader_name = current_leader_shipp != nullptr
		? current_leader_shipp->ship_name : nullptr;

	// No-op when ship is already the leader.
	if (current_leader_shipp == &shipp) {
		json_t *result = json_object();
		json_object_set_new(result, "ship", json_safe_string(ship_name));
		json_object_set_new(result, "previous_leader", json_safe_string(ship_name));
		req->result_json = make_json_tool_result(result);
		req->success = true;
		return;
	}

	if (current_leader_shipp != nullptr && current_leader_shipp->wingnum >= 0) {
		sink.set_error("Cannot take dock leadership from '%s' because it is a wing member; "
			"its arrival cue is the wing's, not its own. Remove it from its wing first.",
			current_leader_shipp->ship_name);
		return;
	}

	// If there's a current leader, swap cues: the new leader takes the
	// current leader's cue; the current leader is parked on Locked_sexp_false.
	// Raw assignment instead of set_cue_to_false because that helper would
	// free the old leader's cue before we could move it to the new leader.
	// In the leaderless case (rare: external code cleared the flag) we just
	// promote the requested ship and leave its existing cue intact.
	if (current_leader_shipp != nullptr) {
		if (shipp.arrival_cue != Locked_sexp_false)
			free_sexp2(shipp.arrival_cue);
		shipp.arrival_cue = current_leader_shipp->arrival_cue;
		current_leader_shipp->arrival_cue = Locked_sexp_false;
		current_leader_shipp->flags.remove(Ship::Ship_Flags::Dock_leader);
	}
	shipp.flags.set(Ship::Ship_Flags::Dock_leader);

	mcp_sexp_forest_mark_dirty();
	mark_modified("MCP: set_dock_leader %s", ship_name);

	json_t *result = json_object();
	json_object_set_new(result, "ship", json_safe_string(ship_name));
	if (previous_leader_name != nullptr)
		json_object_set_new(result, "previous_leader", json_safe_string(previous_leader_name));
	else
		json_object_set_new(result, "previous_leader", json_null());
	req->result_json = make_json_tool_result(result);
	req->success = true;
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
	int percent = fl2ir((float)count * 100.0f / (float)max_count);
	return std::clamp(percent, 0, 100);
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

// Big_only weapons (capital missiles, etc.) require a big-or-huge ship.  The
// dialog filters these from the dropdown when the ship isn't big_or_huge
// (weaponeditordlg.cpp:250-251).  Treated as a structural constraint -- force
// does NOT bypass this.
static bool weapon_size_matches_ship(int weapon_idx, int ship_class, McpErrorSink &sink)
{
	const weapon_info &wip = Weapon_info[weapon_idx];
	if (!wip.wi_flags[Weapon::Info_Flags::Big_only])
		return true;
	const ship_info &sip = Ship_info[ship_class];
	if (sip.is_big_or_huge())
		return true;
	sink.set_error("Weapon '%s' has the restricted_to_big_ships flag and cannot be loaded onto ship class '%s' "
		"(not big or huge).", wip.name, sip.name);
	return false;
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

static void handle_list_ship_weapons(json_t *input, McpToolRequest *req)
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
			if (!weapon_size_matches_ship(new_weapon_idx, shipp->ship_info_index, sink))
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
	}
	// When only the weapon class is changing (no explicit ammo_count), leave
	// the stored percent untouched.  build_bank_json will surface the count
	// derived from that percent against the new weapon's max.  Round-tripping
	// the percent through ammo_count_to_percent(ammo_percent_to_count(...))
	// can drift on integer division (e.g. 67% with max=7 -> count 5 -> 71%).

	// Compare against current stored state so redundant calls (clearing an
	// already-empty bank, setting ammo to its current value) don't autosave.
	int new_percent = ammo_count_to_percent(new_ammo_count, new_max_ammo);
	int existing_percent = bank_ammo_ref(swp, is_primary, bank_zb);
	bool ammo_will_change = (clearing || ammo_specified) && (new_percent != existing_percent);

	if (weapon_changing || ammo_will_change) {
		if (weapon_changing)
			bank_weapon_ref(swp, is_primary, bank_zb) = new_weapon_idx;
		if (ammo_will_change)
			bank_ammo_ref(swp, is_primary, bank_zb) = new_percent;

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
	if (!weapon_size_matches_ship(weapon_idx, shipp->ship_info_index, sink))
		return;

	int max_ammo = compute_max_ammo(shipp->ship_info_index, swp, subsys,
		is_primary, bank_zb, weapon_idx);

	json_t *result = json_object();
	json_object_set_new(result, "max_ammo_count", json_integer(max_ammo));
	req->result_json = make_json_tool_result(result);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Subsystem tools -- mirror the per-subsystem fields of the Initial Status
// dialog (fred2/initialstatus.cpp): health, cargo, cargo title, and (for
// turrets) AI class.  Weapon banks on turrets are handled separately via
// update_ship_weapon_bank.
// ---------------------------------------------------------------------------

// Matches initialstatus.cpp:587-589 -- a ship has scannable subsystems iff
// (is_huge_ship) XOR (Toggle_subsystem_scanning flag).
static bool ship_has_scannable_subsystems(const ship &shipp)
{
	bool scannable = Ship_info[shipp.ship_info_index].is_huge_ship();
	if (shipp.flags[Ship::Ship_Flags::Toggle_subsystem_scanning])
		scannable = !scannable;
	return scannable;
}

// Build { subsystem, subsystem_type, initial_health_percent, cargo?, cargo_title?, ai_class? }.
static json_t *build_subsystem_json(const ship_subsys *subsys)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "subsystem", json_safe_string(subsys->system_info->subobj_name));

	int type = subsys->system_info->type;
	if (type < 0 || type >= SUBSYSTEM_MAX)
		type = SUBSYSTEM_UNKNOWN;
	json_object_set_new(obj, "subsystem_type", json_safe_string(Subsystem_types[type]));

	// FRED stores current_hits as 0-100 percent during edit (see
	// missionparse.cpp:2858 Fred_running branch); fl2ir rounds half-away-from-zero
	// to keep round-trips stable.
	json_object_set_new(obj, "initial_health_percent",
		json_integer(fl2ir(subsys->current_hits)));

	if (subsys->subsys_cargo_name > 0 && subsys->subsys_cargo_name < Num_cargo)
		json_object_set_new(obj, "cargo", json_safe_string(Cargo_names[subsys->subsys_cargo_name]));
	set_optional_string(obj, "cargo_title", subsys->subsys_cargo_title, true);

	if (subsys->system_info->type == SUBSYSTEM_TURRET) {
		int ai_class = subsys->weapons.ai_class;
		if (ai_class >= 0 && ai_class < Num_ai_classes)
			json_object_set_new(obj, "ai_class", json_safe_string(Ai_class_names[ai_class]));
	}

	return obj;
}

static void handle_list_ship_subsystems(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	ship &shipp = Ships[ship_idx];

	json_t *result = json_object();
	json_object_set_new(result, "ship", json_safe_string(shipp.ship_name));

	json_t *arr = json_array();
	for (ship_subsys *ss = GET_FIRST(&shipp.subsys_list);
		ss != END_OF_LIST(&shipp.subsys_list);
		ss = GET_NEXT(ss))
	{
		if (ss->system_info == nullptr)
			continue;
		json_array_append_new(arr, build_subsystem_json(ss));
	}
	json_object_set_new(result, "subsystems", arr);

	req->result_json = make_json_tool_result(result);
	req->success = true;
}

static void handle_get_ship_subsystem(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	auto subsystem_name = get_required_string(input, "subsystem", sink, true);
	if (!subsystem_name) return;

	ship &shipp = Ships[ship_idx];
	ship_subsys *subsys = ship_get_subsys(&shipp, subsystem_name);
	if (subsys == nullptr) {
		sink.set_error("Subsystem '%s' not found on ship '%s'.", subsystem_name, shipp.ship_name);
		return;
	}

	req->result_json = make_json_tool_result(build_subsystem_json(subsys));
	req->success = true;
}

static void handle_update_ship_subsystem(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	auto subsystem_name = get_required_string(input, "subsystem", sink, true);
	if (!subsystem_name) return;

	ship &shipp = Ships[ship_idx];
	ship_subsys *subsys = ship_get_subsys(&shipp, subsystem_name);
	if (subsys == nullptr) {
		sink.set_error("Subsystem '%s' not found on ship '%s'.", subsystem_name, shipp.ship_name);
		return;
	}

	auto health     = get_optional_integer(input, "initial_health_percent", sink);
	auto cargo_arg  = get_optional_string(input, "cargo", sink, NAME_LENGTH - 1);
	auto title_arg  = get_optional_string(input, "cargo_title", sink, NAME_LENGTH - 1);
	auto ai_arg     = get_optional_string(input, "ai_class", sink);
	auto force_arg  = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;
	bool force = force_arg.value_or(false);

	// Range-check health up-front so a bad value aborts before mutation.
	if (health.has_value()
		&& !check_int_range(*health, 0, 100, "initial_health_percent", sink))
		return;

	// ai_class only valid on turrets.
	int ai_class_idx = -1;
	if (ai_arg) {
		if (subsys->system_info->type != SUBSYSTEM_TURRET) {
			int type = subsys->system_info->type;
			if (type < 0 || type >= SUBSYSTEM_MAX)
				type = SUBSYSTEM_UNKNOWN;
			sink.set_error("ai_class can only be set on turret subsystems; '%s' on '%s' is type '%s'.",
				subsystem_name, shipp.ship_name, Subsystem_types[type]);
			return;
		}
		if (!stricmp(ai_arg, "<default>")) {
			ai_class_idx = Ship_info[shipp.ship_info_index].ai_class;
		} else {
			ai_class_idx = check_lookup(ai_arg,
				[](const char *s) { return string_lookup(s, Ai_class_names, (size_t)Num_ai_classes); },
				"ai_class", sink);
			if (ai_class_idx < 0) return;
		}
	}

	// Cargo / cargo_title gate.
	bool cargo_specified = (cargo_arg != nullptr) || (title_arg != nullptr);
	if (cargo_specified && !force && !ship_has_scannable_subsystems(shipp)) {
		sink.set_error("Ship '%s' does not have scannable subsystems "
			"(is_huge_ship XOR toggle-subsystem-scanning flag must be true). "
			"Pass force=true to set cargo/cargo_title anyway.", shipp.ship_name);
		return;
	}

	// cargo_title is meaningless without cargo, and missionsave drops the
	// +Subsystem: block (and title) when there's no cargo / damage / turret
	// weapons.  Require cargo to be set (now or already) when setting a non-empty
	// title, so a successful write here always survives a save.
	if (title_arg && *title_arg && stricmp(title_arg, "<none>") != 0) {
		bool will_have_cargo;
		if (cargo_arg)
			will_have_cargo = (*cargo_arg && stricmp(cargo_arg, "<none>") != 0);
		else
			will_have_cargo = (subsys->subsys_cargo_name > 0);
		if (!will_have_cargo) {
			sink.set_error("cargo_title requires cargo to be set on subsystem '%s'.  "
				"Provide cargo in the same call, or clear cargo_title with \"<none>\".",
				subsystem_name);
			return;
		}
	}

	// Resolve cargo index (auto-adds if new).
	char cargo_idx = 0;
	if (cargo_arg && !resolve_or_add_cargo(cargo_arg, cargo_idx, sink))
		return;

	// All validation done -- mutate.
	bool changed = false;

	if (health.has_value()) {
		subsys->current_hits = (float)*health;
		changed = true;
	}
	if (cargo_arg) {
		subsys->subsys_cargo_name = cargo_idx;
		changed = true;
	}
	if (title_arg) {
		if (!*title_arg || !stricmp(title_arg, "<none>"))
			subsys->subsys_cargo_title[0] = '\0';
		else
			strncpy_s(subsys->subsys_cargo_title, title_arg, NAME_LENGTH - 1);
		changed = true;
	}
	if (ai_arg) {
		subsys->weapons.ai_class = ai_class_idx;
		changed = true;
	}

	if (changed)
		mark_modified("MCP: update ship %s subsystem %s", shipp.ship_name, subsystem_name);

	req->result_json = make_json_tool_result(build_subsystem_json(subsys));
	req->success = true;
}

// ---------------------------------------------------------------------------
// Special explosion tools -- mirror the per-ship "Special Explosion" dialog
// (fred2/shipspecialdamage.cpp).  When the master toggle is on, the engine
// uses these override fields instead of the ship class's shockwave_create_info
// at death.
// ---------------------------------------------------------------------------

static json_t *build_special_explosion_json(const ship &shipp)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "ship", json_safe_string(shipp.ship_name));
	json_object_set_new(obj, "enabled", json_boolean(shipp.use_special_explosion));
	if (!shipp.use_special_explosion)
		return obj;

	json_object_set_new(obj, "damage", json_integer(shipp.special_exp_damage));
	json_object_set_new(obj, "blast_force", json_integer(shipp.special_exp_blast));
	json_object_set_new(obj, "inner_radius", json_integer(shipp.special_exp_inner));
	json_object_set_new(obj, "outer_radius", json_integer(shipp.special_exp_outer));
	json_object_set_new(obj, "shockwave_enabled", json_boolean(shipp.use_shockwave));
	if (shipp.use_shockwave)
		json_object_set_new(obj, "shockwave_speed", json_integer(shipp.special_exp_shockwave_speed));
	// 0 is the engine sentinel for "use class default death-roll time"; omit it.
	if (shipp.special_exp_deathroll_time > 0)
		json_object_set_new(obj, "deathroll_time_ms", json_integer(shipp.special_exp_deathroll_time));
	return obj;
}

static void handle_get_ship_special_explosion(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	req->result_json = make_json_tool_result(build_special_explosion_json(Ships[ship_idx]));
	req->success = true;
}

// Working copy used during partial updates -- assembled from current state (if
// already enabled) or ship class defaults (if transitioning from disabled), then
// overlaid with explicit caller-supplied values.
struct special_explosion_stage {
	int damage;
	int blast;
	int inner;
	int outer;
	bool use_shockwave;
	int shockwave_speed;
	int deathroll_time;
};

static special_explosion_stage seed_from_class_defaults(const ship_info &sip)
{
	special_explosion_stage s;
	s.damage = (int)sip.shockwave.damage;
	s.blast = (int)sip.shockwave.blast;
	s.inner = (int)sip.shockwave.inner_rad;
	s.outer = (int)sip.shockwave.outer_rad;
	s.use_shockwave = (sip.explosion_propagates != 0);
	s.shockwave_speed = (int)sip.shockwave.speed;
	s.deathroll_time = 0;
	// Engine-edge clamps the dialog applies on fresh open
	// (shipspecialdamage.cpp:140-148).
	if (s.inner < 1) s.inner = 1;
	if (s.outer < 2) s.outer = 2;
	if (s.shockwave_speed < 1) s.shockwave_speed = 1;
	return s;
}

static special_explosion_stage seed_from_current(const ship &shipp)
{
	special_explosion_stage s;
	s.damage = shipp.special_exp_damage;
	s.blast = shipp.special_exp_blast;
	s.inner = shipp.special_exp_inner;
	s.outer = shipp.special_exp_outer;
	s.use_shockwave = shipp.use_shockwave;
	s.shockwave_speed = shipp.special_exp_shockwave_speed;
	s.deathroll_time = shipp.special_exp_deathroll_time;
	return s;
}

static bool any_explosion_field_supplied(json_t *input)
{
	static const char *fields[] = {
		"damage", "blast_force", "inner_radius", "outer_radius",
		"shockwave_enabled", "shockwave_speed", "deathroll_time_ms",
	};
	for (const char *f : fields)
		if (json_object_get(input, f) != nullptr)
			return true;
	return false;
}

static void handle_update_ship_special_explosion(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	auto enable = get_required_bool(input, "enable", sink);
	if (!enable) return;

	ship &shipp = Ships[ship_idx];

	// --- Disable path: wipe to engine sentinels.  Reject any other fields. ---
	if (!*enable) {
		if (any_explosion_field_supplied(input)) {
			sink.set_error("When 'enable' is false, no other special-explosion fields may "
				"be supplied (they would be discarded by the wipe).");
			return;
		}
		shipp.use_special_explosion = false;
		shipp.special_exp_damage = -1;
		shipp.special_exp_blast = -1;
		shipp.special_exp_inner = -1;
		shipp.special_exp_outer = -1;
		shipp.use_shockwave = false;
		shipp.special_exp_shockwave_speed = 0;
		shipp.special_exp_deathroll_time = 0;
		mark_modified("MCP: update ship %s special explosion (disabled)", name);
		req->result_json = make_json_tool_result(build_special_explosion_json(shipp));
		req->success = true;
		return;
	}

	// --- Enable path: parse partial fields, seed staging, overlay, validate. ---
	auto damage_arg = get_optional_integer(input, "damage", sink);
	auto blast_arg = get_optional_integer(input, "blast_force", sink);
	auto inner_arg = get_optional_integer(input, "inner_radius", sink);
	auto outer_arg = get_optional_integer(input, "outer_radius", sink);
	auto shock_enabled_arg = get_optional_bool(input, "shockwave_enabled", sink);
	auto shock_speed_arg = get_optional_integer(input, "shockwave_speed", sink);
	auto deathroll_arg = get_optional_integer(input, "deathroll_time_ms", sink);
	if (sink.has_error()) return;

	bool was_enabled = shipp.use_special_explosion;
	special_explosion_stage stage = was_enabled
		? seed_from_current(shipp)
		: seed_from_class_defaults(Ship_info[shipp.ship_info_index]);

	if (damage_arg.has_value())        stage.damage = *damage_arg;
	if (blast_arg.has_value())         stage.blast = *blast_arg;
	if (inner_arg.has_value())         stage.inner = *inner_arg;
	if (outer_arg.has_value())         stage.outer = *outer_arg;
	if (shock_enabled_arg.has_value()) stage.use_shockwave = *shock_enabled_arg;
	if (shock_speed_arg.has_value())   stage.shockwave_speed = *shock_speed_arg;
	if (deathroll_arg.has_value())     stage.deathroll_time = *deathroll_arg;

	// Validation -- mirrors shipspecialdamage.cpp:192-237.
	if (stage.damage < 0) {
		sink.set_error("damage must be non-negative (got %d).", stage.damage);
		return;
	}
	if (stage.blast < 0) {
		sink.set_error("blast_force must be non-negative (got %d).", stage.blast);
		return;
	}
	if (stage.inner < 1) {
		sink.set_error("inner_radius must be at least 1 (got %d).", stage.inner);
		return;
	}
	if (stage.outer < 2) {
		sink.set_error("outer_radius must be at least 2 (got %d).", stage.outer);
		return;
	}
	if (stage.inner > stage.outer) {
		sink.set_error("inner_radius (%d) must not exceed outer_radius (%d).",
			stage.inner, stage.outer);
		return;
	}
	if (stage.deathroll_time != 0 && stage.deathroll_time < 2) {
		sink.set_error("deathroll_time_ms must be 0 (inherit class default) or at least 2 ms (got %d).",
			stage.deathroll_time);
		return;
	}
	if (stage.use_shockwave && stage.shockwave_speed < 1) {
		sink.set_error("shockwave_speed must be at least 1 when shockwave_enabled is true (got %d).",
			stage.shockwave_speed);
		return;
	}
	// Cleanup: when shockwave is off, speed has no meaning -- drop to 0 so the
	// engine field doesn't carry stale data.
	if (!stage.use_shockwave)
		stage.shockwave_speed = 0;

	shipp.use_special_explosion = true;
	shipp.special_exp_damage = stage.damage;
	shipp.special_exp_blast = stage.blast;
	shipp.special_exp_inner = stage.inner;
	shipp.special_exp_outer = stage.outer;
	shipp.use_shockwave = stage.use_shockwave;
	shipp.special_exp_shockwave_speed = stage.shockwave_speed;
	shipp.special_exp_deathroll_time = stage.deathroll_time;

	mark_modified("MCP: update ship %s special explosion", name);
	req->result_json = make_json_tool_result(build_special_explosion_json(shipp));
	req->success = true;
}

// ---------------------------------------------------------------------------
// Special hitpoints tools -- mirror the per-ship "Special Hitpoints" dialog
// (fred2/shipspecialhitpoints.cpp).  Two independent overrides (hull, shield);
// commit of hull also recalculates kamikaze_damage.
// ---------------------------------------------------------------------------

static json_t *build_special_hitpoints_json(const ship &shipp)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "ship", json_safe_string(shipp.ship_name));
	// Engine sentinels (0 for hull, -1 for shield) are surfaced as field-omission;
	// see shipspecialhitpoints.cpp.
	if (shipp.special_hitpoints > 0)
		json_object_set_new(obj, "hull", json_integer(shipp.special_hitpoints));
	if (shipp.special_shield >= 0)
		json_object_set_new(obj, "shield", json_integer(shipp.special_shield));
	return obj;
}

static void handle_get_ship_special_hitpoints(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	req->result_json = make_json_tool_result(build_special_hitpoints_json(Ships[ship_idx]));
	req->success = true;
}

static void handle_update_ship_special_hitpoints(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_ships, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx = lookup_ship(name, sink);
	if (ship_idx < 0) return;

	// null = disable, numeric = enable+set, omitted = leave unchanged.
	bool hull_null = is_parameter_present_and_null(input, "hull");
	auto hull_arg = hull_null ? std::nullopt : get_optional_integer(input, "hull", sink);
	bool shield_null = is_parameter_present_and_null(input, "shield");
	auto shield_arg = shield_null ? std::nullopt : get_optional_integer(input, "shield", sink);
	if (sink.has_error()) return;

	if (hull_arg.has_value() && *hull_arg < 1) {
		sink.set_error("hull must be at least 1 (got %d).  Pass null to disable.", *hull_arg);
		return;
	}
	if (shield_arg.has_value() && *shield_arg < 0) {
		sink.set_error("shield must be non-negative (got %d).  Pass null to disable.", *shield_arg);
		return;
	}

	ship &shipp = Ships[ship_idx];
	bool changed_hull = false, changed_shield = false;

	if (hull_null) {
		if (shipp.special_hitpoints != 0) {
			shipp.special_hitpoints = 0;
			changed_hull = true;
		}
	} else if (hull_arg.has_value()) {
		if (shipp.special_hitpoints != *hull_arg) {
			shipp.special_hitpoints = *hull_arg;
			changed_hull = true;
		}
	}
	if (shield_null) {
		if (shipp.special_shield != -1) {
			shipp.special_shield = -1;
			changed_shield = true;
		}
	} else if (shield_arg.has_value()) {
		if (shipp.special_shield != *shield_arg) {
			shipp.special_shield = *shield_arg;
			changed_shield = true;
		}
	}

	if (changed_hull) {
		// Recalculate kamikaze_damage from the post-update hull (matches
		// shipspecialhitpoints.cpp:205-215).  In contrast to the dialog,
		// only updates on hull change.
		float hull_for_calc = (shipp.special_hitpoints > 0)
			? (float)shipp.special_hitpoints
			: Ship_info[shipp.ship_info_index].max_hull_strength;
		Ai_info[shipp.ai_index].kamikaze_damage =
			std::min(1000, 200 + (int)(hull_for_calc / 4.0f));
	}

	if (changed_hull || changed_shield)
		mark_modified("MCP: update ship %s special hitpoints", name);

	req->result_json = make_json_tool_result(build_special_hitpoints_json(shipp));
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

	// dock_ships
	{
		json_t *props = json_object();
		add_string_prop(props, "docker", "Name of the docker ship.");
		add_string_prop(props, "dockee", "Name of the dockee ship (must differ from docker).");
		add_string_prop(props, "docker_point",
			"Dockpoint name on the docker's ship class. Use list_ship_class_dockpoints "
			"for available names, or get_ship_class_model_details.docking_bays for full info.");
		add_string_prop(props, "dockee_point",
			"Dockpoint name on the dockee's ship class. Type of dockpoint (cargo/rearm/generic) "
			"must be compatible with docker_point.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("docker"));
		json_array_append_new(req, json_string("dockee"));
		json_array_append_new(req, json_string("docker_point"));
		json_array_append_new(req, json_string("dockee_point"));
		register_tool(tools, "dock_ships",
			"Link two ships at mission start via the named dockpoints (\"+Docked With:\" in the .fs2 file). "
			"The chain's dock leader is auto-resolved per FRED's rules: the one ship whose arrival cue is "
			"not locked-false, tie-broken by ship_class_compare. Non-leader arrival cues may be rewritten "
			"to locked-false to keep only the leader arriving on cue. Note: after running this tool, the "
			"docker will be moved and reoriented to its docked location on the dockee's dockpoint.",
			props, req);
	}

	// undock_ships
	{
		json_t *props = json_object();
		add_string_prop(props, "ship", "Name of the ship.");
		add_string_prop(props, "other_ship",
			"Optional partner ship name. If omitted, the ship must be docked to exactly one other; "
			"otherwise the call errors as ambiguous. The response always echoes the resolved partner.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("ship"));
		register_tool(tools, "undock_ships",
			"Separate two directly-docked ships. Ships are not physically moved apart; issue update_ship "
			"with a new position if you need spatial separation. Arrival cues that were forced to "
			"locked-false by an earlier dock-leader resolution are restored to locked-true if the ship "
			"is no longer docked and is not part of a wing. The response includes still_docked_to: an "
			"array of ship names that 'ship' remains directly docked to after this call (empty when "
			"'ship' has no further dock attachments).",
			props, req);
	}

	// undock_all_ships
	{
		json_t *props = json_object();
		add_string_prop(props, "ship", "Name of the ship to fully undock.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("ship"));
		register_tool(tools, "undock_all_ships",
			"Separate the ship from every other ship it is directly docked to, in one call. "
			"Returns formerly_docked_to listing every ship that was undocked (empty if the ship had no "
			"dock attachments). Otherwise behaves like undock_ships applied iteratively.",
			props, req);
	}

	// list_docked_group
	{
		json_t *props = json_object();
		add_string_prop(props, "ship", "Name of any ship in the docked group.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("ship"));
		register_tool(tools, "list_docked_group",
			"Enumerate every ship transitively docked together with the given ship, including the chain's "
			"dock leader. Prefer this over recursively walking get_ship.docked_to. Solitary ships are "
			"reported as a single-element group with dock_leader: null.",
			props, req);
	}

	// set_dock_leader
	{
		json_t *props = json_object();
		add_string_prop(props, "ship",
			"Name of the ship to designate as the dock leader. Must already be in a docked group "
			"and must not be a wing member.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("ship"));
		register_tool(tools, "set_dock_leader",
			"Override the auto-resolved dock leader for the group containing this ship. Swaps the "
			"previous leader's arrival cue onto the new leader (preserving the chain's effective "
			"arrival behavior) and parks the previous leader on locked-false. Refuses if either the "
			"new leader or the current leader is a wing member, since wing-member effective cues "
			"come from the wing, not the individual ship.",
			props, req);
	}

	// list_ship_weapons
	register_tool_with_required_string(tools, "list_ship_weapons",
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
			"rules (bank range, weapon/bank type match, restricted_to_big_ships match, ammo bounds).");
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
			"Mirrors the engine's per-bank-and-weapon clamp -- use this value as the upper bound "
			"when passing ammo_count to update_ship_weapon_bank.  "
			"Note: the engine rounds pilot banks (capacity / cargo_size) with std::lround but "
			"truncates turret banks; for the same weapon and capacity, the turret value can be "
			"one less than the pilot value.  This tool mirrors whichever rule applies to the "
			"requested bank, so the returned max matches what update_ship_weapon_bank will "
			"accept.",
			props, req);
	}

	// list_ship_subsystems
	register_tool_with_required_string(tools, "list_ship_subsystems",
		"List all subsystems on a ship with their initial-state details: subsystem name, type "
		"(Engines/Turrets/Radar/etc.), initial_health_percent, cargo (if set), cargo_title "
		"(if set), and ai_class (turrets only). For a single subsystem, see get_ship_subsystem; "
		"for updates, see update_ship_subsystem.",
		"name", "Name of the ship");

	// get_ship_subsystem
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship.");
		add_string_prop(props, "subsystem",
			"Subsystem name (matches subobj_name from the model, case-insensitive). Use "
			"list_ship_subsystems to enumerate available names.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("subsystem"));
		register_tool(tools, "get_ship_subsystem",
			"Get one subsystem by name with its initial-state details. For all subsystems on a "
			"ship in one call, see list_ship_subsystems.",
			props, req);
	}

	// update_ship_subsystem
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship.");
		add_string_prop(props, "subsystem",
			"Subsystem name (matches subobj_name from the model, case-insensitive).");
		add_integer_prop(props, "initial_health_percent",
			"Initial health at mission start, as a percent of class max (0-100). "
			"100 = full, 0 = destroyed. Mirrors the Initial Status dialog's \"Damage\" field "
			"(damage = 100 - health).");
		add_string_prop(props, "cargo",
			"Per-subsystem cargo name. Auto-added to the mission cargo pool if new (subject to "
			"MAX_CARGO). Pass \"<none>\" or an empty string to clear. Separate from the ship's "
			"own cargo field on update_ship.");
		add_string_prop(props, "cargo_title",
			"Display label paired with cargo (non-retail; e.g. \"Cargo:\" or \"Passengers:\"). "
			"Requires cargo to also be set (in this call or already on the subsystem); "
			"pass \"<none>\" or an empty string to clear.");
		add_string_prop(props, "ai_class",
			"AI class name. Turret subsystems only; rejected on any other subsystem type. "
			"Use list_ai_classes for valid names. Pass \"<default>\" to reset to the "
			"ship class default.");
		add_bool_prop(props, "force",
			"If true, bypass the scannable-subsystems gate on cargo/cargo_title writes. By "
			"default, those fields are only writable on ships with scannable subsystems "
			"(is_huge_ship XOR toggle-subsystem-scanning flag). Default false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("subsystem"));
		register_tool(tools, "update_ship_subsystem",
			"Update initial-state fields for a single subsystem (health, cargo, cargo_title, "
			"ai_class). All update fields are optional; omitted fields are left unchanged. "
			"Returns the post-update subsystem record.",
			props, req);
	}

	// get_ship_special_explosion
	register_tool_with_required_string(tools, "get_ship_special_explosion",
		"Get the per-ship Special Explosion override fields (changes explosion characteristics "
		"from the ship class defaults).  When the master toggle is off, returns "
		"{ ship, enabled: false } with no other fields.  When on, returns the override values "
		"(damage, blast_force, inner_radius, outer_radius, shockwave_enabled, optional "
		"shockwave_speed when shockwave_enabled is true, optional deathroll_time_ms when "
		"non-zero).  If a ship has no special explosion, the ship class default values "
		"(see get_ship_class) are used.",
		"name", "Name of the ship");

	// update_ship_special_explosion
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship.");
		add_bool_prop(props, "enable",
			"Master toggle for the override (use_special_explosion).  When false, all override "
			"fields are wiped and no other fields may be supplied.  When true, partial updates "
			"apply; any value not specified uses the previously specified value, or the ship class "
			"default value (see get_ship_class) if never specified.");
		add_integer_prop(props, "damage", "Explosion damage. Non-negative.");
		add_integer_prop(props, "blast_force",
			"Explosion blast force. Non-negative.");
		add_integer_prop(props, "inner_radius",
			"Inner shockwave radius (>= 1, must not exceed outer_radius).");
		add_integer_prop(props, "outer_radius",
			"Outer shockwave radius (>= 2).");
		add_bool_prop(props, "shockwave_enabled",
			"Whether the explosion should produce a shockwave.");
		add_integer_prop(props, "shockwave_speed",
			"Shockwave propagation speed. Must be >= 1 when shockwave_enabled is true; "
			"wiped to 0 when shockwave_enabled is false.");
		add_integer_prop(props, "deathroll_time_ms",
			"How long, in milliseconds, the ship tumbles in its death roll before the final explosion.  0 means inherit the class default; "
			"otherwise must be at least 2 ms.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("enable"));
		register_tool(tools, "update_ship_special_explosion",
			"Update the per-ship Special Explosion override fields.  Set 'enable' to false to "
			"wipe the override (no other fields permitted in that case).  Set 'enable' to true "
			"to activate or modify the override; partial updates are allowed (omitted fields "
			"retain their current value or use the ship class's defaults if never overridden).  "
			"Validation: inner_radius <= outer_radius; "
			"deathroll_time_ms is 0 or >= 2; shockwave_speed >= 1 when shockwave_enabled.",
			props, req);
	}

	// get_ship_special_hitpoints
	register_tool_with_required_string(tools, "get_ship_special_hitpoints",
		"Get the per-ship Special Hitpoints overrides (changes hull and shield hitpoints from the "
		"ship class standard values).  Each field is independent and is omitted from the response "
		"when not overridden.  Standard values (max_hull_strength, max_shield_strength) are "
		"exposed via get_ship_class.",
		"name", "Name of the ship");

	// update_ship_special_hitpoints
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship.");
		add_integer_prop(props, "hull",
			"Hull strength override.  Numeric value (>= 1) enables and sets; explicit null "
			"disables the override; omitting the field leaves it unchanged.");
		add_integer_prop(props, "shield",
			"Shield strength override.  Numeric value (>= 0; 0 is a valid no-shields config) "
			"enables and sets; explicit null disables the override; omitting the field leaves "
			"it unchanged.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_ship_special_hitpoints",
			"Update the per-ship Special Hitpoints overrides.  Each of 'hull' and 'shield' is "
			"independently optional: numeric value enables+sets, explicit null disables, "
			"omitting leaves unchanged.  When either field changes, "
			"kamikaze_damage is also recalculated, but the kamikaze flag is not changed.",
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
	} else if (strcmp(tool_name, "dock_ships") == 0) {
		handle_dock_ships(input_json, req);
	} else if (strcmp(tool_name, "undock_ships") == 0) {
		handle_undock_ships(input_json, req);
	} else if (strcmp(tool_name, "undock_all_ships") == 0) {
		handle_undock_all_ships(input_json, req);
	} else if (strcmp(tool_name, "list_docked_group") == 0) {
		handle_list_docked_group(input_json, req);
	} else if (strcmp(tool_name, "set_dock_leader") == 0) {
		handle_set_dock_leader(input_json, req);
	} else if (strcmp(tool_name, "list_ship_weapons") == 0) {
		handle_list_ship_weapons(input_json, req);
	} else if (strcmp(tool_name, "get_ship_weapon_bank") == 0) {
		handle_get_ship_weapon_bank(input_json, req);
	} else if (strcmp(tool_name, "update_ship_weapon_bank") == 0) {
		handle_update_ship_weapon_bank(input_json, req);
	} else if (strcmp(tool_name, "get_max_ammo_for_bank") == 0) {
		handle_get_max_ammo_for_bank(input_json, req);
	} else if (strcmp(tool_name, "list_ship_subsystems") == 0) {
		handle_list_ship_subsystems(input_json, req);
	} else if (strcmp(tool_name, "get_ship_subsystem") == 0) {
		handle_get_ship_subsystem(input_json, req);
	} else if (strcmp(tool_name, "update_ship_subsystem") == 0) {
		handle_update_ship_subsystem(input_json, req);
	} else if (strcmp(tool_name, "get_ship_special_explosion") == 0) {
		handle_get_ship_special_explosion(input_json, req);
	} else if (strcmp(tool_name, "update_ship_special_explosion") == 0) {
		handle_update_ship_special_explosion(input_json, req);
	} else if (strcmp(tool_name, "get_ship_special_hitpoints") == 0) {
		handle_get_ship_special_hitpoints(input_json, req);
	} else if (strcmp(tool_name, "update_ship_special_hitpoints") == 0) {
		handle_update_ship_special_hitpoints(input_json, req);
	} else {
		return false;
	}
	return true;
}
