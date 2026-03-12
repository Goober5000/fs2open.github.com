/*
 * JSON mission load implementation for FreeSpace Open.
 * Provides an alternate, structured mission format for use by AI agents.
 */

#include "mission/mission_json.h"

// Jansson's json_array_foreach / json_object_foreach macros use assignment
// inside conditionals, which is intentional.  Suppress MSVC C4706 for this file.
#ifdef _MSC_VER
#pragma warning(disable: 4706)
#endif

#include <cstring>

#include "ai/ai.h"
#include "ai/aigoals.h"
#include "ai/ai_profiles.h"
#include "asteroid/asteroid.h"
#include "cfile/cfile.h"
#include "gamesnd/eventmusic.h"
#include "globalincs/version.h"
#include "hud/hudsquadmsg.h"
#include "iff_defs/iff_defs.h"
#include "jumpnode/jumpnode.h"
#include "libs/jansson.h"
#include "lighting/lighting_profiles.h"
#include "mission/missionbriefcommon.h"
#include "mission/missiongoals.h"
#include "mission/missionmessage.h"
#include "mission/missionparse.h"
#include "mission/mission_flags.h"
#include "missionui/fictionviewer.h"
#include "missionui/missioncmdbrief.h"
#include "nebula/neb.h"
#include "object/waypoint.h"
#include "parse/parselo.h"
#include "parse/sexp.h"
#include "parse/sexp_container.h"
#include "prop/prop.h"
#include "ship/ship.h"
#include "sound/ds.h"
#include "starfield/starfield.h"
#include "weapon/weapon.h"

// Forward declarations for internal functions from missionparse.cpp
extern bool post_process_mission(mission *pm);
extern int allocate_subsys_status();
extern int get_anchor(const char *name);

// ============================================================
// Helper implementations
// ============================================================

vec3d mission_json::json_to_vec3d(const json_t* obj)
{
	vec3d v = vmd_zero_vector;
	if (!obj || !json_is_object(obj))
		return v;
	v.xyz.x = static_cast<float>(json_real_value(json_object_get(obj, "x")));
	v.xyz.y = static_cast<float>(json_real_value(json_object_get(obj, "y")));
	v.xyz.z = static_cast<float>(json_real_value(json_object_get(obj, "z")));
	return v;
}

matrix mission_json::json_to_matrix(const json_t* obj)
{
	matrix m = vmd_identity_matrix;
	if (!obj || !json_is_object(obj))
		return m;
	m.vec.fvec = json_to_vec3d(json_object_get(obj, "fvec"));
	m.vec.rvec = json_to_vec3d(json_object_get(obj, "rvec"));
	m.vec.uvec = json_to_vec3d(json_object_get(obj, "uvec"));
	return m;
}

angles mission_json::json_to_angles(const json_t* obj)
{
	angles a = {0.0f, 0.0f, 0.0f};
	if (!obj || !json_is_object(obj))
		return a;
	a.p = static_cast<float>(json_real_value(json_object_get(obj, "p")));
	a.b = static_cast<float>(json_real_value(json_object_get(obj, "b")));
	a.h = static_cast<float>(json_real_value(json_object_get(obj, "h")));
	return a;
}

int mission_json::json_to_sexp(const json_t* val)
{
	if (!val || json_is_null(val))
		return -1;

	const char* sexp_str = json_string_value(val);
	if (!sexp_str || strlen(sexp_str) == 0)
		return -1;

	// Temporarily redirect the parse pointer to our SEXP string
	char* saved_mp = Mp;
	SCP_string temp_buf(sexp_str);
	Mp = temp_buf.data();

	int node = get_sexp_main();

	Mp = saved_mp;
	return node;
}

int mission_json::json_get_int(const json_t* obj, const char* key, int default_val)
{
	const json_t* val = json_object_get(obj, key);
	if (!val || !json_is_integer(val))
		return default_val;
	return static_cast<int>(json_integer_value(val));
}

float mission_json::json_get_float(const json_t* obj, const char* key, float default_val)
{
	const json_t* val = json_object_get(obj, key);
	if (!val)
		return default_val;
	if (json_is_real(val))
		return static_cast<float>(json_real_value(val));
	if (json_is_integer(val))
		return static_cast<float>(json_integer_value(val));
	return default_val;
}

const char* mission_json::json_get_string(const json_t* obj, const char* key, const char* default_val)
{
	const json_t* val = json_object_get(obj, key);
	if (!val || !json_is_string(val))
		return default_val;
	return json_string_value(val);
}

bool mission_json::json_get_bool(const json_t* obj, const char* key, bool default_val)
{
	const json_t* val = json_object_get(obj, key);
	if (!val)
		return default_val;
	if (json_is_true(val))
		return true;
	if (json_is_false(val))
		return false;
	return default_val;
}

// Extern declarations needed for flag tables defined in missionparse.cpp
extern flag_def_list_new<Mission::Parse_Object_Flags> Parse_object_flags[];
extern const size_t Num_parse_object_flags;
extern flag_def_list_new<Mission::Mission_Flags> Parse_mission_flags[];
extern const size_t Num_parse_mission_flags;
extern SCP_vector<parsed_prop> Parse_props;

