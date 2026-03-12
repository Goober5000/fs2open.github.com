/*
 * JSON mission save implementation for FreeSpace Open.
 * Provides an alternate, structured mission format for use by AI agents.
 */

#include "missioneditor/mission_json.h"
#include "missioneditor/missionsave.h"

#include <ctime>

#include "ai/ai.h"
#include "ai/ai_flags.h"
#include "ai/aigoals.h"
#include "ai/ailua.h"
#include "asteroid/asteroid.h"
#include "cfile/cfile.h"
#include "fireball/fireballs.h"
#include "gamesnd/eventmusic.h"
#include "globalincs/alphacolors.h"
#include "graphics/2d.h"
#include "gamesnd/gamesnd.h"
#include "globalincs/version.h"
#include "hud/hudsquadmsg.h"
#include "iff_defs/iff_defs.h"
#include "jumpnode/jumpnode.h"
#include "libs/jansson.h"
#include "lighting/lighting_profiles.h"
#include "mission/missionbriefcommon.h"
#include "model/model.h"
#include "mission/missiongoals.h"
#include "mission/missionmessage.h"
#include "mission/missionparse.h"
#include "mission/mission_flags.h"
#include "missionui/fictionviewer.h"
#include "missionui/missioncmdbrief.h"
#include "nebula/neb.h"
#include "nebula/volumetrics.h"
#include "object/object.h"
#include "object/objectdock.h"
#include "object/objectshield.h"
#include "object/waypoint.h"
#include "parse/parselo.h"
#include "parse/sexp.h"
#include "parse/sexp_container.h"
#include "prop/prop.h"
#include "ship/ship.h"
#include "ship/ship_flags.h"
#include "ship/shipfx.h"
#include "sound/ds.h"
#include "starfield/starfield.h"
#include "weapon/weapon.h"

// ============================================================
// Helper implementations
// ============================================================

json_t* mission_json::vec3d_to_json(const vec3d& v)
{
	json_t* obj = json_object();
	json_object_set_new(obj, "x", json_real(v.xyz.x));
	json_object_set_new(obj, "y", json_real(v.xyz.y));
	json_object_set_new(obj, "z", json_real(v.xyz.z));
	return obj;
}

json_t* mission_json::matrix_to_json(const matrix& m)
{
	json_t* obj = json_object();
	json_object_set_new(obj, "fvec", vec3d_to_json(m.vec.fvec));
	json_object_set_new(obj, "rvec", vec3d_to_json(m.vec.rvec));
	json_object_set_new(obj, "uvec", vec3d_to_json(m.vec.uvec));
	return obj;
}

json_t* mission_json::angles_to_json(const angles& a)
{
	json_t* obj = json_object();
	json_object_set_new(obj, "p", json_real(a.p));
	json_object_set_new(obj, "b", json_real(a.b));
	json_object_set_new(obj, "h", json_real(a.h));
	return obj;
}

json_t* mission_json::sexp_to_json(int node_index)
{
	if (node_index < 0)
		return json_null();

	SCP_string sexp_str;
	convert_sexp_to_string(sexp_str, node_index, SEXP_SAVE_MODE);
	return json_string(sexp_str.c_str());
}

// Extern declarations needed for flag tables defined in missionparse.cpp
extern flag_def_list_new<Mission::Parse_Object_Flags> Parse_object_flags[];
extern const size_t Num_parse_object_flags;
extern flag_def_list_new<Mission::Mission_Flags> Parse_mission_flags[];
extern const size_t Num_parse_mission_flags;
extern int Num_builtin_messages;
extern SCP_vector<parsed_prop> Parse_props;

