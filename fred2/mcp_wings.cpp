#include "stdafx.h"
#include "mcp_wings.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcp_sexp.h"
#include "mcp_sexp_forest.h"
#include "mcpserver.h"

#include <jansson.h>
#include <cstring>
#include <optional>

#include "ai/ai.h"                    // get_absolute_wing_pos
#include "globalincs/utility.h"       // find_item_with_string
#include "mission/missionparse.h"
#include "missioneditor/common.h"     // FredWingSlotConfig, reassign_wing_slot, swap_wing_slots, update_custom_wing_indexes
#include "object/object.h"
#include "parse/sexp.h"
#include "parse/parselo.h"
#include "ship/anchor_t.h"
#include "ship/ship.h"                // Wings, MAX_WINGS, ship_query_general_type, wing_formation_lookup, wing_bash_ship_name

#include "management.h"               // cur_wing, wing_objects, cur_object_index, mark_object, unmark_all, find_free_wing, invalidate_references
#include "wing.h"                     // create_wing, delete_wing, disband_wing

// ---------------------------------------------------------------------------
// Dialog conflict guard
// ---------------------------------------------------------------------------

static bool validate_dialog_for_wings(SCP_string &error_msg)
{
	return validate_single_dialog("wings", "wing", error_msg);
}

// ---------------------------------------------------------------------------
// Wing JSON serialization
// ---------------------------------------------------------------------------

static json_t *build_wing_flags_array(int wing_idx);

static json_t *build_wing_json(int wing_idx, bool include_details)
{
	const wing &wingp = Wings[wing_idx];

	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_safe_string(wingp.name));
	json_object_set_new(obj, "wave_count", json_integer(wingp.wave_count));
	json_object_set_new(obj, "num_waves", json_integer(wingp.num_waves));

	// Member ship names in slot order
	json_t *members = json_array();
	for (int i = 0; i < wingp.wave_count; ++i)
		json_array_append_new(members, json_safe_string(Ships[wingp.ship_index[i]].ship_name));
	json_object_set_new(obj, "members", members);

	if (!include_details)
		return obj;

	// Identity
	if (wingp.has_display_name())
		json_object_set_new(obj, "display_name", json_safe_string(wingp.display_name.c_str()));
	if (wingp.hotkey >= 0)
		json_object_set_new(obj, "hotkey", json_integer(wingp.hotkey));
	set_optional_filename(obj, "wing_squad_logo_filename", wingp.wing_squad_filename);

	// Waves
	json_object_set_new(obj, "new_wave_threshold", json_integer(wingp.threshold));
	json_object_set_new(obj, "wave_delay_min", json_integer(wingp.wave_delay_min));
	json_object_set_new(obj, "wave_delay_max", json_integer(wingp.wave_delay_max));

	// Arrival
	json_object_set_new(obj, "arrival_location",
		json_safe_string(Arrival_location_names[static_cast<int>(wingp.arrival_location)]));
	auto arr_tgt = anchor_to_name(wingp.arrival_anchor);
	if (!arr_tgt.empty())
		json_object_set_new(obj, "arrival_target", json_safe_string(arr_tgt.c_str()));
	json_object_set_new(obj, "arrival_distance", json_integer(wingp.arrival_distance));
	json_object_set_new(obj, "arrival_delay", json_integer(wingp.arrival_delay));
	json_object_set_new(obj, "arrival_cue", json_integer(wingp.arrival_cue));

	// Departure
	json_object_set_new(obj, "departure_location",
		json_safe_string(Departure_location_names[static_cast<int>(wingp.departure_location)]));
	auto dep_tgt = anchor_to_name(wingp.departure_anchor);
	if (!dep_tgt.empty())
		json_object_set_new(obj, "departure_target", json_safe_string(dep_tgt.c_str()));
	json_object_set_new(obj, "departure_delay", json_integer(wingp.departure_delay));
	json_object_set_new(obj, "departure_cue", json_integer(wingp.departure_cue));

	// Formation
	if (wingp.formation >= 0 && wingp.formation < (int)Wing_formations.size())
		json_object_set_new(obj, "formation", json_safe_string(Wing_formations[wingp.formation].name));
	json_object_set_new(obj, "formation_scale", json_real(wingp.formation_scale));

	// Flags (Wing_Flags; see list_wing_flags)
	json_object_set_new(obj, "wing_flags", build_wing_flags_array(wing_idx));

	return obj;
}

// ---------------------------------------------------------------------------
// Wing flags (single Wing_Flags enum; iterate Parse_wing_flags directly so we
// cover all 11 mission-parseable wing flags -- the engine's sexp_check_flag_array
// only reaches 8 of them).
// ---------------------------------------------------------------------------

// don't include these in the valid flags for MCP for various reasons:
//   reinforcement: edits go through reinforcement tools (to keep the Reinforcements vector in sync)
bool mcp_wing_flag_excluded(const Ship::Wing_Flags &candidate)
{
	return candidate == Ship::Wing_Flags::Reinforcement;
}

static int find_wing_flag_by_name(const char *name)
{
	for (size_t i = 0; i < Num_parse_wing_flags; i++) {
		if (!Parse_wing_flags[i].in_use)
			continue;
		if (mcp_wing_flag_excluded(Parse_wing_flags[i].def))
			continue;
		if (!stricmp(name, Parse_wing_flags[i].name))
			return (int)i;
	}
	return -1;
}

static json_t *build_wing_flags_array(int wing_idx)
{
	const wing &wingp = Wings[wing_idx];
	json_t *arr = json_array();
	for (size_t i = 0; i < Num_parse_wing_flags; i++) {
		if (!Parse_wing_flags[i].in_use)
			continue;
		if (wingp.flags[Parse_wing_flags[i].def] && !mcp_wing_flag_excluded(Parse_wing_flags[i].def))
			json_array_append_new(arr, json_string(Parse_wing_flags[i].name));
	}
	return arr;
}