// ============================================================
// Internal load helpers (anonymous namespace)
// ============================================================
namespace {

using namespace mission_json;

ArrivalLocation parse_arrival_location(const char* str)
{
	if (!stricmp(str, "Near Ship"))          return ArrivalLocation::NEAR_SHIP;
	if (!stricmp(str, "In Front of Ship"))   return ArrivalLocation::IN_FRONT_OF_SHIP;
	if (!stricmp(str, "In Back of Ship"))    return ArrivalLocation::IN_BACK_OF_SHIP;
	if (!stricmp(str, "Above Ship"))         return ArrivalLocation::ABOVE_SHIP;
	if (!stricmp(str, "Below Ship"))         return ArrivalLocation::BELOW_SHIP;
	if (!stricmp(str, "To Left of Ship"))    return ArrivalLocation::TO_LEFT_OF_SHIP;
	if (!stricmp(str, "To Right of Ship"))   return ArrivalLocation::TO_RIGHT_OF_SHIP;
	if (!stricmp(str, "From Dock Bay"))      return ArrivalLocation::FROM_DOCK_BAY;
	return ArrivalLocation::AT_LOCATION;
}

DepartureLocation parse_departure_location(const char* str)
{
	if (!stricmp(str, "To Dock Bay")) return DepartureLocation::TO_DOCK_BAY;
	return DepartureLocation::AT_LOCATION;
}

int json_to_anchor(const json_t* obj, const char* key)
{
	const json_t* val = json_object_get(obj, key);
	if (!val || json_is_null(val))
		return -1;

	if (json_is_string(val))
		return get_anchor(json_string_value(val));

	if (json_is_integer(val))
		return static_cast<int>(json_integer_value(val));

	return -1;
}

void load_parse_object_flags_json(const json_t* arr, flagset<Mission::Parse_Object_Flags>& flags)
{
	if (!arr || !json_is_array(arr))
		return;

	size_t index;
	json_t* val;
	json_array_foreach(arr, index, val) {
		const char* name = json_string_value(val);
		if (!name) continue;
		for (size_t i = 0; i < Num_parse_object_flags; i++) {
			if (!stricmp(name, Parse_object_flags[i].name)) {
				flags.set(Parse_object_flags[i].def);
				break;
			}
		}
	}
}

void load_mission_flags_json(const json_t* arr, flagset<Mission::Mission_Flags>& flags)
{
	if (!arr || !json_is_array(arr))
		return;

	size_t index;
	json_t* val;
	json_array_foreach(arr, index, val) {
		const char* name = json_string_value(val);
		if (!name) continue;
		for (size_t i = 0; i < Num_parse_mission_flags; i++) {
			if (!stricmp(name, Parse_mission_flags[i].name)) {
				flags.set(Parse_mission_flags[i].def);
				break;
			}
		}
	}
}

void load_ai_goals_json(const json_t* arr, ai_goal* goalp)
{
	if (!arr || !json_is_array(arr))
		return;

	size_t count = json_array_size(arr);
	if (count > MAX_AI_GOALS)
		count = MAX_AI_GOALS;

	for (size_t i = 0; i < count; i++) {
		const json_t* g = json_array_get(arr, i);
		if (!g) continue;

		const char* mode_str = json_get_string(g, "mode", "");
		ai_goal_mode mode = AI_GOAL_NONE;

		if (!stricmp(mode_str, "ai-chase"))              mode = AI_GOAL_CHASE;
		else if (!stricmp(mode_str, "ai-chase-wing"))    mode = AI_GOAL_CHASE_WING;
		else if (!stricmp(mode_str, "ai-chase-ship-class")) mode = AI_GOAL_CHASE_SHIP_CLASS;
		else if (!stricmp(mode_str, "ai-chase-ship-type")) mode = AI_GOAL_CHASE_SHIP_TYPE;
		else if (!stricmp(mode_str, "ai-chase-any"))     mode = AI_GOAL_CHASE_ANY;
		else if (!stricmp(mode_str, "ai-guard"))         mode = AI_GOAL_GUARD;
		else if (!stricmp(mode_str, "ai-guard-wing"))    mode = AI_GOAL_GUARD_WING;
		else if (!stricmp(mode_str, "ai-waypoints"))     mode = AI_GOAL_WAYPOINTS;
		else if (!stricmp(mode_str, "ai-waypoints-once")) mode = AI_GOAL_WAYPOINTS_ONCE;
		else if (!stricmp(mode_str, "ai-dock"))          mode = AI_GOAL_DOCK;
		else if (!stricmp(mode_str, "ai-undock"))        mode = AI_GOAL_UNDOCK;
		else if (!stricmp(mode_str, "ai-destroy-subsystem")) mode = AI_GOAL_DESTROY_SUBSYSTEM;
		else if (!stricmp(mode_str, "ai-disable-ship"))  mode = AI_GOAL_DISABLE_SHIP;
		else if (!stricmp(mode_str, "ai-disable-ship-tactical")) mode = AI_GOAL_DISABLE_SHIP_TACTICAL;
		else if (!stricmp(mode_str, "ai-disarm-ship"))   mode = AI_GOAL_DISARM_SHIP;
		else if (!stricmp(mode_str, "ai-disarm-ship-tactical")) mode = AI_GOAL_DISARM_SHIP_TACTICAL;
		else if (!stricmp(mode_str, "ai-ignore"))        mode = AI_GOAL_IGNORE;
		else if (!stricmp(mode_str, "ai-ignore-new"))    mode = AI_GOAL_IGNORE_NEW;
		else if (!stricmp(mode_str, "ai-evade-ship"))    mode = AI_GOAL_EVADE_SHIP;
		else if (!stricmp(mode_str, "ai-stay-near-ship")) mode = AI_GOAL_STAY_NEAR_SHIP;
		else if (!stricmp(mode_str, "ai-keep-safe-distance")) mode = AI_GOAL_KEEP_SAFE_DISTANCE;
		else if (!stricmp(mode_str, "ai-form-on-wing"))  mode = AI_GOAL_FORM_ON_WING;
		else if (!stricmp(mode_str, "ai-stay-still"))    mode = AI_GOAL_STAY_STILL;
		else if (!stricmp(mode_str, "ai-play-dead"))     mode = AI_GOAL_PLAY_DEAD;
		else if (!stricmp(mode_str, "ai-play-dead-persistent")) mode = AI_GOAL_PLAY_DEAD_PERSISTENT;
		else if (!stricmp(mode_str, "ai-warp-out"))      mode = AI_GOAL_WARP;
		else if (!stricmp(mode_str, "ai-rearm-repair"))  mode = AI_GOAL_REARM_REPAIR;
		else if (!stricmp(mode_str, "ai-fly-to-ship"))   mode = AI_GOAL_FLY_TO_SHIP;
		else {
			mprintf(("mission_json: Unknown AI goal mode '%s'\n", mode_str));
			continue;
		}

		goalp[i].ai_mode = mode;
		goalp[i].priority = json_get_int(g, "priority", 50);

		const char* target = json_get_string(g, "target_name", nullptr);
		if (target) {
			goalp[i].target_name = ai_get_goal_target_name(target, &goalp[i].target_name_index);
		}

		if (mode == AI_GOAL_DOCK) {
			const char* docker_pt = json_get_string(g, "docker_point", nullptr);
			if (docker_pt)
				goalp[i].docker.name = ai_get_goal_target_name(docker_pt, &goalp[i].docker.index);
			const char* dockee_pt = json_get_string(g, "dockee_point", nullptr);
			if (dockee_pt)
				goalp[i].dockee.name = ai_get_goal_target_name(dockee_pt, &goalp[i].dockee.index);
		}

		if (mode == AI_GOAL_DESTROY_SUBSYSTEM) {
			const char* subsys = json_get_string(g, "subsystem", nullptr);
			if (subsys)
				goalp[i].docker.name = ai_get_goal_target_name(subsys, &goalp[i].docker.index);
		}
	}
}

int ai_goals_json_to_sexp(const json_t* arr)
{
	if (!arr || !json_is_array(arr) || json_array_size(arr) == 0)
		return -1;

	SCP_string sexp_str = "( goals ";

	size_t index;
	json_t* val;
	json_array_foreach(arr, index, val) {
		const char* mode_str = json_get_string(val, "mode", "");
		int priority = json_get_int(val, "priority", 50);
		const char* target = json_get_string(val, "target_name", nullptr);

		bool no_target = (!stricmp(mode_str, "ai-chase-any") ||
		                  !stricmp(mode_str, "ai-undock") ||
		                  !stricmp(mode_str, "ai-keep-safe-distance") ||
		                  !stricmp(mode_str, "ai-play-dead") ||
		                  !stricmp(mode_str, "ai-play-dead-persistent") ||
		                  !stricmp(mode_str, "ai-warp-out"));

		sexp_str += "( ";
		sexp_str += mode_str;
		sexp_str += " ";

		if (!no_target && target) {
			if (!stricmp(mode_str, "ai-destroy-subsystem")) {
				const char* subsys = json_get_string(val, "subsystem", nullptr);
				sexp_str += "\"";
				sexp_str += target;
				sexp_str += "\" ";
				if (subsys) {
					sexp_str += "\"";
					sexp_str += subsys;
					sexp_str += "\" ";
				}
			} else if (!stricmp(mode_str, "ai-dock")) {
				const char* docker_pt = json_get_string(val, "docker_point", nullptr);
				const char* dockee_pt = json_get_string(val, "dockee_point", nullptr);
				sexp_str += "\"";
				sexp_str += target;
				sexp_str += "\" ";
				if (docker_pt) {
					sexp_str += "\"";
					sexp_str += docker_pt;
					sexp_str += "\" ";
				}
				if (dockee_pt) {
					sexp_str += "\"";
					sexp_str += dockee_pt;
					sexp_str += "\" ";
				}
			} else {
				sexp_str += "\"";
				sexp_str += target;
				sexp_str += "\" ";
			}
		}

		char pri_buf[16];
		sprintf(pri_buf, "%d", priority);
		sexp_str += pri_buf;
		sexp_str += " ) ";
	}

	sexp_str += ")";

	char* saved_mp = Mp;
	SCP_string temp_buf(sexp_str);
	Mp = temp_buf.data();
	int node = get_sexp_main();
	Mp = saved_mp;

	return node;
}

int find_persona_index(const char* name)
{
	for (int i = 0; i < static_cast<int>(Personas.size()); i++) {
		if (!stricmp(Personas[i].name, name))
			return i;
	}
	return -1;
}

int find_ai_class(const char* name)
{
	for (int i = 0; i < Num_ai_classes; i++) {
		if (!stricmp(Ai_class_names[i], name))
			return i;
	}
	return -1;
}

// --- Section load functions ---

void load_mission_info_json(const json_t* obj, mission* pm)
{
	if (!obj) return;

	strcpy_s(pm->name, json_get_string(obj, "name", "Unnamed"));
	pm->author = json_get_string(obj, "author", "");

	pm->required_fso_version = gameversion::version(
		json_get_int(obj, "version_major", 0),
		json_get_int(obj, "version_minor", 0),
		json_get_int(obj, "version_build", 0),
		json_get_int(obj, "version_revision", 0)
	);

	strcpy_s(pm->created, json_get_string(obj, "created", ""));
	strcpy_s(pm->modified, json_get_string(obj, "modified", ""));
	strcpy_s(pm->notes, json_get_string(obj, "notes", ""));
	strcpy_s(pm->mission_desc, json_get_string(obj, "mission_desc", ""));

	pm->game_type = json_get_int(obj, "game_type", 0);
	load_mission_flags_json(json_object_get(obj, "flags"), pm->flags);

	pm->num_players = json_get_int(obj, "num_players", 1);
	pm->num_respawns = json_get_int(obj, "num_respawns", 0);
	pm->max_respawn_delay = json_get_int(obj, "max_respawn_delay", 0);

	// Support ships
	const json_t* ss = json_object_get(obj, "support_ships");
	if (ss) {
		pm->support_ships.arrival_location = parse_arrival_location(json_get_string(ss, "arrival_location", "At Location"));
		pm->support_ships.arrival_anchor = json_to_anchor(ss, "arrival_anchor");
		pm->support_ships.departure_location = parse_departure_location(json_get_string(ss, "departure_location", "At Location"));
		pm->support_ships.departure_anchor = json_to_anchor(ss, "departure_anchor");
		pm->support_ships.max_hull_repair_val = json_get_float(ss, "max_hull_repair_val", 0.0f);
		pm->support_ships.max_subsys_repair_val = json_get_float(ss, "max_subsys_repair_val", 100.0f);
		pm->support_ships.max_support_ships = json_get_int(ss, "max_support_ships", -1);
		pm->support_ships.max_concurrent_ships = json_get_int(ss, "max_concurrent_ships", 1);
		const char* sc_name = json_get_string(ss, "ship_class", nullptr);
		if (sc_name)
			pm->support_ships.ship_class = ship_info_lookup(sc_name);
	}

	// Nebula
	pm->neb_near_multi = json_get_float(obj, "fog_near_mult", 1.0f);
	pm->neb_far_multi = json_get_float(obj, "fog_far_mult", 1.0f);
	Neb2_fog_near_mult = pm->neb_near_multi;
	Neb2_fog_far_mult = pm->neb_far_multi;

	if (pm->flags[Mission::Mission_Flags::Fullneb]) {
		Neb2_awacs = json_get_float(obj, "neb_awacs", -1.0f);
		strcpy_s(Mission_parse_storm_name, json_get_string(obj, "storm_name", ""));
	}

	pm->contrail_threshold = json_get_int(obj, "contrail_threshold", CONTRAIL_THRESHOLD_DEFAULT);
	pm->ambient_light_level = json_get_int(obj, "ambient_light_level", 0);

	// Squadron info
	strcpy_s(pm->squad_name, json_get_string(obj, "squad_name", ""));
	strcpy_s(pm->squad_filename, json_get_string(obj, "squad_filename", ""));

	// Loading screens
	strcpy_s(pm->loading_screen[GR_640], json_get_string(obj, "loading_screen_640", ""));
	strcpy_s(pm->loading_screen[GR_1024], json_get_string(obj, "loading_screen_1024", ""));

	// Skybox
	strcpy_s(pm->skybox_model, json_get_string(obj, "skybox_model", ""));
	const json_t* sky_orient = json_object_get(obj, "skybox_orientation");
	if (sky_orient)
		pm->skybox_orientation = json_to_matrix(sky_orient);
	pm->skybox_flags = json_get_int(obj, "skybox_flags", DEFAULT_NMODEL_FLAGS);

	// Environment map
	strcpy_s(pm->envmap_name, json_get_string(obj, "envmap_name", ""));

	// AI profile
	const char* ai_prof = json_get_string(obj, "ai_profile", nullptr);
	if (ai_prof) {
		char buf[NAME_LENGTH];
		strcpy_s(buf, ai_prof);
		int idx = ai_profile_lookup(buf);
		if (idx >= 0)
			pm->ai_profile = &Ai_profiles[idx];
	}

	// Lighting profile
	pm->lighting_profile_name = json_get_string(obj, "lighting_profile", "");

	// Sound environment
	const json_t* snd = json_object_get(obj, "sound_environment");
	if (snd) {
		const char* preset_name = json_get_string(snd, "preset", "");
		for (size_t i = 0; i < EFX_presets.size(); i++) {
			if (!stricmp(EFX_presets[i].name.c_str(), preset_name)) {
				pm->sound_environment.id = static_cast<int>(i);
				break;
			}
		}
		pm->sound_environment.volume = json_get_float(snd, "volume", 1.0f);
		pm->sound_environment.damping = json_get_float(snd, "damping", 1.0f);
		pm->sound_environment.decay = json_get_float(snd, "decay", 1.0f);
	}

	// Gravity
	const json_t* grav = json_object_get(obj, "gravity");
	if (grav)
		pm->gravity = json_to_vec3d(grav);

	pm->HUD_timer_padding = json_get_int(obj, "hud_timer_padding", 0);

	// Command persona/sender
	const char* cmd_persona = json_get_string(obj, "command_persona", nullptr);
	if (cmd_persona)
		pm->command_persona = find_persona_index(cmd_persona);
	strcpy_s(pm->command_sender, json_get_string(obj, "command_sender", ""));

	// Wing names
	const json_t* sw = json_object_get(obj, "starting_wing_names");
	if (sw && json_is_array(sw)) {
		size_t count = std::min(json_array_size(sw), static_cast<size_t>(MAX_STARTING_WINGS));
		for (size_t i = 0; i < count; i++) {
			strcpy_s(Starting_wing_names[i], json_string_value(json_array_get(sw, i)));
		}
	}
	const json_t* sq = json_object_get(obj, "squadron_wing_names");
	if (sq && json_is_array(sq)) {
		size_t count = std::min(json_array_size(sq), static_cast<size_t>(MAX_SQUADRON_WINGS));
		for (size_t i = 0; i < count; i++) {
			strcpy_s(Squadron_wing_names[i], json_string_value(json_array_get(sq, i)));
		}
	}
	const json_t* tv = json_object_get(obj, "tvt_wing_names");
	if (tv && json_is_array(tv)) {
		size_t count = std::min(json_array_size(tv), static_cast<size_t>(MAX_TVT_WINGS));
		for (size_t i = 0; i < count; i++) {
			strcpy_s(TVT_wing_names[i], json_string_value(json_array_get(tv, i)));
		}
	}
}

void load_variables_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	size_t index;
	json_t* val;
	int var_idx = 0;

