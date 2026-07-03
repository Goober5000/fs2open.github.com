#include "stdafx.h"
#include "mcp_mission_info.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "mcp_app.h"
#include "mcp_mission_tools.h"
#include "freddoc.h"
#include "mainfrm.h"
#include "Management.h"

#include "mission/missionparse.h"
#include "mission/missionmessage.h"
#include "missioneditor/common.h"     // update_custom_wing_indexes
#include "ship/ship.h"                 // Starting_wing_names, Squadron_wing_names, TVT_wing_names
#include "ai/ai.h"
#include "ai/ai_profiles.h"
#include "sound/ds.h"
#include "sound/sound.h"
#include "graphics/2d.h"

#include <cstdarg>
#include <cstring>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int game_type_from_string(const char *str, McpErrorSink &sink)
{
	if (!stricmp(str, "single-player"))
		return MISSION_TYPE_SINGLE;
	if (!stricmp(str, "single-player training"))
		return MISSION_TYPE_TRAINING;
	if (!stricmp(str, "multiplayer co-op"))
		return MISSION_TYPE_MULTI | MISSION_TYPE_MULTI_COOP;
	if (!stricmp(str, "multiplayer team-versus-team"))
		return MISSION_TYPE_MULTI | MISSION_TYPE_MULTI_TEAMS;
	if (!stricmp(str, "multiplayer dogfight"))
		return MISSION_TYPE_MULTI | MISSION_TYPE_MULTI_DOGFIGHT;

	sink.set_error("Parameter 'game_type' has unknown value '%s'", str);
	return 0;
}

static bool find_mission_flag_by_name(const char *name, Mission::Mission_Flags &out)
{
	for (size_t i = 0; i < Num_parse_mission_flags; i++) {
		if (!Parse_mission_flags[i].in_use)
			continue;
		if (!stricmp(name, Parse_mission_flags[i].name)) {
			out = Parse_mission_flags[i].def;
			return true;
		}
	}
	return false;
}

static bool validate_dialog_for_mission_info(SCP_string &error_msg)
{
	return validate_single_dialog("mission information", "mission notes", error_msg);
}

// ---------------------------------------------------------------------------
// build_mission_info_json — shared JSON builder for get/update
// ---------------------------------------------------------------------------

