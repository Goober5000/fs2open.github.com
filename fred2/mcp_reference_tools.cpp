#include "stdafx.h"
#include "mcp_reference_tools.h"
#include "mcpserver.h"

#include <jansson.h>
#include <climits>
#include <cstring>

#include "mainfrm.h"
#include "ship/ship.h"
#include "weapon/weapon.h"
#include "species_defs/species_defs.h"
#include "parse/sexp.h"
#include "parse/sexp/sexp_lookup.h"
#include "menuui/techmenu.h"
#include "iff_defs/iff_defs.h"
#include "cfile/cfile.h"
#include "def_files/def_files.h"


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
		case OPF_NULL:                         return "null";
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
		case OPR_NULL:               return "null";
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

// Returns the effective weapon category.  Most beam weapons have subtype
// WP_LASER (because they appear in #Primary Weapons), so we use is_beam()
// rather than relying on the raw subtype field.
static const char *weapon_category_str(const weapon_info &wip)
{
	if (wip.is_beam())
		return "beam";
	if (wip.is_secondary())
		return "secondary";
	return "primary";
}

static const char *subsystem_type_str(int type)
{
	switch (type) {
		case SUBSYSTEM_ENGINE:        return "engine";
		case SUBSYSTEM_TURRET:        return "turret";
		case SUBSYSTEM_RADAR:         return "radar";
		case SUBSYSTEM_NAVIGATION:    return "navigation";
		case SUBSYSTEM_COMMUNICATION: return "communication";
		case SUBSYSTEM_WEAPONS:       return "weapons";
		case SUBSYSTEM_SENSORS:       return "sensors";
		case SUBSYSTEM_SOLAR:         return "solar";
		case SUBSYSTEM_GAS_COLLECT:   return "gas_collect";
		case SUBSYSTEM_ACTIVATION:    return "activation";
		case SUBSYSTEM_UNKNOWN:       return "unknown";
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
			"Get detailed stats for a specific ship class, including hull, shields, "
			"speed, weapons, subsystems, and per-bank weapon restrictions.",
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

	// list_sexp_categories
	register_tool(tools, "list_sexp_categories",
		"List all SEXP operator categories and their subcategories, with operator "
		"counts per category. Use this to discover the category hierarchy before "
		"listing operators. Includes 'Unlisted' (hidden operators) and "
		"'Uncategorized' categories if any such operators exist.",
		json_object());

	// list_sexp_operators
	{
		json_t *props = json_object();
		add_string_prop(props, "category", "Filter by category name (e.g. \"Objectives\", \"Change\", \"Status\", \"Conditional\"). Use list_sexp_categories to see valid names.");
		add_string_prop(props, "subcategory", "Filter by subcategory name (requires category to also be specified)");
		add_string_prop(props, "search", "Substring search against operator names");
		register_tool(tools, "list_sexp_operators",
			"List SEXP (S-expression) operators used in mission event logic. "
			"Optionally filter by category, subcategory, and/or name substring. "
			"max_args is -1 for variadic (unlimited argument) operators.",
			props);
	}

	// get_sexp_operator
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the SEXP operator (e.g. \"is-destroyed\")");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "get_sexp_operator",
			"Get full details of a SEXP operator, including help text, argument types, "
			"return type, category, and whether it is a dynamic (mod-provided) SEXP. "
			"max_args is -1 for variadic operators; for these, variadic_arg_type "
			"indicates the repeating argument type.",
			props, req);
	}

	// get_mod_info
	register_tool(tools, "get_mod_info",
		"Get information about the current mod, including its setting, factions, "
		"lore, and mission design conventions. Returns Markdown. Call this early "
		"in a session to understand the context you are working in.",
		json_object());

	// get_reference_notes
	{
		json_t *props = json_object();
		add_string_prop(props, "topic", "Topic to look up (omit to list all available topics)");
		register_tool(tools, "get_reference_notes",
			"Get explanatory notes about FreeSpace game concepts and domain knowledge "
			"that may not be obvious from raw data alone. Call without arguments to "
			"list all available topics.",
			props);
	}

	// get_sexp_argument_type
	{
		json_t *props = json_object();
		add_string_prop(props, "name",
			"Argument type name as returned by get_sexp_operator (e.g. \"ship\", \"ship_or_wing\"). "
			"Omit to list all known argument types.");
		register_tool(tools, "get_sexp_argument_type",
			"Get details about a SEXP argument type, including what values it accepts. "
			"Call without arguments to list all known argument types.",
			props);
	}

	// get_sexp_return_type
	{
		json_t *props = json_object();
		add_string_prop(props, "name",
			"Return type name as returned by get_sexp_operator (e.g. \"boolean\", \"number\"). "
			"Omit to list all known return types.");
		register_tool(tools, "get_sexp_return_type",
			"Get details about a SEXP return type, including what argument types it is compatible with. "
			"Call without arguments to list all known return types.",
			props);
	}

	// get_ship_model_details
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship class (e.g. \"GTD Orion\")");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "get_ship_model_details",
			"Get 3D model details for a ship class, including bounding box dimensions, "
			"docking bays, and navigation paths (for subsystem attack, docking, and "
			"fighter bay arrival/departure). Note: if the model is not already loaded, "
			"this tool may take several seconds to respond while the model is loaded "
			"into memory.",
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

	// Weapons — per-bank details
	{
		json_t *banks = json_array();
		for (int b = 0; b < sip.num_primary_banks; b++) {
			json_t *bank_obj = json_object();
			json_object_set_new(bank_obj, "index", json_integer(b));

			int wi = sip.primary_bank_weapons[b];
			if (wi >= 0 && wi < weapon_info_size())
				json_object_set_new(bank_obj, "default_weapon", json_string(Weapon_info[wi].name));
			else
				json_object_set_new(bank_obj, "default_weapon", json_string("(none)"));

			json_object_set_new(bank_obj, "ammo_capacity", json_integer(sip.primary_bank_ammo_capacity[b]));

			// Per-bank restrictions (primaries use indices 0..MAX_SHIP_PRIMARY_BANKS-1)
			bool has_regular = (b < (int)sip.restricted_loadout_flag.size())
				&& (sip.restricted_loadout_flag[b] & REGULAR_WEAPON);
			json_object_set_new(bank_obj, "restricted", json_boolean(has_regular));
			if (has_regular && b < (int)sip.allowed_bank_restricted_weapons.size()) {
				json_t *rw = json_array();
				for (const auto &wf : sip.allowed_bank_restricted_weapons[b].weapon_and_flags) {
					if ((wf.second & REGULAR_WEAPON) && wf.first >= 0 && wf.first < weapon_info_size())
						json_array_append_new(rw, json_string(Weapon_info[wf.first].name));
				}
				json_object_set_new(bank_obj, "allowed_weapons", rw);
			}

			bool has_dogfight = (b < (int)sip.restricted_loadout_flag.size())
				&& (sip.restricted_loadout_flag[b] & DOGFIGHT_WEAPON);
			json_object_set_new(bank_obj, "restricted_dogfight", json_boolean(has_dogfight));
			if (has_dogfight && b < (int)sip.allowed_bank_restricted_weapons.size()) {
				json_t *rw = json_array();
				for (const auto &wf : sip.allowed_bank_restricted_weapons[b].weapon_and_flags) {
					if ((wf.second & DOGFIGHT_WEAPON) && wf.first >= 0 && wf.first < weapon_info_size())
						json_array_append_new(rw, json_string(Weapon_info[wf.first].name));
				}
				json_object_set_new(bank_obj, "allowed_dogfight_weapons", rw);
			}

			json_array_append_new(banks, bank_obj);
		}
		json_object_set_new(obj, "primary_banks", banks);
	}

	{
		json_t *banks = json_array();
		for (int b = 0; b < sip.num_secondary_banks; b++) {
			json_t *bank_obj = json_object();
			json_object_set_new(bank_obj, "index", json_integer(b));

			int wi = sip.secondary_bank_weapons[b];
			if (wi >= 0 && wi < weapon_info_size())
				json_object_set_new(bank_obj, "default_weapon", json_string(Weapon_info[wi].name));
			else
				json_object_set_new(bank_obj, "default_weapon", json_string("(none)"));

			json_object_set_new(bank_obj, "ammo_capacity", json_integer(sip.secondary_bank_ammo_capacity[b]));

			// Per-bank restrictions (secondaries use indices MAX_SHIP_PRIMARY_BANKS + b)
			int ri = MAX_SHIP_PRIMARY_BANKS + b;
			bool has_regular = (ri < (int)sip.restricted_loadout_flag.size())
				&& (sip.restricted_loadout_flag[ri] & REGULAR_WEAPON);
			json_object_set_new(bank_obj, "restricted", json_boolean(has_regular));
			if (has_regular && ri < (int)sip.allowed_bank_restricted_weapons.size()) {
				json_t *rw = json_array();
				for (const auto &wf : sip.allowed_bank_restricted_weapons[ri].weapon_and_flags) {
					if ((wf.second & REGULAR_WEAPON) && wf.first >= 0 && wf.first < weapon_info_size())
						json_array_append_new(rw, json_string(Weapon_info[wf.first].name));
				}
				json_object_set_new(bank_obj, "allowed_weapons", rw);
			}

			bool has_dogfight = (ri < (int)sip.restricted_loadout_flag.size())
				&& (sip.restricted_loadout_flag[ri] & DOGFIGHT_WEAPON);
			json_object_set_new(bank_obj, "restricted_dogfight", json_boolean(has_dogfight));
			if (has_dogfight && ri < (int)sip.allowed_bank_restricted_weapons.size()) {
				json_t *rw = json_array();
				for (const auto &wf : sip.allowed_bank_restricted_weapons[ri].weapon_and_flags) {
					if ((wf.second & DOGFIGHT_WEAPON) && wf.first >= 0 && wf.first < weapon_info_size())
						json_array_append_new(rw, json_string(Weapon_info[wf.first].name));
				}
				json_object_set_new(bank_obj, "allowed_dogfight_weapons", rw);
			}

			json_array_append_new(banks, bank_obj);
		}
		json_object_set_new(obj, "secondary_banks", banks);
	}

	// Global allowed weapons
	{
		json_t *aw = json_array();
		json_t *daw = json_array();
		for (const auto &wf : sip.allowed_weapons.weapon_and_flags) {
			if (wf.first >= 0 && wf.first < weapon_info_size()) {
				if (wf.second & REGULAR_WEAPON)
					json_array_append_new(aw, json_string(Weapon_info[wf.first].name));
				if (wf.second & DOGFIGHT_WEAPON)
					json_array_append_new(daw, json_string(Weapon_info[wf.first].name));
			}
		}
		if (json_array_size(aw) > 0)
			json_object_set_new(obj, "allowed_weapons", aw);
		else
			json_decref(aw);
		if (json_array_size(daw) > 0)
			json_object_set_new(obj, "allowed_dogfight_weapons", daw);
		else
			json_decref(daw);
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
			const auto &ss = sip.subsystems[s];
			json_t *ss_obj = json_object();

			// Use subobj_name (from table parsing); the 'name' field is only
			// populated after the model is loaded.
			json_object_set_new(ss_obj, "name", json_string(ss.subobj_name));
			if (ss.type != SUBSYSTEM_NONE)
				json_object_set_new(ss_obj, "type", json_string(subsystem_type_str(ss.type)));
			json_object_set_new(ss_obj, "max_hitpoints", json_real(ss.max_subsys_strength));

			// Turret-specific info — check type if model is loaded, or
			// check for weapons/turning rate from table data
			bool has_turret_data = (ss.type == SUBSYSTEM_TURRET) ||
				(ss.primary_banks[0] >= 0) || (ss.secondary_banks[0] >= 0) ||
				(ss.turret_turning_rate > 0.0f);

			if (has_turret_data) {
				if (ss.turret_num_firing_points > 0)
					json_object_set_new(ss_obj, "turret_num_firing_points", json_integer(ss.turret_num_firing_points));
				if (ss.turret_turning_rate > 0.0f)
					json_object_set_new(ss_obj, "turret_turning_rate", json_real(ss.turret_turning_rate));

				// Convert FOV from dot-product to degrees for readability
				if (ss.turret_fov > -1.0f && ss.turret_fov < 1.0f) {
					float fov_deg = acosf(ss.turret_fov) * (180.0f / PI);
					json_object_set_new(ss_obj, "turret_fov_degrees", json_real(fov_deg));
				}

				// Turret primary weapons
				json_t *t_pri = json_array();
				for (int b = 0; b < MAX_SHIP_PRIMARY_BANKS; b++) {
					if (ss.primary_banks[b] >= 0 && ss.primary_banks[b] < weapon_info_size())
						json_array_append_new(t_pri, json_string(Weapon_info[ss.primary_banks[b]].name));
				}
				if (json_array_size(t_pri) > 0)
					json_object_set_new(ss_obj, "primary_weapons", t_pri);
				else
					json_decref(t_pri);

				// Turret secondary weapons
				json_t *t_sec = json_array();
				for (int b = 0; b < MAX_SHIP_SECONDARY_BANKS; b++) {
					if (ss.secondary_banks[b] >= 0 && ss.secondary_banks[b] < weapon_info_size())
						json_array_append_new(t_sec, json_string(Weapon_info[ss.secondary_banks[b]].name));
				}
				if (json_array_size(t_sec) > 0)
					json_object_set_new(ss_obj, "secondary_weapons", t_sec);
				else
					json_decref(t_sec);
			}

			// AWACS info
			if (ss.awacs_intensity > 0.0f) {
				json_object_set_new(ss_obj, "awacs_intensity", json_real(ss.awacs_intensity));
				json_object_set_new(ss_obj, "awacs_radius", json_real(ss.awacs_radius));
			}

			json_array_append_new(subsys, ss_obj);
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
	// Optional category filter.  We use the semantic helpers (is_beam(),
	// is_secondary(), is_non_beam_primary()) instead of the raw subtype field,
	// because most beam weapons have subtype WP_LASER rather than WP_BEAM.
	enum { FILTER_NONE, FILTER_PRIMARY, FILTER_SECONDARY, FILTER_BEAM } filter = FILTER_NONE;
	if (arguments) {
		json_t *v = json_object_get(arguments, "subtype");
		if (v && json_is_string(v)) {
			const char *st = json_string_value(v);
			if (stricmp(st, "primary") == 0)
				filter = FILTER_PRIMARY;
			else if (stricmp(st, "secondary") == 0)
				filter = FILTER_SECONDARY;
			else if (stricmp(st, "beam") == 0)
				filter = FILTER_BEAM;
			else
				return make_tool_result("Invalid subtype. Use \"primary\", \"secondary\", or \"beam\".", true);
		}
	}

	json_t *arr = json_array();
	for (int i = 0; i < weapon_info_size(); i++) {
		const auto &wip = Weapon_info[i];

		if (filter == FILTER_PRIMARY && !wip.is_non_beam_primary())
			continue;
		if (filter == FILTER_SECONDARY && !wip.is_secondary())
			continue;
		if (filter == FILTER_BEAM && !wip.is_beam())
			continue;

		json_t *item = json_object();
		json_object_set_new(item, "name", json_string(wip.name));
		if (wip.has_display_name())
			json_object_set_new(item, "display_name", json_string(wip.get_display_name()));
		json_object_set_new(item, "subtype", json_string(weapon_category_str(wip)));
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
	json_object_set_new(obj, "subtype", json_string(weapon_category_str(wip)));
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
	if (wip.is_secondary()) {
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

static const char *mcp_category_name(int category_id)
{
	if (category_id == OP_CATEGORY_NONE)
		return "Uncategorized";
	return get_category_name(category_id);
}

static const char *get_category_description(int category_id)
{
	switch (category_id) {
		case OP_CATEGORY_OBJECTIVE:    return "Conditions for mission goals (is-destroyed, is-departed, etc.)";
		case OP_CATEGORY_TIME:         return "Time-related checks and delays";
		case OP_CATEGORY_LOGICAL:      return "Boolean logic (and, or, not, etc.)";
		case OP_CATEGORY_ARITHMETIC:   return "Math operations (+, -, *, /, etc.)";
		case OP_CATEGORY_STATUS:       return "Query game state (shield strength, distance, etc.)";
		case OP_CATEGORY_CHANGE:       return "Modify game state (set hull, change AI, etc.)";
		case OP_CATEGORY_CONDITIONAL:  return "Control flow (when, if-then-else, etc.)";
		case OP_CATEGORY_AI:           return "AI orders and goals";
		case OP_CATEGORY_GOAL_EVENT:   return "Operators used inside event and goal definitions";
		case OP_CATEGORY_TRAINING:     return "Training mission specific operators";
		case OP_CATEGORY_UNLISTED:     return "Hidden operators which are not shown to mission designers, either because they are internal operators or because they are only used in campaign files";
		case OP_CATEGORY_NONE:         return "Operators without an assigned category";
		default:                       return nullptr;
	}
}

static json_t *handle_list_sexp_categories()
{
	json_t *arr = json_array();

	for (size_t m = 0; m < op_menu.size(); m++) {
		json_t *cat = json_object();
		json_object_set_new(cat, "name", json_string(op_menu[m].name.c_str()));

		const char *desc = get_category_description(op_menu[m].id);
		if (desc)
			json_object_set_new(cat, "description", json_string(desc));

		// Find subcategories belonging to this category
		json_t *subcats = json_array();
		for (size_t s = 0; s < op_submenu.size(); s++) {
			if (category_of_subcategory(op_submenu[s].id) == op_menu[m].id)
				json_array_append_new(subcats, json_string(op_submenu[s].name.c_str()));
		}
		json_object_set_new(cat, "subcategories", subcats);

		// Count operators in this category
		int count = 0;
		for (size_t i = 0; i < Operators.size(); i++) {
			if (get_category(Operators[i].value) == op_menu[m].id)
				count++;
		}
		json_object_set_new(cat, "operator_count", json_integer(count));

		json_array_append_new(arr, cat);
	}

	// Add extra categories not in op_menu (only if they have operators)
	static const int extra_categories[] = { OP_CATEGORY_NONE };
	for (int extra_id : extra_categories) {
		int count = 0;
		for (size_t i = 0; i < Operators.size(); i++) {
			if (get_category(Operators[i].value) == extra_id)
				count++;
		}
		if (count > 0) {
			json_t *cat = json_object();
			json_object_set_new(cat, "name", json_string(mcp_category_name(extra_id)));

			const char *desc = get_category_description(extra_id);
			if (desc)
				json_object_set_new(cat, "description", json_string(desc));

			json_object_set_new(cat, "subcategories", json_array());
			json_object_set_new(cat, "operator_count", json_integer(count));
			json_array_append_new(arr, cat);
		}
	}

	return make_json_tool_result(arr);
}

static json_t *handle_list_sexp_operators(json_t *arguments)
{
	const char *filter_category = nullptr;
	const char *filter_subcategory = nullptr;
	const char *filter_search = nullptr;
	if (arguments) {
		json_t *v = json_object_get(arguments, "category");
		if (v && json_is_string(v))
			filter_category = json_string_value(v);
		v = json_object_get(arguments, "subcategory");
		if (v && json_is_string(v))
			filter_subcategory = json_string_value(v);
		v = json_object_get(arguments, "search");
		if (v && json_is_string(v))
			filter_search = json_string_value(v);
	}

	// Subcategory requires category (names can appear in multiple categories)
	if (filter_subcategory && !filter_category)
		return make_tool_result("The subcategory filter requires category to also be specified.", true);

	// Resolve category name to ID if filtering
	int filter_category_id = -1;
	if (filter_category) {
		for (size_t i = 0; i < op_menu.size(); i++) {
			if (stricmp(op_menu[i].name.c_str(), filter_category) == 0) {
				filter_category_id = op_menu[i].id;
				break;
			}
		}
		// Check extra categories not in op_menu
		if (filter_category_id < 0) {
			if (stricmp(filter_category, "Uncategorized") == 0)
				filter_category_id = OP_CATEGORY_NONE;
		}
		if (filter_category_id < 0)
			return make_tool_result("Category not found. Use list_sexp_categories to see valid names.", true);
	}

	// Resolve subcategory name to ID if filtering
	int filter_subcategory_id = -1;
	if (filter_subcategory) {
		for (size_t i = 0; i < op_submenu.size(); i++) {
			if (stricmp(op_submenu[i].name.c_str(), filter_subcategory) == 0 &&
				category_of_subcategory(op_submenu[i].id) == filter_category_id) {
				filter_subcategory_id = op_submenu[i].id;
				break;
			}
		}
		if (filter_subcategory_id < 0)
			return make_tool_result("Subcategory not found in this category. Use list_sexp_categories to see valid names.", true);
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

		// Subcategory filter
		if (filter_subcategory_id >= 0) {
			int subcat = get_subcategory(op.value);
			if (subcat != filter_subcategory_id)
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
		json_object_set_new(item, "category", json_string(mcp_category_name(cat_id)));

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
	json_object_set_new(obj, "category", json_string(mcp_category_name(cat_id)));

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

	// Dynamic SEXP flag
	json_object_set_new(obj, "is_dynamic", json_boolean(sexp::get_dynamic_sexp(op.value) != nullptr));

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
			int atype = query_operator_argument_type(op_index, a);
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
// Config file loading helper
// ---------------------------------------------------------------------------

// Load a file from the mod's data/config directory.  If try_defaults is true,
// falls back to the built-in default embedded in the executable.  Returns the
// file content as a string, or an empty string on failure.
static SCP_string load_config_file(const char *filename, bool try_defaults)
{
	SCP_string content;

	if (cf_exists_full(filename, CF_TYPE_CONFIG)) {
		CFILE *fp = cfopen(filename, "rt", CF_TYPE_CONFIG);
		if (fp) {
			int len = cfilelength(fp);
			content.resize(len);
			cfread(content.data(), len + 1, 1, fp);
			cfclose(fp);
		}
	} else if (try_defaults) {
		auto def = defaults_get_file(filename);
		if (def.data)
			content.assign(reinterpret_cast<const char *>(def.data), def.size);
	}

	return content;
}

// ---------------------------------------------------------------------------
// get_mod_info — mod context for AI agents
// ---------------------------------------------------------------------------

static json_t *handle_get_mod_info()
{
	SCP_string content = load_config_file("MOD_INFO.md", false);
	if (content.empty())
		return make_tool_result(
			"No MOD_INFO.md was provided for this mod. No mod-specific context is available.");

	return make_tool_result(content.c_str());
}

// ---------------------------------------------------------------------------
// Reference notes — domain knowledge for AI agents
// ---------------------------------------------------------------------------

static json_t *load_reference_notes()
{
	SCP_string content = load_config_file("mcp_reference_notes.json", true);
	if (content.empty())
		return nullptr;

	json_error_t err;
	json_t *root = json_loads(content.c_str(), 0, &err);
	if (!root)
		mprintf(("MCP: Failed to parse mcp_reference_notes.json: %s (line %d)\n", err.text, err.line));

	return root;
}

static json_t *handle_get_reference_notes(json_t *arguments)
{
	const char *topic = nullptr;
	if (arguments) {
		json_t *v = json_object_get(arguments, "topic");
		if (v && json_is_string(v))
			topic = json_string_value(v);
	}

	json_t *notes = load_reference_notes();
	if (!notes)
		return make_tool_result("Failed to load reference notes.", true);

	if (!json_is_array(notes)) {
		json_decref(notes);
		return make_tool_result("Reference notes file has invalid format (expected JSON array).", true);
	}

	// If no topic specified, list all available topics
	if (!topic || topic[0] == '\0') {
		json_t *arr = json_array();
		size_t index;
		json_t *entry;
		json_array_foreach(notes, index, entry) {
			json_t *item = json_object();
			json_object_set_new(item, "topic", json_copy(json_object_get(entry, "topic")));
			json_object_set_new(item, "title", json_copy(json_object_get(entry, "title")));
			json_array_append_new(arr, item);
		}
		json_decref(notes);
		return make_json_tool_result(arr);
	}

	// Look up the requested topic (case-insensitive)
	size_t index;
	json_t *entry;
	json_array_foreach(notes, index, entry) {
		const char *t = json_string_value(json_object_get(entry, "topic"));
		if (t && stricmp(topic, t) == 0) {
			json_t *obj = json_object();
			json_object_set_new(obj, "topic", json_copy(json_object_get(entry, "topic")));
			json_object_set_new(obj, "title", json_copy(json_object_get(entry, "title")));
			json_object_set_new(obj, "text", json_copy(json_object_get(entry, "text")));
			json_decref(notes);
			return make_json_tool_result(obj);
		}
	}

	json_decref(notes);
	return make_tool_result("Topic not found. Call get_reference_notes without arguments to list available topics.", true);
}

// ---------------------------------------------------------------------------
// SEXP argument and return type metadata
// ---------------------------------------------------------------------------

// Metadata for a SEXP argument type (OPF_*)
struct opf_type_info {
	const char *name;        // string from opf_to_string
	const char *description; // what this type accepts
	const char **accepts;    // null-terminated list of value categories
	const char *notes;       // optional extra context (may be nullptr)
};

// Value category labels used in the "accepts" arrays
static const char *ac_number[]          = { "number", nullptr };
static const char *ac_positive[]        = { "positive_number", nullptr };
static const char *ac_boolean[]         = { "boolean_expression", nullptr };
static const char *ac_string[]          = { "string", nullptr };
static const char *ac_ship[]            = { "ship_name", nullptr };
static const char *ac_wing[]            = { "wing_name", nullptr };
static const char *ac_ship_wing[]       = { "ship_name", "wing_name", nullptr };
static const char *ac_ship_point[]      = { "ship_name", "waypoint_name", nullptr };
static const char *ac_ship_wing_point[] = { "ship_name", "wing_name", "waypoint_name", nullptr };
static const char *ac_ship_wing_point_none[] = { "ship_name", "wing_name", "waypoint_name", "<none>", nullptr };
static const char *ac_ship_wing_team[]  = { "ship_name", "wing_name", "iff_team", nullptr };
static const char *ac_ship_wing_team_point[] = { "ship_name", "wing_name", "iff_team", "waypoint_name", nullptr };
static const char *ac_order_recipient[] = { "ship_name", "wing_name", "All Fighters", nullptr };
static const char *ac_ship_none[]       = { "ship_name", "<none>", nullptr };
static const char *ac_ship_prop[]       = { "ship_name", "prop_name", nullptr };
static const char *ac_subsystem[]       = { "subsystem_name", nullptr };
static const char *ac_subsystem_none[]  = { "subsystem_name", "<none>", nullptr };
static const char *ac_subsys_generic[]  = { "subsystem_name", "subsystem_type_name", nullptr };
static const char *ac_point[]           = { "waypoint_name", "3d_coordinate", nullptr };
static const char *ac_waypoint[]        = { "waypoint_path_name", nullptr };
static const char *ac_iff[]             = { "iff_name", nullptr };
static const char *ac_message[]         = { "message_name", nullptr };
static const char *ac_message_string[]  = { "message_name", "string", nullptr };
static const char *ac_who_from[]        = { "ship_name", "wing_name", "#Command", "#Terran Command", "#Vasudan Command", "<any wingman>", "<none>", nullptr };
static const char *ac_priority[]        = { "Low", "Normal", "High", nullptr };
static const char *ac_table_name[]      = { "table_entry_name", nullptr };
static const char *ac_variable[]        = { "sexp_variable_name", nullptr };
static const char *ac_container[]       = { "container_name", nullptr };
static const char *ac_anything[]        = { "any_value", nullptr };
static const char *ac_ai_goal[]         = { "ai_goal_expression", nullptr };
static const char *ac_ai_order[]        = { "ai_order_name", nullptr };
static const char *ac_enum[]            = { "enumerated_value", nullptr };
static const char *ac_none[]            = { nullptr };

static const opf_type_info Opf_type_info[] = {
	// Value types
	{ "number",          "Any integer (positive or negative)", ac_number, nullptr },
	{ "positive_number", "Non-negative integer (>= 0)", ac_positive, nullptr },
	{ "boolean",         "A boolean sub-expression (true/false)", ac_boolean, nullptr },
	{ "string",          "An arbitrary text string", ac_string, nullptr },
	{ "anything",        "Any value", ac_anything, nullptr },

	// Game objects from the mission
	{ "ship",            "A ship name from the current mission", ac_ship, nullptr },
	{ "wing",            "A wing name from the current mission", ac_wing, nullptr },
	{ "subsystem",       "A subsystem name on a ship", ac_subsystem, nullptr },
	{ "point",           "A waypoint name or 3D coordinate", ac_point, nullptr },
	{ "waypoint_path",   "A waypoint path name from the current mission", ac_waypoint, nullptr },
	{ "message",         "A message name defined in the mission", ac_message, nullptr },
	{ "prop",            "A prop (scenery object) name from the current mission", ac_table_name, nullptr },

	// Composite object types
	{ "ship_or_wing",    "A ship name or wing name", ac_ship_wing, nullptr },
	{ "ship_or_waypoint","A ship name or waypoint name", ac_ship_point, nullptr },
	{ "ship_or_wing_or_waypoint", "A ship, wing, or waypoint name", ac_ship_wing_point, nullptr },
	{ "ship_or_wing_or_waypoint_or_none", "A ship, wing, or waypoint name; or <none>", ac_ship_wing_point_none, "Use the literal string \"<none>\" to indicate no target." },
	{ "ship_or_wing_or_team", "A ship name, wing name, or IFF team name (to target all ships on that team)", ac_ship_wing_team, nullptr },
	{ "ship_or_wing_or_team_or_waypoint", "A ship, wing, IFF team, or waypoint name", ac_ship_wing_team_point, nullptr },
	{ "order_recipient", "A ship, wing, or the special value \"All Fighters\"", ac_order_recipient, nullptr },
	{ "ship_or_none",    "A ship name or <none>", ac_ship_none, "Use the literal string \"<none>\" to indicate no ship." },
	{ "ship_not_player", "Any ship in the mission except the player ship", ac_ship, nullptr },
	{ "ship_with_bay",   "A ship that has a fighter bay subsystem", ac_ship, nullptr },
	{ "ship_or_prop",    "A ship name or prop name", ac_ship_prop, nullptr },

	// Subsystem variants
	{ "subsystem_or_none",    "A subsystem name or <none>", ac_subsystem_none, "Use the literal string \"<none>\" to indicate no subsystem." },
	{ "subsystem_type",       "A generic subsystem type name (e.g. \"Engines\", \"Weapons\", \"Navigation\"), not a specific subsystem instance", ac_enum, nullptr },
	{ "subsystem_or_generic", "A specific subsystem name or a generic subsystem type name", ac_subsys_generic, nullptr },
	{ "awacs_subsystem",      "A subsystem with AWACS (sensor) capability", ac_subsystem, nullptr },
	{ "rotating_subsystem",   "A rotating subsystem", ac_subsystem, nullptr },
	{ "translating_subsystem","A translating subsystem", ac_subsystem, nullptr },

	// Table lookups (names from game data tables)
	{ "ship_class_name",    "A ship class name from ships.tbl (e.g. \"GTF Ulysses\")", ac_table_name, "Use list_ship_classes to see available classes." },
	{ "ship_type",          "A ship type category (e.g. \"fighter\", \"bomber\", \"cruiser\")", ac_table_name, "Use list_ship_types to see available types." },
	{ "weapon_name",        "A weapon class name from weapons.tbl", ac_table_name, "Use list_weapon_classes to see available weapons." },
	{ "huge_weapon",        "A bomb-type secondary weapon (large anti-capital-ship ordnance)", ac_table_name, nullptr },
	{ "iff",                "An IFF team name (e.g. \"Friendly\", \"Hostile\", \"Unknown\")", ac_iff, nullptr },
	{ "species",            "A species name (e.g. \"Terran\", \"Vasudan\", \"Shivan\")", ac_table_name, "Use list_species to see available species." },
	{ "ai_class",           "An AI class name that controls AI skill/behavior", ac_table_name, nullptr },
	{ "intel_name",         "A tech database (intel) entry name", ac_table_name, "Use list_intel_entries to see available entries." },
	{ "support_ship_class", "A support ship class name", ac_table_name, nullptr },
	{ "armor_type",         "An armor type name, or \"<none>\" for no armor", ac_table_name, nullptr },
	{ "damage_type",        "A damage type name, or \"<none>\" for default damage", ac_table_name, nullptr },
	{ "prop_class_name",    "A prop class name from props.tbl", ac_table_name, nullptr },

	// Message/communication types
	{ "who_from",           "The sender of a message: a ship name, wing name, or a special sender token", ac_who_from, nullptr },
	{ "priority",           "Message priority level", ac_priority, nullptr },
	{ "message_or_string",  "A message name or an arbitrary string", ac_message_string, nullptr },
	{ "message_type",       "A message type category", ac_enum, nullptr },
	{ "persona",            "A character persona name", ac_table_name, nullptr },

	// Mission/campaign references
	{ "mission_name",       "A mission filename", ac_table_name, nullptr },
	{ "goal_name",          "A mission goal name", ac_table_name, nullptr },
	{ "event_name",         "A mission event name", ac_table_name, nullptr },
	{ "mission_custom_string", "A custom string defined in the mission", ac_string, nullptr },
	{ "arrival_location",   "Where a ship or wing arrives (e.g. \"Hyperspace\", \"Near Ship\", \"In Front of Ship\")", ac_enum, nullptr },
	{ "departure_location", "Where a ship or wing departs (e.g. \"Hyperspace\", \"Bay\")", ac_enum, nullptr },
	{ "arrival_anchor",     "What a ship arrives near (a ship, waypoint, or special anchor)", ac_anything, nullptr },

	// AI and behavioral
	{ "ai_goal",            "An AI goal sub-expression", ac_ai_goal, "Must be an ai-goals SEXP operator." },
	{ "ai_order",           "A squadron order that the player can issue to ships", ac_ai_order, nullptr },
	{ "skill_level",        "A difficulty/skill level name", ac_enum, nullptr },
	{ "lua_general_order",  "A general order defined in sexps.tbl (Lua scripted)", ac_table_name, nullptr },

	// Docking
	{ "docker_point",       "A docking point on the docker (the ship that initiates docking)", ac_table_name, nullptr },
	{ "dockee_point",       "A docking point on the dockee (the ship being docked with)", ac_table_name, nullptr },

	// Navigation
	{ "jump_node_name",     "A jump node name from the mission", ac_table_name, nullptr },
	{ "nav_point",          "A navigation point name", ac_table_name, nullptr },

	// Audio/sound
	{ "soundtrack_name",    "A music soundtrack name", ac_table_name, nullptr },
	{ "game_sound",         "A game sound name or index", ac_table_name, nullptr },
	{ "sound_environment",  "An EAX/EFX sound environment preset name", ac_enum, nullptr },
	{ "sound_environment_option", "A sound environment option", ac_enum, nullptr },
	{ "audio_volume_option","An audio volume option", ac_enum, nullptr },
	{ "explosion_option",   "An explosion option", ac_enum, nullptr },

	// Visual/HUD
	{ "custom_hud_gauge",   "A custom HUD gauge name", ac_table_name, nullptr },
	{ "builtin_hud_gauge",  "A built-in HUD gauge name", ac_enum, nullptr },
	{ "any_hud_gauge",      "Any HUD gauge name (custom or built-in)", ac_table_name, nullptr },
	{ "hud_element",        "A specific HUD element name", ac_enum, nullptr },
	{ "keypress",           "A keyboard key name", ac_string, nullptr },
	{ "skybox_model_name",  "A skybox model filename", ac_table_name, nullptr },
	{ "skybox_flags",       "A skybox flag", ac_enum, nullptr },
	{ "background_bitmap",  "A background bitmap name", ac_table_name, nullptr },
	{ "sun_bitmap",         "A sun bitmap name", ac_table_name, nullptr },
	{ "nebula_storm_type",  "A nebula storm type name", ac_table_name, nullptr },
	{ "nebula_poof",        "A nebula poof effect name", ac_table_name, nullptr },
	{ "nebula_pattern",     "A full nebula background pattern name", ac_table_name, nullptr },
	{ "post_effect",        "A post-processing effect type", ac_enum, nullptr },
	{ "ship_effect",        "A per-ship visual effect name", ac_table_name, nullptr },

	// Weapons/combat details
	{ "weapon_bank_number", "A weapon bank number or \"all\"", ac_enum, nullptr },
	{ "turret_target_order","A turret target type priority", ac_enum, nullptr },
	{ "turret_type",        "A turret type", ac_enum, nullptr },
	{ "target_priorities",  "A target priority setting", ac_table_name, nullptr },

	// Flags
	{ "ship_flag",          "A ship flag name", ac_enum, nullptr },
	{ "wing_flag",          "A wing flag name", ac_enum, nullptr },
	{ "team_color",         "A team color setting name", ac_table_name, nullptr },

	// Variables and containers
	{ "variable_name",      "A SEXP variable name (variables are prefixed with '$' in the SEXP tree)", ac_variable, nullptr },
	{ "ambiguous",          "Type determined by the target variable (used with modify-variable)", ac_anything, nullptr },
	{ "container_name",     "A SEXP container name (list or map)", ac_container, nullptr },
	{ "list_container_name","A list container name", ac_container, nullptr },
	{ "map_container_name", "A map container name", ac_container, nullptr },
	{ "container_value",    "A value appropriate for the container's data type", ac_anything, nullptr },
	{ "data_or_string_container", "A data or string-keyed container name", ac_container, nullptr },

	// Miscellaneous
	{ "cargo",              "A cargo type string", ac_string, nullptr },
	{ "fireball",           "A fireball entry name or index", ac_table_name, nullptr },
	{ "bolt_type",          "A lightning bolt type name", ac_table_name, nullptr },
	{ "medal_name",         "A medal or award name", ac_table_name, nullptr },
	{ "font",               "A font name", ac_table_name, nullptr },
	{ "animation_type",     "An animation type", ac_enum, nullptr },
	{ "animation_name",     "An animation name", ac_string, nullptr },
	{ "mission_mood",       "A mission mood (determines built-in message selection)", ac_enum, nullptr },
	{ "wing_formation",     "A wing formation name", ac_enum, nullptr },
	{ "asteroid_type",      "An asteroid type name", ac_table_name, nullptr },
	{ "debris_type",        "A debris type name", ac_table_name, nullptr },
	{ "motion_debris",      "A motion debris type name", ac_table_name, nullptr },
	{ "ssm_class",          "A subspace missile class name", ac_table_name, nullptr },
	{ "traitor_override",   "A traitor override setting", ac_enum, nullptr },
	{ "language",           "A language identifier", ac_enum, nullptr },
	{ "functional_when_eval_type", "A functional evaluation type for when operators", ac_enum, nullptr },
	{ "flexible_argument",  "A flexible argument (used with when-argument operators)", ac_anything, nullptr },
	{ "child_lua_enum",     "A Lua-defined enum value", ac_enum, nullptr },

	// Special
	{ "none",               "No argument expected at this position", ac_none, nullptr },
	{ "null",               "Null type (used for type matching)", ac_none, nullptr },

	// sentinel
	{ nullptr, nullptr, nullptr, nullptr }
};

static const opf_type_info *find_opf_type_info(const char *name)
{
	for (const opf_type_info *t = Opf_type_info; t->name; t++) {
		if (stricmp(name, t->name) == 0)
			return t;
	}
	return nullptr;
}

static json_t *handle_get_sexp_argument_type(json_t *arguments)
{
	const char *name = nullptr;
	if (arguments) {
		json_t *v = json_object_get(arguments, "name");
		if (v && json_is_string(v))
			name = json_string_value(v);
	}

	// List all types if no name given
	if (!name || name[0] == '\0') {
		json_t *arr = json_array();
		for (const opf_type_info *t = Opf_type_info; t->name; t++) {
			json_t *item = json_object();
			json_object_set_new(item, "name", json_string(t->name));
			json_object_set_new(item, "description", json_string(t->description));
			json_array_append_new(arr, item);
		}
		return make_json_tool_result(arr);
	}

	const opf_type_info *info = find_opf_type_info(name);
	if (!info)
		return make_tool_result("Unknown argument type. Call get_sexp_argument_type without arguments to list all types.", true);

	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(info->name));
	json_object_set_new(obj, "description", json_string(info->description));

	json_t *accepts = json_array();
	if (info->accepts) {
		for (const char **a = info->accepts; *a; a++)
			json_array_append_new(accepts, json_string(*a));
	}
	json_object_set_new(obj, "accepts", accepts);

	if (info->notes)
		json_object_set_new(obj, "notes", json_string(info->notes));

	return make_json_tool_result(obj);
}

// ---------------------------------------------------------------------------
// SEXP return type metadata
// ---------------------------------------------------------------------------

struct opr_type_info {
	const char *name;
	const char *description;
	const char **compatible_with; // null-terminated list of argument type names this can satisfy
};

static const char *compat_number[]    = { "number", "positive_number", nullptr };
static const char *compat_positive[]  = { "positive_number", "number", nullptr };
static const char *compat_boolean[]   = { "boolean", nullptr };
static const char *compat_null[]      = { nullptr };
static const char *compat_ai_goal[]   = { "ai_goal", nullptr };
static const char *compat_flexible[]  = { "flexible_argument", nullptr };
static const char *compat_ambiguous[] = { "ambiguous", nullptr };
static const char *compat_string[]    = { nullptr };

static const opr_type_info Opr_type_info[] = {
	{ "number",            "Returns any integer (positive or negative)", compat_number },
	{ "positive_number",   "Returns a non-negative integer (>= 0)", compat_positive },
	{ "boolean",           "Returns true or false", compat_boolean },
	{ "null",              "Action operator that does not return a value", compat_null },
	{ "ai_goal",           "Returns an AI goal (used as argument to ai-goals operators)", compat_ai_goal },
	{ "string",            "Returns a string value", compat_string },
	{ "ambiguous",         "Return type depends on variable type (used with variables)", compat_ambiguous },
	{ "flexible_argument", "Flexible argument return (used with when-argument operators)", compat_flexible },
	{ "none",              "No return type", compat_null },

	// sentinel
	{ nullptr, nullptr, nullptr }
};

static json_t *handle_get_sexp_return_type(json_t *arguments)
{
	const char *name = nullptr;
	if (arguments) {
		json_t *v = json_object_get(arguments, "name");
		if (v && json_is_string(v))
			name = json_string_value(v);
	}

	// List all types if no name given
	if (!name || name[0] == '\0') {
		json_t *arr = json_array();
		for (const opr_type_info *t = Opr_type_info; t->name; t++) {
			json_t *item = json_object();
			json_object_set_new(item, "name", json_string(t->name));
			json_object_set_new(item, "description", json_string(t->description));
			json_array_append_new(arr, item);
		}
		return make_json_tool_result(arr);
	}

	// Look up the type
	for (const opr_type_info *t = Opr_type_info; t->name; t++) {
		if (stricmp(name, t->name) == 0) {
			json_t *obj = json_object();
			json_object_set_new(obj, "name", json_string(t->name));
			json_object_set_new(obj, "description", json_string(t->description));

			json_t *compat = json_array();
			if (t->compatible_with) {
				for (const char **c = t->compatible_with; *c; c++)
					json_array_append_new(compat, json_string(*c));
			}
			json_object_set_new(obj, "compatible_with", compat);

			return make_json_tool_result(obj);
		}
	}

	return make_tool_result("Unknown return type. Call get_sexp_return_type without arguments to list all types.", true);
}

// ---------------------------------------------------------------------------
// get_ship_model_details — requires model loading on main thread
// ---------------------------------------------------------------------------

// Determine usage annotation for a path by checking if it's referenced
// by docking bays or ship bays.
static const char *determine_path_usage(int path_index, const polymodel *pm)
{
	// Check if this path is a subsystem attack path
	if (pm->paths[path_index].type == MP_TYPE_SUBSYS)
		return "subsystem";

	// Check if this path is referenced by a docking bay
	for (int d = 0; d < pm->n_docks; d++) {
		const auto &bay = pm->docking_bays[d];
		for (int sp = 0; sp < bay.num_spline_paths; sp++) {
			if (bay.splines[sp] == path_index)
				return "dockpoint";
		}
	}

	// Check if this path is referenced by a ship bay (fighter bay)
	if (pm->ship_bay) {
		for (int b = 0; b < pm->ship_bay->num_paths; b++) {
			if (pm->ship_bay->path_indexes[b] == path_index)
				return "ship_bay";
		}
	}

	return "other";
}

static json_t *handle_get_ship_model_details(json_t *arguments)
{
	json_t *name_val = arguments ? json_object_get(arguments, "name") : nullptr;
	const char *name = (name_val && json_is_string(name_val)) ? json_string_value(name_val) : nullptr;
	if (!name || name[0] == '\0')
		return make_tool_result("Missing required parameter: name", true);

	int sip_idx = ship_info_lookup(name);
	if (sip_idx < 0)
		return make_tool_result("Ship class not found", true);

	// Marshal model_load() to the main thread
	if (!Fred_main_wnd || !Fred_main_wnd->m_hWnd)
		return make_tool_result("FRED2 main window is not available", true);

	McpToolRequest req = {};
	req.tool = McpToolId::LOAD_SHIP_MODEL;
	strncpy(req.filepath, name, sizeof(req.filepath) - 1);
	req.filepath[sizeof(req.filepath) - 1] = '\0';
	req.success = false;
	req.result_message[0] = '\0';

	::SendMessage(Fred_main_wnd->m_hWnd, WM_MCP_TOOL_CALL, 0, (LPARAM)&req);

	if (!req.success)
		return make_tool_result(req.result_message, true);

	// Model is now loaded — read polymodel data
	const auto &sip = Ship_info[sip_idx];
	if (sip.model_num < 0)
		return make_tool_result("Model failed to load", true);

	polymodel *pm = model_get(sip.model_num);
	if (!pm)
		return make_tool_result("Model data unavailable", true);

	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(sip.name));

	// Bounding box dimensions
	{
		json_t *mins = json_object();
		json_object_set_new(mins, "x", json_real(pm->mins.xyz.x));
		json_object_set_new(mins, "y", json_real(pm->mins.xyz.y));
		json_object_set_new(mins, "z", json_real(pm->mins.xyz.z));
		json_object_set_new(obj, "mins", mins);

		json_t *maxs = json_object();
		json_object_set_new(maxs, "x", json_real(pm->maxs.xyz.x));
		json_object_set_new(maxs, "y", json_real(pm->maxs.xyz.y));
		json_object_set_new(maxs, "z", json_real(pm->maxs.xyz.z));
		json_object_set_new(obj, "maxs", maxs);
	}

	// Docking bays
	{
		json_t *docks = json_array();
		for (int d = 0; d < pm->n_docks; d++) {
			const auto &bay = pm->docking_bays[d];
			json_t *dock_obj = json_object();

			json_object_set_new(dock_obj, "name", json_string(bay.name));
			json_object_set_new(dock_obj, "num_slots", json_integer(bay.num_slots));

			// Type flags as array of strings
			json_t *type_arr = json_array();
			if (bay.type_flags & DOCK_TYPE_CARGO)
				json_array_append_new(type_arr, json_string("cargo"));
			if (bay.type_flags & DOCK_TYPE_REARM)
				json_array_append_new(type_arr, json_string("rearm"));
			if (bay.type_flags & DOCK_TYPE_GENERIC)
				json_array_append_new(type_arr, json_string("generic"));
			json_object_set_new(dock_obj, "type_flags", type_arr);

			// Slot positions and normals
			json_t *positions = json_array();
			json_t *normals = json_array();
			for (int s = 0; s < bay.num_slots; s++) {
				json_t *pos = json_object();
				json_object_set_new(pos, "x", json_real(bay.pnt[s].xyz.x));
				json_object_set_new(pos, "y", json_real(bay.pnt[s].xyz.y));
				json_object_set_new(pos, "z", json_real(bay.pnt[s].xyz.z));
				json_array_append_new(positions, pos);

				json_t *nrm = json_object();
				json_object_set_new(nrm, "x", json_real(bay.norm[s].xyz.x));
				json_object_set_new(nrm, "y", json_real(bay.norm[s].xyz.y));
				json_object_set_new(nrm, "z", json_real(bay.norm[s].xyz.z));
				json_array_append_new(normals, nrm);
			}
			json_object_set_new(dock_obj, "positions", positions);
			json_object_set_new(dock_obj, "normals", normals);

			// Spline path names
			json_t *spline_names = json_array();
			for (int sp = 0; sp < bay.num_spline_paths; sp++) {
				int spline_idx = bay.splines[sp];
				if (spline_idx >= 0 && spline_idx < pm->n_paths)
					json_array_append_new(spline_names, json_string(pm->paths[spline_idx].name));
			}
			if (json_array_size(spline_names) > 0)
				json_object_set_new(dock_obj, "spline_path_names", spline_names);
			else
				json_decref(spline_names);

			json_array_append_new(docks, dock_obj);
		}
		json_object_set_new(obj, "docking_bays", docks);
	}

	// Paths
	{
		json_t *paths = json_array();
		for (int p = 0; p < pm->n_paths; p++) {
			const auto &path = pm->paths[p];
			json_t *path_obj = json_object();

			json_object_set_new(path_obj, "name", json_string(path.name));
			set_optional_string(path_obj, "parent_name", path.parent_name);
			json_object_set_new(path_obj, "num_vertices", json_integer(path.nverts));
			json_object_set_new(path_obj, "usage", json_string(determine_path_usage(p, pm)));

			// Vertices
			json_t *verts = json_array();
			for (int v = 0; v < path.nverts; v++) {
				const auto &vert = path.verts[v];
				json_t *vert_obj = json_object();

				json_t *pos = json_object();
				json_object_set_new(pos, "x", json_real(vert.pos.xyz.x));
				json_object_set_new(pos, "y", json_real(vert.pos.xyz.y));
				json_object_set_new(pos, "z", json_real(vert.pos.xyz.z));
				json_object_set_new(vert_obj, "pos", pos);

				json_object_set_new(vert_obj, "radius", json_real(vert.radius));

				json_array_append_new(verts, vert_obj);
			}
			json_object_set_new(path_obj, "vertices", verts);

			json_array_append_new(paths, path_obj);
		}
		json_object_set_new(obj, "paths", paths);
	}

	// Ship bay (fighter bay arrival/departure paths)
	if (pm->ship_bay && pm->ship_bay->num_paths > 0) {
		json_t *bay_obj = json_object();
		json_object_set_new(bay_obj, "num_paths", json_integer(pm->ship_bay->num_paths));

		json_t *bay_paths = json_array();
		for (int b = 0; b < pm->ship_bay->num_paths; b++) {
			int path_idx = pm->ship_bay->path_indexes[b];
			json_t *bp = json_object();

			if (path_idx >= 0 && path_idx < pm->n_paths)
				json_object_set_new(bp, "path_name", json_string(pm->paths[path_idx].name));

			json_object_set_new(bp, "arrival", json_boolean((pm->ship_bay->arrive_flags & (1 << b)) != 0));
			json_object_set_new(bp, "departure", json_boolean((pm->ship_bay->depart_flags & (1 << b)) != 0));

			json_array_append_new(bay_paths, bp);
		}
		json_object_set_new(bay_obj, "paths", bay_paths);
		json_object_set_new(obj, "ship_bay", bay_obj);
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
	if (strcmp(tool_name, "list_sexp_categories") == 0)
		return handle_list_sexp_categories();
	if (strcmp(tool_name, "list_sexp_operators") == 0)
		return handle_list_sexp_operators(arguments);
	if (strcmp(tool_name, "get_sexp_operator") == 0)
		return handle_get_sexp_operator(arguments);
	if (strcmp(tool_name, "get_mod_info") == 0)
		return handle_get_mod_info();
	if (strcmp(tool_name, "get_reference_notes") == 0)
		return handle_get_reference_notes(arguments);
	if (strcmp(tool_name, "get_sexp_argument_type") == 0)
		return handle_get_sexp_argument_type(arguments);
	if (strcmp(tool_name, "get_sexp_return_type") == 0)
		return handle_get_sexp_return_type(arguments);
	if (strcmp(tool_name, "get_ship_model_details") == 0)
		return handle_get_ship_model_details(arguments);

	return nullptr;  // not one of our tools
}