	json_array_foreach(arr, index, val) {
		if (var_idx >= MAX_SEXP_VARIABLES)
			break;

		const char* name = json_get_string(val, "name", "");
		const char* default_val = json_get_string(val, "default_value", "");
		const char* type_str = json_get_string(val, "type", "string");

		int type = SEXP_VARIABLE_STRING;
		if (strstr(type_str, "number"))
			type = SEXP_VARIABLE_NUMBER;
		if (strstr(type_str, "block"))
			type |= SEXP_VARIABLE_BLOCK;
		if (strstr(type_str, "network"))
			type |= SEXP_VARIABLE_NETWORK;
		if (strstr(type_str, "eternal"))
			type |= SEXP_VARIABLE_SAVE_TO_PLAYER_FILE;
		if (strstr(type_str, "save-on-mission-close"))
			type |= SEXP_VARIABLE_SAVE_ON_MISSION_CLOSE;
		if (strstr(type_str, "save-on-mission-progress"))
			type |= SEXP_VARIABLE_SAVE_ON_MISSION_PROGRESS;

		sexp_add_variable(default_val, name, type);
		var_idx++;
	}
}

void load_containers_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	SCP_vector<sexp_container> new_containers;
	size_t index;
	json_t* val;

	json_array_foreach(arr, index, val) {
		sexp_container sc;
		sc.container_name = json_get_string(val, "name", "");

		const char* ct = json_get_string(val, "container_type", "list");
		const char* dt = json_get_string(val, "data_type", "string");

		sc.type = ContainerType::NONE;
		if (!stricmp(ct, "list"))
			sc.type = sc.type | ContainerType::LIST;
		else
			sc.type = sc.type | ContainerType::MAP;

		if (!stricmp(dt, "number"))
			sc.type = sc.type | ContainerType::NUMBER_DATA;
		else
			sc.type = sc.type | ContainerType::STRING_DATA;

		if (sc.is_map()) {
			const char* kt = json_get_string(val, "key_type", "string");
			if (!stricmp(kt, "string"))
				sc.type = sc.type | ContainerType::STRING_KEYS;
		}

		if (json_get_bool(val, "strictly_typed_data"))
			sc.type = sc.type | ContainerType::STRICTLY_TYPED_DATA;
		if (json_get_bool(val, "strictly_typed_keys"))
			sc.type = sc.type | ContainerType::STRICTLY_TYPED_KEYS;
		if (json_get_bool(val, "network"))
			sc.type = sc.type | ContainerType::NETWORK;
		if (json_get_bool(val, "eternal"))
			sc.type = sc.type | ContainerType::SAVE_TO_PLAYER_FILE;
		if (json_get_bool(val, "save_on_mission_close"))
			sc.type = sc.type | ContainerType::SAVE_ON_MISSION_CLOSE;
		if (json_get_bool(val, "save_on_mission_progress"))
			sc.type = sc.type | ContainerType::SAVE_ON_MISSION_PROGRESS;

		json_t* data = json_object_get(val, "data");
		if (data) {
			if (sc.is_list() && json_is_array(data)) {
				size_t di;
				json_t* dv;
				json_array_foreach(data, di, dv) {
					sc.list_data.push_back(json_string_value(dv));
				}
			} else if (sc.is_map() && json_is_object(data)) {
				const char* key;
				json_t* dv;
				json_object_foreach(data, key, dv) {
					sc.map_data[key] = json_string_value(dv);
				}
			}
		}

		new_containers.push_back(std::move(sc));
	}

	SCP_unordered_map<SCP_string, SCP_string, SCP_string_lcase_hash, SCP_string_lcase_equal_to> empty_renames;
	update_sexp_containers(new_containers, empty_renames);
}