static json_t *build_mission_info_json()
{
	json_t *info = json_object();

	// Full filename with extension
	if (Mission_filename[0] != '\0') {
		SCP_string full_name;
		sprintf(full_name, "%s%s", Mission_filename, FS_MISSION_FILE_EXT);
		json_object_set_new(info, "filename", json_safe_string(full_name.c_str()));
	} else {
		json_object_set_new(info, "filename", json_string(""));
	}

	json_object_set_new(info, "title", json_safe_string(The_mission.name.c_str()));
	json_object_set_new(info, "author", json_safe_string(The_mission.author.c_str()));
	json_object_set_new(info, "created", json_safe_string(The_mission.created));
	json_object_set_new(info, "modified", json_safe_string(The_mission.modified));
	json_object_set_new(info, "notes", json_safe_string(The_mission.notes));
	json_object_set_new(info, "mission_desc", json_safe_string(The_mission.mission_desc));

	// Game type as human-readable string
	{
		const char *type_str = "unknown";
		int gt = The_mission.game_type;
		if (gt & MISSION_TYPE_MULTI_DOGFIGHT)
			type_str = "multiplayer dogfight";
		else if (gt & MISSION_TYPE_MULTI_TEAMS)
			type_str = "multiplayer team-versus-team";
		else if (gt & MISSION_TYPE_MULTI_COOP)
			type_str = "multiplayer co-op";
		else if (gt & MISSION_TYPE_TRAINING)
			type_str = "single-player training";
		else if (gt & MISSION_TYPE_SINGLE)
			type_str = "single-player";
		json_object_set_new(info, "game_type", json_string(type_str));
	}

	// Respawn settings
	json_object_set_new(info, "respawns", json_integer((int)The_mission.num_respawns));
	json_object_set_new(info, "max_respawn_delay", json_integer(The_mission.max_respawn_delay));

	// Player entry delay (edited in FRED's team loadout editor, but mission-level data)
	json_object_set_new(info, "player_entry_delay", json_integer(f2i(Entry_delay_time)));

	// Mission flags — list only the names of flags that are set
	{
		json_t *flags_arr = json_array();
		for (size_t i = 0; i < Num_parse_mission_flags; i++) {
			if (!Parse_mission_flags[i].in_use)
				continue;
			if (The_mission.flags[Parse_mission_flags[i].def])
				json_array_append_new(flags_arr, json_string(Parse_mission_flags[i].name));
		}
		json_object_set_new(info, "mission_flags", flags_arr);
	}
	json_object_set_new(info, "all_teams_at_war", json_boolean(Mission_all_attack != 0));

	// Support ships
	{
		json_t *support = json_object();
		json_object_set_new(support, "disallowed",
			json_boolean(The_mission.support_ships.max_support_ships == 0));
		json_object_set_new(support, "max_hull_repair",
			json_real(The_mission.support_ships.max_hull_repair_val));
		json_object_set_new(support, "max_subsys_repair",
			json_real(The_mission.support_ships.max_subsys_repair_val));
		json_object_set_new(info, "support_ships", support);
	}

	// Contrail threshold
	json_object_set_new(info, "contrail_threshold", json_integer(The_mission.contrail_threshold));

	// Command messages
	json_object_set_new(info, "command_sender", json_safe_string(The_mission.command_sender));
	if (The_mission.command_persona >= 0 && The_mission.command_persona < (int)Personas.size())
		json_object_set_new(info, "command_persona", json_safe_string(Personas[The_mission.command_persona].name));
	else
		json_object_set_new(info, "command_persona", json_null());

	// Loading screens
	set_optional_filename(info, "loading_screen_640", The_mission.loading_screen[GR_640]);
	set_optional_filename(info, "loading_screen_1024", The_mission.loading_screen[GR_1024]);

	// Squadron
	set_optional_string(info, "squadron_name", The_mission.squad_name, true);
	set_optional_filename(info, "squadron_logo_filename", The_mission.squad_filename);

	// AI profile
	if (The_mission.ai_profile != nullptr)
		json_object_set_new(info, "ai_profile", json_safe_string(The_mission.ai_profile->profile_name));
	else
		json_object_set_new(info, "ai_profile", json_null());

	// Sound environment
	{
		const sound_env &env = The_mission.sound_environment;
		if (env.id >= 0 && env.id < (int)EFX_presets.size()) {
			json_t *env_obj = json_object();
			json_object_set_new(env_obj, "preset", json_safe_string(EFX_presets[env.id].name.c_str()));
			json_object_set_new(env_obj, "volume", json_real(env.volume));
			json_object_set_new(env_obj, "damping", json_real(env.damping));
			json_object_set_new(env_obj, "decay", json_real(env.decay));
			json_object_set_new(info, "sound_environment", env_obj);
		} else {
			json_object_set_new(info, "sound_environment", json_null());
		}
	}

	// Custom data
	if (!The_mission.custom_data.empty()) {
		json_t *data_obj = json_object();
		for (const auto &kv : The_mission.custom_data) {
			json_object_set_new(data_obj, kv.first.c_str(), json_safe_string(kv.second.c_str()));
		}
		json_object_set_new(info, "custom_data", data_obj);
	}

	// Custom strings
	if (!The_mission.custom_strings.empty()) {
		json_t *strings_arr = json_array();
		for (const auto &cs : The_mission.custom_strings) {
			json_t *cs_obj = json_object();
			json_object_set_new(cs_obj, "name", json_safe_string(cs.name.c_str()));
			json_object_set_new(cs_obj, "value", json_safe_string(cs.value.c_str()));
			json_object_set_new(cs_obj, "text", json_safe_string(cs.text.c_str()));
			json_array_append_new(strings_arr, cs_obj);
		}
		json_object_set_new(info, "custom_strings", strings_arr);
	}

	return info;
}

// ---------------------------------------------------------------------------
// handle_get_mission_info
// ---------------------------------------------------------------------------

static void handle_get_mission_info(json_t * /*input*/, McpToolRequest *req)
{
	req->result_json = make_json_tool_result(build_mission_info_json());
	req->success = true;
}

// ---------------------------------------------------------------------------
// handle_update_mission_info
// ---------------------------------------------------------------------------

