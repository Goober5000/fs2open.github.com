#include "stdafx.h"
#include "mcp_reference_tools.h"
#include "mcpserver.h"

#include <jansson.h>
#include <climits>
#include <cstring>

#include "ship/ship.h"
#include "weapon/weapon.h"
#include "species_defs/species_defs.h"
#include "parse/sexp.h"
#include "menuui/techmenu.h"
#include "iff_defs/iff_defs.h"

std::atomic<bool> mcp_tables_ready{false};

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// Build an MCP tool-result whose text content is pretty-printed JSON.
// Takes ownership of `data` (decrefs it after serializing).
static json_t *make_json_tool_result(json_t *data)
{
	char *text = json_dumps(data, JSON_INDENT(2));
	json_t *result = make_tool_result(text);
	free(text);
	json_decref(data);
	return result;
}

// Convenience: add a string property schema to a properties object.
static void add_string_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("string"));
	json_object_set_new(p, "description", json_string(description));
	json_object_set_new(props, name, p);
}

// Helper: set a string field only if the source is non-null and non-empty.
static void set_optional_string(json_t *obj, const char *key, const char *value)
{
	if (value && value[0] != '\0')
		json_object_set_new(obj, key, json_string(value));
}

// ---------------------------------------------------------------------------
// OPF / OPR enum-to-string helpers
// ---------------------------------------------------------------------------