// Validate-only pass: confirm flags_obj is an object of {name: bool} where each
// name resolves to a Parse_wing_flags entry.
static bool validate_wing_flags_only(json_t *flags_obj, McpErrorSink &sink)
{
	if (!flags_obj)
		return true;
	if (!json_is_object(flags_obj)) {
		sink.set_error("Parameter 'wing_flags' must be an object of {flag_name: bool}");
		return false;
	}
	const char *key;
	json_t *val;
	json_object_foreach(flags_obj, key, val) {
		if (!json_is_boolean(val)) {
			sink.set_error("wing_flags.%s must be a boolean", key);
			return false;
		}
		if (find_wing_flag_by_name(key) < 0) {
			sink.set_error("Unknown wing flag '%s' (use list_wing_flags to see valid names)", key);
			return false;
		}
	}
	return true;
}

// Apply a partial wing_flags update.  flags_obj may be null (no-op).
// Returns true iff wingp.flags actually changed.  Callers must validate
// flags_obj up-front via validate_wing_flags_only.
static bool apply_wing_flags_partial(int wing_idx, json_t *flags_obj, McpErrorSink &/*sink*/)
{
	if (!flags_obj)
		return false;

	wing &wingp = Wings[wing_idx];
	auto before = wingp.flags;

	const char *key;
	json_t *val;
	json_object_foreach(flags_obj, key, val) {
		int idx = find_wing_flag_by_name(key);
		wingp.flags.set(Parse_wing_flags[idx].def, json_boolean_value(val));
	}
	return wingp.flags != before;
}

// ---------------------------------------------------------------------------
// Display name helper (mirrors apply_display_name in mcp_ships.cpp)
// ---------------------------------------------------------------------------

// Returns true iff the call actually changed display_name or the
// Has_display_name flag.
static bool apply_wing_display_name(wing &wingp, const char *display_name)
{
	// A blank display name is a valid display name.  To remove a display
	// name, pass "<none>" or a string matching the wing's regular name.
	bool clearing = !stricmp(display_name, "<none>") || !strcmp(display_name, wingp.name);
	bool had_flag = wingp.flags[Ship::Wing_Flags::Has_display_name];

	if (clearing) {
		if (!had_flag && wingp.display_name.empty())
			return false;
		wingp.display_name = "";
		wingp.flags.remove(Ship::Wing_Flags::Has_display_name);
		return true;
	}

	if (had_flag && wingp.display_name == display_name)
		return false;
	wingp.display_name = display_name;
	wingp.flags.set(Ship::Wing_Flags::Has_display_name);
	return true;
}

// ---------------------------------------------------------------------------
// Wing rename (no engine helper exists; mirrors rename_ship semantics)
// ---------------------------------------------------------------------------

// Rewrites Wings[w].name, cascades the rename through SEXP references, AI
// goals, texture replacements, and re-bashes member ship names to match.
// Caller must have already validated new_name (length, uniqueness).
static void rename_wing(int wing_idx, const char *new_name)
{
	wing &wingp = Wings[wing_idx];

	if (!strcmp(wingp.name, new_name))
		return;

	// Cascade the rename through global references.
	update_sexp_references(wingp.name, new_name);
	ai_update_goal_references(sexp_ref_type::WING, wingp.name, new_name);
	update_texture_replacements(wingp.name, new_name);
	int reinforcement_idx = find_item_with_string(Reinforcements, &reinforcements::name, wingp.name);
	if (reinforcement_idx >= 0)
		strcpy_s(Reinforcements[reinforcement_idx].name, new_name);

	// Re-bash member ship names: "OldWing 1" -> "NewWing 1", etc.
	for (int i = 0; i < wingp.wave_count; ++i) {
		char bashed[NAME_LENGTH];
		wing_bash_ship_name(bashed, new_name, i + 1);
		rename_ship(wingp.ship_index[i], bashed);
	}

	// Update the wing's own name and re-derive display name (same rule as rename_ship).
	string_copy(wingp.name, new_name, NAME_LENGTH - 1);
	if (get_pointer_to_first_hash_symbol(wingp.name)) {
		wingp.display_name = wingp.name;
		end_string_at_first_hash_symbol(wingp.display_name);
		wingp.flags.set(Ship::Wing_Flags::Has_display_name);
	} else {
		wingp.display_name = "";
		wingp.flags.remove(Ship::Wing_Flags::Has_display_name);
	}

	update_custom_wing_indexes();
}

// ---------------------------------------------------------------------------
// Member-ship preflight for form_wing
//
// Validate every named member exists, isn't already in a wing (unless we're
// re-forming, in which case members of the wing being re-formed are OK since
// they'll be released first), and has a ship class allowed in wings.  Done
// up-front so create_wing() never has a reason to pop a modal MessageBox.
// ---------------------------------------------------------------------------