void load_cutscenes_json(const json_t* arr, mission* pm)
{
	if (!arr || !json_is_array(arr))
		return;

	size_t index;
	json_t* val;
	json_array_foreach(arr, index, val) {
		mission_cutscene cs;
		cs.type = json_get_int(val, "type", 0);
		strcpy_s(cs.filename, json_get_string(val, "filename", ""));
		cs.formula = json_to_sexp(json_object_get(val, "formula"));
		pm->cutscenes.push_back(cs);
	}
}

void load_fiction_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	Fiction_viewer_stages.clear();
	size_t index;
	json_t* val;
	json_array_foreach(arr, index, val) {
		fiction_viewer_stage fvs;
		memset(&fvs, 0, sizeof(fvs));
		strcpy_s(fvs.story_filename, json_get_string(val, "story_filename", ""));
		strcpy_s(fvs.font_filename, json_get_string(val, "font_filename", ""));
		strcpy_s(fvs.voice_filename, json_get_string(val, "voice_filename", ""));
		strcpy_s(fvs.ui_name, json_get_string(val, "ui_name", ""));
		strcpy_s(fvs.background[GR_640], json_get_string(val, "background_640", ""));
		strcpy_s(fvs.background[GR_1024], json_get_string(val, "background_1024", ""));
		fvs.formula = json_to_sexp(json_object_get(val, "formula"));
		Fiction_viewer_stages.push_back(fvs);
	}
}

void load_cmd_briefs_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	size_t count = std::min(json_array_size(arr), static_cast<size_t>(MAX_TVT_TEAMS));
	for (size_t t = 0; t < count; t++) {
		const json_t* obj = json_array_get(arr, t);
		cmd_brief& cb = Cmd_briefs[t];
		cb.num_stages = json_get_int(obj, "num_stages", 0);

		strcpy_s(cb.background[GR_640], json_get_string(obj, "background_640", ""));
		strcpy_s(cb.background[GR_1024], json_get_string(obj, "background_1024", ""));

		const json_t* stages = json_object_get(obj, "stages");
		if (stages && json_is_array(stages)) {
			size_t sc = std::min(json_array_size(stages), static_cast<size_t>(CMD_BRIEF_STAGES_MAX));
			for (size_t i = 0; i < sc; i++) {
				const json_t* s = json_array_get(stages, i);
				cb.stage[i].text = json_get_string(s, "text", "");
				strcpy_s(cb.stage[i].ani_filename, json_get_string(s, "ani_filename", ""));
				strcpy_s(cb.stage[i].wave_filename, json_get_string(s, "wave_filename", ""));
			}
		}
	}
}

void load_briefing_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	size_t count = std::min(json_array_size(arr), static_cast<size_t>(MAX_TVT_TEAMS));
	for (size_t t = 0; t < count; t++) {
		const json_t* obj = json_array_get(arr, t);
		briefing& b = Briefings[t];
		b.num_stages = json_get_int(obj, "num_stages", 0);

		strcpy_s(b.background[GR_640], json_get_string(obj, "background_640", ""));
		strcpy_s(b.background[GR_1024], json_get_string(obj, "background_1024", ""));

		const json_t* stages = json_object_get(obj, "stages");
		if (stages && json_is_array(stages)) {
			size_t sc = std::min(json_array_size(stages), static_cast<size_t>(MAX_BRIEF_STAGES));
			for (size_t i = 0; i < sc; i++) {
				const json_t* s = json_array_get(stages, i);
				brief_stage& bs = b.stages[i];
				bs.text = json_get_string(s, "text", "");
				strcpy_s(bs.voice, json_get_string(s, "voice", ""));
				bs.camera_pos = json_to_vec3d(json_object_get(s, "camera_pos"));
				bs.camera_orient = json_to_matrix(json_object_get(s, "camera_orient"));
				bs.camera_time = json_get_int(s, "camera_time", 0);
				bs.flags = json_get_int(s, "flags", 0);
				bs.formula = json_to_sexp(json_object_get(s, "formula"));
				bs.draw_grid = json_get_bool(s, "draw_grid", true);

				const json_t* icons = json_object_get(s, "icons");
				if (icons && json_is_array(icons)) {
					bs.num_icons = static_cast<int>(json_array_size(icons));
					bs.icons = new brief_icon[bs.num_icons];
					for (int j = 0; j < bs.num_icons; j++) {
						const json_t* ic = json_array_get(icons, j);
						memset(&bs.icons[j], 0, sizeof(brief_icon));
						bs.icons[j].type = json_get_int(ic, "type", 0);
						bs.icons[j].team = json_get_int(ic, "team", 0);
						bs.icons[j].id = json_get_int(ic, "id", 0);
						bs.icons[j].pos = json_to_vec3d(json_object_get(ic, "pos"));
						bs.icons[j].flags = json_get_int(ic, "flags", 0);

						const char* sc_name = json_get_string(ic, "ship_class", nullptr);
						if (sc_name)
							bs.icons[j].ship_class = ship_info_lookup(sc_name);
						else
							bs.icons[j].ship_class = -1;

						strcpy_s(bs.icons[j].label, json_get_string(ic, "label", ""));
						strcpy_s(bs.icons[j].closeup_label, json_get_string(ic, "closeup_label", ""));
					}
				}

				const json_t* lines = json_object_get(s, "lines");
				if (lines && json_is_array(lines)) {
					bs.num_lines = static_cast<int>(json_array_size(lines));
					bs.lines = new brief_line[bs.num_lines];
					for (int j = 0; j < bs.num_lines; j++) {
						const json_t* ln = json_array_get(lines, j);
						bs.lines[j].start_icon = json_get_int(ln, "start_icon", 0);
						bs.lines[j].end_icon = json_get_int(ln, "end_icon", 0);
					}
				}
			}
		}
	}
}

