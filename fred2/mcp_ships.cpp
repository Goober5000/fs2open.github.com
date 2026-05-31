#include "stdafx.h"
#include "mcp_ships.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcp_sexp.h"
#include "mcp_sexp_forest.h"
#include "mcpserver.h"

#include <jansson.h>
#include <cstring>
#include <optional>

#include "globalincs/utility.h"

#include "ai/ai.h"
#include "iff_defs/iff_defs.h"
#include "mission/missionmessage.h"   // Personas
#include "mission/missionparse.h"
#include "missioneditor/common.h"     // target_to_anchor, anchor_to_target, stuff_special_arrival_anchor_name
#include "object/object.h"
#include "parse/parselo.h"
#include "parse/sexp.h"
#include "ship/anchor_t.h"
#include "ship/ship.h"

#include "fred.h"                     // Ship_editor_dialog
#include "management.h"               // create_ship, delete_ship, rename_ship, invalidate_references, etc.

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

	return obj;
}

// ---------------------------------------------------------------------------
// Display name helper - empty / "<none>" / matches ship name => clear the flag
// ---------------------------------------------------------------------------

static void apply_display_name(ship &shipp, const char *display_name)
{
	if (!*display_name || !stricmp(display_name, "<none>") || !strcmp(display_name, shipp.ship_name)) {
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

	int ship_idx = ship_name_lookup(name, 1);
	if (ship_idx < 0) {
		set_not_found_error(sink, "Ship", name);
		return;
	}

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
	auto assist_pct        = get_optional_double(input, "assist_score_fraction", sink);

	auto arrival_loc_str   = get_optional_string(input, "arrival_location", sink);
	auto arrival_tgt_str   = get_optional_string(input, "arrival_target", sink);
	auto arrival_distance  = get_optional_integer(input, "arrival_distance", sink);
	auto arrival_delay     = get_optional_integer(input, "arrival_delay", sink);
	auto arrival_cue       = get_optional_integer(input, "arrival_cue", sink);

	auto departure_loc_str = get_optional_string(input, "departure_location", sink);
	auto departure_tgt_str = get_optional_string(input, "departure_target", sink);
	auto departure_delay   = get_optional_integer(input, "departure_delay", sink);
	auto departure_cue     = get_optional_integer(input, "departure_cue", sink);

	if (sink.has_error()) return;

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
		shipp.assist_score_pct = std::clamp((float)(*assist_pct), 0.0f, 1.0f);
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

	int ship_idx = ship_name_lookup(name, 1);
	if (ship_idx < 0) {
		set_not_found_error(sink, "Ship", name);
		return;
	}

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
	auto assist_pct        = get_optional_double(input, "assist_score_fraction", sink);
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

	if (sink.has_error()) return;

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
		shipp.assist_score_pct = std::clamp((float)*assist_pct, 0.0f, 1.0f);
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

	int ship_idx = ship_name_lookup(name, 1);
	if (ship_idx < 0) {
		set_not_found_error(sink, "Ship", name);
		return;
	}

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
			"Optional display name shown to the player (if different from `name`). Pass an empty string or \"<none>\" to clear.");
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
			"Display name shown to the player. Empty/\"<none>\" clears.");
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
			"Toggle player-start flag. Refused if turning off would leave zero player starts.");
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
	} else {
		return false;
	}
	return true;
}