static bool preflight_member_ships(const SCP_vector<SCP_string> &member_names,
	int reform_wing_idx, SCP_vector<int> &out_ship_indices, McpErrorSink &sink)
{
	if (member_names.empty()) {
		sink.set_error("Parameter 'members' must list at least one ship name.");
		return false;
	}
	if ((int)member_names.size() > MAX_SHIPS_PER_WING) {
		sink.set_error("Wing can have at most %d ships; got " SIZE_T_ARG ".",
			MAX_SHIPS_PER_WING, member_names.size());
		return false;
	}

	int reference_team = -1;
	for (const auto &name : member_names) {
		int ship_idx = ship_name_lookup(name.c_str(), 1);
		if (ship_idx < 0) {
			sink.set_error("Member ship not found: '%s'", name.c_str());
			return false;
		}
		if (Ships[ship_idx].wingnum >= 0 && Ships[ship_idx].wingnum != reform_wing_idx) {
			sink.set_error("Member ship '%s' is already in wing '%s'.",
				name.c_str(), Wings[Ships[ship_idx].wingnum].name);
			return false;
		}
		int ship_type = ship_query_general_type(ship_idx);
		if (ship_type < 0 ||
			!Ship_types[ship_type].flags[Ship::Type_Info_Flags::AI_can_form_wing]) {
			sink.set_error("Member ship '%s' has a class that cannot form a wing.", name.c_str());
			return false;
		}
		if (reference_team < 0)
			reference_team = Ships[ship_idx].team;
		else if (Ships[ship_idx].team != reference_team) {
			sink.set_error("Member ship '%s' is on a different team than the other members; "
				"all wing members must share a team.", name.c_str());
			return false;
		}
		out_ship_indices.push_back(ship_idx);
	}
	return true;
}

// ---------------------------------------------------------------------------
// In-place wing re-form: replace the roster, preserve everything else.
//
// Releases all current members (renames each back to its engine-default name
// via remove_ship_from_wing), then re-attaches the supplied members using the
// same name-bashing / cue-locking pattern as create_wing()'s add-ships loop.
//
// What is preserved (because the wing slot is reused):
//   name, display_name, squad logo, hotkey, num_waves, threshold,
//   wave_delay_min/max, arrival/departure location/anchor/distance/delay,
//   arrival/departure cues, formation, formation_scale, wing_flags,
//   reinforcement entry, SEXP / AI-goal references to the wing name.
//
// What is reset: special_ship becomes 0 (first new member is the leader),
// matching the engine convention.  Num_wings is unchanged on net (no decrement
// during dissolve, no increment during re-add).
// ---------------------------------------------------------------------------

static void reform_wing_in_place(int wing_num, const SCP_vector<int> &new_member_ship_indices)
{
	// Iterate from the back: remove_ship_from_wing shifts the last slot into
	// the removed position, so a forward loop would re-process the shifted
	// element and walk off the end.
	for (int i = Wings[wing_num].wave_count - 1; i >= 0; --i) {
		object *ptr = &Objects[wing_objects[wing_num][i]];
		if (ptr->type == OBJ_SHIP)
			remove_ship_from_wing(ptr->instance, 0);
		else if (ptr->type == OBJ_START)
			remove_player_from_wing(ptr->instance, 0);
	}

	// Re-attach the new members.  Mirrors create_wing()'s add-ships loop.
	int count = 0;
	for (int ship_idx : new_member_ship_indices) {
		int slot = count++;
		char bashed[NAME_LENGTH];
		wing_bash_ship_name(bashed, Wings[wing_num].name, slot + 1);
		rename_ship(ship_idx, bashed);
		// bash again for the display name
		wing_bash_ship_name(&Ships[ship_idx], &Wings[wing_num], slot + 1, true);

		Wings[wing_num].ship_index[slot] = ship_idx;
		Ships[ship_idx].wingnum = wing_num;

		if (Ships[ship_idx].arrival_cue >= 0)
			free_sexp2(Ships[ship_idx].arrival_cue);
		Ships[ship_idx].arrival_cue = Locked_sexp_false;
		if (Ships[ship_idx].departure_cue >= 0)
			free_sexp2(Ships[ship_idx].departure_cue);
		Ships[ship_idx].departure_cue = Locked_sexp_false;

		wing_objects[wing_num][slot] = Ships[ship_idx].objnum;
	}

	Wings[wing_num].wave_count = count;
	Wings[wing_num].special_ship = 0;	// first new member is the leader
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void handle_list_wings(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_wings, sink)) return;

	json_t *arr = json_array();
	// Same slot walk as wing_slot_at_public_index, so a wing's 1-based
	// position in this list is its move_wing/swap_wings index
	for (int i = 0; i < MAX_WINGS; ++i) {
		if (Wings[i].wave_count == 0)
			continue;
		json_array_append_new(arr, build_wing_json(i, false));
	}

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_wing(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_wings, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int wing_idx = lookup_wing(name, sink);
	if (wing_idx < 0) return;

	req->result_json = make_json_tool_result(build_wing_json(wing_idx, true));
	req->success = true;
}