void load_debriefing_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	size_t count = std::min(json_array_size(arr), static_cast<size_t>(MAX_TVT_TEAMS));
	for (size_t t = 0; t < count; t++) {
		const json_t* obj = json_array_get(arr, t);
		debriefing& d = Debriefings[t];
		d.num_stages = json_get_int(obj, "num_stages", 0);

		strcpy_s(d.background[GR_640], json_get_string(obj, "background_640", ""));
		strcpy_s(d.background[GR_1024], json_get_string(obj, "background_1024", ""));

		const json_t* stages = json_object_get(obj, "stages");
		if (stages && json_is_array(stages)) {
			size_t sc = std::min(json_array_size(stages), static_cast<size_t>(MAX_DEBRIEF_STAGES));
			for (size_t i = 0; i < sc; i++) {
				const json_t* s = json_array_get(stages, i);
				debrief_stage& ds = d.stages[i];
				ds.formula = json_to_sexp(json_object_get(s, "formula"));
				ds.text = json_get_string(s, "text", "");
				strcpy_s(ds.voice, json_get_string(s, "voice", ""));
				ds.recommendation_text = json_get_string(s, "recommendation_text", "");
			}
		}
	}
}

void load_players_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	Num_teams = static_cast<int>(std::min(json_array_size(arr), static_cast<size_t>(MAX_TVT_TEAMS)));

	for (int t = 0; t < Num_teams; t++) {
		const json_t* team = json_array_get(arr, t);
		team_data& td = Team_data[t];

		if (t == 0) {
			const char* ssn = json_get_string(team, "starting_shipname", nullptr);
			if (ssn)
				strcpy_s(Player_start_shipname, ssn);
		}

		const char* ds = json_get_string(team, "default_ship", nullptr);
		if (ds)
			td.default_ship = ship_info_lookup(ds);

		td.loadout_total = json_get_int(team, "loadout_total", 0);
		td.do_not_validate = json_get_bool(team, "do_not_validate", false);

		// Ship choices
		const json_t* ships = json_object_get(team, "ship_choices");
		if (ships && json_is_array(ships)) {
			td.num_ship_choices = static_cast<int>(std::min(json_array_size(ships), static_cast<size_t>(MAX_SHIP_CLASSES)));
			for (int i = 0; i < td.num_ship_choices; i++) {
				const json_t* entry = json_array_get(ships, i);

				const char* var = json_get_string(entry, "variable", nullptr);
				if (var) {
					strcpy_s(td.ship_list_variables[i], var);
					td.ship_list[i] = -1;
				} else {
					const char* sc_name = json_get_string(entry, "ship_class", "");
					td.ship_list[i] = ship_info_lookup(sc_name);
					td.ship_list_variables[i][0] = '\0';
				}

				const char* cv = json_get_string(entry, "count_variable", nullptr);
				if (cv) {
					strcpy_s(td.ship_count_variables[i], cv);
					td.ship_count[i] = 0;
				} else {
					td.ship_count[i] = json_get_int(entry, "count", 0);
					td.ship_count_variables[i][0] = '\0';
				}
			}
		}

		// Weapon pool
		const json_t* weapons = json_object_get(team, "weapon_pool");
		if (weapons && json_is_array(weapons)) {
			td.num_weapon_choices = static_cast<int>(std::min(json_array_size(weapons), static_cast<size_t>(MAX_WEAPON_TYPES)));
			for (int i = 0; i < td.num_weapon_choices; i++) {
				const json_t* entry = json_array_get(weapons, i);

				const char* var = json_get_string(entry, "variable", nullptr);
				if (var) {
					strcpy_s(td.weaponry_pool_variable[i], var);
					td.weaponry_pool[i] = -1;
				} else {
					const char* wc_name = json_get_string(entry, "weapon_class", "");
					td.weaponry_pool[i] = weapon_info_lookup(wc_name);
					td.weaponry_pool_variable[i][0] = '\0';
				}

				const char* cv = json_get_string(entry, "count_variable", nullptr);
				if (cv) {
					strcpy_s(td.weaponry_amount_variable[i], cv);
					td.weaponry_count[i] = 0;
				} else {
					td.weaponry_count[i] = json_get_int(entry, "count", 0);
					td.weaponry_amount_variable[i][0] = '\0';
				}

				td.weapon_required[i] = json_get_bool(entry, "required", false);
			}
		}
	}
}