static void handle_update_mission_info(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);

	if (!validate(validate_dialog_for_mission_info, sink))
		return;

	// --- Phase 1: extract and validate all fields (no mutations yet) ---
	auto title             = get_optional_string(input, "title", sink);
	auto author            = get_optional_string(input, "author", sink);
	auto notes             = get_optional_string(input, "notes", sink, NOTES_LENGTH - 2);	// -2 to leave room for pad_with_newline
	auto mission_desc      = get_optional_string(input, "mission_desc", sink, MISSION_DESC_LENGTH - 1);
	auto game_type_str     = get_optional_string(input, "game_type", sink);
	auto respawns_opt      = get_optional_integer(input, "respawns", sink);
	auto max_delay_opt     = get_optional_integer(input, "max_respawn_delay", sink);
	auto entry_delay_opt   = get_optional_integer(input, "player_entry_delay", sink);
	auto contrail_opt      = get_optional_integer(input, "contrail_threshold", sink);
	auto all_war_opt       = get_optional_bool(input, "all_teams_at_war", sink);
	auto command_sender    = get_optional_string(input, "command_sender", sink, NAME_LENGTH - 1);
	auto command_persona   = get_optional_string(input, "command_persona", sink);
	auto squadron_name     = get_optional_string(input, "squadron_name", sink, NAME_LENGTH - 1);
	auto squadron_logo     = get_optional_filename(input, "squadron_logo_filename", sink, false);
	auto loading_640       = get_optional_filename(input, "loading_screen_640", sink, false);
	auto loading_1024      = get_optional_filename(input, "loading_screen_1024", sink, false);
	auto ai_profile_str    = get_optional_string(input, "ai_profile", sink);

	if (sink.has_error()) return;

	// Range checks (dialog DDV_MinMax bounds)
	if (respawns_opt.has_value() && !check_int_range(*respawns_opt, 0, 99, "respawns", sink)) return;
	if (max_delay_opt.has_value() && !check_int_range(*max_delay_opt, -1, 999, "max_respawn_delay", sink)) return;
	if (contrail_opt.has_value() && !check_int_range(*contrail_opt, -1, 1000, "contrail_threshold", sink)) return;

	// player_entry_delay lives in the team loadout editor, so it gets that
	// dialog's guard (only when actually being changed)
	if (entry_delay_opt.has_value()) {
		if (!check_int_range(*entry_delay_opt, 0, 30, "player_entry_delay", sink)) return;
		SCP_string guard_msg;
		if (!validate_single_dialog("the player entry delay", "player start", guard_msg)) {
			sink.set_error("%s", guard_msg.c_str());
			return;
		}
	}

	// Enum / lookup validation
	int new_game_type = 0;
	bool game_type_touched = false;
	if (game_type_str) {
		new_game_type = game_type_from_string(game_type_str, sink);
		if (sink.has_error()) return;
		game_type_touched = true;
	}

	int new_ai_profile = -1;
	if (ai_profile_str) {
		new_ai_profile = check_lookup(ai_profile_str, ai_profile_lookup, "ai_profile", sink);
		if (new_ai_profile < 0) return;
	}

	// command_persona: empty string → -1 (clear), else lookup
	std::optional<int> new_cmd_persona;
	if (command_persona) {
		if (!command_persona[0]) {
			new_cmd_persona = -1;
		} else {
			int idx = check_lookup(command_persona, message_persona_name_lookup, "command_persona", sink);
			if (idx < 0) return;
			new_cmd_persona = idx;
		}
	}

	// mission_flags object: validate all flag names up front, stash (def, bool) pairs
	SCP_vector<std::pair<Mission::Mission_Flags, bool>> flag_updates;
	json_t *flags_in = input ? json_object_get(input, "mission_flags") : nullptr;
	if (flags_in) {
		if (!json_is_object(flags_in)) {
			sink.set_error("Parameter 'mission_flags' must be an object");
			return;
		}
		const char *key;
		json_t *val;
		json_object_foreach(flags_in, key, val) {
			if (!json_is_boolean(val)) {
				sink.set_error("mission_flags.%s must be a boolean", key);
				return;
			}
			Mission::Mission_Flags def;
			if (!find_mission_flag_by_name(key, def)) {
				sink.set_error("Unknown mission flag '%s' (use list_mission_flags to see valid names)", key);
				return;
			}
			flag_updates.emplace_back(def, json_boolean_value(val));
		}
	}

	// support_ships nested object
	bool has_support_disallowed = false;
	bool support_disallowed = false;
	std::optional<float> new_hull_repair, new_subsys_repair;
	json_t *support_in = input ? json_object_get(input, "support_ships") : nullptr;
	if (support_in) {
		if (!json_is_object(support_in)) {
			sink.set_error("Parameter 'support_ships' must be an object");
			return;
		}
		auto d = get_optional_bool(support_in, "disallowed", sink);
		if (d.has_value()) { has_support_disallowed = true; support_disallowed = *d; }
		new_hull_repair = get_optional_float(support_in, "max_hull_repair", sink);
		new_subsys_repair = get_optional_float(support_in, "max_subsys_repair", sink);
		if (sink.has_error()) return;
		if (new_hull_repair.has_value() && (*new_hull_repair < 0.0f || *new_hull_repair > 100.0f)) {
			sink.set_error("support_ships.max_hull_repair must be 0-100");
			return;
		}
		if (new_subsys_repair.has_value() && (*new_subsys_repair < 0.0f || *new_subsys_repair > 100.0f)) {
			sink.set_error("support_ships.max_subsys_repair must be 0-100");
			return;
		}
	}

	// sound_environment: null disables, object is partial update
	bool sound_env_touched = false;
	sound_env pending_env = The_mission.sound_environment;
	json_t *env_in = input ? json_object_get(input, "sound_environment") : nullptr;
	if (env_in) {
		sound_env_touched = true;
		if (json_is_null(env_in)) {
			pending_env.id = -1;
		} else if (json_is_object(env_in)) {
			auto preset = get_optional_string(env_in, "preset", sink);
			auto volume = get_optional_float(env_in, "volume", sink);
			auto damping = get_optional_float(env_in, "damping", sink);
			auto decay = get_optional_float(env_in, "decay", sink);
			if (sink.has_error()) return;
			if (preset) {
				if (!preset[0]) {
					pending_env.id = -1;
				} else {
					int id = ds_eax_get_preset_id(preset);
					if (id < 0) {
						sink.set_error("Unknown sound environment preset '%s' "
							"(use list_sound_environment_presets to see valid names)", preset);
						return;
					}
					pending_env.id = id;
				}
			}
			if (pending_env.id < 0 && (volume.has_value() || damping.has_value() || decay.has_value())) {
				sink.set_error("sound_environment is disabled; provide 'preset' to enable it before setting volume/damping/decay");
				return;
			}
			if (volume.has_value())  pending_env.volume  = *volume;
			if (damping.has_value()) pending_env.damping = *damping;
			if (decay.has_value())   pending_env.decay   = *decay;
		} else {
			sink.set_error("Parameter 'sound_environment' must be an object or null");
			return;
		}
	}

	// custom_data: full replacement
	bool custom_data_touched = false;
	SCP_map<SCP_string, SCP_string> pending_custom_data;
	json_t *cd_in = input ? json_object_get(input, "custom_data") : nullptr;
	if (cd_in) {
		custom_data_touched = true;
		if (!json_is_object(cd_in)) {
			sink.set_error("Parameter 'custom_data' must be an object");
			return;
		}
		const char *k;
		json_t *v;
		json_object_foreach(cd_in, k, v) {
			if (!json_is_string(v)) {
				sink.set_error("custom_data.%s must be a string", k);
				return;
			}
			pending_custom_data[k] = json_string_value(v);
		}
	}

	// custom_strings: full replacement
	bool custom_strings_touched = false;
	SCP_vector<custom_string> pending_custom_strings;
	json_t *cs_in = input ? json_object_get(input, "custom_strings") : nullptr;
	if (cs_in) {
		custom_strings_touched = true;
		if (!json_is_array(cs_in)) {
			sink.set_error("Parameter 'custom_strings' must be an array");
			return;
		}
		size_t idx;
		json_t *elem;
		json_array_foreach(cs_in, idx, elem) {
			if (!json_is_object(elem)) {
				sink.set_error("custom_strings[%zu] must be an object", idx);
				return;
			}
			auto n = get_required_string(elem, "name", sink, true);
			if (!n) return;
			auto v = get_required_string(elem, "value", sink, false);
			if (!v) return;
			auto t = get_required_string(elem, "text", sink, false);
			if (!t) return;
			custom_string cs;
			cs.name = n;
			cs.value = v;
			cs.text = t;
			pending_custom_strings.push_back(std::move(cs));
		}
	}

	// --- Phase 2: apply all changes ---
	bool changed = false;

	if (title && The_mission.name != title) {
		The_mission.name = title;
		changed = true;
	}
	if (author && The_mission.author != author) {
		The_mission.author = author;
		changed = true;
	}
	if (notes) {
		// Ensure trailing newline to prevent "$End Notes:" from being interpreted as part of a comment
		SCP_string padded = notes;
		if (padded.empty() || padded.back() != '\n')
			padded += '\n';
		if (strcmp(The_mission.notes, padded.c_str()) != 0) {
			strcpy_s(The_mission.notes, padded.c_str());
			changed = true;
		}
	}
	if (mission_desc && strcmp(The_mission.mission_desc, mission_desc) != 0) {
		strcpy_s(The_mission.mission_desc, mission_desc);
		changed = true;
	}
	if (command_sender && strcmp(The_mission.command_sender, command_sender) != 0) {
		strcpy_s(The_mission.command_sender, command_sender);
		changed = true;
	}
	if (squadron_name && strcmp(The_mission.squad_name, squadron_name) != 0) {
		strcpy_s(The_mission.squad_name, squadron_name);
		changed = true;
	}
	if (squadron_logo && strcmp(The_mission.squad_filename, squadron_logo) != 0) {
		strcpy_s(The_mission.squad_filename, squadron_logo);
		changed = true;
	}
	if (loading_640 && strcmp(The_mission.loading_screen[GR_640], loading_640) != 0) {
		strcpy_s(The_mission.loading_screen[GR_640], loading_640);
		changed = true;
	}
	if (loading_1024 && strcmp(The_mission.loading_screen[GR_1024], loading_1024) != 0) {
		strcpy_s(The_mission.loading_screen[GR_1024], loading_1024);
		changed = true;
	}

	if (game_type_touched && The_mission.game_type != new_game_type) {
		The_mission.game_type = new_game_type;
		changed = true;
		// Update Num_teams to match (same as dialog OnOK)
		Num_teams = ((new_game_type & MISSION_TYPE_MULTI) && (new_game_type & MISSION_TYPE_MULTI_TEAMS)) ? 2 : 1;
	}

	if (respawns_opt.has_value() && (int)The_mission.num_respawns != *respawns_opt) {
		The_mission.num_respawns = (uint)*respawns_opt;
		changed = true;
	}
	if (max_delay_opt.has_value() && The_mission.max_respawn_delay != *max_delay_opt) {
		The_mission.max_respawn_delay = *max_delay_opt;
		changed = true;
	}
	if (entry_delay_opt.has_value() && f2i(Entry_delay_time) != *entry_delay_opt) {
		Entry_delay_time = i2f(*entry_delay_opt);
		changed = true;
	}
	if (contrail_opt.has_value()) {
		int new_val = (*contrail_opt == -1) ? CONTRAIL_THRESHOLD_DEFAULT : *contrail_opt;
		if (The_mission.contrail_threshold != new_val) {
			The_mission.contrail_threshold = new_val;
			changed = true;
		}
	}
	if (all_war_opt.has_value() && (Mission_all_attack != 0) != *all_war_opt) {
		Mission_all_attack = *all_war_opt ? 1 : 0;
		changed = true;
	}

	for (const auto &fu : flag_updates) {
		if (The_mission.flags[fu.first] != fu.second) {
			The_mission.flags.set(fu.first, fu.second);
			changed = true;
		}
	}

	if (has_support_disallowed) {
		int target = support_disallowed ? 0 : -1;
		if (The_mission.support_ships.max_support_ships != target) {
			The_mission.support_ships.max_support_ships = target;
			changed = true;
		}
	}
	if (new_hull_repair.has_value() && The_mission.support_ships.max_hull_repair_val != *new_hull_repair) {
		The_mission.support_ships.max_hull_repair_val = *new_hull_repair;
		changed = true;
	}
	if (new_subsys_repair.has_value() && The_mission.support_ships.max_subsys_repair_val != *new_subsys_repair) {
		The_mission.support_ships.max_subsys_repair_val = *new_subsys_repair;
		changed = true;
	}

	if (ai_profile_str && The_mission.ai_profile != &Ai_profiles[new_ai_profile]) {
		The_mission.ai_profile = &Ai_profiles[new_ai_profile];
		changed = true;
	}

	if (new_cmd_persona.has_value() && The_mission.command_persona != *new_cmd_persona) {
		The_mission.command_persona = *new_cmd_persona;
		changed = true;
	}

	if (sound_env_touched && !(The_mission.sound_environment == pending_env)) {
		The_mission.sound_environment = pending_env;
		changed = true;
	}

	if (custom_data_touched && The_mission.custom_data != pending_custom_data) {
		The_mission.custom_data = std::move(pending_custom_data);
		changed = true;
	}
	if (custom_strings_touched && The_mission.custom_strings != pending_custom_strings) {
		The_mission.custom_strings = std::move(pending_custom_strings);
		changed = true;
	}

	if (changed)
		mark_modified("MCP: update mission info");

	req->result_json = make_json_tool_result(build_mission_info_json());
	req->success = true;
}