// ============================================================
// Internal save helpers (anonymous namespace)
// ============================================================
namespace {

// File-scope pointer to the save configuration, set at the start of mission_json::save().
static const FredSaveConfig* json_save_config = nullptr;

const char* arrival_location_name(ArrivalLocation loc)
{
	switch (loc) {
		case ArrivalLocation::AT_LOCATION:       return "At Location";
		case ArrivalLocation::NEAR_SHIP:         return "Near Ship";
		case ArrivalLocation::IN_FRONT_OF_SHIP:  return "In Front of Ship";
		case ArrivalLocation::IN_BACK_OF_SHIP:   return "In Back of Ship";
		case ArrivalLocation::ABOVE_SHIP:        return "Above Ship";
		case ArrivalLocation::BELOW_SHIP:        return "Below Ship";
		case ArrivalLocation::TO_LEFT_OF_SHIP:   return "To Left of Ship";
		case ArrivalLocation::TO_RIGHT_OF_SHIP:  return "To Right of Ship";
		case ArrivalLocation::FROM_DOCK_BAY:     return "From Dock Bay";
		default: return "At Location";
	}
}

const char* departure_location_name(DepartureLocation loc)
{
	switch (loc) {
		case DepartureLocation::AT_LOCATION: return "At Location";
		case DepartureLocation::TO_DOCK_BAY: return "To Dock Bay";
		default: return "At Location";
	}
}

json_t* anchor_to_json(int anchor)
{
	if (anchor < 0)
		return json_null();

	if (anchor & SPECIAL_ARRIVAL_ANCHOR_FLAG) {
		int iff_index = anchor & ~SPECIAL_ARRIVAL_ANCHOR_FLAG & ~SPECIAL_ARRIVAL_ANCHOR_PLAYER_FLAG;
		bool restrict_to_players = (anchor & SPECIAL_ARRIVAL_ANCHOR_PLAYER_FLAG) != 0;

		if (iff_index >= 0 && iff_index < static_cast<int>(Iff_info.size())) {
			char tmp[NAME_LENGTH + 15];
			if (restrict_to_players)
				sprintf(tmp, "<any %s player>", Iff_info[iff_index].iff_name);
			else
				sprintf(tmp, "<any %s>", Iff_info[iff_index].iff_name);
			strlwr(tmp);
			return json_string(tmp);
		}
		return json_null();
	}

	if (anchor >= 0 && anchor < MAX_SHIPS && Ships[anchor].objnum >= 0)
		return json_string(Ships[anchor].ship_name);

	return json_null();
}

json_t* save_parse_object_flags_json(const flagset<Mission::Parse_Object_Flags>& flags)
{
	json_t* arr = json_array();

	for (size_t i = 0; i < Num_parse_object_flags; i++) {
		if (flags[Parse_object_flags[i].def]) {
			json_array_append_new(arr, json_string(Parse_object_flags[i].name));
		}
	}
	return arr;
}

json_t* save_mission_flags_json(const flagset<Mission::Mission_Flags>& flags)
{
	json_t* arr = json_array();

	for (size_t i = 0; i < Num_parse_mission_flags; i++) {
		if (Parse_mission_flags[i].in_use && flags[Parse_mission_flags[i].def]) {
			json_array_append_new(arr, json_string(Parse_mission_flags[i].name));
		}
	}
	return arr;
}

json_t* save_ai_goals_json(ai_goal* goalp)
{
	json_t* arr = json_array();

	for (int i = 0; i < MAX_AI_GOALS; i++) {
		if (goalp[i].ai_mode == AI_GOAL_NONE)
			continue;

		json_t* goal = json_object();
		const char* mode_str = nullptr;

		switch (goalp[i].ai_mode) {
			case AI_GOAL_CHASE:               mode_str = "ai-chase"; break;
			case AI_GOAL_CHASE_WING:          mode_str = "ai-chase-wing"; break;
			case AI_GOAL_CHASE_SHIP_CLASS:    mode_str = "ai-chase-ship-class"; break;
			case AI_GOAL_CHASE_SHIP_TYPE:     mode_str = "ai-chase-ship-type"; break;
			case AI_GOAL_CHASE_ANY:           mode_str = "ai-chase-any"; break;
			case AI_GOAL_GUARD:               mode_str = "ai-guard"; break;
			case AI_GOAL_GUARD_WING:          mode_str = "ai-guard-wing"; break;
			case AI_GOAL_WAYPOINTS:           mode_str = "ai-waypoints"; break;
			case AI_GOAL_WAYPOINTS_ONCE:      mode_str = "ai-waypoints-once"; break;
			case AI_GOAL_DOCK:                mode_str = "ai-dock"; break;
			case AI_GOAL_UNDOCK:              mode_str = "ai-undock"; break;
			case AI_GOAL_DESTROY_SUBSYSTEM:   mode_str = "ai-destroy-subsystem"; break;
			case AI_GOAL_DISABLE_SHIP:        mode_str = "ai-disable-ship"; break;
			case AI_GOAL_DISABLE_SHIP_TACTICAL: mode_str = "ai-disable-ship-tactical"; break;
			case AI_GOAL_DISARM_SHIP:         mode_str = "ai-disarm-ship"; break;
			case AI_GOAL_DISARM_SHIP_TACTICAL: mode_str = "ai-disarm-ship-tactical"; break;
			case AI_GOAL_IGNORE:              mode_str = "ai-ignore"; break;
			case AI_GOAL_IGNORE_NEW:          mode_str = "ai-ignore-new"; break;
			case AI_GOAL_EVADE_SHIP:          mode_str = "ai-evade-ship"; break;
			case AI_GOAL_STAY_NEAR_SHIP:      mode_str = "ai-stay-near-ship"; break;
			case AI_GOAL_KEEP_SAFE_DISTANCE:  mode_str = "ai-keep-safe-distance"; break;
			case AI_GOAL_FORM_ON_WING:        mode_str = "ai-form-on-wing"; break;
			case AI_GOAL_STAY_STILL:          mode_str = "ai-stay-still"; break;
			case AI_GOAL_PLAY_DEAD:           mode_str = "ai-play-dead"; break;
			case AI_GOAL_PLAY_DEAD_PERSISTENT: mode_str = "ai-play-dead-persistent"; break;
			case AI_GOAL_WARP:                mode_str = "ai-warp-out"; break;
			case AI_GOAL_REARM_REPAIR:        mode_str = "ai-rearm-repair"; break;
			case AI_GOAL_FLY_TO_SHIP:         mode_str = "ai-fly-to-ship"; break;
			default: mode_str = "unknown"; break;
		}

		json_object_set_new(goal, "mode", json_string(mode_str));
		json_object_set_new(goal, "priority", json_integer(goalp[i].priority));

		if (goalp[i].target_name)
			json_object_set_new(goal, "target_name", json_string(goalp[i].target_name));

		if (goalp[i].ai_mode == AI_GOAL_DOCK) {
			if (goalp[i].docker.name)
				json_object_set_new(goal, "docker_point", json_string(goalp[i].docker.name));
			if (goalp[i].dockee.name)
				json_object_set_new(goal, "dockee_point", json_string(goalp[i].dockee.name));
		}

		if (goalp[i].ai_mode == AI_GOAL_DESTROY_SUBSYSTEM && goalp[i].docker.name) {
			json_object_set_new(goal, "subsystem", json_string(goalp[i].docker.name));
		}

		json_array_append_new(arr, goal);
	}

	return arr;
}

// --- Section save functions ---

json_t* save_mission_info_json()
{
	json_t* obj = json_object();

	// Version
	json_object_set_new(obj, "version_major", json_integer(The_mission.required_fso_version.major));
	json_object_set_new(obj, "version_minor", json_integer(The_mission.required_fso_version.minor));
	json_object_set_new(obj, "version_build", json_integer(The_mission.required_fso_version.build));
	json_object_set_new(obj, "version_revision", json_integer(The_mission.required_fso_version.revision));

	// Basic metadata
	json_object_set_new(obj, "name", json_string(The_mission.name));
	json_object_set_new(obj, "author", json_string(The_mission.author.c_str()));
	json_object_set_new(obj, "created", json_string(The_mission.created));
	json_object_set_new(obj, "modified", json_string(The_mission.modified));
	json_object_set_new(obj, "notes", json_string(The_mission.notes));
	json_object_set_new(obj, "mission_desc", json_string(The_mission.mission_desc));

	// Game type and flags
	json_object_set_new(obj, "game_type", json_integer(The_mission.game_type));
	json_object_set_new(obj, "flags", save_mission_flags_json(The_mission.flags));

	// Nebula settings
	if (The_mission.flags[Mission::Mission_Flags::Fullneb]) {
		json_object_set_new(obj, "neb_awacs", json_real(Neb2_awacs));
		json_object_set_new(obj, "storm_name", json_string(Mission_parse_storm_name));
	}
	json_object_set_new(obj, "fog_near_mult", json_real(The_mission.neb_near_multi));
	json_object_set_new(obj, "fog_far_mult", json_real(The_mission.neb_far_multi));
	if (The_mission.contrail_threshold != CONTRAIL_THRESHOLD_DEFAULT)
		json_object_set_new(obj, "contrail_threshold", json_integer(The_mission.contrail_threshold));

	// Volumetric nebula
	if (The_mission.volumetrics) {
		json_t* vol = json_object();
		const auto& v = *The_mission.volumetrics;
		json_object_set_new(vol, "hull_pof", json_string(v.getHullPof().c_str()));
		json_object_set_new(vol, "position", mission_json::vec3d_to_json(v.getPos()));

		const auto& nc = v.getNebulaColor();
		json_object_set_new(vol, "color_r", json_integer(static_cast<int>(std::get<0>(nc) * 255.0f)));
		json_object_set_new(vol, "color_g", json_integer(static_cast<int>(std::get<1>(nc) * 255.0f)));
		json_object_set_new(vol, "color_b", json_integer(static_cast<int>(std::get<2>(nc) * 255.0f)));
		json_object_set_new(vol, "alpha_lim", json_real(v.getAlphaLim()));
		json_object_set_new(vol, "opacity_distance", json_real(v.getOpacityDistance()));
		json_object_set_new(vol, "steps", json_integer(v.getSteps()));
		json_object_set_new(vol, "resolution", json_integer(v.getResolution()));
		json_object_set_new(vol, "oversampling", json_integer(v.getOversampling()));
		json_object_set_new(vol, "smoothing", json_real(v.getSmoothing()));
		json_object_set_new(vol, "henyey_greenstein_coeff", json_real(v.getHenyeyGreensteinCoeff()));
		json_object_set_new(vol, "global_light_distance_factor", json_real(v.getGlobalLightDistanceFactor()));
		json_object_set_new(vol, "global_light_steps", json_integer(v.getGlobalLightSteps()));
		json_object_set_new(vol, "emissive_spread", json_real(v.getEmissiveSpread()));
		json_object_set_new(vol, "emissive_intensity", json_real(v.getEmissiveIntensity()));
		json_object_set_new(vol, "emissive_falloff", json_real(v.getEmissiveFalloff()));

		if (v.getNoiseActive()) {
			json_t* noise = json_object();
			const auto& ns = v.getNoiseColorScale();
			json_object_set_new(noise, "scale_x", json_real(std::get<0>(ns)));
			json_object_set_new(noise, "scale_y", json_real(std::get<1>(ns)));
			const auto& nnc = v.getNoiseColor();
			json_object_set_new(noise, "color_r", json_integer(static_cast<int>(std::get<0>(nnc) * 255.0f)));
			json_object_set_new(noise, "color_g", json_integer(static_cast<int>(std::get<1>(nnc) * 255.0f)));
			json_object_set_new(noise, "color_b", json_integer(static_cast<int>(std::get<2>(nnc) * 255.0f)));
			json_object_set_new(noise, "intensity", json_real(v.getNoiseColorIntensity()));
			if (v.noiseColorFunc1)
				json_object_set_new(noise, "function_base", json_string(v.noiseColorFunc1->c_str()));
			if (v.noiseColorFunc2)
				json_object_set_new(noise, "function_sub", json_string(v.noiseColorFunc2->c_str()));
			json_object_set_new(noise, "resolution", json_integer(v.getNoiseResolution()));
			json_object_set_new(vol, "noise", noise);
		}

		json_object_set_new(obj, "volumetric_nebula", vol);
	}

	// Multiplayer settings
	if (The_mission.game_type & MISSION_TYPE_MULTI) {
		json_object_set_new(obj, "num_players", json_integer(Player_starts));
		json_object_set_new(obj, "num_respawns", json_integer(The_mission.num_respawns));
		json_object_set_new(obj, "max_respawn_delay", json_integer(The_mission.max_respawn_delay));
	}

	// Support ship info
	{
		json_t* ss = json_object();
		json_object_set_new(ss, "max_support_ships", json_integer(The_mission.support_ships.max_support_ships));
		json_object_set_new(ss, "max_hull_repair_val", json_real(The_mission.support_ships.max_hull_repair_val));
		json_object_set_new(ss, "max_subsys_repair_val", json_real(The_mission.support_ships.max_subsys_repair_val));
		json_object_set_new(ss, "max_concurrent_ships", json_integer(The_mission.support_ships.max_concurrent_ships));
		if (The_mission.support_ships.ship_class >= 0)
			json_object_set_new(ss, "ship_class", json_string(Ship_info[The_mission.support_ships.ship_class].name));
		json_object_set_new(ss, "arrival_location", json_string(arrival_location_name(The_mission.support_ships.arrival_location)));
		json_object_set_new(ss, "arrival_anchor", anchor_to_json(The_mission.support_ships.arrival_anchor));
		json_object_set_new(ss, "departure_location", json_string(departure_location_name(The_mission.support_ships.departure_location)));
		json_object_set_new(ss, "departure_anchor", anchor_to_json(The_mission.support_ships.departure_anchor));
		json_object_set_new(obj, "support_ships", ss);
	}

	// All teams attack
	if (Mission_all_attack)
		json_object_set_new(obj, "all_teams_attack", json_boolean(true));

	// Player entry delay
	if (Entry_delay_time)
		json_object_set_new(obj, "player_entry_delay", json_real(f2fl(Entry_delay_time)));

	// Viewer position and orientation (editor state)
	Assertion(json_save_config != nullptr, "json_save_config must be set before saving mission info");
	json_object_set_new(obj, "viewer_pos", mission_json::vec3d_to_json(json_save_config->view_pos));
	json_object_set_new(obj, "viewer_orient", mission_json::matrix_to_json(json_save_config->view_orient));

	// Squadron info
	if (!(The_mission.game_type & MISSION_TYPE_MULTI) && strlen(The_mission.squad_name) > 0) {
		json_object_set_new(obj, "squad_name", json_string(The_mission.squad_name));
		if (strlen(The_mission.squad_filename) > 0)
			json_object_set_new(obj, "squad_filename", json_string(The_mission.squad_filename));
	}

	// Wing names
	if (strcmp(Starting_wing_names[0], "Alpha") || strcmp(Starting_wing_names[1], "Beta") ||
		strcmp(Starting_wing_names[2], "Gamma")) {
		json_t* sw = json_array();
		for (int i = 0; i < MAX_STARTING_WINGS; i++)
			json_array_append_new(sw, json_string(Starting_wing_names[i]));
		json_object_set_new(obj, "starting_wing_names", sw);
	}
	if (strcmp(Squadron_wing_names[0], "Alpha") || strcmp(Squadron_wing_names[1], "Beta") ||
		strcmp(Squadron_wing_names[2], "Gamma") || strcmp(Squadron_wing_names[3], "Delta") ||
		strcmp(Squadron_wing_names[4], "Epsilon")) {
		json_t* sq = json_array();
		for (int i = 0; i < MAX_SQUADRON_WINGS; i++)
			json_array_append_new(sq, json_string(Squadron_wing_names[i]));
		json_object_set_new(obj, "squadron_wing_names", sq);
	}
	if (strcmp(TVT_wing_names[0], "Alpha") || strcmp(TVT_wing_names[1], "Zeta")) {
		json_t* tv = json_array();
		for (int i = 0; i < MAX_TVT_WINGS; i++)
			json_array_append_new(tv, json_string(TVT_wing_names[i]));
		json_object_set_new(obj, "tvt_wing_names", tv);
	}

	// Loading screens
	if (strlen(The_mission.loading_screen[GR_640]) > 0)
		json_object_set_new(obj, "loading_screen_640", json_string(The_mission.loading_screen[GR_640]));
	if (strlen(The_mission.loading_screen[GR_1024]) > 0)
		json_object_set_new(obj, "loading_screen_1024", json_string(The_mission.loading_screen[GR_1024]));

	// Skybox
	if (strlen(The_mission.skybox_model) > 0) {
		json_object_set_new(obj, "skybox_model", json_string(The_mission.skybox_model));

		// Skybox animations and moveables
		const auto& anim_triggers = The_mission.skybox_model_animations.getRegisteredAnimNames();
		const auto& moveable_triggers = The_mission.skybox_model_animations.getRegisteredMoveables();

		if (!anim_triggers.empty()) {
			json_t* anims = json_array();
			for (const auto& animation : anim_triggers)
				json_array_append_new(anims, json_string(animation.c_str()));
			json_object_set_new(obj, "skybox_model_animations", anims);
		}
		if (!moveable_triggers.empty()) {
			json_t* moveables = json_array();
			for (const auto& moveable : moveable_triggers)
				json_array_append_new(moveables, json_string(moveable.c_str()));
			json_object_set_new(obj, "skybox_model_moveables", moveables);
		}
	}

	if (!vm_matrix_same(&vmd_identity_matrix, &The_mission.skybox_orientation))
		json_object_set_new(obj, "skybox_orientation", mission_json::matrix_to_json(The_mission.skybox_orientation));

	if (The_mission.skybox_flags != DEFAULT_NMODEL_FLAGS)
		json_object_set_new(obj, "skybox_flags", json_integer(The_mission.skybox_flags));

	// AI profile
	if (The_mission.ai_profile)
		json_object_set_new(obj, "ai_profile", json_string(The_mission.ai_profile->profile_name));

	// Lighting profile
	if (The_mission.lighting_profile_name != lighting_profiles::default_name())
		json_object_set_new(obj, "lighting_profile", json_string(The_mission.lighting_profile_name.c_str()));

	// Sound environment
	{
		sound_env* m_env = &The_mission.sound_environment;
		if (m_env->id >= 0 && m_env->id < static_cast<int>(EFX_presets.size())) {
			json_t* snd = json_object();
			json_object_set_new(snd, "preset", json_string(EFX_presets[m_env->id].name.c_str()));
			json_object_set_new(snd, "volume", json_real(m_env->volume));
			json_object_set_new(snd, "damping", json_real(m_env->damping));
			json_object_set_new(snd, "decay", json_real(m_env->decay));
			json_object_set_new(obj, "sound_environment", snd);
		}
	}

	return obj;
}

json_t* save_variables_json()
{
	json_t* arr = json_array();
	int count = sexp_variable_count();

	for (int i = 0; i < count; i++) {
		if (Sexp_variables[i].type & SEXP_VARIABLE_NOT_USED)
			continue;

		json_t* var = json_object();
		json_object_set_new(var, "name", json_string(Sexp_variables[i].variable_name));
		json_object_set_new(var, "default_value", json_string(Sexp_variables[i].text));

		SCP_string type_str;
		if (Sexp_variables[i].type & SEXP_VARIABLE_NUMBER)
			type_str = "number";
		else
			type_str = "string";

		if (Sexp_variables[i].type & SEXP_VARIABLE_BLOCK)
			type_str += " block";
		if (Sexp_variables[i].type & SEXP_VARIABLE_NETWORK)
			type_str += " network";
		if (Sexp_variables[i].type & SEXP_VARIABLE_SAVE_TO_PLAYER_FILE)
			type_str += " eternal";
		if (Sexp_variables[i].type & SEXP_VARIABLE_SAVE_ON_MISSION_CLOSE)
			type_str += " save-on-mission-close";
		if (Sexp_variables[i].type & SEXP_VARIABLE_SAVE_ON_MISSION_PROGRESS)
			type_str += " save-on-mission-progress";

		json_object_set_new(var, "type", json_string(type_str.c_str()));
		json_array_append_new(arr, var);
	}

	return arr;
}

json_t* save_containers_json()
{
	json_t* arr = json_array();
	const auto& containers = get_all_sexp_containers();

	for (const auto& sc : containers) {
		json_t* obj = json_object();
		json_object_set_new(obj, "name", json_string(sc.container_name.c_str()));

		if (sc.is_list()) {
			json_object_set_new(obj, "container_type", json_string("list"));
			json_t* data = json_array();
			for (const auto& item : sc.list_data) {
				json_array_append_new(data, json_string(item.c_str()));
			}
			json_object_set_new(obj, "data", data);
		} else {
			json_object_set_new(obj, "container_type", json_string("map"));
			json_t* data = json_object();
			for (const auto& pair : sc.map_data) {
				json_object_set_new(data, pair.first.c_str(), json_string(pair.second.c_str()));
			}
			json_object_set_new(obj, "data", data);
		}

		if (any(sc.type & ContainerType::NUMBER_DATA))
			json_object_set_new(obj, "data_type", json_string("number"));
		else
			json_object_set_new(obj, "data_type", json_string("string"));

		if (sc.is_map()) {
			if (any(sc.type & ContainerType::STRING_KEYS))
				json_object_set_new(obj, "key_type", json_string("string"));
			else
				json_object_set_new(obj, "key_type", json_string("number"));
		}

		if (any(sc.type & ContainerType::STRICTLY_TYPED_DATA))
			json_object_set_new(obj, "strictly_typed_data", json_true());
		if (any(sc.type & ContainerType::STRICTLY_TYPED_KEYS))
			json_object_set_new(obj, "strictly_typed_keys", json_true());
		if (any(sc.type & ContainerType::NETWORK))
			json_object_set_new(obj, "network", json_true());
		if (any(sc.type & ContainerType::SAVE_TO_PLAYER_FILE))
			json_object_set_new(obj, "eternal", json_true());
		if (any(sc.type & ContainerType::SAVE_ON_MISSION_CLOSE))
			json_object_set_new(obj, "save_on_mission_close", json_true());
		if (any(sc.type & ContainerType::SAVE_ON_MISSION_PROGRESS))
			json_object_set_new(obj, "save_on_mission_progress", json_true());

		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_cutscenes_json()
{
	json_t* arr = json_array();

	for (const auto& cs : The_mission.cutscenes) {
		json_t* obj = json_object();
		json_object_set_new(obj, "type", json_integer(cs.type));
		json_object_set_new(obj, "filename", json_string(cs.filename));
		json_object_set_new(obj, "formula", mission_json::sexp_to_json(cs.formula));
		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_fiction_json()
{
	json_t* arr = json_array();

	for (const auto& fvs : Fiction_viewer_stages) {
		json_t* obj = json_object();
		json_object_set_new(obj, "story_filename", json_string(fvs.story_filename));
		if (strlen(fvs.font_filename) > 0)
			json_object_set_new(obj, "font_filename", json_string(fvs.font_filename));
		if (strlen(fvs.voice_filename) > 0)
			json_object_set_new(obj, "voice_filename", json_string(fvs.voice_filename));
		if (strlen(fvs.ui_name) > 0)
			json_object_set_new(obj, "ui_name", json_string(fvs.ui_name));
		if (strlen(fvs.background[GR_640]) > 0)
			json_object_set_new(obj, "background_640", json_string(fvs.background[GR_640]));
		if (strlen(fvs.background[GR_1024]) > 0)
			json_object_set_new(obj, "background_1024", json_string(fvs.background[GR_1024]));
		json_object_set_new(obj, "formula", mission_json::sexp_to_json(fvs.formula));
		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_cmd_brief_json(const cmd_brief& cb)
{
	json_t* obj = json_object();
	json_object_set_new(obj, "num_stages", json_integer(cb.num_stages));

	if (strlen(cb.background[GR_640]) > 0)
		json_object_set_new(obj, "background_640", json_string(cb.background[GR_640]));
	if (strlen(cb.background[GR_1024]) > 0)
		json_object_set_new(obj, "background_1024", json_string(cb.background[GR_1024]));

	json_t* stages = json_array();
	for (int i = 0; i < cb.num_stages; i++) {
		json_t* s = json_object();
		json_object_set_new(s, "text", json_string(cb.stage[i].text.c_str()));
		json_object_set_new(s, "ani_filename", json_string(cb.stage[i].ani_filename));
		json_object_set_new(s, "wave_filename", json_string(cb.stage[i].wave_filename));
		json_array_append_new(stages, s);
	}
	json_object_set_new(obj, "stages", stages);

	return obj;
}

json_t* save_cmd_briefs_json()
{
	json_t* arr = json_array();
	for (int i = 0; i < Num_teams; i++) {
		json_array_append_new(arr, save_cmd_brief_json(Cmd_briefs[i]));
	}
	return arr;
}

json_t* save_briefing_icon_json(const brief_icon& icon)
{
	json_t* obj = json_object();
	json_object_set_new(obj, "type", json_integer(icon.type));
	json_object_set_new(obj, "team", json_string(Iff_info[icon.team].iff_name));
	json_object_set_new(obj, "id", json_integer(icon.id));
	json_object_set_new(obj, "pos", mission_json::vec3d_to_json(icon.pos));

	if (icon.ship_class >= 0 && icon.ship_class < static_cast<int>(Ship_info.size()))
		json_object_set_new(obj, "ship_class", json_string(Ship_info[icon.ship_class].name));

	if (strlen(icon.label) > 0)
		json_object_set_new(obj, "label", json_string(icon.label));
	if (strlen(icon.closeup_label) > 0)
		json_object_set_new(obj, "closeup_label", json_string(icon.closeup_label));

	if (icon.scale_factor != 1.0f)
		json_object_set_new(obj, "icon_scale", json_integer(static_cast<int>(icon.scale_factor * 100.0f)));

	json_object_set_new(obj, "highlight", json_boolean((icon.flags & BI_HIGHLIGHT) != 0));
	json_object_set_new(obj, "mirror", json_boolean((icon.flags & BI_MIRROR_ICON) != 0));
	if (icon.flags & BI_USE_WING_ICON)
		json_object_set_new(obj, "use_wing_icon", json_true());
	if (icon.flags & BI_USE_CARGO_ICON)
		json_object_set_new(obj, "use_cargo_icon", json_true());

	return obj;
}

json_t* save_briefing_json()
{
	json_t* arr = json_array();

	for (int t = 0; t < Num_teams; t++) {
		const briefing& b = Briefings[t];
		json_t* obj = json_object();
		json_object_set_new(obj, "num_stages", json_integer(b.num_stages));

		if (strlen(b.background[GR_640]) > 0)
			json_object_set_new(obj, "background_640", json_string(b.background[GR_640]));
		if (strlen(b.background[GR_1024]) > 0)
			json_object_set_new(obj, "background_1024", json_string(b.background[GR_1024]));
		if (strlen(b.ship_select_background[GR_640]) > 0)
			json_object_set_new(obj, "ship_select_bg_640", json_string(b.ship_select_background[GR_640]));
		if (strlen(b.ship_select_background[GR_1024]) > 0)
			json_object_set_new(obj, "ship_select_bg_1024", json_string(b.ship_select_background[GR_1024]));
		if (strlen(b.weapon_select_background[GR_640]) > 0)
			json_object_set_new(obj, "weapon_select_bg_640", json_string(b.weapon_select_background[GR_640]));
		if (strlen(b.weapon_select_background[GR_1024]) > 0)
			json_object_set_new(obj, "weapon_select_bg_1024", json_string(b.weapon_select_background[GR_1024]));

		json_t* stages = json_array();
		for (int i = 0; i < b.num_stages; i++) {
			const brief_stage& s = b.stages[i];
			json_t* stage = json_object();
			json_object_set_new(stage, "text", json_string(s.text.c_str()));
			json_object_set_new(stage, "voice", json_string(s.voice));
			json_object_set_new(stage, "camera_pos", mission_json::vec3d_to_json(s.camera_pos));
			json_object_set_new(stage, "camera_orient", mission_json::matrix_to_json(s.camera_orient));
			json_object_set_new(stage, "camera_time", json_integer(s.camera_time));
			json_object_set_new(stage, "flags", json_integer(s.flags));
			json_object_set_new(stage, "formula", mission_json::sexp_to_json(s.formula));
			json_object_set_new(stage, "draw_grid", json_boolean(s.draw_grid));

			if (!gr_compare_color_values(s.grid_color, Color_briefing_grid)) {
				json_t* gc = json_object();
				json_object_set_new(gc, "red", json_integer(s.grid_color.red));
				json_object_set_new(gc, "green", json_integer(s.grid_color.green));
				json_object_set_new(gc, "blue", json_integer(s.grid_color.blue));
				json_object_set_new(gc, "alpha", json_integer(s.grid_color.alpha));
				json_object_set_new(stage, "grid_color", gc);
			}

			json_t* icons = json_array();
			for (int j = 0; j < s.num_icons; j++) {
				json_array_append_new(icons, save_briefing_icon_json(s.icons[j]));
			}
			json_object_set_new(stage, "icons", icons);

			json_t* lines = json_array();
			for (int j = 0; j < s.num_lines; j++) {
				json_t* line = json_object();
				json_object_set_new(line, "start_icon", json_integer(s.lines[j].start_icon));
				json_object_set_new(line, "end_icon", json_integer(s.lines[j].end_icon));
				json_array_append_new(lines, line);
			}
			json_object_set_new(stage, "lines", lines);

			json_array_append_new(stages, stage);
		}
		json_object_set_new(obj, "stages", stages);
		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_debriefing_json()
{
	json_t* arr = json_array();

	for (int t = 0; t < Num_teams; t++) {
		const debriefing& d = Debriefings[t];
		json_t* obj = json_object();
		json_object_set_new(obj, "num_stages", json_integer(d.num_stages));

		if (strlen(d.background[GR_640]) > 0)
			json_object_set_new(obj, "background_640", json_string(d.background[GR_640]));
		if (strlen(d.background[GR_1024]) > 0)
			json_object_set_new(obj, "background_1024", json_string(d.background[GR_1024]));

		json_t* stages = json_array();
		for (int i = 0; i < d.num_stages; i++) {
			const debrief_stage& s = d.stages[i];
			json_t* stage = json_object();
			json_object_set_new(stage, "formula", mission_json::sexp_to_json(s.formula));
			json_object_set_new(stage, "text", json_string(s.text.c_str()));
			json_object_set_new(stage, "voice", json_string(s.voice));
			json_object_set_new(stage, "recommendation_text", json_string(s.recommendation_text.c_str()));
			json_array_append_new(stages, stage);
		}
		json_object_set_new(obj, "stages", stages);
		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_players_json()
{
	json_t* obj = json_object();

	// Alternate types list
	if (Mission_alt_type_count > 0) {
		json_t* alt_arr = json_array();
		for (int i = 0; i < Mission_alt_type_count; i++) {
			json_array_append_new(alt_arr, json_string(Mission_alt_types[i]));
		}
		json_object_set_new(obj, "alternate_types", alt_arr);
	}

	// Callsigns list
	if (Mission_callsign_count > 0) {
		json_t* cs_arr = json_array();
		for (int i = 0; i < Mission_callsign_count; i++) {
			json_array_append_new(cs_arr, json_string(Mission_callsigns[i]));
		}
		json_object_set_new(obj, "callsigns", cs_arr);
	}

	// General orders enabled
	{
		SCP_vector<SCP_string> e_list = ai_lua_get_general_orders(true);
		if (!e_list.empty()) {
			json_t* orders = json_array();
			for (const SCP_string& order : e_list) {
				json_array_append_new(orders, json_string(order.c_str()));
			}
			json_object_set_new(obj, "general_orders_enabled", orders);
		}
	}

	// General orders valid
	{
		SCP_vector<SCP_string> v_list = ai_lua_get_general_orders(false, true);
		if (!v_list.empty()) {
			json_t* orders = json_array();
			for (const SCP_string& order : v_list) {
				json_array_append_new(orders, json_string(order.c_str()));
			}
			json_object_set_new(obj, "general_orders_valid", orders);
		}
	}

	// Teams
	json_t* teams_arr = json_array();
	for (int t = 0; t < Num_teams; t++) {
		json_t* team = json_object();

		// Starting shipname for all teams
		if (Player_start_shipnum >= 0)
			json_object_set_new(team, "starting_shipname", json_string(Ships[Player_start_shipnum].ship_name));

		if (Team_data[t].do_not_validate)
			json_object_set_new(team, "do_not_validate", json_true());

		// Ship choices
		json_t* ships = json_array();
		for (int i = 0; i < Team_data[t].num_ship_choices; i++) {
			json_t* entry = json_object();
			int si = Team_data[t].ship_list[i];
			if (si >= 0 && si < static_cast<int>(Ship_info.size())) {
				if (strlen(Team_data[t].ship_list_variables[i]) > 0)
					json_object_set_new(entry, "variable", json_string(Team_data[t].ship_list_variables[i]));
				else
					json_object_set_new(entry, "ship_class", json_string(Ship_info[si].name));
			}

			if (strlen(Team_data[t].ship_count_variables[i]) > 0)
				json_object_set_new(entry, "count_variable", json_string(Team_data[t].ship_count_variables[i]));
			else
				json_object_set_new(entry, "count", json_integer(Team_data[t].ship_count[i]));

			json_array_append_new(ships, entry);
		}
		json_object_set_new(team, "ship_choices", ships);

		// Weapon pool
		json_t* weapons = json_array();
		for (int i = 0; i < Team_data[t].num_weapon_choices; i++) {
			json_t* entry = json_object();
			int wi = Team_data[t].weaponry_pool[i];
			if (wi >= 0 && wi < static_cast<int>(Weapon_info.size())) {
				if (strlen(Team_data[t].weaponry_pool_variable[i]) > 0)
					json_object_set_new(entry, "variable", json_string(Team_data[t].weaponry_pool_variable[i]));
				else
					json_object_set_new(entry, "weapon_class", json_string(Weapon_info[wi].name));
			}

			if (strlen(Team_data[t].weaponry_amount_variable[i]) > 0)
				json_object_set_new(entry, "count_variable", json_string(Team_data[t].weaponry_amount_variable[i]));
			else
				json_object_set_new(entry, "count", json_integer(Team_data[t].weaponry_count[i]));

			json_array_append_new(weapons, entry);
		}
		json_object_set_new(team, "weapon_pool", weapons);

		// Required weapons for mission
		{
			json_t* req_arr = json_array();
			for (int j = 0; j < weapon_info_size(); j++) {
				if (Team_data[t].weapon_required[j])
					json_array_append_new(req_arr, json_string(Weapon_info[j].name));
			}
			if (json_array_size(req_arr) > 0)
				json_object_set_new(team, "required_weapons", req_arr);
			else
				json_decref(req_arr);
		}

		json_array_append_new(teams_arr, team);
	}
	json_object_set_new(obj, "teams", teams_arr);

	return obj;
}

// Save warp parameters for a ship, mirroring save_warp_params() in missionsave.cpp.
// Only saves fields that differ from the ship_info defaults.
// Returns a json object with the differing params, or nullptr if params match defaults.
json_t* save_warp_params_json(WarpDirection direction, ship* shipp)
{
	WarpParams *shipp_params, *sip_params;
	if (direction == WarpDirection::WARP_IN) {
		if (shipp->warpin_params_index == Ship_info[shipp->ship_info_index].warpin_params_index)
			return nullptr;

		shipp_params = &Warp_params[shipp->warpin_params_index];
		sip_params = &Warp_params[Ship_info[shipp->ship_info_index].warpin_params_index];
	} else {
		if (shipp->warpout_params_index == Ship_info[shipp->ship_info_index].warpout_params_index)
			return nullptr;

		shipp_params = &Warp_params[shipp->warpout_params_index];
		sip_params = &Warp_params[Ship_info[shipp->ship_info_index].warpout_params_index];
	}

	json_t* obj = json_object();

	if (shipp_params->warp_type != sip_params->warp_type) {
		if (shipp_params->warp_type & WT_DEFAULT_WITH_FIREBALL) {
			json_object_set_new(obj, "type", json_string(Fireball_info[shipp_params->warp_type & WT_FLAG_MASK].unique_id));
		} else if (shipp_params->warp_type >= 0 && shipp_params->warp_type < Num_warp_types) {
			json_object_set_new(obj, "type", json_string(Warp_types[shipp_params->warp_type]));
		}
	}

	if (shipp_params->snd_start != sip_params->snd_start) {
		if (shipp_params->snd_start.isValid())
			json_object_set_new(obj, "start_sound", json_string(gamesnd_get_game_sound(shipp_params->snd_start)->name.c_str()));
	}

	if (shipp_params->snd_end != sip_params->snd_end) {
		if (shipp_params->snd_end.isValid())
			json_object_set_new(obj, "end_sound", json_string(gamesnd_get_game_sound(shipp_params->snd_end)->name.c_str()));
	}

	if (direction == WarpDirection::WARP_OUT && shipp_params->warpout_engage_time != sip_params->warpout_engage_time) {
		if (shipp_params->warpout_engage_time > 0)
			json_object_set_new(obj, "engage_time", json_real(i2fl(shipp_params->warpout_engage_time) / 1000.0f));
	}

	if (shipp_params->speed != sip_params->speed) {
		if (shipp_params->speed > 0.0f)
			json_object_set_new(obj, "speed", json_real(shipp_params->speed));
	}

	if (shipp_params->time != sip_params->time) {
		if (shipp_params->time > 0)
			json_object_set_new(obj, "time", json_real(i2fl(shipp_params->time) / 1000.0f));
	}

	if (shipp_params->accel_exp != sip_params->accel_exp) {
		if (shipp_params->accel_exp > 0.0f) {
			const char* key = (direction == WarpDirection::WARP_IN) ? "decel_exp" : "accel_exp";
			json_object_set_new(obj, key, json_real(shipp_params->accel_exp));
		}
	}

	if (shipp_params->radius != sip_params->radius) {
		if (shipp_params->radius > 0.0f)
			json_object_set_new(obj, "radius", json_real(shipp_params->radius));
	}

	if (stricmp(shipp_params->anim, sip_params->anim) != 0) {
		if (strlen(shipp_params->anim) > 0)
			json_object_set_new(obj, "animation", json_string(shipp_params->anim));
	}

	if (shipp_params->supercap_warp_physics != sip_params->supercap_warp_physics) {
		json_object_set_new(obj, "supercap_warp_physics", json_boolean(shipp_params->supercap_warp_physics));
	}

	if (direction == WarpDirection::WARP_OUT &&
		shipp_params->warpout_player_speed != sip_params->warpout_player_speed) {
		if (shipp_params->warpout_player_speed > 0.0f)
			json_object_set_new(obj, "player_warpout_speed", json_real(shipp_params->warpout_player_speed));
	}

	if (json_object_size(obj) == 0) {
		json_decref(obj);
		return nullptr;
	}

	return obj;
}

// Save turret info for a subsystem, mirroring save_turret_info() in missionsave.cpp.
// Only saves AI class and weapon banks that differ from the subsystem defaults.
void save_turret_info_json(json_t* sub, ship_subsys* ptr, int ship_index)
{
	int i, z;
	ship_weapon* wp = &ptr->weapons;

	if (wp->ai_class != Ship_info[Ships[ship_index].ship_info_index].ai_class) {
		json_object_set_new(sub, "ai_class", json_string(Ai_class_names[wp->ai_class]));
	}

	z = 0;
	i = wp->num_primary_banks;
	while (i--)
		if (wp->primary_bank_weapons[i] != ptr->system_info->primary_banks[i])
			z = 1;

	if (z) {
		json_t* prim = json_array();
		for (i = 0; i < wp->num_primary_banks; i++) {
			if (wp->primary_bank_weapons[i] != -1)
				json_array_append_new(prim, json_string(Weapon_info[wp->primary_bank_weapons[i]].name));
			else
				json_array_append_new(prim, json_string(""));
		}
		json_object_set_new(sub, "primary_banks", prim);
	}

	z = 0;
	i = wp->num_secondary_banks;
	while (i--)
		if (wp->secondary_bank_weapons[i] != ptr->system_info->secondary_banks[i])
			z = 1;

	if (z) {
		json_t* sec = json_array();
		for (i = 0; i < wp->num_secondary_banks; i++) {
			if (wp->secondary_bank_weapons[i] != -1)
				json_array_append_new(sec, json_string(Weapon_info[wp->secondary_bank_weapons[i]].name));
			else
				json_array_append_new(sec, json_string(""));
		}
		json_object_set_new(sub, "secondary_banks", sec);
	}

	z = 0;
	i = wp->num_secondary_banks;
	while (i--)
		if (wp->secondary_bank_ammo[i] != 100)
			z = 1;

	if (z) {
		json_t* ammo = json_array();
		for (i = 0; i < wp->num_secondary_banks; i++)
			json_array_append_new(ammo, json_integer(wp->secondary_bank_ammo[i]));
		json_object_set_new(sub, "secondary_ammo", ammo);
	}
}

// Save common object data (initial state, weapon banks, subsystems),
// mirroring save_common_object_data() in missionsave.cpp.
json_t* save_common_object_data_json(object* objp, ship* shipp)
{
	int j, z;
	ship_info* sip = &Ship_info[shipp->ship_info_index];
	json_t* obj = json_object();

	// Initial velocity/hull/shields
	if (static_cast<int>(objp->phys_info.speed))
		json_object_set_new(obj, "initial_velocity", json_integer(static_cast<int>(objp->phys_info.speed)));

	if (fl2i(objp->hull_strength) != 100)
		json_object_set_new(obj, "initial_hull", json_integer(fl2i(objp->hull_strength)));

	if (fl2i(objp->shield_quadrant[0]) != 100)
		json_object_set_new(obj, "initial_shields", json_integer(fl2i(objp->shield_quadrant[0])));

	// Ship-level primary banks (Pilot subsystem)
	ship_weapon* wp = &shipp->weapons;
	z = 0;
	j = wp->num_primary_banks;
	while (j-- && (j >= 0))
		if (wp->primary_bank_weapons[j] != sip->primary_bank_weapons[j])
			z = 1;

	if (z) {
		json_t* prim = json_array();
		for (j = 0; j < wp->num_primary_banks; j++) {
			if (wp->primary_bank_weapons[j] != -1)
				json_array_append_new(prim, json_string(Weapon_info[wp->primary_bank_weapons[j]].name));
			else
				json_array_append_new(prim, json_string(""));
		}
		json_object_set_new(obj, "primary_banks", prim);
	}

	z = 0;
	j = wp->num_secondary_banks;
	while (j-- && (j >= 0))
		if (wp->secondary_bank_weapons[j] != sip->secondary_bank_weapons[j])
			z = 1;

	if (z) {
		json_t* sec = json_array();
		for (j = 0; j < wp->num_secondary_banks; j++) {
			if (wp->secondary_bank_weapons[j] != -1)
				json_array_append_new(sec, json_string(Weapon_info[wp->secondary_bank_weapons[j]].name));
			else
				json_array_append_new(sec, json_string(""));
		}
		json_object_set_new(obj, "secondary_banks", sec);
	}

	z = 0;
	j = wp->num_secondary_banks;
	while (j-- && (j >= 0))
		if (wp->secondary_bank_ammo[j] != 100)
			z = 1;

	if (z) {
		json_t* ammo = json_array();
		for (j = 0; j < wp->num_secondary_banks; j++)
			json_array_append_new(ammo, json_integer(wp->secondary_bank_ammo[j]));
		json_object_set_new(obj, "secondary_ammo", ammo);
	}

	// Subsystems
	json_t* subsys_arr = json_array();
	ship_subsys* ptr = GET_FIRST(&shipp->subsys_list);

	while (ptr != END_OF_LIST(&shipp->subsys_list) && ptr) {
		// Only write subsystem if it has damage, cargo, or is a turret (matching standard)
		if ((ptr->current_hits) || (ptr->system_info && ptr->system_info->type == SUBSYSTEM_TURRET) ||
			(ptr->subsys_cargo_name > 0)) {

			json_t* sub = json_object();
			json_object_set_new(sub, "name", json_string(ptr->system_info->subobj_name));

			if (ptr->current_hits) {
				json_object_set_new(sub, "damage", json_integer(static_cast<int>(ptr->current_hits)));
			}

			if (ptr->subsys_cargo_name > 0) {
				json_object_set_new(sub, "cargo_name", json_string(Cargo_names[ptr->subsys_cargo_name]));
			}

			if (ptr->subsys_cargo_title[0] != '\0') {
				json_object_set_new(sub, "cargo_title", json_string(ptr->subsys_cargo_title));
			}

			if (ptr->system_info->type == SUBSYSTEM_TURRET)
				save_turret_info_json(sub, ptr, SHIP_INDEX(shipp));

			json_array_append_new(subsys_arr, sub);
		}

		ptr = GET_NEXT(ptr);
	}

	if (json_array_size(subsys_arr) > 0)
		json_object_set_new(obj, "subsystems", subsys_arr);
	else
		json_decref(subsys_arr);

	return obj;
}

// Save a single dock instance, mirroring save_single_dock_instance() in missionsave.cpp.
json_t* save_single_dock_instance_json(ship* shipp, dock_instance* dock_ptr)
{
	object* objp_a = &Objects[shipp->objnum];
	object* objp_b = dock_ptr->docked_objp;
	ship* other_shipp = &Ships[objp_b->instance];

	json_t* dock = json_object();
	json_object_set_new(dock, "docked_to", json_string(other_shipp->ship_name));

	// Note: standard format has docker/dockee reversed (Volition bug preserved for compatibility).
	// JSON format uses the correct naming.
	int my_dockpoint = dock_find_dockpoint_used_by_object(objp_a, objp_b);
	int other_dockpoint = dock_find_dockpoint_used_by_object(objp_b, objp_a);

	if (other_dockpoint >= 0)
		json_object_set_new(dock, "docker_point", json_string(
			model_get_dock_name(Ship_info[other_shipp->ship_info_index].model_num, other_dockpoint)));
	if (my_dockpoint >= 0)
		json_object_set_new(dock, "dockee_point", json_string(
			model_get_dock_name(Ship_info[shipp->ship_info_index].model_num, my_dockpoint)));

	return dock;
}

json_t* save_objects_json()
{
	json_t* arr = json_array();

	for (int i = 0; i < MAX_SHIPS; i++) {
		ship* shipp = &Ships[i];
		if (shipp->objnum < 0)
			continue;

		object* objp = &Objects[shipp->objnum];
		ship_info* sip = &Ship_info[shipp->ship_info_index];

		auto j = objp->type;
		if ((j != OBJ_SHIP) && (j != OBJ_START))
			continue;

		json_t* obj = json_object();

		// --- Name and display name ---
		json_object_set_new(obj, "name", json_string(shipp->ship_name));

		if ((json_save_config->always_save_display_names && shipp->wingnum < 0) || shipp->has_display_name()) {
			char truncated_name[NAME_LENGTH];
			strcpy_s(truncated_name, shipp->ship_name);
			end_string_at_first_hash_symbol(truncated_name);

			if ((json_save_config->always_save_display_names && shipp->wingnum < 0) || strcmp(shipp->get_display_name(), truncated_name) != 0)
				json_object_set_new(obj, "display_name", json_string(shipp->get_display_name()));
		}

		// --- Class and alt classes ---
		json_object_set_new(obj, "ship_class", json_string(sip->name));

		if (!shipp->s_alt_classes.empty()) {
			json_t* alt_arr = json_array();
			for (const auto& ac : shipp->s_alt_classes) {
				json_t* a = json_object();
				if (ac.variable_index != -1)
					json_object_set_new(a, "variable", json_string(Sexp_variables[ac.variable_index].variable_name));
				else if (ac.ship_class >= 0 && ac.ship_class < static_cast<int>(Ship_info.size()))
					json_object_set_new(a, "ship_class", json_string(Ship_info[ac.ship_class].name));
				json_object_set_new(a, "default", json_boolean(ac.default_to_this_class));
				json_array_append_new(alt_arr, a);
			}
			json_object_set_new(obj, "alt_classes", alt_arr);
		}

		// --- Alt name and callsign ---
		Assertion(json_save_config->fred_alt_names != nullptr, "json_save_config->fred_alt_names is null!");
		if (strlen(json_save_config->fred_alt_names[i]))
			json_object_set_new(obj, "alt", json_string(json_save_config->fred_alt_names[i]));

		Assertion(json_save_config->fred_callsigns != nullptr, "json_save_config->fred_callsigns is null!");
		if (strlen(json_save_config->fred_callsigns[i]))
			json_object_set_new(obj, "callsign", json_string(json_save_config->fred_callsigns[i]));

		// --- Team and team color ---
		json_object_set_new(obj, "team", json_string(Iff_info[shipp->team].iff_name));

		if (sip->uses_team_colors && !shipp->team_name.empty())
			json_object_set_new(obj, "team_color_setting", json_string(shipp->team_name.c_str()));

		// --- Position and orientation ---
		json_object_set_new(obj, "position", mission_json::vec3d_to_json(objp->pos));
		json_object_set_new(obj, "orientation", mission_json::matrix_to_json(objp->orient));

		// --- AI class (only if different from ship_info default) ---
		if (shipp->weapons.ai_class != sip->ai_class)
			json_object_set_new(obj, "ai_class", json_string(Ai_class_names[shipp->weapons.ai_class]));

		// --- AI goals ---
		{
			ai_info* aip = &Ai_info[shipp->ai_index];
			json_t* goals = save_ai_goals_json(aip->goals);
			if (json_array_size(goals) > 0)
				json_object_set_new(obj, "ai_goals", goals);
			else
				json_decref(goals);
		}

		// --- Cargo and cargo title ---
		json_object_set_new(obj, "cargo", json_string(Cargo_names[shipp->cargo1]));

		if (shipp->cargo_title[0] != '\0')
			json_object_set_new(obj, "cargo_title", json_string(shipp->cargo_title));

		// --- Common object data (initial state, weapon banks, subsystems) ---
		{
			json_t* common = save_common_object_data_json(objp, shipp);
			// Merge common object data fields into the ship object
			const char* key;
			json_t* value;
			json_object_foreach(common, key, value) {
				json_object_set(obj, key, value);
			}
			json_decref(common);
		}

		// --- Arrival ---
		json_object_set_new(obj, "arrival_location", json_string(arrival_location_name(shipp->arrival_location)));
		if (shipp->arrival_distance != 0)
			json_object_set_new(obj, "arrival_distance", json_integer(shipp->arrival_distance));
		if (shipp->arrival_anchor >= 0)
			json_object_set_new(obj, "arrival_anchor", anchor_to_json(shipp->arrival_anchor));
		if (shipp->arrival_path_mask != 0)
			json_object_set_new(obj, "arrival_path_mask", json_integer(shipp->arrival_path_mask));
		if (shipp->arrival_delay)
			json_object_set_new(obj, "arrival_delay", json_integer(shipp->arrival_delay));
		json_object_set_new(obj, "arrival_cue", mission_json::sexp_to_json(shipp->arrival_cue));

		// --- Departure ---
		json_object_set_new(obj, "departure_location", json_string(departure_location_name(shipp->departure_location)));
		if (shipp->departure_anchor >= 0)
			json_object_set_new(obj, "departure_anchor", anchor_to_json(shipp->departure_anchor));
		if (shipp->departure_path_mask != 0)
			json_object_set_new(obj, "departure_path_mask", json_integer(shipp->departure_path_mask));
		if (shipp->departure_delay)
			json_object_set_new(obj, "departure_delay", json_integer(shipp->departure_delay));
		json_object_set_new(obj, "departure_cue", mission_json::sexp_to_json(shipp->departure_cue));

		// --- Warp parameters ---
		{
			json_t* warpin = save_warp_params_json(WarpDirection::WARP_IN, shipp);
			if (warpin)
				json_object_set_new(obj, "warpin_params", warpin);

			json_t* warpout = save_warp_params_json(WarpDirection::WARP_OUT, shipp);
			if (warpout)
				json_object_set_new(obj, "warpout_params", warpout);
		}

		// --- Flags ---
		// Combines both +Flags: and +Flags2: from standard format into a single array
		{
			json_t* flags_arr = json_array();

			// +Flags: block
			if (shipp->flags[Ship::Ship_Flags::Cargo_revealed])
				json_array_append_new(flags_arr, json_string("cargo-known"));
			if (shipp->flags[Ship::Ship_Flags::Ignore_count])
				json_array_append_new(flags_arr, json_string("ignore-count"));
			if (objp->flags[Object::Object_Flags::Protected])
				json_array_append_new(flags_arr, json_string("protect-ship"));
			if (shipp->flags[Ship::Ship_Flags::Reinforcement])
				json_array_append_new(flags_arr, json_string("reinforcement"));
			if (objp->flags[Object::Object_Flags::No_shields] &&
				!sip->flags[Ship::Info_Flags::Intrinsic_no_shields])
				json_array_append_new(flags_arr, json_string("no-shields"));
			if (shipp->flags[Ship::Ship_Flags::Escort])
				json_array_append_new(flags_arr, json_string("escort"));
			if (objp->type == OBJ_START)
				json_array_append_new(flags_arr, json_string("player-start"));
			if (shipp->flags[Ship::Ship_Flags::No_arrival_music])
				json_array_append_new(flags_arr, json_string("no-arrival-music"));
			if (shipp->flags[Ship::Ship_Flags::No_arrival_warp])
				json_array_append_new(flags_arr, json_string("no-arrival-warp"));
			if (shipp->flags[Ship::Ship_Flags::No_departure_warp])
				json_array_append_new(flags_arr, json_string("no-departure-warp"));
			if (objp->flags[Object::Object_Flags::Invulnerable])
				json_array_append_new(flags_arr, json_string("invulnerable"));
			if (shipp->flags[Ship::Ship_Flags::Hidden_from_sensors])
				json_array_append_new(flags_arr, json_string("hidden-from-sensors"));
			if (shipp->flags[Ship::Ship_Flags::Scannable])
				json_array_append_new(flags_arr, json_string("scannable"));
			if (Ai_info[shipp->ai_index].ai_flags[AI::AI_Flags::Kamikaze])
				json_array_append_new(flags_arr, json_string("kamikaze"));
			if (Ai_info[shipp->ai_index].ai_flags[AI::AI_Flags::No_dynamic])
				json_array_append_new(flags_arr, json_string("no-dynamic"));
			if (shipp->flags[Ship::Ship_Flags::Red_alert_store_status])
				json_array_append_new(flags_arr, json_string("red-alert-carry"));
			if (objp->flags[Object::Object_Flags::Beam_protected])
				json_array_append_new(flags_arr, json_string("beam-protect-ship"));
			if (objp->flags[Object::Object_Flags::Flak_protected])
				json_array_append_new(flags_arr, json_string("flak-protect-ship"));
			if (objp->flags[Object::Object_Flags::Laser_protected])
				json_array_append_new(flags_arr, json_string("laser-protect-ship"));
			if (objp->flags[Object::Object_Flags::Missile_protected])
				json_array_append_new(flags_arr, json_string("missile-protect-ship"));
			if (shipp->ship_guardian_threshold != 0)
				json_array_append_new(flags_arr, json_string("guardian"));
			if (objp->flags[Object::Object_Flags::Special_warpin])
				json_array_append_new(flags_arr, json_string("special-warp"));
			if (shipp->flags[Ship::Ship_Flags::Vaporize])
				json_array_append_new(flags_arr, json_string("vaporize"));
			if (shipp->flags[Ship::Ship_Flags::Stealth])
				json_array_append_new(flags_arr, json_string("stealth"));
			if (shipp->flags[Ship::Ship_Flags::Friendly_stealth_invis])
				json_array_append_new(flags_arr, json_string("friendly-stealth-invisible"));
			if (shipp->flags[Ship::Ship_Flags::Dont_collide_invis])
				json_array_append_new(flags_arr, json_string("don't-collide-invisible"));

			// +Flags2: block
			if (shipp->flags[Ship::Ship_Flags::Primitive_sensors])
				json_array_append_new(flags_arr, json_string("primitive-sensors"));
			if (shipp->flags[Ship::Ship_Flags::No_subspace_drive])
				json_array_append_new(flags_arr, json_string("no-subspace-drive"));
			if (shipp->flags[Ship::Ship_Flags::Navpoint_carry])
				json_array_append_new(flags_arr, json_string("nav-carry-status"));
			if (shipp->flags[Ship::Ship_Flags::Affected_by_gravity])
				json_array_append_new(flags_arr, json_string("affected-by-gravity"));
			if (shipp->flags[Ship::Ship_Flags::Toggle_subsystem_scanning])
				json_array_append_new(flags_arr, json_string("toggle-subsystem-scanning"));
			if (objp->flags[Object::Object_Flags::Targetable_as_bomb])
				json_array_append_new(flags_arr, json_string("targetable-as-bomb"));
			if (shipp->flags[Ship::Ship_Flags::No_builtin_messages])
				json_array_append_new(flags_arr, json_string("no-builtin-messages"));
			if (shipp->flags[Ship::Ship_Flags::Primaries_locked])
				json_array_append_new(flags_arr, json_string("primaries-locked"));
			if (shipp->flags[Ship::Ship_Flags::Secondaries_locked])
				json_array_append_new(flags_arr, json_string("secondaries-locked"));
			if (shipp->flags[Ship::Ship_Flags::No_death_scream])
				json_array_append_new(flags_arr, json_string("no-death-scream"));
			if (shipp->flags[Ship::Ship_Flags::Always_death_scream])
				json_array_append_new(flags_arr, json_string("always-death-scream"));
			if (shipp->flags[Ship::Ship_Flags::Navpoint_needslink])
				json_array_append_new(flags_arr, json_string("nav-needslink"));
			if (shipp->flags[Ship::Ship_Flags::Hide_ship_name])
				json_array_append_new(flags_arr, json_string("hide-ship-name"));
			if (shipp->flags[Ship::Ship_Flags::Set_class_dynamically])
				json_array_append_new(flags_arr, json_string("set-class-dynamically"));
			if (shipp->flags[Ship::Ship_Flags::Lock_all_turrets_initially])
				json_array_append_new(flags_arr, json_string("lock-all-turrets"));
			if (shipp->flags[Ship::Ship_Flags::Afterburner_locked])
				json_array_append_new(flags_arr, json_string("afterburners-locked"));
			if (shipp->flags[Ship::Ship_Flags::Force_shields_on])
				json_array_append_new(flags_arr, json_string("force-shields-on"));
			if (objp->flags[Object::Object_Flags::Dont_change_position])
				json_array_append_new(flags_arr, json_string("don't-change-position"));
			if (objp->flags[Object::Object_Flags::Dont_change_orientation])
				json_array_append_new(flags_arr, json_string("don't-change-orientation"));
			if (shipp->flags[Ship::Ship_Flags::No_ets])
				json_array_append_new(flags_arr, json_string("no-ets"));
			if (shipp->flags[Ship::Ship_Flags::Cloaked])
				json_array_append_new(flags_arr, json_string("cloaked"));
			if (shipp->flags[Ship::Ship_Flags::Ship_locked])
				json_array_append_new(flags_arr, json_string("ship-locked"));
			if (shipp->flags[Ship::Ship_Flags::Weapons_locked])
				json_array_append_new(flags_arr, json_string("weapons-locked"));
			if (shipp->flags[Ship::Ship_Flags::Scramble_messages])
				json_array_append_new(flags_arr, json_string("scramble-messages"));
			if (!(objp->flags[Object::Object_Flags::Collides]))
				json_array_append_new(flags_arr, json_string("no_collide"));
			if (shipp->flags[Ship::Ship_Flags::No_disabled_self_destruct])
				json_array_append_new(flags_arr, json_string("no-disabled-self-destruct"));
			if (shipp->flags[Ship::Ship_Flags::Same_arrival_warp_when_docked])
				json_array_append_new(flags_arr, json_string("same-arrival-warp-when-docked"));
			if (shipp->flags[Ship::Ship_Flags::Same_departure_warp_when_docked])
				json_array_append_new(flags_arr, json_string("same-departure-warp-when-docked"));
			if (objp->flags[Object::Object_Flags::Attackable_if_no_collide])
				json_array_append_new(flags_arr, json_string("ai-attackable-if-no-collide"));
			if (shipp->flags[Ship::Ship_Flags::Fail_sound_locked_primary])
				json_array_append_new(flags_arr, json_string("fail-sound-locked-primary"));
			if (shipp->flags[Ship::Ship_Flags::Fail_sound_locked_secondary])
				json_array_append_new(flags_arr, json_string("fail-sound-locked-secondary"));
			if (shipp->flags[Ship::Ship_Flags::Aspect_immune])
				json_array_append_new(flags_arr, json_string("aspect-immune"));
			if (shipp->flags[Ship::Ship_Flags::Cannot_perform_scan_hide_cargo])
				json_array_append_new(flags_arr, json_string("cannot-perform-scan-hide-cargo"));
			if (shipp->flags[Ship::Ship_Flags::Cannot_perform_scan_show_cargo])
				json_array_append_new(flags_arr, json_string("cannot-perform-scan-show-cargo"));
			if (shipp->flags[Ship::Ship_Flags::No_targeting_limits])
				json_array_append_new(flags_arr, json_string("no-targeting-limits"));

			json_object_set_new(obj, "flags", flags_arr);
		}

		// --- Respawn priority (always written, matching standard) ---
		json_object_set_new(obj, "respawn_priority", json_integer(shipp->respawn_priority));

		// --- Escort priority ---
		if (shipp->flags[Ship::Ship_Flags::Escort])
			json_object_set_new(obj, "escort_priority", json_integer(shipp->escort_priority));

		// --- Guardian threshold ---
		if (shipp->ship_guardian_threshold != 0)
			json_object_set_new(obj, "guardian_threshold", json_integer(shipp->ship_guardian_threshold));

		// --- Special explosion ---
		if (shipp->use_special_explosion) {
			json_t* sexp_obj = json_object();
			json_object_set_new(sexp_obj, "damage", json_integer(shipp->special_exp_damage));
			json_object_set_new(sexp_obj, "blast", json_integer(shipp->special_exp_blast));
			json_object_set_new(sexp_obj, "inner_radius", json_integer(shipp->special_exp_inner));
			json_object_set_new(sexp_obj, "outer_radius", json_integer(shipp->special_exp_outer));
			if (shipp->use_shockwave && (shipp->special_exp_shockwave_speed > 0))
				json_object_set_new(sexp_obj, "shockwave_speed", json_integer(shipp->special_exp_shockwave_speed));
			if (shipp->special_exp_deathroll_time > 0)
				json_object_set_new(sexp_obj, "deathroll_time", json_integer(shipp->special_exp_deathroll_time));
			json_object_set_new(obj, "special_explosion", sexp_obj);
		}

		// --- Special hitpoints and shield ---
		if (shipp->special_hitpoints)
			json_object_set_new(obj, "special_hitpoints", json_integer(shipp->special_hitpoints));
		if (shipp->special_shield >= 0)
			json_object_set_new(obj, "special_shield", json_integer(shipp->special_shield));

		// --- Kamikaze damage ---
		if (Ai_info[shipp->ai_index].ai_flags[AI::AI_Flags::Kamikaze])
			json_object_set_new(obj, "kamikaze_damage", json_integer(Ai_info[shipp->ai_index].kamikaze_damage));

		// --- Hotkey ---
		if (shipp->hotkey >= 0)
			json_object_set_new(obj, "hotkey", json_integer(shipp->hotkey));

		// --- Dock list ---
		if (object_is_docked(objp)) {
			json_t* dock_arr = json_array();

			if (dock_check_docked_one_on_one(objp)) {
				// retail-style: only save for non-leaders
				if (!(shipp->flags[Ship::Ship_Flags::Dock_leader])) {
					json_array_append_new(dock_arr, save_single_dock_instance_json(shipp, objp->dock_list));
				}
			} else {
				// multiply docked: save all instances
				for (dock_instance* dock_ptr = objp->dock_list; dock_ptr != nullptr; dock_ptr = dock_ptr->next) {
					json_array_append_new(dock_arr, save_single_dock_instance_json(shipp, dock_ptr));
				}
			}

			if (json_array_size(dock_arr) > 0)
				json_object_set_new(obj, "dock_list", dock_arr);
			else
				json_decref(dock_arr);
		}

		// --- Destroy at ---
		if (shipp->flags[Ship::Ship_Flags::Kill_before_mission])
			json_object_set_new(obj, "destroy_at", json_integer(shipp->final_death_time));

		// --- Orders accepted ---
		if (shipp->orders_accepted != ship_get_default_orders_accepted(sip)) {
			json_t* orders_arr = json_array();
			for (size_t order_id : shipp->orders_accepted) {
				const auto& order = Player_orders[order_id];
				json_array_append_new(orders_arr, json_string(order.parse_name.c_str()));
			}
			json_object_set_new(obj, "orders_accepted", orders_arr);
		}

		// --- Group ---
		if (shipp->group >= 0)
			json_object_set_new(obj, "group", json_integer(shipp->group));

		// --- Score ---
		if (sip->score == shipp->score) {
			json_object_set_new(obj, "use_table_score", json_true());
		}
		json_object_set_new(obj, "score", json_integer(shipp->score));

		if (shipp->assist_score_pct != 0.0f)
			json_object_set_new(obj, "assist_score_pct", json_real(shipp->assist_score_pct));

		// --- Persona ---
		if (shipp->persona_index >= 0 && shipp->persona_index < static_cast<int>(Personas.size()))
			json_object_set_new(obj, "persona", json_string(Personas[shipp->persona_index].name));

		// --- Texture replacements ---
		if (!Fred_texture_replacements.empty()) {
			json_t* tex_arr = json_array();

			for (const auto& tr : Fred_texture_replacements) {
				if (!stricmp(shipp->ship_name, tr.ship_name) && !tr.from_table) {
					json_t* tex = json_object();
					json_object_set_new(tex, "old_texture", json_string(tr.old_texture));
					json_object_set_new(tex, "new_texture", json_string(tr.new_texture));
					json_array_append_new(tex_arr, tex);
				}
			}

			if (json_array_size(tex_arr) > 0)
				json_object_set_new(obj, "replacement_textures", tex_arr);
			else
				json_decref(tex_arr);
		}

		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_wings_json()
{
	json_t* arr = json_array();

	for (int i = 0; i < Num_wings; i++) {
		wing& w = Wings[i];
		json_t* obj = json_object();

		json_object_set_new(obj, "name", json_string(w.name));

		if (strlen(w.wing_squad_filename) > 0)
			json_object_set_new(obj, "squad_logo", json_string(w.wing_squad_filename));

		json_object_set_new(obj, "num_waves", json_integer(w.num_waves));
		json_object_set_new(obj, "wave_threshold", json_integer(w.threshold));
		json_object_set_new(obj, "special_ship", json_integer(w.special_ship));

		if (w.formation >= 0)
			json_object_set_new(obj, "formation", json_integer(w.formation));
		if (w.formation_scale != 1.0f)
			json_object_set_new(obj, "formation_scale", json_real(w.formation_scale));

		// Ships in this wing
		json_t* ships = json_array();
		for (int j = 0; j < w.wave_count; j++) {
			if (w.ship_index[j] >= 0) {
				json_array_append_new(ships, json_string(Ships[w.ship_index[j]].ship_name));
			}
		}
		json_object_set_new(obj, "ships", ships);

		// Arrival / Departure
		json_object_set_new(obj, "arrival_location", json_string(arrival_location_name(w.arrival_location)));
		if (w.arrival_distance != 0)
			json_object_set_new(obj, "arrival_distance", json_integer(w.arrival_distance));
		if (w.arrival_anchor >= 0)
			json_object_set_new(obj, "arrival_anchor", anchor_to_json(w.arrival_anchor));
		json_object_set_new(obj, "arrival_cue", mission_json::sexp_to_json(w.arrival_cue));
		if (w.arrival_delay != 0)
			json_object_set_new(obj, "arrival_delay", json_integer(w.arrival_delay));
		if (w.arrival_path_mask != 0)
			json_object_set_new(obj, "arrival_path_mask", json_integer(w.arrival_path_mask));

		json_object_set_new(obj, "departure_location", json_string(departure_location_name(w.departure_location)));
		if (w.departure_anchor >= 0)
			json_object_set_new(obj, "departure_anchor", anchor_to_json(w.departure_anchor));
		json_object_set_new(obj, "departure_cue", mission_json::sexp_to_json(w.departure_cue));
		if (w.departure_delay != 0)
			json_object_set_new(obj, "departure_delay", json_integer(w.departure_delay));
		if (w.departure_path_mask != 0)
			json_object_set_new(obj, "departure_path_mask", json_integer(w.departure_path_mask));

		// AI goals
		json_t* goals = save_ai_goals_json(w.ai_goals);
		if (json_array_size(goals) > 0)
			json_object_set_new(obj, "ai_goals", goals);
		else
			json_decref(goals);

		if (w.hotkey >= 0)
			json_object_set_new(obj, "hotkey", json_integer(w.hotkey));

		// Wave delay
		if (w.wave_delay_min > 0)
			json_object_set_new(obj, "wave_delay_min", json_integer(w.wave_delay_min));
		if (w.wave_delay_max > 0)
			json_object_set_new(obj, "wave_delay_max", json_integer(w.wave_delay_max));

		// Wing flags
		{
			json_t* wflags = json_array();
			for (size_t fi = 0; fi < Num_wing_flag_names; fi++) {
				if (w.flags[Wing_flag_names[fi].flag]) {
					json_array_append_new(wflags, json_string(Wing_flag_names[fi].flag_name));
				}
			}
			if (json_array_size(wflags) > 0)
				json_object_set_new(obj, "flags", wflags);
			else
				json_decref(wflags);
		}

		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_props_json()
{
	json_t* arr = json_array();
	for (const auto& prop : Parse_props) {
		json_t* obj = json_object();
		json_object_set_new(obj, "name", json_string(prop.name));
		if (prop.prop_info_index >= 0)
			json_object_set_new(obj, "class", json_string(Prop_info[prop.prop_info_index].name.c_str()));
		json_object_set_new(obj, "position", mission_json::vec3d_to_json(prop.position));
		json_object_set_new(obj, "orientation", mission_json::matrix_to_json(prop.orientation));
		json_object_set_new(obj, "flags", save_parse_object_flags_json(prop.flags));
		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_events_json()
{
	json_t* arr = json_array();

	for (const auto& evt : Mission_events) {
		json_t* obj = json_object();
		json_object_set_new(obj, "name", json_string(evt.name.c_str()));
		json_object_set_new(obj, "formula", mission_json::sexp_to_json(evt.formula));
		json_object_set_new(obj, "repeat_count", json_integer(evt.repeat_count));
		json_object_set_new(obj, "trigger_count", json_integer(evt.trigger_count));
		json_object_set_new(obj, "interval", json_integer(evt.interval));

		if (evt.score != 0)
			json_object_set_new(obj, "score", json_integer(evt.score));
		if (evt.chain_delay >= 0)
			json_object_set_new(obj, "chain_delay", json_integer(evt.chain_delay));
		if (!evt.objective_text.empty())
			json_object_set_new(obj, "objective_text", json_string(evt.objective_text.c_str()));
		if (!evt.objective_key_text.empty())
			json_object_set_new(obj, "objective_key_text", json_string(evt.objective_key_text.c_str()));
		if (evt.team >= 0)
			json_object_set_new(obj, "team", json_integer(evt.team));
		if (evt.flags != 0)
			json_object_set_new(obj, "event_flags", json_integer(evt.flags));
		if (evt.mission_log_flags != 0)
			json_object_set_new(obj, "mission_log_flags", json_integer(evt.mission_log_flags));

		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_goals_json()
{
	json_t* arr = json_array();

	for (const auto& goal : Mission_goals) {
		json_t* obj = json_object();
		json_object_set_new(obj, "name", json_string(goal.name.c_str()));

		const char* type_str = "primary";
		switch (goal.type & GOAL_TYPE_MASK) {
			case PRIMARY_GOAL:   type_str = "primary"; break;
			case SECONDARY_GOAL: type_str = "secondary"; break;
			case BONUS_GOAL:     type_str = "bonus"; break;
		}
		json_object_set_new(obj, "type", json_string(type_str));
		json_object_set_new(obj, "formula", mission_json::sexp_to_json(goal.formula));

		if (!goal.message.empty())
			json_object_set_new(obj, "message", json_string(goal.message.c_str()));
		if (goal.score != 0)
			json_object_set_new(obj, "score", json_integer(goal.score));
		if (goal.flags != 0)
			json_object_set_new(obj, "flags", json_integer(goal.flags));
		if (goal.team != 0)
			json_object_set_new(obj, "team", json_integer(goal.team));

		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_waypoints_json()
{
	json_t* obj = json_object();

	// Jump nodes
	json_t* jn_arr = json_array();
	for (const auto& jn : Jump_nodes) {
		json_t* node = json_object();
		json_object_set_new(node, "name", json_string(jn.GetName()));
		json_object_set_new(node, "position", mission_json::vec3d_to_json(*jn.GetPosition()));
		if (strlen(jn.GetDisplayName()) > 0 && strcmp(jn.GetName(), jn.GetDisplayName()) != 0)
			json_object_set_new(node, "display_name", json_string(jn.GetDisplayName()));
		json_array_append_new(jn_arr, node);
	}
	json_object_set_new(obj, "jump_nodes", jn_arr);

	// Waypoint lists
	json_t* wp_arr = json_array();
	for (const auto& wpl : Waypoint_lists) {
		json_t* list = json_object();
		json_object_set_new(list, "name", json_string(wpl.get_name()));

		json_t* points = json_array();
		for (const auto& wp : wpl.get_waypoints()) {
			json_array_append_new(points, mission_json::vec3d_to_json(*wp.get_pos()));
		}
		json_object_set_new(list, "points", points);
		json_array_append_new(wp_arr, list);
	}
	json_object_set_new(obj, "waypoint_lists", wp_arr);

	return obj;
}

json_t* save_messages_json()
{
	json_t* result = json_object();

	// Command sender and persona (mission-wide, mirroring save_messages() in missionsave.cpp)
	if (stricmp(The_mission.command_sender, DEFAULT_COMMAND))
		json_object_set_new(result, "command_sender", json_string(The_mission.command_sender));

	if (The_mission.command_persona != Default_command_persona)
		json_object_set_new(result, "command_persona", json_string(Personas[The_mission.command_persona].name));

	// Message list
	json_t* arr = json_array();
	for (int i = Num_builtin_messages; i < static_cast<int>(Messages.size()); i++) {
		const MMessage& msg = Messages[i];
		json_t* obj = json_object();
		json_object_set_new(obj, "name", json_string(msg.name));

		json_object_set_new(obj, "team", json_integer(
			((msg.multi_team < 0) || (msg.multi_team >= 2)) ? -1 : msg.multi_team));

		json_object_set_new(obj, "message", json_string(msg.message));

		if (msg.persona_index >= 0 && msg.persona_index < static_cast<int>(Personas.size()))
			json_object_set_new(obj, "persona", json_string(Personas[msg.persona_index].name));

		if (msg.avi_info.name)
			json_object_set_new(obj, "avi_name", json_string(msg.avi_info.name));
		if (msg.wave_info.name)
			json_object_set_new(obj, "wave_name", json_string(msg.wave_info.name));

		if (!msg.note.empty())
			json_object_set_new(obj, "note", json_string(msg.note.c_str()));

		json_array_append_new(arr, obj);
	}
	json_object_set_new(result, "messages", arr);

	return result;
}

json_t* save_reinforcements_json()
{
	json_t* arr = json_array();

	for (int i = 0; i < Num_reinforcements; i++) {
		const reinforcements& r = Reinforcements[i];
		json_t* obj = json_object();
		json_object_set_new(obj, "name", json_string(r.name));
		json_object_set_new(obj, "type", json_integer(r.type));
		json_object_set_new(obj, "uses", json_integer(r.uses));
		json_object_set_new(obj, "arrival_delay", json_integer(r.arrival_delay));

		json_t* no_msgs = json_array();
		for (int j = 0; j < MAX_REINFORCEMENT_MESSAGES; j++) {
			if (strlen(r.no_messages[j]) > 0)
				json_array_append_new(no_msgs, json_string(r.no_messages[j]));
		}
		if (json_array_size(no_msgs) > 0)
			json_object_set_new(obj, "no_messages", no_msgs);
		else
			json_decref(no_msgs);

		json_t* yes_msgs = json_array();
		for (int j = 0; j < MAX_REINFORCEMENT_MESSAGES; j++) {
			if (strlen(r.yes_messages[j]) > 0)
				json_array_append_new(yes_msgs, json_string(r.yes_messages[j]));
		}
		if (json_array_size(yes_msgs) > 0)
			json_object_set_new(obj, "yes_messages", yes_msgs);
		else
			json_decref(yes_msgs);

		json_array_append_new(arr, obj);
	}

	return arr;
}

json_t* save_bitmaps_json()
{
	json_t* obj = json_object();

	json_object_set_new(obj, "ambient_light_level", json_integer(The_mission.ambient_light_level));

	if (strlen(The_mission.envmap_name) > 0)
		json_object_set_new(obj, "environment_map", json_string(The_mission.envmap_name));

	// Backgrounds
	json_t* bg_arr = json_array();
	for (const auto& bg : Backgrounds) {
		json_t* bg_obj = json_object();

		json_t* suns = json_array();
		for (const auto& sun : bg.suns) {
			json_t* s = json_object();
			json_object_set_new(s, "filename", json_string(sun.filename));
			json_object_set_new(s, "angles", mission_json::angles_to_json(sun.ang));
			json_object_set_new(s, "scale_x", json_real(sun.scale_x));
			json_object_set_new(s, "scale_y", json_real(sun.scale_y));
			json_array_append_new(suns, s);
		}
		json_object_set_new(bg_obj, "suns", suns);

		json_t* bitmaps = json_array();
		for (const auto& bm : bg.bitmaps) {
			json_t* b = json_object();
			json_object_set_new(b, "filename", json_string(bm.filename));
			json_object_set_new(b, "angles", mission_json::angles_to_json(bm.ang));
			json_object_set_new(b, "scale_x", json_real(bm.scale_x));
			json_object_set_new(b, "scale_y", json_real(bm.scale_y));
			json_object_set_new(b, "div_x", json_integer(bm.div_x));
			json_object_set_new(b, "div_y", json_integer(bm.div_y));
			json_array_append_new(bitmaps, b);
		}
		json_object_set_new(bg_obj, "bitmaps", bitmaps);

		json_array_append_new(bg_arr, bg_obj);
	}
	json_object_set_new(obj, "backgrounds", bg_arr);

	return obj;
}

json_t* save_asteroid_fields_json()
{
	json_t* obj = json_object();
	const asteroid_field& af = Asteroid_field;

	if (af.num_initial_asteroids <= 0)
		return obj;

	json_object_set_new(obj, "num_initial_asteroids", json_integer(af.num_initial_asteroids));
	json_object_set_new(obj, "field_type", json_integer(static_cast<int>(af.field_type)));
	json_object_set_new(obj, "debris_genre", json_integer(static_cast<int>(af.debris_genre)));

	json_t* debris_types = json_array();
	for (int dt : af.field_debris_type) {
		json_array_append_new(debris_types, json_integer(dt));
	}
	json_object_set_new(obj, "field_debris_type", debris_types);

	json_t* asteroid_names = json_array();
	for (const auto& name : af.field_asteroid_type) {
		json_array_append_new(asteroid_names, json_string(name.c_str()));
	}
	json_object_set_new(obj, "field_asteroid_type", asteroid_names);

	json_object_set_new(obj, "speed", json_real(af.speed));
	json_object_set_new(obj, "min_bound", mission_json::vec3d_to_json(af.min_bound));
	json_object_set_new(obj, "max_bound", mission_json::vec3d_to_json(af.max_bound));

	if (af.has_inner_bound) {
		json_object_set_new(obj, "has_inner_bound", json_true());
		json_object_set_new(obj, "inner_min_bound", mission_json::vec3d_to_json(af.inner_min_bound));
		json_object_set_new(obj, "inner_max_bound", mission_json::vec3d_to_json(af.inner_max_bound));
	}

	if (af.enhanced_visibility_checks)
		json_object_set_new(obj, "enhanced_visibility_checks", json_true());

	if (!af.target_names.empty()) {
		json_t* targets = json_array();
		for (const auto& t : af.target_names) {
			json_array_append_new(targets, json_string(t.c_str()));
		}
		json_object_set_new(obj, "target_names", targets);
	}

	return obj;
}

json_t* save_music_json()
{
	json_t* obj = json_object();

	if (strlen(The_mission.event_music_name) > 0)
		json_object_set_new(obj, "event_music", json_string(The_mission.event_music_name));
	if (strlen(The_mission.briefing_music_name) > 0)
		json_object_set_new(obj, "briefing_music", json_string(The_mission.briefing_music_name));
	if (strlen(The_mission.substitute_event_music_name) > 0)
		json_object_set_new(obj, "substitute_event_music", json_string(The_mission.substitute_event_music_name));
	if (strlen(The_mission.substitute_briefing_music_name) > 0)
		json_object_set_new(obj, "substitute_briefing_music", json_string(The_mission.substitute_briefing_music_name));

	return obj;
}

json_t* save_custom_data_json()
{
	json_t* obj = json_object();

	if (!The_mission.custom_data.empty()) {
		json_t* data = json_object();
		for (const auto& pair : The_mission.custom_data) {
			json_object_set_new(data, pair.first.c_str(), json_string(pair.second.c_str()));
		}
		json_object_set_new(obj, "data", data);
	}

	if (!The_mission.custom_strings.empty()) {
		json_t* strings = json_array();
		for (const auto& cs : The_mission.custom_strings) {
			json_t* s = json_object();
			json_object_set_new(s, "name", json_string(cs.name.c_str()));
			json_object_set_new(s, "value", json_string(cs.value.c_str()));
			json_object_set_new(s, "text", json_string(cs.text.c_str()));
			json_array_append_new(strings, s);
		}
		json_object_set_new(obj, "strings", strings);
	}

	return obj;
}

} // anonymous namespace

// ============================================================
// Public save entry point
// ============================================================

int mission_json::save(const char* pathname, const FredSaveConfig& config)
{
	json_save_config = &config;

	json_t* root = json_object();
	json_object_set_new(root, "format_version", json_integer(FORMAT_VERSION));

	json_object_set_new(root, "mission_info", save_mission_info_json());
	json_object_set_new(root, "variables", save_variables_json());
	json_object_set_new(root, "containers", save_containers_json());
	json_object_set_new(root, "cutscenes", save_cutscenes_json());
	json_object_set_new(root, "fiction", save_fiction_json());
	json_object_set_new(root, "command_briefings", save_cmd_briefs_json());
	json_object_set_new(root, "briefing", save_briefing_json());
	json_object_set_new(root, "debriefing", save_debriefing_json());
	json_object_set_new(root, "players", save_players_json());
	json_object_set_new(root, "objects", save_objects_json());
	json_object_set_new(root, "wings", save_wings_json());
	json_object_set_new(root, "props", save_props_json());
	json_object_set_new(root, "events", save_events_json());
	json_object_set_new(root, "goals", save_goals_json());
	json_object_set_new(root, "waypoints", save_waypoints_json());
	json_object_set_new(root, "messages", save_messages_json());
	json_object_set_new(root, "reinforcements", save_reinforcements_json());
	json_object_set_new(root, "background_bitmaps", save_bitmaps_json());
	json_object_set_new(root, "asteroid_fields", save_asteroid_fields_json());
	json_object_set_new(root, "music", save_music_json());
	json_object_set_new(root, "custom_data", save_custom_data_json());

	CFILE* fp = cfopen(pathname, "wt", CF_TYPE_MISSIONS);
	if (!fp) {
		json_decref(root);
		mprintf(("mission_json::save - Failed to open '%s' for writing\n", pathname));
		json_save_config = nullptr;
		return -1;
	}

	int result = json_dump_cfile(root, fp, JSON_INDENT(2));
	cfclose(fp);
	json_decref(root);

	if (result != 0) {
		mprintf(("mission_json::save - Failed to write JSON to '%s'\n", pathname));
		json_save_config = nullptr;
		return -2;
	}

	json_save_config = nullptr;
	return 0;
}
