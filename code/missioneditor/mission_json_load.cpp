/*
 * JSON mission load implementation for FreeSpace Open.
 * Provides an alternate, structured mission format for use by AI agents.
 */

#include "missioneditor/mission_json.h"

// Jansson's json_array_foreach / json_object_foreach macros use assignment
// inside conditionals, which is intentional.  Suppress MSVC C4706 for this file.
#ifdef _MSC_VER
#pragma warning(disable: 4706)
#endif

#include <cstring>

#include "ai/ai.h"
#include "ai/aigoals.h"
#include "ai/ailua.h"
#include "ai/ai_profiles.h"
#include "asteroid/asteroid.h"
#include "cfile/cfile.h"
#include "fireball/fireballs.h"
#include "gamesnd/eventmusic.h"
#include "gamesnd/gamesnd.h"
#include "globalincs/version.h"
#include "hud/hudsquadmsg.h"
#include "iff_defs/iff_defs.h"
#include "globalincs/alphacolors.h"
#include "jumpnode/jumpnode.h"
#include "libs/jansson.h"
#include "math/bitarray.h"
#include "lighting/lighting_profiles.h"
#include "mission/missionbriefcommon.h"
#include "mission/missiongoals.h"
#include "mission/missionmessage.h"
#include "mission/missionparse.h"
#include "mission/mission_flags.h"
#include "missionui/fictionviewer.h"
#include "missionui/missioncmdbrief.h"
#include "model/animation/modelanimation.h"
#include "nebula/neb.h"
#include "nebula/volumetrics.h"
#include "object/waypoint.h"
#include "parse/parselo.h"
#include "parse/sexp.h"
#include "parse/sexp_container.h"
#include "prop/prop.h"
#include "ship/ship.h"
#include "ship/shipfx.h"
#include "sound/ds.h"
#include "starfield/nebula.h"
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

	// All teams attack
	if (json_get_bool(obj, "all_teams_attack", false))
		Mission_all_attack = 1;

	// Player entry delay
	{
		float entry_delay = json_get_float(obj, "player_entry_delay", 0.0f);
		if (entry_delay > 0.0f)
			Entry_delay_time = fl2f(entry_delay);
	}

	// Viewer position and orientation (editor state)
	const json_t* vp = json_object_get(obj, "viewer_pos");
	if (vp)
		Parse_viewer_pos = json_to_vec3d(vp);
	const json_t* vo = json_object_get(obj, "viewer_orient");
	if (vo)
		Parse_viewer_orient = json_to_matrix(vo);

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

	// Volumetric nebula
	const json_t* vol = json_object_get(obj, "volumetric_nebula");
	if (vol) {
		pm->volumetrics.emplace();
		auto& v = *pm->volumetrics;
		v.setHullPof(json_get_string(vol, "hull_pof", ""));

		auto color_to_unit = [](int value) { return static_cast<float>(std::clamp(value, 0, 255)) / 255.0f; };

		float mainR = color_to_unit(json_get_int(vol, "color_r", 0));
		float mainG = color_to_unit(json_get_int(vol, "color_g", 0));
		float mainB = color_to_unit(json_get_int(vol, "color_b", 0));

		std::optional<float> noiseScaleBase, noiseScaleSub;
		std::optional<float> noiseR, noiseG, noiseB;
		std::optional<float> noiseIntensity;
		std::optional<int> noiseRes;
		std::optional<bool> noiseIsActive;

		const json_t* noise = json_object_get(vol, "noise");
		if (noise) {
			noiseIsActive = true;
			noiseScaleBase = json_get_float(noise, "scale_x", 1.0f);
			noiseScaleSub = json_get_float(noise, "scale_y", 1.0f);
			noiseR = color_to_unit(json_get_int(noise, "color_r", 0));
			noiseG = color_to_unit(json_get_int(noise, "color_g", 0));
			noiseB = color_to_unit(json_get_int(noise, "color_b", 0));
			noiseIntensity = json_get_float(noise, "intensity", 1.0f);
			noiseRes = json_get_int(noise, "resolution", 4);

			const char* func1 = json_get_string(noise, "function_base", nullptr);
			if (func1)
				v.setNoiseColorFunc1(func1);
			const char* func2 = json_get_string(noise, "function_sub", nullptr);
			if (func2)
				v.setNoiseColorFunc2(func2);
		}

		const json_t* pos_obj = json_object_get(vol, "position");
		std::optional<vec3d> position;
		if (pos_obj)
			position = json_to_vec3d(pos_obj);

		v.set_runtime_params(
			position,
			json_get_int(vol, "steps", 0),			// 0 means use default
			json_get_int(vol, "global_light_steps", 0),
			json_get_float(vol, "opacity_distance", 0.0f),
			json_get_float(vol, "alpha_lim", 0.0f),
			json_get_float(vol, "emissive_spread", 0.0f),
			json_get_float(vol, "emissive_intensity", 0.0f),
			json_get_float(vol, "emissive_falloff", 0.0f),
			json_get_float(vol, "henyey_greenstein_coeff", 0.0f),
			json_get_float(vol, "global_light_distance_factor", 0.0f),
			noiseIsActive,
			noiseScaleBase,
			noiseScaleSub,
			noiseIntensity,
			mainR, mainG, mainB,
			noiseR, noiseG, noiseB,
			json_get_int(vol, "resolution", 0),
			json_get_int(vol, "oversampling", 0),
			json_get_float(vol, "smoothing", 0.0f),
			noiseRes);
	}

	// Squadron info
	strcpy_s(pm->squad_name, json_get_string(obj, "squad_name", ""));
	strcpy_s(pm->squad_filename, json_get_string(obj, "squad_filename", ""));

	// Loading screens
	strcpy_s(pm->loading_screen[GR_640], json_get_string(obj, "loading_screen_640", ""));
	strcpy_s(pm->loading_screen[GR_1024], json_get_string(obj, "loading_screen_1024", ""));

	// Skybox
	strcpy_s(pm->skybox_model, json_get_string(obj, "skybox_model", ""));

	// Skybox model animations
	const json_t* sky_anims = json_object_get(obj, "skybox_model_animations");
	if (sky_anims && json_is_array(sky_anims)) {
		SCP_vector<SCP_string> animNames;
		size_t idx2;
		json_t* val2;
		json_array_foreach(sky_anims, idx2, val2) {
			if (json_is_string(val2))
				animNames.emplace_back(json_string_value(val2));
		}
		animation::ModelAnimationParseHelper::loadAnimsetInfo(pm->skybox_model_animations, 'b', pm->name, animNames);
	}

	// Skybox model moveables
	const json_t* sky_moveables = json_object_get(obj, "skybox_model_moveables");
	if (sky_moveables && json_is_array(sky_moveables)) {
		SCP_vector<SCP_string> moveableNames;
		size_t idx2;
		json_t* val2;
		json_array_foreach(sky_moveables, idx2, val2) {
			if (json_is_string(val2))
				moveableNames.emplace_back(json_string_value(val2));
		}
		animation::ModelAnimationParseHelper::loadMoveablesetInfo(pm->skybox_model_animations, moveableNames);
	}

	const json_t* sky_orient = json_object_get(obj, "skybox_orientation");
	if (sky_orient)
		pm->skybox_orientation = json_to_matrix(sky_orient);
	pm->skybox_flags = json_get_int(obj, "skybox_flags", DEFAULT_NMODEL_FLAGS);

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
		strcpy_s(b.ship_select_background[GR_640], json_get_string(obj, "ship_select_bg_640", ""));
		strcpy_s(b.ship_select_background[GR_1024], json_get_string(obj, "ship_select_bg_1024", ""));
		strcpy_s(b.weapon_select_background[GR_640], json_get_string(obj, "weapon_select_bg_640", ""));
		strcpy_s(b.weapon_select_background[GR_1024], json_get_string(obj, "weapon_select_bg_1024", ""));

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

				// Grid color
				const json_t* gc = json_object_get(s, "grid_color");
				if (gc && json_is_object(gc)) {
					gr_init_color(&bs.grid_color,
						json_get_int(gc, "red", 0),
						json_get_int(gc, "green", 0),
						json_get_int(gc, "blue", 0));
					bs.grid_color.alpha = static_cast<ubyte>(json_get_int(gc, "alpha", 255));
				} else {
					bs.grid_color = Color_briefing_grid;
				}

				const json_t* icons = json_object_get(s, "icons");
				if (icons && json_is_array(icons)) {
					bs.num_icons = static_cast<int>(json_array_size(icons));
					bs.icons = new brief_icon[bs.num_icons];
					for (int j = 0; j < bs.num_icons; j++) {
						const json_t* ic = json_array_get(icons, j);
						memset(&bs.icons[j], 0, sizeof(brief_icon));
						bs.icons[j].type = json_get_int(ic, "type", 0);

						// Team — saved as IFF name string
						const char* team_name = json_get_string(ic, "team", nullptr);
						if (team_name)
							bs.icons[j].team = iff_lookup(team_name);
						else
							bs.icons[j].team = 0;

						bs.icons[j].id = json_get_int(ic, "id", 0);
						bs.icons[j].pos = json_to_vec3d(json_object_get(ic, "pos"));

						// Icon flags — saved as individual booleans
						bs.icons[j].flags = 0;
						if (json_get_bool(ic, "highlight", false))
							bs.icons[j].flags |= BI_HIGHLIGHT;
						if (json_get_bool(ic, "mirror", false))
							bs.icons[j].flags |= BI_MIRROR_ICON;
						if (json_get_bool(ic, "use_wing_icon", false))
							bs.icons[j].flags |= BI_USE_WING_ICON;
						if (json_get_bool(ic, "use_cargo_icon", false))
							bs.icons[j].flags |= BI_USE_CARGO_ICON;

						// Icon scale
						int icon_scale_pct = json_get_int(ic, "icon_scale", 100);
						bs.icons[j].scale_factor = icon_scale_pct / 100.0f;

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

void load_players_json(const json_t* obj)
{
	if (!obj || !json_is_object(obj))
		return;

	// Alternate types list
	const json_t* alt_arr = json_object_get(obj, "alternate_types");
	if (alt_arr && json_is_array(alt_arr)) {
		size_t ai;
		json_t* av;
		json_array_foreach(alt_arr, ai, av) {
			const char* name = json_string_value(av);
			if (name)
				mission_parse_add_alt(name);
		}
	}

	// Callsigns list
	const json_t* cs_arr = json_object_get(obj, "callsigns");
	if (cs_arr && json_is_array(cs_arr)) {
		size_t ci;
		json_t* cv;
		json_array_foreach(cs_arr, ci, cv) {
			const char* name = json_string_value(cv);
			if (name)
				mission_parse_add_callsign(name);
		}
	}

	// General orders
	ai_lua_reset_general_orders();

	const json_t* orders_enabled = json_object_get(obj, "general_orders_enabled");
	if (orders_enabled && json_is_array(orders_enabled)) {
		size_t oi;
		json_t* ov;
		json_array_foreach(orders_enabled, oi, ov) {
			const char* name = json_string_value(ov);
			if (name) {
				int lua_order_id = ai_lua_find_general_order_id(name);
				if (lua_order_id >= 0)
					ai_lua_enable_general_order(lua_order_id, true);
			}
		}
	}

	const json_t* orders_valid = json_object_get(obj, "general_orders_valid");
	if (orders_valid && json_is_array(orders_valid)) {
		size_t oi;
		json_t* ov;
		json_array_foreach(orders_valid, oi, ov) {
			const char* name = json_string_value(ov);
			if (name) {
				int lua_order_id = ai_lua_find_general_order_id(name);
				if (lua_order_id >= 0)
					ai_lua_validate_general_order(lua_order_id, true);
			}
		}
	}

	// Teams
	const json_t* teams_arr = json_object_get(obj, "teams");
	if (!teams_arr || !json_is_array(teams_arr))
		return;

	Num_teams = static_cast<int>(std::min(json_array_size(teams_arr), static_cast<size_t>(MAX_TVT_TEAMS)));

	for (int t = 0; t < Num_teams; t++) {
		const json_t* team = json_array_get(teams_arr, t);
		team_data& td = Team_data[t];

		// Starting shipname (saved for all teams now)
		const char* ssn = json_get_string(team, "starting_shipname", nullptr);
		if (ssn)
			strcpy_s(Player_start_shipname, ssn);

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

				const char* count_var = json_get_string(entry, "count_variable", nullptr);
				if (count_var) {
					strcpy_s(td.ship_count_variables[i], count_var);
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

				const char* count_var = json_get_string(entry, "count_variable", nullptr);
				if (count_var) {
					strcpy_s(td.weaponry_amount_variable[i], count_var);
					td.weaponry_count[i] = 0;
				} else {
					td.weaponry_count[i] = json_get_int(entry, "count", 0);
					td.weaponry_amount_variable[i][0] = '\0';
				}
			}
		}

		// Required weapons
		const json_t* req_arr = json_object_get(team, "required_weapons");
		if (req_arr && json_is_array(req_arr)) {
			size_t ri;
			json_t* rv;
			json_array_foreach(req_arr, ri, rv) {
				const char* wname = json_string_value(rv);
				if (wname) {
					int wi = weapon_info_lookup(wname);
					if (wi >= 0)
						td.weapon_required[wi] = true;
				}
			}
		}
	}
}

// Load warp parameters from a JSON object, starting from defaults inherited from the ship class.
// Returns the warp params index, or -1 if no custom params were specified.
int load_warp_params_json(const json_t* obj, WarpDirection direction, int default_params_index)
{
	if (!obj || !json_is_object(obj) || json_object_size(obj) == 0)
		return -1;

	// Start with a copy of the defaults
	WarpParams params = Warp_params[default_params_index];
	params.direction = direction;

	// Warp type
	const char* type_str = json_get_string(obj, "type", nullptr);
	if (type_str) {
		// Check for fireball type first
		int fireball_idx = -1;
		for (int i = 0; i < static_cast<int>(Fireball_info.size()); i++) {
			if (!stricmp(type_str, Fireball_info[i].unique_id)) {
				fireball_idx = i;
				break;
			}
		}
		if (fireball_idx >= 0) {
			params.warp_type = fireball_idx | WT_DEFAULT_WITH_FIREBALL;
		} else {
			int wt = warptype_match(type_str);
			if (wt >= 0)
				params.warp_type = wt;
		}
	}

	// Sounds
	const char* start_snd = json_get_string(obj, "start_sound", nullptr);
	if (start_snd)
		params.snd_start = gamesnd_get_by_name(start_snd);

	const char* end_snd = json_get_string(obj, "end_sound", nullptr);
	if (end_snd)
		params.snd_end = gamesnd_get_by_name(end_snd);

	// Engage time (warpout only)
	const json_t* engage = json_object_get(obj, "engage_time");
	if (engage && json_is_real(engage))
		params.warpout_engage_time = fl2i(static_cast<float>(json_real_value(engage)) * 1000.0f);

	// Speed
	const json_t* speed = json_object_get(obj, "speed");
	if (speed && json_is_real(speed))
		params.speed = static_cast<float>(json_real_value(speed));

	// Time
	const json_t* time_val = json_object_get(obj, "time");
	if (time_val && json_is_real(time_val))
		params.time = fl2i(static_cast<float>(json_real_value(time_val)) * 1000.0f);

	// Accel/decel exponent (note: save uses "decel_exp" for warp-in, "accel_exp" for warp-out, both stored in accel_exp)
	const json_t* accel = json_object_get(obj, "accel_exp");
	if (accel && json_is_real(accel))
		params.accel_exp = static_cast<float>(json_real_value(accel));

	const json_t* decel = json_object_get(obj, "decel_exp");
	if (decel && json_is_real(decel))
		params.accel_exp = static_cast<float>(json_real_value(decel));

	// Radius
	const json_t* radius = json_object_get(obj, "radius");
	if (radius && json_is_real(radius))
		params.radius = static_cast<float>(json_real_value(radius));

	// Animation
	const char* anim = json_get_string(obj, "animation", nullptr);
	if (anim)
		strcpy_s(params.anim, anim);

	// Supercap warp physics
	const json_t* supercap = json_object_get(obj, "supercap_warp_physics");
	if (supercap && json_is_boolean(supercap))
		params.supercap_warp_physics = json_is_true(supercap);

	// Player warpout speed (warpout only)
	const json_t* player_speed = json_object_get(obj, "player_warpout_speed");
	if (player_speed && json_is_real(player_speed))
		params.warpout_player_speed = static_cast<float>(json_real_value(player_speed));

	return find_or_add_warp_params(params);
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

		// If name has a hash, create a default display name (mirroring standard parser)
		if (get_pointer_to_first_hash_symbol(po.name)) {
			po.display_name = po.name;
			end_string_at_first_hash_symbol(po.display_name);
			po.flags.set(Mission::Parse_Object_Flags::SF_Has_display_name);
		}

		// Explicit display name overrides the hash-based default
		const char* display = json_get_string(val, "display_name", nullptr);
		if (display) {
			po.display_name = display;
			po.flags.set(Mission::Parse_Object_Flags::SF_Has_display_name);
		}

		const char* sc_name = json_get_string(val, "ship_class", "");
		po.ship_class = ship_info_lookup(sc_name);

		// Initialize class-specific defaults (mirroring parse_object in missionparse.cpp)
		if (po.ship_class >= 0) {
			ship_info* sip = &Ship_info[po.ship_class];
			po.ai_class = sip->ai_class;
			po.warpin_params_index = sip->warpin_params_index;
			po.warpout_params_index = sip->warpout_params_index;
			po.ship_max_shield_strength = sip->max_shield_strength;
			po.ship_max_hull_strength = sip->max_hull_strength;
			po.max_shield_recharge = sip->max_shield_recharge;
			po.replacement_textures = sip->replacement_textures;
		}

		const char* team_name = json_get_string(val, "team", "");
		po.team = iff_lookup(team_name);
		po.loadout_team = po.team;

		const char* tcs = json_get_string(val, "team_color_setting", nullptr);
		if (tcs) po.team_color_setting = tcs;

		// Alt name and callsign (indexes into Mission_alt_types/Mission_callsigns arrays)
		const char* alt_name = json_get_string(val, "alt", nullptr);
		if (alt_name) {
			po.alt_type_index = mission_parse_lookup_alt(alt_name);
			if (po.alt_type_index < 0)
				Warning(LOCATION, "Error looking up alternate ship type name %s!", alt_name);
		}

		const char* callsign_name = json_get_string(val, "callsign", nullptr);
		if (callsign_name) {
			po.callsign_index = mission_parse_lookup_callsign(callsign_name);
			if (po.callsign_index < 0)
				Warning(LOCATION, "Error looking up callsign %s!", callsign_name);
		}

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

		const char* cargo_title = json_get_string(val, "cargo_title", nullptr);
		if (cargo_title)
			strcpy_s(po.cargo_title, cargo_title);

		po.initial_velocity = json_get_int(val, "initial_velocity", 0);
		po.initial_hull = json_get_int(val, "initial_hull", 100);
		po.initial_shields = json_get_int(val, "initial_shields", 100);

		// Arrival / Departure
		po.arrival_location = parse_arrival_location(json_get_string(val, "arrival_location", "At Location"));
		po.arrival_distance = json_get_int(val, "arrival_distance", 0);

		po.arrival_anchor = json_to_anchor(val, "arrival_anchor");
		po.arrival_cue = json_to_sexp(json_object_get(val, "arrival_cue"));
		{
			int delay = json_get_int(val, "arrival_delay", 0);
			if (!Fred_running)
				po.arrival_delay = -delay;		// use negative numbers to mean we haven't set up a timer yet
			else
				po.arrival_delay = delay;
		}
		po.arrival_path_mask = json_get_int(val, "arrival_path_mask", 0);

		po.departure_location = parse_departure_location(json_get_string(val, "departure_location", "At Location"));
		po.departure_anchor = json_to_anchor(val, "departure_anchor");
		po.departure_cue = json_to_sexp(json_object_get(val, "departure_cue"));
		{
			int delay = json_get_int(val, "departure_delay", 0);
			if (!Fred_running)
				po.departure_delay = -delay;	// use negative numbers to mean that delay timer not yet set
			else
				po.departure_delay = delay;
		}
		po.departure_path_mask = json_get_int(val, "departure_path_mask", 0);

		// Warp parameters
		{
			int warpin_idx = load_warp_params_json(json_object_get(val, "warpin_params"), WarpDirection::WARP_IN, po.warpin_params_index);
			if (warpin_idx >= 0)
				po.warpin_params_index = warpin_idx;

			int warpout_idx = load_warp_params_json(json_object_get(val, "warpout_params"), WarpDirection::WARP_OUT, po.warpout_params_index);
			if (warpout_idx >= 0)
				po.warpout_params_index = warpout_idx;
		}

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

		// Score — if use_table_score is set, use the ship class table default
		if (json_get_bool(val, "use_table_score", false) && po.ship_class >= 0) {
			po.score = Ship_info[po.ship_class].score;
		} else {
			po.score = json_get_int(val, "score", 0);
		}
		po.assist_score_pct = json_get_float(val, "assist_score_pct", 0.0f);

		const char* persona_name = json_get_string(val, "persona", nullptr);
		if (persona_name)
			po.persona_index = find_persona_index(persona_name);

		po.kamikaze_damage = json_get_int(val, "kamikaze_damage", 0);
		po.group = json_get_int(val, "group", -1);
		po.respawn_priority = json_get_int(val, "respawn_priority", 0);

		// Guardian threshold
		po.ship_guardian_threshold = json_get_int(val, "guardian_threshold", 0);

		// Destroy before mission time
		int destroy_at = json_get_int(val, "destroy_at", -1);
		if (destroy_at >= 0)
			po.destroy_before_mission_time = destroy_at;

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

		// Orders accepted (saved as string names via Player_orders[].parse_name)
		const json_t* orders = json_object_get(val, "orders_accepted");
		if (orders && json_is_array(orders)) {
			size_t oi;
			json_t* ov;
			json_array_foreach(orders, oi, ov) {
				const char* order_name = json_string_value(ov);
				if (!order_name) continue;

				for (size_t order_id = 0; order_id < Player_orders.size(); order_id++) {
					if (!stricmp(order_name, Player_orders[order_id].parse_name.c_str())) {
						po.orders_accepted.insert(order_id);
						break;
					}
				}
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
				ssp->percent = json_get_float(sv, "damage", 0.0f);

				// Subsystem cargo
				const char* subsys_cargo = json_get_string(sv, "cargo_name", nullptr);
				if (subsys_cargo) {
					int ci;
					for (ci = 0; ci < Num_cargo; ci++) {
						if (!stricmp(Cargo_names[ci], subsys_cargo))
							break;
					}
					if (ci == Num_cargo && Num_cargo < MAX_CARGO) {
						strcpy_s(Cargo_names_buf[Num_cargo], subsys_cargo);
						Cargo_names[Num_cargo] = Cargo_names_buf[Num_cargo];
						Num_cargo++;
					}
					ssp->subsys_cargo_name = ci;
				}

				const char* subsys_cargo_title = json_get_string(sv, "cargo_title", nullptr);
				if (subsys_cargo_title)
					strcpy_s(ssp->subsys_cargo_title, subsys_cargo_title);

				// Turret AI class
				const char* subsys_ai_class = json_get_string(sv, "ai_class", nullptr);
				if (subsys_ai_class)
					ssp->ai_class = find_ai_class(subsys_ai_class);

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
		const char* formation_name = json_get_string(val, "formation", nullptr);
		if (formation_name)
			w.formation = wing_formation_lookup(formation_name);
		else
			w.formation = -1;
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
		{
			int delay = json_get_int(val, "arrival_delay", 0);
			if (!Fred_running)
				w.arrival_delay = -delay;		// use negative numbers to mean we haven't set up a timer yet
			else
				w.arrival_delay = delay;
		}
		w.arrival_path_mask = json_get_int(val, "arrival_path_mask", 0);

		w.departure_location = parse_departure_location(json_get_string(val, "departure_location", "At Location"));
		w.departure_anchor = json_to_anchor(val, "departure_anchor");
		w.departure_cue = json_to_sexp(json_object_get(val, "departure_cue"));
		{
			int delay = json_get_int(val, "departure_delay", 0);
			if (!Fred_running)
				w.departure_delay = -delay;		// use negative numbers to mean that delay timer not yet set
			else
				w.departure_delay = delay;
		}
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

		// annotations are only used in FRED
		const json_t* ann_arr = json_object_get(val, "annotations");
		if (Fred_running && ann_arr && json_is_array(ann_arr)) {
			size_t ai;
			json_t* av;
			json_array_foreach(ann_arr, ai, av) {
				event_annotation ea;

				const char* comment = json_get_string(av, "comment", nullptr);
				if (comment)
					ea.comment = comment;

				const json_t* bg_color = json_object_get(av, "background_color");
				if (bg_color && json_is_object(bg_color)) {
					ea.r = static_cast<ubyte>(json_get_int(bg_color, "r", 255));
					ea.g = static_cast<ubyte>(json_get_int(bg_color, "g", 255));
					ea.b = static_cast<ubyte>(json_get_int(bg_color, "b", 255));
				}

				// Path: prepend event index, then append saved path elements
				ea.path.push_back(static_cast<int>(index));
				const json_t* path = json_object_get(av, "path");
				if (path && json_is_array(path)) {
					size_t pi;
					json_t* pv;
					json_array_foreach(path, pi, pv) {
						ea.path.push_back(static_cast<int>(json_integer_value(pv)));
					}
				}

				Event_annotations.push_back(std::move(ea));
			}
		}
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

			// Display name
			const char* display_name = json_get_string(val, "display_name", nullptr);
			if (display_name)
				jn.SetDisplayName(display_name);

			// Model file
			const char* model_file = json_get_string(val, "model_file", nullptr);
			if (model_file)
				jn.SetModel(model_file);

			// Alpha color
			const json_t* color_obj = json_object_get(val, "alphacolor");
			if (color_obj && json_is_object(color_obj)) {
				int r = json_get_int(color_obj, "red", 0);
				int g = json_get_int(color_obj, "green", 255);
				int b = json_get_int(color_obj, "blue", 0);
				int a = json_get_int(color_obj, "alpha", 255);
				jn.SetAlphaColor(r, g, b, a);
			}

			// Hidden
			if (json_get_bool(val, "hidden", false))
				jn.SetVisibility(false);

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

void load_messages_json(const json_t* obj)
{
	if (!obj || !json_is_object(obj))
		return;

	// Command sender and persona (mission-wide, moved here from mission_info)
	const char* cmd_sender = json_get_string(obj, "command_sender", nullptr);
	if (cmd_sender)
		strcpy_s(The_mission.command_sender, cmd_sender);

	const char* cmd_persona = json_get_string(obj, "command_persona", nullptr);
	if (cmd_persona) {
		int idx = find_persona_index(cmd_persona);
		if (idx >= 0)
			The_mission.command_persona = idx;
	}

	// Message list
	const json_t* arr = json_object_get(obj, "messages");
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

		// Team field (saved for all messages)
		msg.multi_team = json_get_int(val, "team", -1);

		strcpy_s(msg.message, json_get_string(val, "message", ""));

		const char* persona = json_get_string(val, "persona", nullptr);
		if (persona)
			msg.persona_index = find_persona_index(persona);

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

	Num_stars = json_get_int(obj, "num_stars", 0);
	The_mission.ambient_light_level = json_get_int(obj, "ambient_light_level", 0);

	// Neb2 settings (when Fullneb flag is set)
	if (The_mission.flags[Mission::Mission_Flags::Fullneb]) {
		const char* neb2_tex = json_get_string(obj, "neb2_texture", nullptr);
		if (neb2_tex)
			strcpy_s(Neb2_texture_name, neb2_tex);

		const json_t* neb_color = json_object_get(obj, "neb2_color");
		if (neb_color && json_is_object(neb_color)) {
			Neb2_fog_color[0] = static_cast<ubyte>(json_get_int(neb_color, "r", 0));
			Neb2_fog_color[1] = static_cast<ubyte>(json_get_int(neb_color, "g", 0));
			Neb2_fog_color[2] = static_cast<ubyte>(json_get_int(neb_color, "b", 0));
			The_mission.flags.set(Mission::Mission_Flags::Neb2_fog_color_override);
		}

		const json_t* poofs = json_object_get(obj, "neb2_poofs");
		if (poofs && json_is_array(poofs)) {
			size_t pi;
			json_t* pv;
			json_array_foreach(poofs, pi, pv) {
				const char* poof_name = json_string_value(pv);
				if (poof_name) {
					for (size_t k = 0; k < Poof_info.size(); k++) {
						if (!stricmp(poof_name, Poof_info[k].name)) {
							set_bit(Neb2_poof_flags.get(), k);
							break;
						}
					}
				}
			}
		}
		// initialize neb effect. its gross to do this here, but Fred is dumb so I have no choice ... :(
		if (Fred_running)
			neb2_post_level_init(The_mission.flags[Mission::Mission_Flags::Neb2_fog_color_override]);
	}
	// Legacy nebula (neb1)
	else {
		const char* nebula = json_get_string(obj, "nebula", nullptr);
		if (nebula) {
			for (int i = 0; i < NUM_NEBULAS; i++) {
				if (!stricmp(nebula, Nebula_filenames[i])) {
					Nebula_index = i;
					break;
				}
			}

			const char* neb_color_name = json_get_string(obj, "nebula_color", nullptr);
			if (neb_color_name) {
				for (int i = 0; i < NUM_NEBULA_COLORS; i++) {
					if (!stricmp(neb_color_name, Nebula_colors[i])) {
						Mission_palette = i;
						break;
					}
				}
			}

			Nebula_pitch = json_get_int(obj, "nebula_pitch", 0);
			Nebula_bank = json_get_int(obj, "nebula_bank", 0);
			Nebula_heading = json_get_int(obj, "nebula_heading", 0);
		}
	}

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

			// Background flags
			const json_t* bf = json_object_get(val, "flags");
			if (bf && json_is_array(bf)) {
				size_t fi;
				json_t* fv;
				json_array_foreach(bf, fi, fv) {
					const char* flag_name = json_string_value(fv);
					if (flag_name && !stricmp(flag_name, "corrected angles"))
						bg.flags.set(Starfield::Background_Flags::Corrected_angles_in_mission_file);
				}
			}

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
					sle.scale_y = sle.scale_x;  // suns only have scale_x
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
	strcpy_s(pm->substitute_event_music_name, json_get_string(obj, "substitute_event_music", "None"));
	strcpy_s(pm->substitute_briefing_music_name, json_get_string(obj, "substitute_briefing_music", "None"));

	// Debriefing music
	const char* debrief_success = json_get_string(obj, "debriefing_success_music", nullptr);
	if (debrief_success)
		Mission_music[SCORE_DEBRIEFING_SUCCESS] = event_music_get_spooled_music_index(debrief_success);

	const char* debrief_avg = json_get_string(obj, "debriefing_average_music", nullptr);
	if (debrief_avg)
		Mission_music[SCORE_DEBRIEFING_AVERAGE] = event_music_get_spooled_music_index(debrief_avg);

	const char* debrief_fail = json_get_string(obj, "debriefing_fail_music", nullptr);
	if (debrief_fail)
		Mission_music[SCORE_DEBRIEFING_FAILURE] = event_music_get_spooled_music_index(debrief_fail);

	// Fiction viewer music
	const char* fiction_music = json_get_string(obj, "fiction_viewer_music", nullptr);
	if (fiction_music)
		Mission_music[SCORE_FICTION_VIEWER] = event_music_get_spooled_music_index(fiction_music);
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