// ---------------------------------------------------------------------------
// Custom wing names
//
// Three mission-global string tables that name special wing roles:
//   Starting_wing_names[3]  — wings the player starts a mission with
//   Squadron_wing_names[5]  — wings shown on the HUD gauge
//   TVT_wing_names[2]       — starting wings for team-versus-team play
// Edited together by fred2/customwingnames.cpp.  The dialog enforces three
// constraints we mirror here:
//   - Starting_wing_names[0] must equal TVT_wing_names[0]
//   - no case-insensitive duplicates within any one table
//   - each name <= NAME_LENGTH - 1
// ---------------------------------------------------------------------------

static bool validate_dialog_for_custom_wing_names(SCP_string &error_msg)
{
	// The custom-wing-names dialog is modal under the mission specs editor; if
	// that editor is open we refuse the edit (mirrors the pattern for sub-editors).
	return validate_single_dialog("custom wing names", "mission notes", error_msg);
}

static json_t *build_custom_wing_names_json()
{
	auto build_array = [](const char (*names)[NAME_LENGTH], int count) {
		json_t *arr = json_array();
		for (int i = 0; i < count; ++i)
			json_array_append_new(arr, json_safe_string(names[i]));
		return arr;
	};

	json_t *obj = json_object();
	json_object_set_new(obj, "starting_wings", build_array(Starting_wing_names, MAX_STARTING_WINGS));
	json_object_set_new(obj, "squadron_wings", build_array(Squadron_wing_names, MAX_SQUADRON_WINGS));
	json_object_set_new(obj, "tvt_wings",      build_array(TVT_wing_names,      MAX_TVT_WINGS));
	return obj;
}