void load_objects_json(const json_t* arr, mission* pm)
{
	if (!arr || !json_is_array(arr))
		return;

	Parse_objects.clear();

	size_t index;
	json_t* val;
	json_array_foreach(arr, index, val) {
		p_object po;  // default member initializers handle most fields

		strcpy_s(po.name, json_get_string(val, "name", ""));

		const char* display = json_get_string(val, "display_name", nullptr);
		if (display) {
			po.display_name = display;
			po.flags.set(Mission::Parse_Object_Flags::SF_Has_display_name);
		}

		const char* sc_name = json_get_string(val, "ship_class", "");
		po.ship_class = ship_info_lookup(sc_name);

		const char* team_name = json_get_string(val, "team", "");
		po.team = iff_lookup(team_name);
		po.loadout_team = po.team;

		const char* tcs = json_get_string(val, "team_color_setting", nullptr);
		if (tcs) po.team_color_setting = tcs;

		po.pos = json_to_vec3d(json_object_get(val, "position"));
		po.orient = json_to_matrix(json_object_get(val, "orientation"));

		const char* ai_class_name = json_get_string(val, "ai_class", nullptr);
		if (ai_class_name)
			po.ai_class = find_ai_class(ai_class_name);

		// AI goals
		po.ai_goals = ai_goals_json_to_sexp(json_object_get(val, "ai_goals"));

		// Cargo
		const char* cargo = json_get_string(val, "cargo", nullptr);
		if (cargo) {
			int ci;
			for (ci = 0; ci < Num_cargo; ci++) {
				if (!stricmp(Cargo_names[ci], cargo))
					break;
			}
			if (ci == Num_cargo && Num_cargo < MAX_CARGO) {
				strcpy_s(Cargo_names_buf[Num_cargo], cargo);
				Cargo_names[Num_cargo] = Cargo_names_buf[Num_cargo];
				Num_cargo++;
			}
			po.cargo1 = static_cast<char>(ci);
		}

		po.initial_velocity = json_get_int(val, "initial_velocity", 0);
		po.initial_hull = json_get_int(val, "initial_hull", 100);
		po.initial_shields = json_get_int(val, "initial_shields", 100);

		// Arrival / Departure
		po.arrival_location = parse_arrival_location(json_get_string(val, "arrival_location", "At Location"));
		po.arrival_distance = json_get_int(val, "arrival_distance", 0);

		po.arrival_anchor = json_to_anchor(val, "arrival_anchor");
		po.arrival_cue = json_to_sexp(json_object_get(val, "arrival_cue"));
		po.arrival_delay = json_get_int(val, "arrival_delay", 0);
		po.arrival_path_mask = json_get_int(val, "arrival_path_mask", 0);

		po.departure_location = parse_departure_location(json_get_string(val, "departure_location", "At Location"));
		po.departure_anchor = json_to_anchor(val, "departure_anchor");
		po.departure_cue = json_to_sexp(json_object_get(val, "departure_cue"));
		po.departure_delay = json_get_int(val, "departure_delay", 0);
		po.departure_path_mask = json_get_int(val, "departure_path_mask", 0);

		// Flags
		load_parse_object_flags_json(json_object_get(val, "flags"), po.flags);

		if (po.flags[Mission::Parse_Object_Flags::OF_Player_start]) {
			po.flags.set(Mission::Parse_Object_Flags::SF_Cargo_known);
			Player_starts++;

			if (*Player_start_shipname == '\0') {
				strcpy_s(Player_start_shipname, po.name);
			}
		}

		po.escort_priority = json_get_int(val, "escort_priority", 0);
		po.hotkey = json_get_int(val, "hotkey", -1);
		po.score = json_get_int(val, "score", 0);
		po.assist_score_pct = json_get_float(val, "assist_score_pct", 0.0f);

		const char* persona_name = json_get_string(val, "persona", nullptr);
		if (persona_name)
			po.persona_index = find_persona_index(persona_name);

		po.kamikaze_damage = json_get_int(val, "kamikaze_damage", 0);
		po.group = json_get_int(val, "group", -1);
		po.collision_group_id = json_get_int(val, "collision_group_id", 0);
		po.ship_max_hull_strength = json_get_float(val, "ship_max_hull_strength", 0.0f);
		po.ship_max_shield_strength = json_get_float(val, "ship_max_shield_strength", 0.0f);
		po.destroy_before_mission_time = json_get_int(val, "destroy_before_mission_time", -1);
		po.net_signature = static_cast<ushort>(json_get_int(val, "net_signature", 0));
		po.respawn_count = json_get_int(val, "respawn_count", 0);
		po.respawn_priority = json_get_int(val, "respawn_priority", 0);

		// Special explosion
		const json_t* sexp_obj = json_object_get(val, "special_explosion");
		if (sexp_obj) {
			po.use_special_explosion = true;
			po.special_exp_damage = json_get_int(sexp_obj, "damage", 0);
			po.special_exp_blast = json_get_int(sexp_obj, "blast", 0);
			po.special_exp_inner = json_get_int(sexp_obj, "inner_radius", 0);
			po.special_exp_outer = json_get_int(sexp_obj, "outer_radius", 0);
			po.use_shockwave = json_get_bool(sexp_obj, "use_shockwave", false);
			po.special_exp_shockwave_speed = json_get_int(sexp_obj, "shockwave_speed", 0);
			po.special_exp_deathroll_time = json_get_int(sexp_obj, "deathroll_time", 0);
		}

		po.special_hitpoints = json_get_int(val, "special_hitpoints", 0);
		po.special_shield = json_get_int(val, "special_shield", -1);

		// Texture replacements
		const json_t* tex_arr = json_object_get(val, "replacement_textures");
		if (tex_arr && json_is_array(tex_arr)) {
			size_t ti;
			json_t* tv;
			json_array_foreach(tex_arr, ti, tv) {
				texture_replace tr;
				memset(&tr, 0, sizeof(tr));
				strcpy_s(tr.ship_name, po.name);
				strcpy_s(tr.old_texture, json_get_string(tv, "old_texture", ""));
				strcpy_s(tr.new_texture, json_get_string(tv, "new_texture", ""));
				tr.new_texture_id = -1;
				tr.from_table = false;
				po.replacement_textures.push_back(tr);
			}
		}

		// Alt classes
		const json_t* alt_arr = json_object_get(val, "alt_classes");
		if (alt_arr && json_is_array(alt_arr)) {
			size_t ai;
			json_t* av;
			json_array_foreach(alt_arr, ai, av) {
				alt_class ac;
				const char* acn = json_get_string(av, "ship_class", "");
				ac.ship_class = ship_info_lookup(acn);
				ac.default_to_this_class = json_get_bool(av, "default", false);
				po.alt_classes.push_back(ac);
			}
		}

		// Orders accepted
		const json_t* orders = json_object_get(val, "orders_accepted");
		if (orders && json_is_array(orders)) {
			size_t oi;
			json_t* ov;
			json_array_foreach(orders, oi, ov) {
				po.orders_accepted.insert(static_cast<size_t>(json_integer_value(ov)));
			}
		}

			// Subsystem status from JSON
		const json_t* subsys_arr = json_object_get(val, "subsystems");
		if (subsys_arr && json_is_array(subsys_arr)) {
			po.subsys_index = Subsys_index;
			po.subsys_count = 0;
			size_t si;
			json_t* sv;
			json_array_foreach(subsys_arr, si, sv) {
				int new_idx = allocate_subsys_status();
				subsys_status* ssp = &Subsys_status[new_idx];

				strcpy_s(ssp->name, json_get_string(sv, "name", "Pilot"));
				ssp->percent = json_get_float(sv, "damage_percent", 0.0f);

				// Primary banks
				const json_t* prim = json_object_get(sv, "primary_banks");
				if (prim && json_is_array(prim)) {
					size_t bc = std::min(json_array_size(prim), static_cast<size_t>(MAX_SHIP_PRIMARY_BANKS));
					for (size_t b = 0; b < bc; b++) {
						const char* wn = json_string_value(json_array_get(prim, b));
						ssp->primary_banks[b] = (wn && strlen(wn) > 0) ? weapon_info_lookup(wn) : -1;
					}
				}

				// Secondary banks
				const json_t* sec = json_object_get(sv, "secondary_banks");
				if (sec && json_is_array(sec)) {
					size_t bc = std::min(json_array_size(sec), static_cast<size_t>(MAX_SHIP_SECONDARY_BANKS));
					for (size_t b = 0; b < bc; b++) {
						const char* wn = json_string_value(json_array_get(sec, b));
						ssp->secondary_banks[b] = (wn && strlen(wn) > 0) ? weapon_info_lookup(wn) : -1;
					}
				}

				const json_t* ammo = json_object_get(sv, "secondary_ammo");
				if (ammo && json_is_array(ammo)) {
					size_t bc = std::min(json_array_size(ammo), static_cast<size_t>(MAX_SHIP_SECONDARY_BANKS));
					for (size_t b = 0; b < bc; b++) {
						ssp->secondary_ammo[b] = static_cast<int>(json_integer_value(json_array_get(ammo, b)));
					}
				}

				po.subsys_count++;
			}
		}

		// Dock list -> Initially_docked
		const json_t* dock_arr = json_object_get(val, "dock_list");
		if (dock_arr && json_is_array(dock_arr)) {
			size_t di;
			json_t* dv;
			json_array_foreach(dock_arr, di, dv) {
				if (Total_initially_docked >= MAX_SHIPS)
					break;

				const char* docked_to = json_get_string(dv, "docked_to", nullptr);
				const char* docker_pt = json_get_string(dv, "docker_point", nullptr);
				const char* dockee_pt = json_get_string(dv, "dockee_point", nullptr);

				if (docked_to) {
					strcpy_s(Initially_docked[Total_initially_docked].docker, po.name);
					strcpy_s(Initially_docked[Total_initially_docked].dockee, docked_to);
					strcpy_s(Initially_docked[Total_initially_docked].docker_point, docker_pt ? docker_pt : "");
					strcpy_s(Initially_docked[Total_initially_docked].dockee_point, dockee_pt ? dockee_pt : "");
					Total_initially_docked++;
				}
			}
		}

		Parse_objects.push_back(po);
	}
}

void load_wings_json(const json_t* arr, mission* pm)
{
	if (!arr || !json_is_array(arr))
		return;

	Num_wings = 0;
	size_t index;
	json_t* val;

	json_array_foreach(arr, index, val) {
		if (Num_wings >= MAX_WINGS)
			break;

		wing& w = Wings[Num_wings];
		w.clear();

		strcpy_s(w.name, json_get_string(val, "name", ""));
		const char* sql = json_get_string(val, "squad_logo", nullptr);
		if (sql)
			strcpy_s(w.wing_squad_filename, sql);

		w.num_waves = json_get_int(val, "num_waves", 1);
		w.threshold = json_get_int(val, "wave_threshold", 0);
		w.special_ship = json_get_int(val, "special_ship", 0);
		w.formation = json_get_int(val, "formation", -1);
		w.formation_scale = json_get_float(val, "formation_scale", 1.0f);

		// Ships in wing - resolve ship names to Parse_objects indices
		const json_t* ships = json_object_get(val, "ships");
		if (ships && json_is_array(ships)) {
			w.wave_count = static_cast<int>(std::min(json_array_size(ships), static_cast<size_t>(MAX_SHIPS_PER_WING)));
			for (int j = 0; j < w.wave_count; j++) {
				const char* ship_name = json_string_value(json_array_get(ships, j));
				if (ship_name) {
					// Find this ship in Parse_objects and set its wingnum
					for (size_t pi = 0; pi < Parse_objects.size(); pi++) {
						if (!stricmp(Parse_objects[pi].name, ship_name)) {
							Parse_objects[pi].wingnum = Num_wings;
							Parse_objects[pi].pos_in_wing = j;
							break;
						}
					}
				}
			}
		}

		// Arrival / Departure
		w.arrival_location = parse_arrival_location(json_get_string(val, "arrival_location", "At Location"));
		w.arrival_distance = json_get_int(val, "arrival_distance", 0);
		w.arrival_anchor = json_to_anchor(val, "arrival_anchor");
		w.arrival_cue = json_to_sexp(json_object_get(val, "arrival_cue"));
		w.arrival_delay = json_get_int(val, "arrival_delay", 0);
		w.arrival_path_mask = json_get_int(val, "arrival_path_mask", 0);

		w.departure_location = parse_departure_location(json_get_string(val, "departure_location", "At Location"));
		w.departure_anchor = json_to_anchor(val, "departure_anchor");
		w.departure_cue = json_to_sexp(json_object_get(val, "departure_cue"));
		w.departure_delay = json_get_int(val, "departure_delay", 0);
		w.departure_path_mask = json_get_int(val, "departure_path_mask", 0);

		// AI goals
		load_ai_goals_json(json_object_get(val, "ai_goals"), w.ai_goals);

		w.hotkey = json_get_int(val, "hotkey", -1);
		w.wave_delay_min = json_get_int(val, "wave_delay_min", 0);
		w.wave_delay_max = json_get_int(val, "wave_delay_max", 0);

		// Wing flags
		const json_t* wflags = json_object_get(val, "flags");
		if (wflags && json_is_array(wflags)) {
			size_t fi;
			json_t* fv;
			json_array_foreach(wflags, fi, fv) {
				const char* name = json_string_value(fv);
				if (!name) continue;
				for (size_t f = 0; f < Num_wing_flag_names; f++) {
					if (!stricmp(name, Wing_flag_names[f].flag_name)) {
						w.flags.set(Wing_flag_names[f].flag);
						break;
					}
				}
			}
		}

		Num_wings++;
	}
}