static const char *opf_to_string(int opf)
{
	switch (opf) {
		case OPF_NONE:                         return "none";
		case OPF_NULL:                         return "void";
		case OPF_BOOL:                         return "boolean";
		case OPF_NUMBER:                       return "number";
		case OPF_SHIP:                         return "ship";
		case OPF_WING:                         return "wing";
		case OPF_SUBSYSTEM:                    return "subsystem";
		case OPF_POINT:                        return "point";
		case OPF_IFF:                          return "iff";
		case OPF_AI_GOAL:                      return "ai_goal";
		case OPF_DOCKER_POINT:                 return "docker_point";
		case OPF_DOCKEE_POINT:                 return "dockee_point";
		case OPF_MESSAGE:                      return "message";
		case OPF_WHO_FROM:                     return "who_from";
		case OPF_PRIORITY:                     return "priority";
		case OPF_WAYPOINT_PATH:                return "waypoint_path";
		case OPF_POSITIVE:                     return "positive_number";
		case OPF_MISSION_NAME:                 return "mission_name";
		case OPF_SHIP_POINT:                   return "ship_or_waypoint";
		case OPF_GOAL_NAME:                    return "goal_name";
		case OPF_SHIP_WING:                    return "ship_or_wing";
		case OPF_SHIP_WING_WHOLETEAM:          return "ship_or_wing_or_team";
		case OPF_SHIP_WING_SHIPONTEAM_POINT:   return "ship_or_wing_or_team_or_waypoint";
		case OPF_SHIP_WING_POINT:              return "ship_or_wing_or_waypoint";
		case OPF_SHIP_WING_POINT_OR_NONE:      return "ship_or_wing_or_waypoint_or_none";
		case OPF_SHIP_TYPE:                    return "ship_type";
		case OPF_KEYPRESS:                     return "keypress";
		case OPF_EVENT_NAME:                   return "event_name";
		case OPF_AI_ORDER:                     return "ai_order";
		case OPF_SKILL_LEVEL:                  return "skill_level";
		case OPF_MEDAL_NAME:                   return "medal_name";
		case OPF_WEAPON_NAME:                  return "weapon_name";
		case OPF_SHIP_CLASS_NAME:              return "ship_class_name";
		case OPF_CUSTOM_HUD_GAUGE:             return "custom_hud_gauge";
		case OPF_HUGE_WEAPON:                  return "huge_weapon";
		case OPF_SHIP_NOT_PLAYER:              return "ship_not_player";
		case OPF_JUMP_NODE_NAME:               return "jump_node_name";
		case OPF_VARIABLE_NAME:                return "variable_name";
		case OPF_AMBIGUOUS:                    return "ambiguous";
		case OPF_AWACS_SUBSYSTEM:              return "awacs_subsystem";
		case OPF_CARGO:                        return "cargo";
		case OPF_AI_CLASS:                     return "ai_class";
		case OPF_SUPPORT_SHIP_CLASS:           return "support_ship_class";
		case OPF_ARRIVAL_LOCATION:             return "arrival_location";
		case OPF_ARRIVAL_ANCHOR_ALL:           return "arrival_anchor";
		case OPF_DEPARTURE_LOCATION:           return "departure_location";
		case OPF_SHIP_WITH_BAY:                return "ship_with_bay";
		case OPF_SOUNDTRACK_NAME:              return "soundtrack_name";
		case OPF_INTEL_NAME:                   return "intel_name";
		case OPF_STRING:                       return "string";
		case OPF_ROTATING_SUBSYSTEM:           return "rotating_subsystem";
		case OPF_NAV_POINT:                    return "nav_point";
		case OPF_SSM_CLASS:                    return "ssm_class";
		case OPF_FLEXIBLE_ARGUMENT:            return "flexible_argument";
		case OPF_ANYTHING:                     return "anything";
		case OPF_SKYBOX_MODEL_NAME:            return "skybox_model_name";
		case OPF_SHIP_OR_NONE:                 return "ship_or_none";
		case OPF_BACKGROUND_BITMAP:            return "background_bitmap";
		case OPF_SUN_BITMAP:                   return "sun_bitmap";
		case OPF_NEBULA_STORM_TYPE:            return "nebula_storm_type";
		case OPF_NEBULA_POOF:                  return "nebula_poof";
		case OPF_TURRET_TARGET_ORDER:          return "turret_target_order";
		case OPF_SUBSYSTEM_OR_NONE:            return "subsystem_or_none";
		case OPF_PERSONA:                      return "persona";
		case OPF_SUBSYS_OR_GENERIC:            return "subsystem_or_generic";
		case OPF_ORDER_RECIPIENT:              return "order_recipient";
		case OPF_SUBSYSTEM_TYPE:               return "subsystem_type";
		case OPF_POST_EFFECT:                  return "post_effect";
		case OPF_TARGET_PRIORITIES:            return "target_priorities";
		case OPF_ARMOR_TYPE:                   return "armor_type";
		case OPF_FONT:                         return "font";
		case OPF_HUD_ELEMENT:                  return "hud_element";
		case OPF_SOUND_ENVIRONMENT:            return "sound_environment";
		case OPF_SOUND_ENVIRONMENT_OPTION:     return "sound_environment_option";
		case OPF_EXPLOSION_OPTION:             return "explosion_option";
		case OPF_AUDIO_VOLUME_OPTION:          return "audio_volume_option";
		case OPF_WEAPON_BANK_NUMBER:           return "weapon_bank_number";
		case OPF_MESSAGE_OR_STRING:            return "message_or_string";
		case OPF_BUILTIN_HUD_GAUGE:            return "builtin_hud_gauge";
		case OPF_DAMAGE_TYPE:                  return "damage_type";
		case OPF_SHIP_EFFECT:                  return "ship_effect";
		case OPF_ANIMATION_TYPE:               return "animation_type";
		case OPF_MISSION_MOOD:                 return "mission_mood";
		case OPF_SHIP_FLAG:                    return "ship_flag";
		case OPF_TEAM_COLOR:                   return "team_color";
		case OPF_NEBULA_PATTERN:               return "nebula_pattern";
		case OPF_SKYBOX_FLAGS:                 return "skybox_flags";
		case OPF_GAME_SND:                     return "game_sound";
		case OPF_FIREBALL:                     return "fireball";
		case OPF_SPECIES:                      return "species";
		case OPF_LANGUAGE:                     return "language";
		case OPF_FUNCTIONAL_WHEN_EVAL_TYPE:    return "functional_when_eval_type";
		case OPF_CONTAINER_NAME:               return "container_name";
		case OPF_LIST_CONTAINER_NAME:          return "list_container_name";
		case OPF_MAP_CONTAINER_NAME:           return "map_container_name";
		case OPF_ANIMATION_NAME:               return "animation_name";
		case OPF_CONTAINER_VALUE:              return "container_value";
		case OPF_DATA_OR_STR_CONTAINER:        return "data_or_string_container";
		case OPF_TRANSLATING_SUBSYSTEM:        return "translating_subsystem";
		case OPF_ANY_HUD_GAUGE:                return "any_hud_gauge";
		case OPF_WING_FLAG:                    return "wing_flag";
		case OPF_ASTEROID_TYPES:               return "asteroid_type";
		case OPF_DEBRIS_TYPES:                 return "debris_type";
		case OPF_WING_FORMATION:               return "wing_formation";
		case OPF_MOTION_DEBRIS:                return "motion_debris";
		case OPF_TURRET_TYPE:                  return "turret_type";
		case OPF_BOLT_TYPE:                    return "bolt_type";
		case OPF_TRAITOR_OVERRIDE:             return "traitor_override";
		case OPF_LUA_GENERAL_ORDER:            return "lua_general_order";
		case OPF_CHILD_LUA_ENUM:               return "child_lua_enum";
		case OPF_MISSION_CUSTOM_STRING:        return "mission_custom_string";
		case OPF_MESSAGE_TYPE:                 return "message_type";
		case OPF_PROP:                         return "prop";
		case OPF_SHIP_PROP:                    return "ship_or_prop";
		case OPF_PROP_CLASS_NAME:              return "prop_class_name";
		default: return "unknown";
	}
}