static void handle_form_wing(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_wings, sink)) return;

	// Required
	auto name = get_required_string(input, "name", sink, true, NAME_LENGTH - 1);
	if (!name) return;

	auto members_opt = get_optional_string_array(input, "members", sink);
	if (sink.has_error()) return;
	if (!members_opt.has_value() || members_opt->empty()) {
		sink.set_error("Parameter 'members' is required and must list at least one ship name.");
		return;
	}

	// Optional
	auto display_name        = get_optional_string(input, "display_name", sink, NAME_LENGTH - 1);
	auto hotkey              = get_optional_integer(input, "hotkey", sink);
	auto wing_squad_filename = get_optional_filename(input, "wing_squad_logo_filename", sink, true);
	auto num_waves           = get_optional_integer(input, "num_waves", sink);
	auto threshold           = get_optional_integer(input, "new_wave_threshold", sink);
	auto wave_delay_min      = get_optional_integer(input, "wave_delay_min", sink);
	auto wave_delay_max      = get_optional_integer(input, "wave_delay_max", sink);

	auto arrival_loc_str     = get_optional_string(input, "arrival_location", sink);
	auto arrival_tgt_str     = get_optional_string(input, "arrival_target", sink);
	auto arrival_distance    = get_optional_integer(input, "arrival_distance", sink);
	auto arrival_delay       = get_optional_integer(input, "arrival_delay", sink);
	auto arrival_cue         = get_optional_integer(input, "arrival_cue", sink);

	auto departure_loc_str   = get_optional_string(input, "departure_location", sink);
	auto departure_tgt_str   = get_optional_string(input, "departure_target", sink);
	auto departure_delay     = get_optional_integer(input, "departure_delay", sink);
	auto departure_cue       = get_optional_integer(input, "departure_cue", sink);

	auto formation_str       = get_optional_string(input, "formation", sink);
	auto formation_scale     = get_optional_float(input, "formation_scale", sink);

	auto reform              = get_optional_bool(input, "reform", sink);

	json_t *wing_flags_in    = json_object_get(input, "wing_flags");

	if (sink.has_error()) return;

	// Validate wing_flags up-front so a bad flag name aborts before mutation.
	if (!validate_wing_flags_only(wing_flags_in, sink)) return;

	// Decide whether this is a re-form (recreate-in-place) or a fresh form.
	int reform_wing_idx = wing_name_lookup(name);
	bool reforming = (reform_wing_idx >= 0) && reform.has_value() && *reform;

	if (!reforming) {
		// Fresh form: name must not already be taken, and we need a free slot.
		if (!check_object_rename("wing", name, sink)) return;

		if (find_free_wing() < 0) {
			sink.set_error("Maximum number of wings (%d) already in use.", MAX_WINGS);
			return;
		}
	}

	SCP_vector<int> member_ship_indices;
	if (!preflight_member_ships(*members_opt, reforming ? reform_wing_idx : -1,
			member_ship_indices, sink))
		return;

	// Pre-resolve optional fields so we can fail before any state changes
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

	if (hotkey.has_value() && !check_int_range(*hotkey, 0, 9, "hotkey", sink))
		return;

	int formation_idx = -1;
	if (formation_str) {
		if (!*formation_str || !stricmp(formation_str, "default")) {
			formation_idx = -1;	// default retail formation
		} else {
			formation_idx = check_lookup(formation_str, wing_formation_lookup, "formation", sink);
			if (formation_idx < 0) return;
		}
	}

	int wing_idx;
	if (reforming) {
		// In-place roster swap: keep the wing slot and all its settings.
		reform_wing_in_place(reform_wing_idx, member_ship_indices);
		wing_idx = reform_wing_idx;
	} else {
		// Fresh form: drive create_wing via the marked-objects protocol.
		SCP_vector<int> previously_marked;
		for (auto p : list_range(&obj_used_list)) {
			if (p->flags[Object::Object_Flags::Marked])
				previously_marked.push_back(OBJ_INDEX(p));
		}
		int saved_cur_object = cur_object_index;

		unmark_all();
		for (int ship_idx : member_ship_indices)
			mark_object(Ships[ship_idx].objnum);
		cur_object_index = Ships[member_ship_indices.front()].objnum;

		int rc = create_wing(name);

		// Restore prior selection regardless of outcome.
		unmark_all();
		for (int obj : previously_marked)
			mark_object(obj);
		cur_object_index = saved_cur_object;

		if (rc != 0) {
			sink.set_error("create_wing() failed for '%s' (engine returned %d).", name, rc);
			return;
		}

		wing_idx = wing_name_lookup(name);
		if (wing_idx < 0) {
			sink.set_error("create_wing() reported success but wing '%s' could not be looked up.", name);
			return;
		}
	}
	wing &wingp = Wings[wing_idx];

	// Apply optional fields
	if (display_name)
		apply_wing_display_name(wingp, display_name);
	if (hotkey.has_value())
		wingp.hotkey = *hotkey;
	if (wing_squad_filename)
		strcpy_s(wingp.wing_squad_filename, wing_squad_filename);
	if (num_waves.has_value())
		wingp.num_waves = *num_waves;
	if (threshold.has_value())
		wingp.threshold = *threshold;
	if (wave_delay_min.has_value())
		wingp.wave_delay_min = *wave_delay_min;
	if (wave_delay_max.has_value())
		wingp.wave_delay_max = *wave_delay_max;

	if (arr_loc >= 0)
		wingp.arrival_location = static_cast<ArrivalLocation>(arr_loc);
	if (arrival_tgt_str)
		wingp.arrival_anchor = arr_anchor;
	if (arrival_distance.has_value())
		wingp.arrival_distance = *arrival_distance;
	if (arrival_delay.has_value())
		wingp.arrival_delay = *arrival_delay;
	if (arrival_cue.has_value())
		replace_cue(wingp.arrival_cue, *arrival_cue);

	if (dep_loc >= 0)
		wingp.departure_location = static_cast<DepartureLocation>(dep_loc);
	if (departure_tgt_str)
		wingp.departure_anchor = dep_anchor;
	if (departure_delay.has_value())
		wingp.departure_delay = *departure_delay;
	if (departure_cue.has_value())
		replace_cue(wingp.departure_cue, *departure_cue);

	if (formation_str)
		wingp.formation = formation_idx;
	if (formation_scale.has_value())
		wingp.formation_scale = *formation_scale;

	// Apply wing_flags (already validated above).
	apply_wing_flags_partial(wing_idx, wing_flags_in, sink);

	mark_modified(reforming ? "MCP: re-form wing %s" : "MCP: form wing %s", name);

	req->result_json = make_json_tool_result(build_wing_json(wing_idx, true));
	req->success = true;
}