void load_props_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	Parse_props.clear();
	size_t index;
	json_t* val;
	json_array_foreach(arr, index, val) {
		parsed_prop p{};

		strcpy_s(p.name, json_get_string(val, "name", ""));

		const char* class_name = json_get_string(val, "class", nullptr);
		if (class_name)
			p.prop_info_index = prop_info_lookup(class_name);
		else
			p.prop_info_index = -1;

		p.position = json_to_vec3d(json_object_get(val, "position"));
		p.orientation = json_to_matrix(json_object_get(val, "orientation"));
		load_parse_object_flags_json(json_object_get(val, "flags"), p.flags);

		Parse_props.push_back(p);
	}
}

void load_events_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	Mission_events.clear();
	size_t index;
	json_t* val;

	json_array_foreach(arr, index, val) {
		mission_event evt;
		evt.name = json_get_string(val, "name", "");
		evt.formula = json_to_sexp(json_object_get(val, "formula"));
		evt.repeat_count = json_get_int(val, "repeat_count", 1);
		evt.trigger_count = json_get_int(val, "trigger_count", 1);
		evt.interval = json_get_int(val, "interval", 1);
		evt.score = json_get_int(val, "score", 0);
		evt.chain_delay = json_get_int(val, "chain_delay", -1);
		evt.objective_text = json_get_string(val, "objective_text", "");
		evt.objective_key_text = json_get_string(val, "objective_key_text", "");
		evt.team = json_get_int(val, "team", -1);
		evt.flags = json_get_int(val, "event_flags", 0);
		evt.mission_log_flags = json_get_int(val, "mission_log_flags", 0);

		Mission_events.push_back(std::move(evt));
	}
}

void load_goals_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	Mission_goals.clear();
	size_t index;
	json_t* val;

	json_array_foreach(arr, index, val) {
		mission_goal goal;
		goal.name = json_get_string(val, "name", "");

		const char* type_str = json_get_string(val, "type", "primary");
		if (!stricmp(type_str, "secondary"))
			goal.type = SECONDARY_GOAL;
		else if (!stricmp(type_str, "bonus"))
			goal.type = BONUS_GOAL;
		else
			goal.type = PRIMARY_GOAL;

		goal.formula = json_to_sexp(json_object_get(val, "formula"));
		goal.message = json_get_string(val, "message", "");
		goal.score = json_get_int(val, "score", 0);
		goal.flags = json_get_int(val, "flags", 0);
		goal.team = json_get_int(val, "team", 0);

		Mission_goals.push_back(std::move(goal));
	}
}

void load_waypoints_json(const json_t* obj)
{
	if (!obj) return;

	// Jump nodes
	const json_t* jn_arr = json_object_get(obj, "jump_nodes");
	if (jn_arr && json_is_array(jn_arr)) {
		size_t index;
		json_t* val;
		json_array_foreach(jn_arr, index, val) {
			vec3d pos = json_to_vec3d(json_object_get(val, "position"));
			CJumpNode jn(&pos);

			const char* name = json_get_string(val, "name", "");
			jn.SetName(name);

			Jump_nodes.push_back(std::move(jn));
		}
	}

	// Waypoint lists
	const json_t* wp_arr = json_object_get(obj, "waypoint_lists");
	if (wp_arr && json_is_array(wp_arr)) {
		size_t index;
		json_t* val;
		json_array_foreach(wp_arr, index, val) {
			const char* name = json_get_string(val, "name", "");
			waypoint_list wpl(name);

			const json_t* points = json_object_get(val, "points");
			if (points && json_is_array(points)) {
				size_t pi;
				json_t* pv;
				json_array_foreach(points, pi, pv) {
					vec3d pt = json_to_vec3d(pv);
					wpl.get_waypoints().push_back(waypoint(&pt));
				}
			}

			Waypoint_lists.push_back(std::move(wpl));
		}
	}
}

void load_messages_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	size_t index;
	json_t* val;
	json_array_foreach(arr, index, val) {
		MMessage msg;
		msg.name[0] = '\0';
		msg.message[0] = '\0';
		msg.persona_index = -1;
		msg.multi_team = -1;
		msg.mood = 0;
		msg.outer_filter_radius = 0;
		msg.boost_level = 0;
		msg.avi_info.index = -1;
		msg.wave_info.index = -1;

		strcpy_s(msg.name, json_get_string(val, "name", ""));
		strcpy_s(msg.message, json_get_string(val, "message", ""));

		const char* persona = json_get_string(val, "persona", nullptr);
		if (persona)
			msg.persona_index = find_persona_index(persona);

		msg.multi_team = json_get_int(val, "multi_team", -1);

		const char* avi = json_get_string(val, "avi_name", nullptr);
		if (avi)
			msg.avi_info.name = vm_strdup(avi);

		const char* wav = json_get_string(val, "wave_name", nullptr);
		if (wav)
			msg.wave_info.name = vm_strdup(wav);

		msg.note = json_get_string(val, "note", "");

		Messages.push_back(msg);
	}
}

void load_reinforcements_json(const json_t* arr)
{
	if (!arr || !json_is_array(arr))
		return;

	Num_reinforcements = 0;
	size_t index;
	json_t* val;

	json_array_foreach(arr, index, val) {
		if (Num_reinforcements >= MAX_REINFORCEMENTS)
			break;

		reinforcements& r = Reinforcements[Num_reinforcements];
		memset(&r, 0, sizeof(r));

		strcpy_s(r.name, json_get_string(val, "name", ""));
		r.type = json_get_int(val, "type", 0);
		r.uses = json_get_int(val, "uses", 1);
		r.arrival_delay = json_get_int(val, "arrival_delay", 0);

		const json_t* no_msgs = json_object_get(val, "no_messages");
		if (no_msgs && json_is_array(no_msgs)) {
			size_t mc = std::min(json_array_size(no_msgs), static_cast<size_t>(MAX_REINFORCEMENT_MESSAGES));
			for (size_t j = 0; j < mc; j++) {
				strcpy_s(r.no_messages[j], json_string_value(json_array_get(no_msgs, j)));
			}
		}

		const json_t* yes_msgs = json_object_get(val, "yes_messages");
		if (yes_msgs && json_is_array(yes_msgs)) {
			size_t mc = std::min(json_array_size(yes_msgs), static_cast<size_t>(MAX_REINFORCEMENT_MESSAGES));
			for (size_t j = 0; j < mc; j++) {
				strcpy_s(r.yes_messages[j], json_string_value(json_array_get(yes_msgs, j)));
			}
		}

		Num_reinforcements++;
	}
}