static const char *opr_to_string(int opr)
{
	switch (opr) {
		case OPR_NONE:               return "none";
		case OPR_NUMBER:             return "number";
		case OPR_BOOL:               return "boolean";
		case OPR_NULL:               return "void";
		case OPR_AI_GOAL:            return "ai_goal";
		case OPR_POSITIVE:           return "positive_number";
		case OPR_STRING:             return "string";
		case OPR_AMBIGUOUS:          return "ambiguous";
		case OPR_FLEXIBLE_ARGUMENT:  return "flexible_argument";
		default: return "unknown";
	}
}

static const char *sexp_oper_type_str(sexp_oper_type t)
{
	switch (t) {
		case sexp_oper_type::NONE:         return "none";
		case sexp_oper_type::CONDITIONAL:  return "conditional";
		case sexp_oper_type::ARGUMENT:     return "argument";
		case sexp_oper_type::ACTION:       return "action";
		case sexp_oper_type::ARITHMETIC:   return "arithmetic";
		case sexp_oper_type::BOOLEAN:      return "boolean";
		case sexp_oper_type::INTEGER:      return "integer";
		case sexp_oper_type::GOAL:         return "goal";
		default: return "unknown";
	}
}

static const char *weapon_subtype_str(int subtype)
{
	switch (subtype) {
		case WP_LASER:   return "primary";
		case WP_MISSILE: return "secondary";
		case WP_BEAM:    return "beam";
		default: return "unknown";
	}
}

// ---------------------------------------------------------------------------
// Tool schema registration
// ---------------------------------------------------------------------------

// Helper: build a tool schema object and append it to the tools array.
static void register_tool(json_t *tools, const char *name, const char *description, json_t *properties, json_t *required_arr = nullptr)
{
	json_t *tool = json_object();
	json_object_set_new(tool, "name", json_string(name));
	json_object_set_new(tool, "description", json_string(description));

	json_t *schema = json_object();
	json_object_set_new(schema, "type", json_string("object"));
	json_object_set_new(schema, "properties", properties ? properties : json_object());
	if (required_arr)
		json_object_set_new(schema, "required", required_arr);
	json_object_set_new(tool, "inputSchema", schema);

	json_array_append_new(tools, tool);
}

void mcp_register_reference_tools(json_t *tools)
{
	// list_ship_types
	register_tool(tools, "list_ship_types",
		"List all ship types (e.g. fighter, bomber, cruiser, capital). "
		"Ship types are abstract categories; individual ship classes belong to a type.",
		json_object());

	// get_ship_type
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship type (e.g. \"fighter\")");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "get_ship_type",
			"Get detailed information about a ship type, including AI behavior flags.",
			props, req);
	}

	// list_ship_classes
	{
		json_t *props = json_object();
		add_string_prop(props, "species", "Filter by species name (e.g. \"Terran\")");
		add_string_prop(props, "type", "Filter by ship type name (e.g. \"fighter\")");
		register_tool(tools, "list_ship_classes",
			"List all ship classes with summary info. Optionally filter by species and/or ship type.",
			props);
	}

	// get_ship_class
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship class (e.g. \"GTF Ulysses\")");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "get_ship_class",
			"Get detailed stats for a specific ship class, including hull, shields, speed, weapons, and subsystems.",
			props, req);
	}

	// list_weapon_classes
	{
		json_t *props = json_object();
		add_string_prop(props, "subtype", "Filter by subtype: \"primary\", \"secondary\", or \"beam\"");
		register_tool(tools, "list_weapon_classes",
			"List all weapon classes with summary info. Optionally filter by subtype.",
			props);
	}

	// get_weapon_class
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the weapon (e.g. \"Subach HL-7\")");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "get_weapon_class",
			"Get detailed stats for a specific weapon, including damage, speed, range, and special properties.",
			props, req);
	}

	// list_species
	register_tool(tools, "list_species",
		"List all species defined in the game (e.g. Terran, Vasudan, Shivan).",
		json_object());

	// list_intel_entries
	register_tool(tools, "list_intel_entries",
		"List all intel/tech database entries. These contain universe lore and descriptions.",
		json_object());

	// get_intel_entry
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the intel entry");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "get_intel_entry",
			"Get the full text of an intel/tech database entry, including its description and custom data.",
			props, req);
	}

	// list_sexp_operators
	{
		json_t *props = json_object();
		add_string_prop(props, "category", "Filter by category name (e.g. \"Objective\", \"Change\", \"Status\", \"Conditional\")");
		add_string_prop(props, "search", "Substring search against operator names");
		register_tool(tools, "list_sexp_operators",
			"List SEXP (S-expression) operators used in mission event logic. "
			"Optionally filter by category and/or name substring.",
			props);
	}

	// get_sexp_operator
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the SEXP operator (e.g. \"is-destroyed\")");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "get_sexp_operator",
			"Get full details of a SEXP operator, including help text, argument types, return type, and category.",
			props, req);
	}
}

// ---------------------------------------------------------------------------
// Tool handlers
// ---------------------------------------------------------------------------