static void handle_get_custom_wing_names(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_custom_wing_names, sink)) return;

	req->result_json = make_json_tool_result(build_custom_wing_names_json());
	req->success = true;
}

// Read an optional fixed-length string-array param into `out`.  Returns:
//   touched == false  -> param was omitted
//   touched == true   -> param was present and parsed into `out` of size `expected`
// Sets sink error and returns touched=false (with sink error) on malformed input.
static bool read_fixed_string_array(json_t *input, const char *param_name,
	int expected_len, SCP_vector<SCP_string> &out, bool &touched, McpErrorSink &sink)
{
	auto arr_opt = get_optional_string_array(input, param_name, sink);
	if (sink.has_error()) return false;
	if (!arr_opt.has_value()) {
		touched = false;
		return true;
	}
	if ((int)arr_opt->size() != expected_len) {
		sink.set_error("Parameter '%s' must be an array of exactly %d strings (got %d).",
			param_name, expected_len, (int)arr_opt->size());
		return false;
	}
	for (const auto &s : *arr_opt) {
		if ((int)s.size() > NAME_LENGTH - 1) {
			sink.set_error("Parameter '%s' entry '%s' exceeds max length (%d).",
				param_name, s.c_str(), NAME_LENGTH - 1);
			return false;
		}
	}
	out = std::move(*arr_opt);
	touched = true;
	return true;
}