void load_bitmaps_json(const json_t* obj)
{
	if (!obj) return;

	The_mission.ambient_light_level = json_get_int(obj, "ambient_light_level", 0);

	const char* envmap = json_get_string(obj, "environment_map", nullptr);
	if (envmap)
		strcpy_s(The_mission.envmap_name, envmap);

	const json_t* bg_arr = json_object_get(obj, "backgrounds");
	if (bg_arr && json_is_array(bg_arr)) {
		Backgrounds.clear();
		size_t index;
		json_t* val;
		json_array_foreach(bg_arr, index, val) {
			background_t bg;

			const json_t* suns = json_object_get(val, "suns");
			if (suns && json_is_array(suns)) {
				size_t si;
				json_t* sv;
				json_array_foreach(suns, si, sv) {
					starfield_list_entry sle;
					memset(&sle, 0, sizeof(sle));
					strcpy_s(sle.filename, json_get_string(sv, "filename", ""));
					sle.ang = json_to_angles(json_object_get(sv, "angles"));
					sle.scale_x = json_get_float(sv, "scale_x", 1.0f);
					sle.scale_y = json_get_float(sv, "scale_y", 1.0f);
					bg.suns.push_back(sle);
				}
			}

			const json_t* bitmaps = json_object_get(val, "bitmaps");
			if (bitmaps && json_is_array(bitmaps)) {
				size_t bi;
				json_t* bv;
				json_array_foreach(bitmaps, bi, bv) {
					starfield_list_entry sle;
					memset(&sle, 0, sizeof(sle));
					strcpy_s(sle.filename, json_get_string(bv, "filename", ""));
					sle.ang = json_to_angles(json_object_get(bv, "angles"));
					sle.scale_x = json_get_float(bv, "scale_x", 1.0f);
					sle.scale_y = json_get_float(bv, "scale_y", 1.0f);
					sle.div_x = json_get_int(bv, "div_x", 1);
					sle.div_y = json_get_int(bv, "div_y", 1);
					bg.bitmaps.push_back(sle);
				}
			}

			Backgrounds.push_back(std::move(bg));
		}
	}
}

void load_asteroid_fields_json(const json_t* obj)
{
	if (!obj) return;

	asteroid_field& af = Asteroid_field;

	af.num_initial_asteroids = json_get_int(obj, "num_initial_asteroids", 0);
	if (af.num_initial_asteroids <= 0)
		return;

	af.field_type = static_cast<field_type_t>(json_get_int(obj, "field_type", 0));
	af.debris_genre = static_cast<debris_genre_t>(json_get_int(obj, "debris_genre", 0));
	af.speed = json_get_float(obj, "speed", 0.0f);

	af.min_bound = json_to_vec3d(json_object_get(obj, "min_bound"));
	af.max_bound = json_to_vec3d(json_object_get(obj, "max_bound"));

	af.has_inner_bound = json_get_bool(obj, "has_inner_bound", false);
	if (af.has_inner_bound) {
		af.inner_min_bound = json_to_vec3d(json_object_get(obj, "inner_min_bound"));
		af.inner_max_bound = json_to_vec3d(json_object_get(obj, "inner_max_bound"));
	}

	af.enhanced_visibility_checks = json_get_bool(obj, "enhanced_visibility_checks", false);

	const json_t* dt = json_object_get(obj, "field_debris_type");
	if (dt && json_is_array(dt)) {
		af.field_debris_type.clear();
		size_t di;
		json_t* dv;
		json_array_foreach(dt, di, dv) {
			af.field_debris_type.push_back(static_cast<int>(json_integer_value(dv)));
		}
	}

	const json_t* at = json_object_get(obj, "field_asteroid_type");
	if (at && json_is_array(at)) {
		af.field_asteroid_type.clear();
		size_t ai;
		json_t* av;
		json_array_foreach(at, ai, av) {
			af.field_asteroid_type.push_back(json_string_value(av));
		}
	}

	const json_t* targets = json_object_get(obj, "target_names");
	if (targets && json_is_array(targets)) {
		af.target_names.clear();
		size_t ti;
		json_t* tv;
		json_array_foreach(targets, ti, tv) {
			af.target_names.push_back(json_string_value(tv));
		}
	}
}

void load_music_json(const json_t* obj, mission* pm)
{
	if (!obj) return;

	strcpy_s(pm->event_music_name, json_get_string(obj, "event_music", ""));
	strcpy_s(pm->briefing_music_name, json_get_string(obj, "briefing_music", ""));
	strcpy_s(pm->substitute_event_music_name, json_get_string(obj, "substitute_event_music", ""));
	strcpy_s(pm->substitute_briefing_music_name, json_get_string(obj, "substitute_briefing_music", ""));
}

void load_custom_data_json(const json_t* obj, mission* pm)
{
	if (!obj) return;

	json_t* data = json_object_get(obj, "data");
	if (data && json_is_object(data)) {
		const char* key;
		json_t* d_val;
		json_object_foreach(data, key, d_val) {
			pm->custom_data[key] = json_string_value(d_val);
		}
	}

	const json_t* strings = json_object_get(obj, "strings");
	if (strings && json_is_array(strings)) {
		size_t index;
		json_t* val;
		json_array_foreach(strings, index, val) {
			custom_string cs;
			cs.name = json_get_string(val, "name", "");
			cs.value = json_get_string(val, "value", "");
			cs.text = json_get_string(val, "text", "");
			pm->custom_strings.push_back(cs);
		}
	}
}

} // anonymous namespace

// ============================================================
// Public load entry point
// ============================================================

bool mission_json::load(const char* pathname, mission* pm, int flags)
{
	CFILE* fp = cfopen(pathname, "rt", CF_TYPE_MISSIONS);
	if (!fp) {
		mprintf(("mission_json::load - Failed to open '%s'\n", pathname));
		return false;
	}

	json_error_t error;
	json_t* root = json_load_cfile(fp, 0, &error);
	cfclose(fp);

	if (!root) {
		mprintf(("mission_json::load - JSON parse error in '%s': %s (line %d)\n", pathname, error.text, error.line));
		return false;
	}

	int version = json_get_int(root, "format_version", 0);
	if (version > FORMAT_VERSION) {
		mprintf(("mission_json::load - File '%s' has format version %d, we only support up to %d\n",
			pathname, version, FORMAT_VERSION));
	}

	reset_parse();
	mission_init(pm, (flags & MPF_ONLY_MISSION_INFO) != 0);

	// Mission info is always loaded
	load_mission_info_json(json_object_get(root, "mission_info"), pm);

	if (flags & MPF_ONLY_MISSION_INFO) {
		json_decref(root);
		return true;
	}

	load_variables_json(json_object_get(root, "variables"));
	load_containers_json(json_object_get(root, "containers"));
	load_cutscenes_json(json_object_get(root, "cutscenes"), pm);
	load_fiction_json(json_object_get(root, "fiction"));
	load_cmd_briefs_json(json_object_get(root, "command_briefings"));
	load_briefing_json(json_object_get(root, "briefing"));
	load_debriefing_json(json_object_get(root, "debriefing"));
	load_players_json(json_object_get(root, "players"));
	load_objects_json(json_object_get(root, "objects"), pm);
	load_wings_json(json_object_get(root, "wings"), pm);
	load_props_json(json_object_get(root, "props"));
	load_events_json(json_object_get(root, "events"));
	load_goals_json(json_object_get(root, "goals"));
	load_waypoints_json(json_object_get(root, "waypoints"));
	load_messages_json(json_object_get(root, "messages"));
	load_reinforcements_json(json_object_get(root, "reinforcements"));
	load_bitmaps_json(json_object_get(root, "background_bitmaps"));
	load_asteroid_fields_json(json_object_get(root, "asteroid_fields"));
	load_music_json(json_object_get(root, "music"), pm);
	load_custom_data_json(json_object_get(root, "custom_data"), pm);

	json_decref(root);

	if (!post_process_mission(pm))
		return false;

	return true;
}