static json_t *handle_list_ship_types()
{
	json_t *arr = json_array();

	for (size_t i = 0; i < Ship_types.size(); i++) {
		const auto &st = Ship_types[i];
		json_t *item = json_object();
		json_object_set_new(item, "name", json_string(st.name));

		// Resolve actively-pursues to names
		json_t *pursues = json_array();
		for (int idx : st.ai_actively_pursues) {
			if (idx >= 0 && idx < (int)Ship_types.size())
				json_array_append_new(pursues, json_string(Ship_types[idx].name));
		}
		json_object_set_new(item, "actively_pursues", pursues);

		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_get_ship_type(json_t *arguments)
{
	json_t *name_val = arguments ? json_object_get(arguments, "name") : nullptr;
	const char *name = (name_val && json_is_string(name_val)) ? json_string_value(name_val) : nullptr;
	if (!name || name[0] == '\0')
		return make_tool_result("Missing required parameter: name", true);

	int idx = ship_type_name_lookup(name);
	if (idx < 0)
		return make_tool_result("Ship type not found", true);

	const auto &st = Ship_types[idx];
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(st.name));

	// Flags
	json_t *flags = json_object();
	json_object_set_new(flags, "praise_destruction", json_boolean(st.flags[Ship::Type_Info_Flags::Praise_destruction]));
	json_object_set_new(flags, "target_as_threat", json_boolean(st.flags[Ship::Type_Info_Flags::Target_as_threat]));
	json_object_set_new(flags, "ai_accept_player_orders", json_boolean(st.flags[Ship::Type_Info_Flags::AI_accept_player_orders]));
	json_object_set_new(flags, "ai_auto_attacks", json_boolean(st.flags[Ship::Type_Info_Flags::AI_auto_attacks]));
	json_object_set_new(flags, "ai_attempt_broadside", json_boolean(st.flags[Ship::Type_Info_Flags::AI_attempt_broadside]));
	json_object_set_new(flags, "ai_guards_attack", json_boolean(st.flags[Ship::Type_Info_Flags::AI_guards_attack]));
	json_object_set_new(flags, "ai_can_form_wing", json_boolean(st.flags[Ship::Type_Info_Flags::AI_can_form_wing]));
	json_object_set_new(flags, "ai_protected_on_cripple", json_boolean(st.flags[Ship::Type_Info_Flags::AI_protected_on_cripple]));
	json_object_set_new(obj, "flags", flags);

	// AI actively pursues
	json_t *pursues = json_array();
	for (int pidx : st.ai_actively_pursues) {
		if (pidx >= 0 && pidx < (int)Ship_types.size())
			json_array_append_new(pursues, json_string(Ship_types[pidx].name));
	}
	json_object_set_new(obj, "ai_actively_pursues", pursues);

	// AI cripple ignores
	json_t *ignores = json_array();
	for (int cidx : st.ai_cripple_ignores) {
		if (cidx >= 0 && cidx < (int)Ship_types.size())
			json_array_append_new(ignores, json_string(Ship_types[cidx].name));
	}
	json_object_set_new(obj, "ai_cripple_ignores", ignores);

	return make_json_tool_result(obj);
}

static json_t *handle_list_ship_classes(json_t *arguments)
{
	// Optional filters
	const char *filter_species = nullptr;
	const char *filter_type = nullptr;
	if (arguments) {
		json_t *v = json_object_get(arguments, "species");
		if (v && json_is_string(v))
			filter_species = json_string_value(v);
		v = json_object_get(arguments, "type");
		if (v && json_is_string(v))
			filter_type = json_string_value(v);
	}

	int filter_species_idx = -1;
	if (filter_species) {
		filter_species_idx = species_info_lookup(filter_species);
		if (filter_species_idx < 0)
			return make_tool_result("Species not found", true);
	}

	int filter_type_idx = -1;
	if (filter_type) {
		filter_type_idx = ship_type_name_lookup(filter_type);
		if (filter_type_idx < 0)
			return make_tool_result("Ship type not found", true);
	}

	json_t *arr = json_array();
	for (int i = 0; i < ship_info_size(); i++) {
		const auto &sip = Ship_info[i];

		if (filter_species_idx >= 0 && sip.species != filter_species_idx)
			continue;
		if (filter_type_idx >= 0 && sip.class_type != filter_type_idx)
			continue;

		json_t *item = json_object();
		json_object_set_new(item, "name", json_string(sip.name));
		if (sip.has_display_name())
			json_object_set_new(item, "display_name", json_string(sip.get_display_name()));

		if (sip.species >= 0 && sip.species < (int)Species_info.size())
			json_object_set_new(item, "species", json_string(Species_info[sip.species].species_name));

		if (sip.class_type >= 0 && sip.class_type < (int)Ship_types.size())
			json_object_set_new(item, "type", json_string(Ship_types[sip.class_type].name));

		json_object_set_new(item, "in_tech_database", json_boolean(sip.flags[Ship::Info_Flags::In_tech_database]));

		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_get_ship_class(json_t *arguments)
{
	json_t *name_val = arguments ? json_object_get(arguments, "name") : nullptr;
	const char *name = (name_val && json_is_string(name_val)) ? json_string_value(name_val) : nullptr;
	if (!name || name[0] == '\0')
		return make_tool_result("Missing required parameter: name", true);

	int idx = ship_info_lookup(name);
	if (idx < 0)
		return make_tool_result("Ship class not found", true);

	const auto &sip = Ship_info[idx];
	json_t *obj = json_object();

	// Identity
	json_object_set_new(obj, "name", json_string(sip.name));
	if (sip.has_display_name())
		json_object_set_new(obj, "display_name", json_string(sip.get_display_name()));

	if (sip.species >= 0 && sip.species < (int)Species_info.size())
		json_object_set_new(obj, "species", json_string(Species_info[sip.species].species_name));
	if (sip.class_type >= 0 && sip.class_type < (int)Ship_types.size())
		json_object_set_new(obj, "type", json_string(Ship_types[sip.class_type].name));

	// Descriptive strings
	set_optional_string(obj, "type_str", sip.type_str.get());
	set_optional_string(obj, "manufacturer", sip.manufacturer_str.get());
	set_optional_string(obj, "description", sip.desc.get());
	set_optional_string(obj, "tech_description", sip.tech_desc.get());
	set_optional_string(obj, "armor", sip.armor_str.get());
	set_optional_string(obj, "maneuverability", sip.maneuverability_str.get());
	set_optional_string(obj, "ship_length", sip.ship_length.get());
	set_optional_string(obj, "gun_mounts", sip.gun_mounts.get());
	set_optional_string(obj, "missile_banks", sip.missile_banks.get());

	// Physics
	{
		json_t *vel = json_object();
		json_object_set_new(vel, "x", json_real(sip.max_vel.xyz.x));
		json_object_set_new(vel, "y", json_real(sip.max_vel.xyz.y));
		json_object_set_new(vel, "z", json_real(sip.max_vel.xyz.z));
		json_object_set_new(obj, "max_velocity", vel);
	}
	json_object_set_new(obj, "max_afterburner_velocity", json_real(sip.afterburner_max_vel.xyz.z));
	json_object_set_new(obj, "max_rear_velocity", json_real(sip.max_rear_vel));

	// Durability
	json_object_set_new(obj, "max_hull_strength", json_real(sip.max_hull_strength));
	json_object_set_new(obj, "max_shield_strength", json_real(sip.max_shield_strength));

	// Energy
	json_object_set_new(obj, "power_output", json_real(sip.power_output));
	json_object_set_new(obj, "max_weapon_reserve", json_real(sip.max_weapon_reserve));

	// Afterburner
	json_object_set_new(obj, "afterburner_fuel_capacity", json_real(sip.afterburner_fuel_capacity));

	// Weapons
	json_object_set_new(obj, "num_primary_banks", json_integer(sip.num_primary_banks));
	{
		json_t *primaries = json_array();
		for (int b = 0; b < sip.num_primary_banks; b++) {
			int wi = sip.primary_bank_weapons[b];
			if (wi >= 0 && wi < weapon_info_size())
				json_array_append_new(primaries, json_string(Weapon_info[wi].name));
			else
				json_array_append_new(primaries, json_string("(none)"));
		}
		json_object_set_new(obj, "default_primary_weapons", primaries);
	}

	json_object_set_new(obj, "num_secondary_banks", json_integer(sip.num_secondary_banks));
	{
		json_t *secondaries = json_array();
		for (int b = 0; b < sip.num_secondary_banks; b++) {
			int wi = sip.secondary_bank_weapons[b];
			if (wi >= 0 && wi < weapon_info_size())
				json_array_append_new(secondaries, json_string(Weapon_info[wi].name));
			else
				json_array_append_new(secondaries, json_string("(none)"));
		}
		json_object_set_new(obj, "default_secondary_weapons", secondaries);
	}

	// Countermeasures
	if (sip.cmeasure_type >= 0 && sip.cmeasure_type < weapon_info_size())
		json_object_set_new(obj, "countermeasure_type", json_string(Weapon_info[sip.cmeasure_type].name));
	json_object_set_new(obj, "countermeasure_max", json_integer(sip.cmeasure_max));

	// Score
	json_object_set_new(obj, "score", json_integer(sip.score));

	// Subsystems
	{
		json_t *subsys = json_array();
		for (int s = 0; s < sip.n_subsystems; s++) {
			json_array_append_new(subsys, json_string(sip.subsystems[s].name));
		}
		json_object_set_new(obj, "subsystems", subsys);
	}

	// Classification helpers
	json_object_set_new(obj, "is_small_ship", json_boolean(sip.is_small_ship()));
	json_object_set_new(obj, "is_big_ship", json_boolean(sip.is_big_ship()));
	json_object_set_new(obj, "is_huge_ship", json_boolean(sip.is_huge_ship()));
	json_object_set_new(obj, "is_flyable", json_boolean(sip.is_flyable()));
	json_object_set_new(obj, "is_fighter_bomber", json_boolean(sip.is_fighter_bomber()));
	json_object_set_new(obj, "in_tech_database", json_boolean(sip.flags[Ship::Info_Flags::In_tech_database]));

	return make_json_tool_result(obj);
}

static json_t *handle_list_weapon_classes(json_t *arguments)
{
	// Optional subtype filter
	int filter_subtype = -1;
	if (arguments) {
		json_t *v = json_object_get(arguments, "subtype");
		if (v && json_is_string(v)) {
			const char *st = json_string_value(v);
			if (stricmp(st, "primary") == 0)
				filter_subtype = WP_LASER;
			else if (stricmp(st, "secondary") == 0)
				filter_subtype = WP_MISSILE;
			else if (stricmp(st, "beam") == 0)
				filter_subtype = WP_BEAM;
			else
				return make_tool_result("Invalid subtype. Use \"primary\", \"secondary\", or \"beam\".", true);
		}
	}

	json_t *arr = json_array();
	for (int i = 0; i < weapon_info_size(); i++) {
		const auto &wip = Weapon_info[i];

		if (filter_subtype >= 0 && wip.subtype != filter_subtype)
			continue;

		json_t *item = json_object();
		json_object_set_new(item, "name", json_string(wip.name));
		if (wip.has_display_name())
			json_object_set_new(item, "display_name", json_string(wip.get_display_name()));
		json_object_set_new(item, "subtype", json_string(weapon_subtype_str(wip.subtype)));
		set_optional_string(item, "title", wip.title);
		json_object_set_new(item, "in_tech_database", json_boolean(wip.wi_flags[Weapon::Info_Flags::In_tech_database]));

		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_get_weapon_class(json_t *arguments)
{
	json_t *name_val = arguments ? json_object_get(arguments, "name") : nullptr;
	const char *name = (name_val && json_is_string(name_val)) ? json_string_value(name_val) : nullptr;
	if (!name || name[0] == '\0')
		return make_tool_result("Missing required parameter: name", true);

	int idx = weapon_info_lookup(name);
	if (idx < 0)
		return make_tool_result("Weapon class not found", true);

	const auto &wip = Weapon_info[idx];
	json_t *obj = json_object();

	// Identity
	json_object_set_new(obj, "name", json_string(wip.name));
	if (wip.has_display_name())
		json_object_set_new(obj, "display_name", json_string(wip.get_display_name()));
	json_object_set_new(obj, "subtype", json_string(weapon_subtype_str(wip.subtype)));
	set_optional_string(obj, "title", wip.title);
	set_optional_string(obj, "description", wip.desc.get());
	set_optional_string(obj, "tech_description", wip.tech_desc.get());

	// Performance
	json_object_set_new(obj, "damage", json_real(wip.damage));
	json_object_set_new(obj, "armor_factor", json_real(wip.armor_factor));
	json_object_set_new(obj, "shield_factor", json_real(wip.shield_factor));
	json_object_set_new(obj, "subsystem_factor", json_real(wip.subsystem_factor));
	json_object_set_new(obj, "max_speed", json_real(wip.max_speed));
	json_object_set_new(obj, "lifetime", json_real(wip.lifetime));
	json_object_set_new(obj, "fire_wait", json_real(wip.fire_wait));
	json_object_set_new(obj, "weapon_range", json_real(wip.weapon_range));
	json_object_set_new(obj, "optimum_range", json_real(wip.optimum_range));

	// Missile-specific
	if (wip.subtype == WP_MISSILE) {
		json_object_set_new(obj, "cargo_size", json_real(wip.cargo_size));
		json_object_set_new(obj, "is_homing", json_boolean(wip.is_homing()));
		json_object_set_new(obj, "is_locked_homing", json_boolean(wip.is_locked_homing()));
		if (wip.is_homing()) {
			json_object_set_new(obj, "min_lock_time", json_real(wip.min_lock_time));
			json_object_set_new(obj, "seeker_strength", json_real(wip.seeker_strength));
		}
		json_object_set_new(obj, "swarm_count", json_integer(wip.swarm_count));
	}

	// Beam
	json_object_set_new(obj, "is_beam", json_boolean(wip.is_beam()));

	// EMP
	if (wip.emp_intensity > 0.0f) {
		json_object_set_new(obj, "emp_intensity", json_real(wip.emp_intensity));
		json_object_set_new(obj, "emp_time", json_real(wip.emp_time));
	}

	// Shockwave
	if (wip.shockwave.speed > 0.0f) {
		json_t *sw = json_object();
		json_object_set_new(sw, "speed", json_real(wip.shockwave.speed));
		json_object_set_new(sw, "inner_radius", json_real(wip.shockwave.inner_rad));
		json_object_set_new(sw, "outer_radius", json_real(wip.shockwave.outer_rad));
		json_object_set_new(sw, "damage", json_real(wip.shockwave.damage));
		json_object_set_new(obj, "shockwave", sw);
	}

	// Flags
	json_object_set_new(obj, "hurts_big_ships", json_boolean(wip.hurts_big_ships()));
	json_object_set_new(obj, "in_tech_database", json_boolean(wip.wi_flags[Weapon::Info_Flags::In_tech_database]));

	return make_json_tool_result(obj);
}

static json_t *handle_list_species()
{
	json_t *arr = json_array();

	for (size_t i = 0; i < Species_info.size(); i++) {
		const auto &sp = Species_info[i];
		json_t *item = json_object();
		json_object_set_new(item, "name", json_string(sp.species_name));

		if (sp.default_iff >= 0 && sp.default_iff < (int)Iff_info.size())
			json_object_set_new(item, "default_iff", json_string(Iff_info[sp.default_iff].iff_name));

		json_object_set_new(item, "awacs_multiplier", json_real(sp.awacs_multiplier));
		set_optional_string(item, "countermeasure", sp.cmeasure_name);
		set_optional_string(item, "support_ship", sp.support_ship_name);

		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_list_intel_entries()
{
	json_t *arr = json_array();

	for (int i = 0; i < intel_info_size(); i++) {
		const auto &entry = Intel_info[i];
		json_t *item = json_object();
		json_object_set_new(item, "name", json_string(entry.name));
		json_object_set_new(item, "in_tech_database", json_boolean((entry.flags & IIF_IN_TECH_DATABASE) != 0));

		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_get_intel_entry(json_t *arguments)
{
	json_t *name_val = arguments ? json_object_get(arguments, "name") : nullptr;
	const char *name = (name_val && json_is_string(name_val)) ? json_string_value(name_val) : nullptr;
	if (!name || name[0] == '\0')
		return make_tool_result("Missing required parameter: name", true);

	int idx = intel_info_lookup(name);
	if (idx < 0)
		return make_tool_result("Intel entry not found", true);

	const auto &entry = Intel_info[idx];
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(entry.name));
	json_object_set_new(obj, "description", json_string(entry.desc.c_str()));
	json_object_set_new(obj, "in_tech_database", json_boolean((entry.flags & IIF_IN_TECH_DATABASE) != 0));
	json_object_set_new(obj, "default_in_tech_database", json_boolean((entry.flags & IIF_DEFAULT_IN_TECH_DATABASE) != 0));

	// Custom data
	if (!entry.custom_data.empty()) {
		json_t *cd = json_object();
		for (const auto &kv : entry.custom_data) {
			json_object_set_new(cd, kv.first.c_str(), json_string(kv.second.c_str()));
		}
		json_object_set_new(obj, "custom_data", cd);
	}

	return make_json_tool_result(obj);
}

static json_t *handle_list_sexp_operators(json_t *arguments)
{
	const char *filter_category = nullptr;
	const char *filter_search = nullptr;
	if (arguments) {
		json_t *v = json_object_get(arguments, "category");
		if (v && json_is_string(v))
			filter_category = json_string_value(v);
		v = json_object_get(arguments, "search");
		if (v && json_is_string(v))
			filter_search = json_string_value(v);
	}

	// Resolve category name to ID if filtering
	int filter_category_id = -1;
	if (filter_category) {
		for (size_t i = 0; i < op_menu.size(); i++) {
			if (stricmp(op_menu[i].name.c_str(), filter_category) == 0) {
				filter_category_id = op_menu[i].id;
				break;
			}
		}
		if (filter_category_id < 0)
			return make_tool_result("Category not found. Use list_sexp_operators without a category filter to see valid categories.", true);
	}

	json_t *arr = json_array();
	for (size_t i = 0; i < Operators.size(); i++) {
		const auto &op = Operators[i];

		// Category filter
		if (filter_category_id >= 0) {
			int cat = get_category(op.value);
			if (cat != filter_category_id)
				continue;
		}

		// Search filter (case-insensitive substring)
		if (filter_search && filter_search[0] != '\0') {
			SCP_string op_lower = op.text;
			SCP_string search_lower = filter_search;
			SCP_tolower(op_lower);
			SCP_tolower(search_lower);
			if (op_lower.find(search_lower) == SCP_string::npos)
				continue;
		}

		json_t *item = json_object();
		json_object_set_new(item, "name", json_string(op.text.c_str()));
		json_object_set_new(item, "min_args", json_integer(op.min));
		json_object_set_new(item, "max_args", json_integer(op.max == INT_MAX ? -1 : op.max));

		int cat_id = get_category(op.value);
		const char *cat_name = get_category_name(cat_id);
		if (cat_name)
			json_object_set_new(item, "category", json_string(cat_name));

		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_get_sexp_operator(json_t *arguments)
{
	json_t *name_val = arguments ? json_object_get(arguments, "name") : nullptr;
	const char *name = (name_val && json_is_string(name_val)) ? json_string_value(name_val) : nullptr;
	if (!name || name[0] == '\0')
		return make_tool_result("Missing required parameter: name", true);

	// Find operator by name
	int op_index = -1;
	for (size_t i = 0; i < Operators.size(); i++) {
		if (stricmp(Operators[i].text.c_str(), name) == 0) {
			op_index = static_cast<int>(i);
			break;
		}
	}
	if (op_index < 0)
		return make_tool_result("SEXP operator not found", true);

	const auto &op = Operators[op_index];
	json_t *obj = json_object();

	json_object_set_new(obj, "name", json_string(op.text.c_str()));
	json_object_set_new(obj, "type", json_string(sexp_oper_type_str(op.type)));
	json_object_set_new(obj, "min_args", json_integer(op.min));
	json_object_set_new(obj, "max_args", json_integer(op.max == INT_MAX ? -1 : op.max));

	// Category and subcategory
	int cat_id = get_category(op.value);
	const char *cat_name = get_category_name(cat_id);
	if (cat_name)
		json_object_set_new(obj, "category", json_string(cat_name));

	int subcat_id = get_subcategory(op.value);
	if (subcat_id != OP_SUBCATEGORY_NONE) {
		// Find subcategory name in op_submenu
		for (size_t i = 0; i < op_submenu.size(); i++) {
			if (op_submenu[i].id == subcat_id) {
				json_object_set_new(obj, "subcategory", json_string(op_submenu[i].name.c_str()));
				break;
			}
		}
	}

	// Return type
	int ret = query_operator_return_type(op.value);
	json_object_set_new(obj, "return_type", json_string(opr_to_string(ret)));

	// Help text
	for (size_t i = 0; i < Sexp_help.size(); i++) {
		if (Sexp_help[i].id == op.value) {
			json_object_set_new(obj, "help", json_string(Sexp_help[i].help.c_str()));
			break;
		}
	}

	// Argument types — enumerate up to max args, capped at 20 for variadic operators
	{
		int max_to_check = op.max;
		if (max_to_check == INT_MAX || max_to_check > 20)
			max_to_check = 20;
		if (max_to_check < op.min)
			max_to_check = op.min;

		json_t *arg_types = json_array();
		int last_type = OPF_NONE;
		for (int a = 0; a < max_to_check; a++) {
			int atype = query_operator_argument_type(op.value, a);
			if (atype == OPF_NONE && a >= op.min)
				break;
			json_array_append_new(arg_types, json_string(opf_to_string(atype)));
			last_type = atype;
		}

		// If variadic and we stopped early, note the repeating type
		if (op.max == INT_MAX && last_type != OPF_NONE) {
			json_object_set_new(obj, "variadic_arg_type", json_string(opf_to_string(last_type)));
		}

		json_object_set_new(obj, "argument_types", arg_types);
	}

	return make_json_tool_result(obj);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

json_t *mcp_handle_reference_tool(const char *tool_name, json_t *arguments)
{
	if (!tool_name)
		return nullptr;

	// Check if tables are loaded
	if (!mcp_tables_ready.load()) {
		// Only intercept if the name matches one of our tools
		static const char *our_tools[] = {
			"list_ship_types", "get_ship_type",
			"list_ship_classes", "get_ship_class",
			"list_weapon_classes", "get_weapon_class",
			"list_species",
			"list_intel_entries", "get_intel_entry",
			"list_sexp_operators", "get_sexp_operator",
			nullptr
		};
		for (const char **t = our_tools; *t; t++) {
			if (strcmp(tool_name, *t) == 0)
				return make_tool_result("Game tables are not yet loaded. Please wait for FRED2 to finish initializing.", true);
		}
		return nullptr;
	}

	if (strcmp(tool_name, "list_ship_types") == 0)
		return handle_list_ship_types();
	if (strcmp(tool_name, "get_ship_type") == 0)
		return handle_get_ship_type(arguments);
	if (strcmp(tool_name, "list_ship_classes") == 0)
		return handle_list_ship_classes(arguments);
	if (strcmp(tool_name, "get_ship_class") == 0)
		return handle_get_ship_class(arguments);
	if (strcmp(tool_name, "list_weapon_classes") == 0)
		return handle_list_weapon_classes(arguments);
	if (strcmp(tool_name, "get_weapon_class") == 0)
		return handle_get_weapon_class(arguments);
	if (strcmp(tool_name, "list_species") == 0)
		return handle_list_species();
	if (strcmp(tool_name, "list_intel_entries") == 0)
		return handle_list_intel_entries();
	if (strcmp(tool_name, "get_intel_entry") == 0)
		return handle_get_intel_entry(arguments);
	if (strcmp(tool_name, "list_sexp_operators") == 0)
		return handle_list_sexp_operators(arguments);
	if (strcmp(tool_name, "get_sexp_operator") == 0)
		return handle_get_sexp_operator(arguments);

	return nullptr;  // not one of our tools
}