static bool check_unique_names(const SCP_vector<SCP_string> &names,
	const char *list_label, McpErrorSink &sink)
{
	for (size_t i = 0; i < names.size(); ++i) {
		for (size_t j = i + 1; j < names.size(); ++j) {
			if (!stricmp(names[i].c_str(), names[j].c_str())) {
				sink.set_error("Duplicate wing name '%s' in %s.", names[i].c_str(), list_label);
				return false;
			}
		}
	}
	return true;
}

static void handle_update_custom_wing_names(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_custom_wing_names, sink)) return;

	// Stage incoming values; if a param is omitted, fall back to current.
	SCP_vector<SCP_string> starting, squadron, tvt;
	bool starting_touched = false, squadron_touched = false, tvt_touched = false;

	if (!read_fixed_string_array(input, "starting_wings", MAX_STARTING_WINGS,
			starting, starting_touched, sink))
		return;
	if (!read_fixed_string_array(input, "squadron_wings", MAX_SQUADRON_WINGS,
			squadron, squadron_touched, sink))
		return;
	if (!read_fixed_string_array(input, "tvt_wings", MAX_TVT_WINGS,
			tvt, tvt_touched, sink))
		return;

	if (!starting_touched) {
		starting.assign(MAX_STARTING_WINGS, {});
		for (int i = 0; i < MAX_STARTING_WINGS; ++i)
			starting[i] = Starting_wing_names[i];
	}
	if (!squadron_touched) {
		squadron.assign(MAX_SQUADRON_WINGS, {});
		for (int i = 0; i < MAX_SQUADRON_WINGS; ++i)
			squadron[i] = Squadron_wing_names[i];
	}
	if (!tvt_touched) {
		tvt.assign(MAX_TVT_WINGS, {});
		for (int i = 0; i < MAX_TVT_WINGS; ++i)
			tvt[i] = TVT_wing_names[i];
	}

	// Cross-array constraint: starting[0] == tvt[0] (case-sensitive, matching
	// CustomWingNames::OnOK).
	if (strcmp(starting[0].c_str(), tvt[0].c_str()) != 0) {
		sink.set_error("The first starting wing ('%s') and the first team-versus-team wing "
			"('%s') must have the same name.", starting[0].c_str(), tvt[0].c_str());
		return;
	}

	if (!check_unique_names(starting, "starting_wings", sink)) return;
	if (!check_unique_names(squadron, "squadron_wings", sink)) return;
	if (!check_unique_names(tvt, "tvt_wings", sink)) return;

	bool changed = false;
	auto apply_array = [&](const SCP_vector<SCP_string> &src, char (*dst)[NAME_LENGTH], int n) {
		for (int i = 0; i < n; ++i) {
			if (strcmp(dst[i], src[i].c_str()) != 0) {
				strcpy_s(dst[i], src[i].c_str());
				changed = true;
			}
		}
	};
	if (starting_touched) apply_array(starting, Starting_wing_names, MAX_STARTING_WINGS);
	if (squadron_touched) apply_array(squadron, Squadron_wing_names, MAX_SQUADRON_WINGS);
	if (tvt_touched)      apply_array(tvt,      TVT_wing_names,      MAX_TVT_WINGS);

	if (changed) {
		update_custom_wing_indexes();
		mark_modified("MCP: update custom wing names");
	}

	req->result_json = make_json_tool_result(build_custom_wing_names_json());
	req->success = true;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void mcp_register_mission_info_tools(json_t *tools)
{
	// get_mission_info
	register_tool(tools, "get_mission_info",
		"Returns metadata about the currently loaded mission (filename, title, author, notes, "
		"game_type, mission flags, support ship settings, command persona, sound environment, "
		"custom data/strings, AI profile, and other Mission Specs fields).",
		nullptr);

	// update_mission_info
	{
		json_t *props = json_object();
		add_string_prop(props, "title", "Mission title");
		add_string_prop(props, "author", "Mission author/designer");
		add_string_prop(props, "notes", "Mission design notes (internal; not shown to player)");
		add_string_prop(props, "mission_desc", "Mission description shown to the player");
		add_string_enum_prop(props, "game_type",
			"Mission game type",
			{ "single-player", "single-player training",
			  "multiplayer co-op", "multiplayer team-versus-team", "multiplayer dogfight" });
		add_integer_prop(props, "respawns", "Number of respawns allowed (multiplayer, 0-99)");
		add_integer_prop(props, "max_respawn_delay", "Max respawn delay in seconds (-1 to 999; -1 = no limit)");
		add_integer_prop(props, "player_entry_delay",
			"Seconds the mission runs before the player enters it (0-30). Shown in FRED's team loadout editor.");
		add_integer_prop(props, "contrail_threshold",
			"Speed threshold at which ships produce contrails (0-1000); -1 to restore the default (45)");
		add_bool_prop(props, "all_teams_at_war", "All teams target each other");
		add_string_prop(props, "command_sender", "Name shown as the sender of #Command messages");
		add_string_prop(props, "command_persona",
			"Persona name used for built-in command messages (empty string clears). Use list_personas to see available names.");
		add_string_prop(props, "squadron_name", "Reassigned squadron name (empty string clears)");
		add_string_prop(props, "squadron_logo_filename", "Reassigned squadron logo filename (empty string clears)");
		add_string_prop(props, "loading_screen_640", "Loading screen filename for 640x480");
		add_string_prop(props, "loading_screen_1024", "Loading screen filename for 1024x768 and higher");
		add_string_prop(props, "ai_profile",
			"AI profile name. Use list_ai_profiles to see available names.");

		add_bool_map_prop(props, "mission_flags",
			"Partial update of mission flags. Keys are flag names from list_mission_flags; "
			"values are booleans. Only provided flags are modified.");

		// support_ships: nested object with known shape
		{
			json_t *support_props = json_object();
			add_bool_prop(support_props, "disallowed", "If true, support ships are disallowed");
			add_number_prop(support_props, "max_hull_repair", "Maximum hull repair percentage (0-100)");
			add_number_prop(support_props, "max_subsys_repair", "Maximum subsystem repair percentage (0-100)");
			add_object_prop(props, "support_ships", "Support ship settings (partial update).", support_props);
		}

		// sound_environment: object or null
		{
			json_t *env_props = json_object();
			add_string_prop(env_props, "preset",
				"Preset name from list_sound_environment_presets (empty string disables)");
			add_number_prop(env_props, "volume", "Environment volume");
			add_number_prop(env_props, "damping", "Environment damping");
			add_number_prop(env_props, "decay", "Environment decay");
			json_t *env_schema = json_object();
			json_t *type_arr = json_array();
			json_array_append_new(type_arr, json_string("object"));
			json_array_append_new(type_arr, json_string("null"));
			json_object_set_new(env_schema, "type", type_arr);
			json_object_set_new(env_schema, "description",
				json_string("Sound environment (partial update). Pass null to disable."));
			json_object_set_new(env_schema, "properties", env_props);
			json_object_set_new(props, "sound_environment", env_schema);
		}

		// custom_data: free-form object of string → string (replacement)
		{
			json_t *cd_schema = json_object();
			json_object_set_new(cd_schema, "type", json_string("object"));
			json_object_set_new(cd_schema, "description",
				json_string("Mission custom_data as a key-value map. REPLACES the existing map; pass {} to clear."));
			json_t *ap = json_object();
			json_object_set_new(ap, "type", json_string("string"));
			json_object_set_new(cd_schema, "additionalProperties", ap);
			json_object_set_new(props, "custom_data", cd_schema);
		}

		// custom_strings: array of {name, value, text} (replacement)
		{
			json_t *elem_props = json_object();
			add_string_prop(elem_props, "name", "Custom string name");
			add_string_prop(elem_props, "value", "Custom string value");
			add_string_prop(elem_props, "text", "Custom string text");
			json_t *elem_req = json_array();
			json_array_append_new(elem_req, json_string("name"));
			json_array_append_new(elem_req, json_string("value"));
			json_array_append_new(elem_req, json_string("text"));
			add_object_array_prop(props, "custom_strings",
				"Mission custom_strings. REPLACES the existing vector; pass [] to clear.",
				elem_props, elem_req);
		}

		register_tool(tools, "update_mission_info",
			"Update fields on the currently loaded mission (Mission Specs). All parameters "
			"are optional; only provided fields are changed. Returns the full updated mission info. "
			"Music fields are NOT included here -- they will belong to a separate tool.",
			props);
	}

	// get_custom_wing_names
	register_tool(tools, "get_custom_wing_names",
		"Returns the mission's custom wing-name tables: starting_wings (" SCP_TOKEN_TO_STR(MAX_STARTING_WINGS) "), "
		"squadron_wings (" SCP_TOKEN_TO_STR(MAX_SQUADRON_WINGS) "), tvt_wings (" SCP_TOKEN_TO_STR(MAX_TVT_WINGS) "). "
		"These name the wings that play special engine roles (wings accessible in the starting loadout, wings shown on "
		"the squadron HUD gauge, and team-vs-team starting wings).",
		nullptr);

	// update_custom_wing_names
	{
		json_t *props = json_object();

		auto add_fixed_string_array = [&](const char *name, int len, const char *desc) {
			json_t *items = json_object();
			json_object_set_new(items, "type", json_string("string"));
			json_t *schema = json_object();
			json_object_set_new(schema, "type", json_string("array"));
			json_object_set_new(schema, "description", json_string(desc));
			json_object_set_new(schema, "items", items);
			json_object_set_new(schema, "minItems", json_integer(len));
			json_object_set_new(schema, "maxItems", json_integer(len));
			json_object_set(props, name, schema);
			json_decref(schema);
		};

		add_fixed_string_array("starting_wings", MAX_STARTING_WINGS,
			"Player-starting wing names (exactly " SCP_TOKEN_TO_STR(MAX_STARTING_WINGS) " strings). REPLACES the current array.");
		add_fixed_string_array("squadron_wings", MAX_SQUADRON_WINGS,
			"Squadron-message HUD wing names (exactly " SCP_TOKEN_TO_STR(MAX_SQUADRON_WINGS) " strings). REPLACES the current array.");
		add_fixed_string_array("tvt_wings", MAX_TVT_WINGS,
			"Team-versus-team starting wing names (exactly " SCP_TOKEN_TO_STR(MAX_TVT_WINGS) " strings). REPLACES the current array.");

		register_tool(tools, "update_custom_wing_names",
			"Replace one or more of the mission's custom wing-name tables. All parameters "
			"are optional; only provided tables are changed. Constraints: first starting wing "
			"must equal first team-versus-team wing; no case-insensitive duplicates within a table; each "
			"name is at most " SCP_TOKEN_TO_STR(NAME_LENGTH_1) " characters. Returns the full updated tables.",
			props);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_mission_info_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "get_mission_info") == 0) {
		handle_get_mission_info(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "update_mission_info") == 0) {
		handle_update_mission_info(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "get_custom_wing_names") == 0) {
		handle_get_custom_wing_names(input_json, req);
		return true;
	}
	if (strcmp(tool_name, "update_custom_wing_names") == 0) {
		handle_update_custom_wing_names(input_json, req);
		return true;
	}
	return false;
}