static void handle_update_wing(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_wings, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int wing_idx = lookup_wing(name, sink);
	if (wing_idx < 0) return;

	auto new_name            = get_optional_string(input, "new_name", sink, NAME_LENGTH - 1);
	auto display_name        = get_optional_string(input, "display_name", sink, NAME_LENGTH - 1);
	auto hotkey              = get_optional_integer(input, "hotkey", sink);
	auto wing_squad_filename = get_optional_filename(input, "wing_squad_logo_filename", sink, true);
	auto num_waves           = get_optional_integer(input, "num_waves", sink);
	auto threshold           = get_optional_integer(input, "new_wave_threshold", sink);
	auto wave_delay_min      = get_optional_integer(input, "wave_delay_min", sink);
	auto wave_delay_max      = get_optional_integer(input, "wave_delay_max", sink);

	auto arrival_loc_str     = get_optional_string(input, "arrival_location", sink);
	auto arrival_tgt_str     = get_optional_string(input, "arrival_target", sink);
	auto arrival_distance    = get_optional_integer(input, "arrival_distance", sink);
	auto arrival_delay       = get_optional_integer(input, "arrival_delay", sink);
	auto arrival_cue         = get_optional_integer(input, "arrival_cue", sink);

	auto departure_loc_str   = get_optional_string(input, "departure_location", sink);
	auto departure_tgt_str   = get_optional_string(input, "departure_target", sink);
	auto departure_delay     = get_optional_integer(input, "departure_delay", sink);
	auto departure_cue       = get_optional_integer(input, "departure_cue", sink);

	auto formation_str       = get_optional_string(input, "formation", sink);
	auto formation_scale     = get_optional_float(input, "formation_scale", sink);

	json_t *wing_flags_in    = json_object_get(input, "wing_flags");

	if (sink.has_error()) return;

	// Validate wing_flags up-front so a bad flag name aborts before mutation.
	if (!validate_wing_flags_only(wing_flags_in, sink)) return;

	wing &wingp = Wings[wing_idx];
	// Each branch sets changed=true only when it actually mutates state, so
	// no-op calls (omitted fields, fields equal to current values) don't autosave.
	bool changed = false;

	// Rename
	if (new_name && stricmp(wingp.name, new_name) != 0) {
		if (!check_object_rename("wing", new_name, sink, -1, wing_idx)) return;
		rename_wing(wing_idx, new_name);
		mcp_sexp_forest_mark_dirty();
		changed = true;
	}

	if (display_name && apply_wing_display_name(wingp, display_name))
		changed = true;

	if (hotkey.has_value()) {
		if (!check_int_range(*hotkey, 0, 9, "hotkey", sink)) return;
		if (wingp.hotkey != *hotkey) {
			wingp.hotkey = *hotkey;
			changed = true;
		}
	}

	if (wing_squad_filename
		&& strcmp(wingp.wing_squad_filename, wing_squad_filename) != 0) {
		strcpy_s(wingp.wing_squad_filename, wing_squad_filename);
		changed = true;
	}

	if (num_waves.has_value() && wingp.num_waves != *num_waves) {
		wingp.num_waves = *num_waves;
		changed = true;
	}
	if (threshold.has_value() && wingp.threshold != *threshold) {
		wingp.threshold = *threshold;
		changed = true;
	}
	if (wave_delay_min.has_value() && wingp.wave_delay_min != *wave_delay_min) {
		wingp.wave_delay_min = *wave_delay_min;
		changed = true;
	}
	if (wave_delay_max.has_value() && wingp.wave_delay_max != *wave_delay_max) {
		wingp.wave_delay_max = *wave_delay_max;
		changed = true;
	}

	if (arrival_loc_str) {
		int loc = check_lookup(arrival_loc_str, arrival_location_enum_values, "arrival_location", sink);
		if (loc < 0) return;
		auto new_loc = static_cast<ArrivalLocation>(loc);
		if (wingp.arrival_location != new_loc) {
			wingp.arrival_location = new_loc;
			changed = true;
		}
	}
	if (arrival_tgt_str) {
		anchor_t a = anchor_t::invalid();
		if (!resolve_target_name_to_anchor(arrival_tgt_str, a, sink)) return;
		if (wingp.arrival_anchor != a) {
			wingp.arrival_anchor = a;
			changed = true;
		}
	}
	if (arrival_distance.has_value() && wingp.arrival_distance != *arrival_distance) {
		wingp.arrival_distance = *arrival_distance;
		changed = true;
	}
	if (arrival_delay.has_value() && wingp.arrival_delay != *arrival_delay) {
		wingp.arrival_delay = *arrival_delay;
		changed = true;
	}
	if (arrival_cue.has_value()) {
		if (!check_sexp_formula(*arrival_cue, OPR_BOOL, sink)) return;
		if (replace_cue(wingp.arrival_cue, *arrival_cue))
			changed = true;
	}

	if (departure_loc_str) {
		int loc = check_lookup(departure_loc_str, departure_location_enum_values, "departure_location", sink);
		if (loc < 0) return;
		auto new_loc = static_cast<DepartureLocation>(loc);
		if (wingp.departure_location != new_loc) {
			wingp.departure_location = new_loc;
			changed = true;
		}
	}
	if (departure_tgt_str) {
		anchor_t a = anchor_t::invalid();
		if (!resolve_target_name_to_anchor(departure_tgt_str, a, sink)) return;
		if (wingp.departure_anchor != a) {
			wingp.departure_anchor = a;
			changed = true;
		}
	}
	if (departure_delay.has_value() && wingp.departure_delay != *departure_delay) {
		wingp.departure_delay = *departure_delay;
		changed = true;
	}
	if (departure_cue.has_value()) {
		if (!check_sexp_formula(*departure_cue, OPR_BOOL, sink)) return;
		if (replace_cue(wingp.departure_cue, *departure_cue))
			changed = true;
	}

	if (formation_str) {
		int new_formation;
		if (!*formation_str || !stricmp(formation_str, "default")) {
			new_formation = -1;
		} else {
			new_formation = check_lookup(formation_str, wing_formation_lookup, "formation", sink);
			if (new_formation < 0) return;
		}
		if (wingp.formation != new_formation) {
			wingp.formation = new_formation;
			changed = true;
		}
	}
	if (formation_scale.has_value() && wingp.formation_scale != *formation_scale) {
		wingp.formation_scale = *formation_scale;
		changed = true;
	}

	// Apply wing_flags (already validated above).
	if (apply_wing_flags_partial(wing_idx, wing_flags_in, sink))
		changed = true;

	if (changed)
		mark_modified("MCP: update wing %s", wingp.name);

	req->result_json = make_json_tool_result(build_wing_json(wing_idx, true));
	req->success = true;
}

// ---------------------------------------------------------------------------
// arrange_in_formation — mirrors FRED's "Align" button (OnWingFormationAlign).
// Optional formation/formation_scale are temporary overrides for this call only;
// the wing's persistent values are restored after the arrangement loop.
// ---------------------------------------------------------------------------

static void handle_arrange_in_formation(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_wings, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int wing_idx = lookup_wing(name, sink);
	if (wing_idx < 0) return;

	auto formation_str   = get_optional_string(input, "formation", sink);
	auto formation_scale = get_optional_float(input, "formation_scale", sink);
	if (sink.has_error()) return;

	wing &wingp = Wings[wing_idx];

	// Save current formation/scale so we can restore after arrangement.
	int old_formation = wingp.formation;
	float old_formation_scale = wingp.formation_scale;

	// Temporarily apply optional overrides.  Validation happens before mutation,
	// so if a bad formation name is given we return without touching the wing.
	if (formation_str) {
		if (!*formation_str || !stricmp(formation_str, "default")) {
			wingp.formation = -1;
		} else {
			int formation_idx = check_lookup(formation_str, wing_formation_lookup, "formation", sink);
			if (formation_idx < 0) return;
			wingp.formation = formation_idx;
		}
	}
	if (formation_scale.has_value())
		wingp.formation_scale = *formation_scale;

	object *leader_objp = &Objects[Ships[wingp.ship_index[0]].objnum];
	for (int i = 1; i < wingp.wave_count; i++) {
		object *objp = &Objects[Ships[wingp.ship_index[i]].objnum];
		get_absolute_wing_pos(&objp->pos, leader_objp, wing_idx, i, false);
		objp->orient = leader_objp->orient;

		// Post-move bookkeeping the FRED Align button omits: drag any docked
		// partners along (no-op for undocked ships)
		object_moved(objp);
	}

	// Restore the wing's persistent formation/scale; overrides applied only to this call.
	wingp.formation = old_formation;
	wingp.formation_scale = old_formation_scale;

	mark_modified("MCP: arrange wing %s", wingp.name);

	req->result_json = make_json_tool_result(build_wing_json(wing_idx, true));
	req->success = true;
}

// Shared pre-check for delete_wing / disband_wing.  Returns true on success
// (caller may proceed); false if blocked or if the caller-supplied error path
// was already populated.
static bool preflight_wing_removal(const char *name, std::optional<bool> force, McpErrorSink &sink)
{
	if (!check_and_report_sexp_refs(sexp_ref_type::WING, "Wing", name, force, sink))
		return false;

	bool forced = force.has_value() && *force;
	if (!forced) {
		auto ai_ref = query_referenced_in_ai_goals(sexp_ref_type::WING, name);
		if (ai_ref.second != sexp_src::NONE) {
			sink.set_error("Wing '%s' is referenced in an AI goal. "
				"Use force=true to remove anyway (references will be invalidated).", name);
			return false;
		}
	} else {
		invalidate_references(name, sexp_ref_type::WING);
		mcp_sexp_forest_mark_dirty();
	}
	return true;
}

static void handle_delete_wing(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_wings, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int wing_idx = lookup_wing(name, sink);
	if (wing_idx < 0) return;

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	if (!preflight_wing_removal(name, force, sink)) return;

	if (delete_wing(wing_idx, 0) != 0) {
		sink.set_error("delete_wing for '%s' failed (engine returned non-zero).", name);
		return;
	}

	mark_modified("MCP: delete wing %s", name);

	sprintf(req->result_message, "Deleted wing: %s", name);
	req->success = true;
}

static void handle_disband_wing(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_wings, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int wing_idx = lookup_wing(name, sink);
	if (wing_idx < 0) return;

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	if (!preflight_wing_removal(name, force, sink)) return;

	disband_wing(wing_idx);

	mark_modified("MCP: disband wing %s", name);

	sprintf(req->result_message, "Disbanded wing: %s (member ships remain standalone)", name);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Move / swap
// ---------------------------------------------------------------------------

// Sentinel matching the engine convention in code/scripting/api/libs/mission.cpp.
static constexpr int COUNT_WINGS = -1000;

// Same pattern as ship_slot_at_public_index (mcp_ships.cpp), adapted for wings.
// Wings aren't Objects, so the occupancy sentinel is wave_count > 0.
static int wing_slot_at_public_index(int index)
{
	int count = 0;
	for (int i = 0; i < MAX_WINGS; ++i) {
		if (Wings[i].wave_count == 0)
			continue;
		++count;
		if (count == index)
			return i;
	}
	return (index == COUNT_WINGS) ? count : -1;
}

static int wing_slot_count()
{
	return wing_slot_at_public_index(COUNT_WINGS);
}

static FredWingSlotConfig make_wing_slot_config()
{
	FredWingSlotConfig wcfg;
	wcfg.wing_objects = wing_objects;
	wcfg.cur_wing = &cur_wing;
	return wcfg;
}

static MoveSwapConfig make_wing_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "wing";
	cfg.count = wing_slot_count();
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_wings;
	cfg.get_name = [](int i) {
		return SCP_string(Wings[wing_slot_at_public_index(i)].name);
	};
	cfg.do_move = [](int from, int to) {
		// Walk the moving element via adjacent swaps.  After each swap the
		// sparse positions of *other* occupied slots are unchanged, so
		// wing_slot_at_public_index remains correct for the next step.
		auto wcfg = make_wing_slot_config();
		int step = (from < to) ? 1 : -1;
		for (int pos = from; pos != to; pos += step) {
			int a = wing_slot_at_public_index(pos);
			int b = wing_slot_at_public_index(pos + step);
			swap_wing_slots(a, b, wcfg);
		}
	};
	cfg.do_swap = [](int a, int b) {
		int sa = wing_slot_at_public_index(a);
		int sb = wing_slot_at_public_index(b);
		swap_wing_slots(sa, sb, make_wing_slot_config());
	};
	return cfg;
}

static void handle_move_wing(json_t *input, McpToolRequest *req)
{
	auto cfg = make_wing_move_swap_config();
	handle_generic_move(input, req, cfg);
}

static void handle_swap_wings(json_t *input, McpToolRequest *req)
{
	auto cfg = make_wing_move_swap_config();
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_wing_tools(json_t *tools)
{
	// list_wings
	register_tool(tools, "list_wings",
		"List all wings in the mission, in the same order that move_wing/swap_wings "
		"indices refer to: a wing's 1-based position in this list is its index. "
		"Each entry has the wing's name, "
		"wave_count (current ship count), num_waves, "
		"and the names of its member ships in slot order.",
		json_object());

	// get_wing
	register_tool_with_required_string(tools, "get_wing",
		"Get full details of a wing by name.  Returns identity, waves, "
		"arrival/departure settings (including SEXP cue node IDs you can edit "
		"via the SEXP tools), formation, and the member ship list.",
		"name", "Name of the wing to retrieve");

	// form_wing
	{
		json_t *props = json_object();
		add_string_prop(props, "name",
			"Unique name for the wing (must not collide with any existing ship, wing, waypoint, or jump node).");
		add_string_array_prop(props, "members",
			"List of existing ship names to form the wing from (at least 1, at most MAX_SHIPS_PER_WING). "
			"All members must share the same team and must not already belong to a wing. "
			"Member ships are renamed to \"<wing_name> 1\", \"<wing_name> 2\", etc.",
			SCP_vector<const char *>{});
		add_string_prop(props, "display_name",
			"Optional display name. Pass \"<none>\" or a string matching the wing's `name` to clear; "
			"a blank string is a valid display name and will be stored as-is.");
		add_integer_prop(props, "hotkey",
			"Hotkey assignment 0-9, or omit for no hotkey.");
		add_string_prop(props, "wing_squad_logo_filename",
			"Squadron logo filename, or omit.");
		add_integer_prop(props, "num_waves", "Number of waves for the wing.");
		add_integer_prop(props, "new_wave_threshold", "Wave-respawn threshold.");
		add_integer_prop(props, "wave_delay_min", "Minimum delay between waves (seconds).");
		add_integer_prop(props, "wave_delay_max", "Maximum delay between waves (seconds).");
		add_string_enum_prop(props, "arrival_location",
			"How the wing arrives. Default \"Hyperspace\".", arrival_location_enum_values);
		add_string_prop(props, "arrival_target",
			"Ship name or special anchor (e.g. \"<any friendly>\") referenced by arrival_location.");
		add_integer_prop(props, "arrival_distance", "Distance offset (meters) from the arrival target.");
		add_integer_prop(props, "arrival_delay", "Seconds to wait after the arrival cue is true.");
		add_integer_prop(props, "arrival_cue",
			"SEXP node ID of the boolean formula that gates arrival. Defaults to a locked \"true\" cue.");
		add_string_enum_prop(props, "departure_location",
			"How the wing departs. Default \"Hyperspace\".", departure_location_enum_values);
		add_string_prop(props, "departure_target",
			"Ship name (typically one with a docking bay when departure_location is \"Docking Bay\").");
		add_integer_prop(props, "departure_delay", "Seconds to wait after the departure cue is true.");
		add_integer_prop(props, "departure_cue",
			"SEXP node ID of the boolean formula that gates departure. Defaults to a locked \"false\" cue.");
		add_string_prop(props, "formation",
			"Formation name. See list_wing_formations for valid values.");
		add_number_prop(props, "formation_scale", "Formation spacing scale (default 1.0).");
		add_bool_map_prop(props, "wing_flags",
			"Partial map of {flag_name: bool} for this wing's flags. Names come from list_wing_flags. "
			"Only listed keys are touched.");
		add_bool_prop(props, "reform",
			"If true and a wing with this name already exists, re-form it in place: release the "
			"current members and attach the supplied members instead.  The wing's other settings "
			"(cues, formation, hotkey, wing_flags, num_waves, threshold, wave delays, "
			"arrival/departure location/target/distance/delay, etc.) are preserved.  Members that "
			"are currently in the wing being re-formed are accepted (they are released first).  "
			"Released members go back to standalone ships with engine-default names.  Members not "
			"in the original roster are bashed to \"<wing_name> N\" as usual.  If the wing does not "
			"already exist, this flag is ignored.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("members"));
		register_tool(tools, "form_wing",
			"Form a new wing from existing ships.  Member ships must share a team, must not "
			"already be in a wing, and must have ship classes that allow wing membership. "
			"On success the members are renamed to \"<wing_name> 1\", etc., and their own "
			"arrival/departure cues are disabled (the wing's cues become authoritative). "
			"Pass reform=true to re-form an existing wing of the same name in place.",
			props, req);
	}

	// update_wing
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the wing to update.");
		add_string_prop(props, "new_name",
			"New name for the wing.  SEXP references, AI goals, texture replacements, and "
			"member ship names are updated automatically (each member is re-bashed to \"<new_name> N\").");
		add_string_prop(props, "display_name",
			"Display name. Pass \"<none>\" or a string matching the wing's `name` to clear; "
			"a blank string is a valid display name and will be stored as-is.");
		add_integer_prop(props, "hotkey", "Hotkey 0-9.");
		add_string_prop(props, "wing_squad_logo_filename", "Squadron logo filename.");
		add_integer_prop(props, "num_waves", "Number of waves.");
		add_integer_prop(props, "new_wave_threshold", "Wave-respawn threshold.");
		add_integer_prop(props, "wave_delay_min", "Minimum delay between waves.");
		add_integer_prop(props, "wave_delay_max", "Maximum delay between waves.");
		add_string_enum_prop(props, "arrival_location", "Arrival mode.", arrival_location_enum_values);
		add_string_prop(props, "arrival_target", "Arrival anchor (ship name or special token).");
		add_integer_prop(props, "arrival_distance", "Arrival distance offset.");
		add_integer_prop(props, "arrival_delay", "Arrival delay.");
		add_integer_prop(props, "arrival_cue", "SEXP node ID for the arrival cue. Previous user-allocated cue is freed.");
		add_string_enum_prop(props, "departure_location", "Departure mode.", departure_location_enum_values);
		add_string_prop(props, "departure_target", "Departure anchor.");
		add_integer_prop(props, "departure_delay", "Departure delay.");
		add_integer_prop(props, "departure_cue", "SEXP node ID for the departure cue.");
		add_string_prop(props, "formation", "Formation name. See list_wing_formations for valid values.");
		add_number_prop(props, "formation_scale", "Formation spacing scale.");
		add_bool_map_prop(props, "wing_flags",
			"Partial map of {flag_name: bool} for this wing's flags. Names come from list_wing_flags. "
			"Only listed keys are touched.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_wing",
			"Update properties of an existing wing.  Only specified fields are changed. "
			"Member list is read-only here; use other tools to change membership.",
			props, req);
	}

	// arrange_in_formation
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Wing name.");
		add_string_prop(props, "formation",
			"Optional formation name to override the wing's current formation for this arrangement only. "
			"The wing's persistent formation is not changed. "
			"See list_wing_formations for valid values.");
		add_number_prop(props, "formation_scale",
			"Optional formation scale to override the wing's current scale for this arrangement only. "
			"The wing's persistent scale is not changed.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "arrange_in_formation",
			"Arrange the members of a wing into their formation positions, relative to the wing leader "
			"(the first ship in the member list). Each non-leader member is moved to its formation position "
			"and reoriented to match the leader. The 'formation' and 'formation_scale' parameters are optional "
			"temporary overrides applied only to this arrangement; the wing's persistent formation settings "
			"are not modified (use update_wing for that). Mirrors the \"Align\" button on FRED's wing editor "
			"dialog. Wings with only a leader (wave_count == 1) are a no-op.",
			props, req);
	}

	// delete_wing
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the wing to delete.");
		add_bool_prop(props, "force",
			"If true, delete even when the wing is referenced in SEXPs or AI goals "
			"(references are invalidated). Default false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "delete_wing",
			"Delete a wing and ALL its member ships.  Mirrors the editor's \"Delete Wing\" command. "
			"Refuses if the wing is referenced in SEXPs or AI goals unless force=true. "
			"To dissolve the wing while keeping ships standalone, use disband_wing.",
			props, req);
	}

	// disband_wing
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the wing to disband.");
		add_bool_prop(props, "force",
			"If true, disband even when the wing is referenced in SEXPs or AI goals "
			"(references are invalidated). Default false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "disband_wing",
			"Dissolve a wing slot but leave its member ships intact.  The ships have their "
			"wingnum reset and are renamed back to default names by the engine. "
			"Refuses if the wing is referenced in SEXPs or AI goals unless force=true.",
			props, req);
	}

	// move_wing
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"1-based index of the wing to move");
		add_integer_prop(props, "to_index",
			"1-based destination index");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_wing",
			"Move a wing from one list position to another.  Indices are 1-based.",
			props, req);
	}

	// swap_wings
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first wing");
		add_integer_prop(props, "index_b",
			"1-based index of the second wing");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_wings",
			"Swap two wings at the given list positions.  Indices are 1-based.",
			props, req);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_wing_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_wings") == 0) {
		handle_list_wings(input_json, req);
	} else if (strcmp(tool_name, "get_wing") == 0) {
		handle_get_wing(input_json, req);
	} else if (strcmp(tool_name, "form_wing") == 0) {
		handle_form_wing(input_json, req);
	} else if (strcmp(tool_name, "update_wing") == 0) {
		handle_update_wing(input_json, req);
	} else if (strcmp(tool_name, "arrange_in_formation") == 0) {
		handle_arrange_in_formation(input_json, req);
	} else if (strcmp(tool_name, "delete_wing") == 0) {
		handle_delete_wing(input_json, req);
	} else if (strcmp(tool_name, "disband_wing") == 0) {
		handle_disband_wing(input_json, req);
	} else if (strcmp(tool_name, "move_wing") == 0) {
		handle_move_wing(input_json, req);
	} else if (strcmp(tool_name, "swap_wings") == 0) {
		handle_swap_wings(input_json, req);
	} else {
		return false;
	}
	return true;
}
