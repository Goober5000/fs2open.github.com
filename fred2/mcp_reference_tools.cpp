#include "stdafx.h"
#include "mcp_reference_tools.h"
#include "mcpserver.h"
#include "mcp_json.h"
#include "mcp_sexp_forest.h"
#include "sexp_tree.h"

#include <jansson.h>
#include <climits>
#include <cstring>
#include <mutex>

#include "globalincs/utility.h"

#include "mainfrm.h"
#include "ship/ship.h"
#include "weapon/weapon.h"
#include "species_defs/species_defs.h"
#include "parse/parselo.h"
#include "parse/sexp.h"
#include "parse/sexp/sexp_lookup.h"
#include "menuui/techmenu.h"
#include "iff_defs/iff_defs.h"
#include "mission/missionmessage.h"
#include "mod_table/mod_table.h"
#include "cfile/cfile.h"
#include "def_files/def_files.h"


// ---------------------------------------------------------------------------
// Model details cache
// ---------------------------------------------------------------------------

static std::mutex model_cache_mutex;
static SCP_unordered_map<SCP_string, json_t*> model_details_cache;

// ---------------------------------------------------------------------------
// OPF / OPR enum-to-string helpers
// ---------------------------------------------------------------------------

static const std::pair<int, const char *> s_opf_names[] = {
	{ OPF_NONE,                        "none"                           },
	{ OPF_NULL,                        "null"                           },
	{ OPF_BOOL,                        "boolean"                        },
	{ OPF_NUMBER,                      "number"                         },
	{ OPF_SHIP,                        "ship"                           },
	{ OPF_WING,                        "wing"                           },
	{ OPF_SUBSYSTEM,                   "subsystem"                      },
	{ OPF_POINT,                       "point"                          },
	{ OPF_IFF,                         "iff"                            },
	{ OPF_AI_GOAL,                     "ai_goal"                        },
	{ OPF_DOCKER_POINT,                "docker_point"                   },
	{ OPF_DOCKEE_POINT,                "dockee_point"                   },
	{ OPF_MESSAGE,                     "message"                        },
	{ OPF_WHO_FROM,                    "who_from"                       },
	{ OPF_PRIORITY,                    "priority"                       },
	{ OPF_WAYPOINT_PATH,               "waypoint_path"                  },
	{ OPF_POSITIVE,                    "positive_number"                },
	{ OPF_MISSION_NAME,                "mission_name"                   },
	{ OPF_SHIP_POINT,                  "ship_or_waypoint"               },
	{ OPF_GOAL_NAME,                   "goal_name"                      },
	{ OPF_SHIP_WING,                   "ship_or_wing"                   },
	{ OPF_SHIP_WING_WHOLETEAM,         "ship_or_wing_or_team"           },
	{ OPF_SHIP_WING_SHIPONTEAM_POINT,  "ship_or_wing_or_team_or_waypoint" },
	{ OPF_SHIP_WING_POINT,             "ship_or_wing_or_waypoint"       },
	{ OPF_SHIP_WING_POINT_OR_NONE,     "ship_or_wing_or_waypoint_or_none" },
	{ OPF_SHIP_TYPE,                   "ship_type"                      },
	{ OPF_KEYPRESS,                    "keypress"                       },
	{ OPF_EVENT_NAME,                  "event_name"                     },
	{ OPF_AI_ORDER,                    "ai_order"                       },
	{ OPF_SKILL_LEVEL,                 "skill_level"                    },
	{ OPF_MEDAL_NAME,                  "medal_name"                     },
	{ OPF_WEAPON_NAME,                 "weapon_name"                    },
	{ OPF_SHIP_CLASS_NAME,             "ship_class_name"                },
	{ OPF_CUSTOM_HUD_GAUGE,            "custom_hud_gauge"               },
	{ OPF_HUGE_WEAPON,                 "huge_weapon"                    },
	{ OPF_SHIP_NOT_PLAYER,             "ship_not_player"                },
	{ OPF_JUMP_NODE_NAME,              "jump_node_name"                 },
	{ OPF_VARIABLE_NAME,               "variable_name"                  },
	{ OPF_AMBIGUOUS,                   "ambiguous"                      },
	{ OPF_AWACS_SUBSYSTEM,             "awacs_subsystem"                },
	{ OPF_CARGO,                       "cargo"                          },
	{ OPF_AI_CLASS,                    "ai_class"                       },
	{ OPF_SUPPORT_SHIP_CLASS,          "support_ship_class"             },
	{ OPF_ARRIVAL_LOCATION,            "arrival_location"               },
	{ OPF_ARRIVAL_ANCHOR_ALL,          "arrival_anchor"                 },
	{ OPF_DEPARTURE_LOCATION,          "departure_location"             },
	{ OPF_SHIP_WITH_BAY,               "ship_with_bay"                  },
	{ OPF_SOUNDTRACK_NAME,             "soundtrack_name"                },
	{ OPF_INTEL_NAME,                  "intel_name"                     },
	{ OPF_STRING,                      "string"                         },
	{ OPF_ROTATING_SUBSYSTEM,          "rotating_subsystem"             },
	{ OPF_NAV_POINT,                   "nav_point"                      },
	{ OPF_SSM_CLASS,                   "ssm_class"                      },
	{ OPF_FLEXIBLE_ARGUMENT,           "flexible_argument"              },
	{ OPF_ANYTHING,                    "anything"                       },
	{ OPF_SKYBOX_MODEL_NAME,           "skybox_model_name"              },
	{ OPF_SHIP_OR_NONE,                "ship_or_none"                   },
	{ OPF_BACKGROUND_BITMAP,           "background_bitmap"              },
	{ OPF_SUN_BITMAP,                  "sun_bitmap"                     },
	{ OPF_NEBULA_STORM_TYPE,           "nebula_storm_type"              },
	{ OPF_NEBULA_POOF,                 "nebula_poof"                    },
	{ OPF_TURRET_TARGET_ORDER,         "turret_target_order"            },
	{ OPF_SUBSYSTEM_OR_NONE,           "subsystem_or_none"              },
	{ OPF_PERSONA,                     "persona"                        },
	{ OPF_SUBSYS_OR_GENERIC,           "subsystem_or_generic"           },
	{ OPF_ORDER_RECIPIENT,             "order_recipient"                },
	{ OPF_SUBSYSTEM_TYPE,              "subsystem_type"                 },
	{ OPF_POST_EFFECT,                 "post_effect"                    },
	{ OPF_TARGET_PRIORITIES,           "target_priorities"              },
	{ OPF_ARMOR_TYPE,                  "armor_type"                     },
	{ OPF_FONT,                        "font"                           },
	{ OPF_HUD_ELEMENT,                 "hud_element"                    },
	{ OPF_SOUND_ENVIRONMENT,           "sound_environment"              },
	{ OPF_SOUND_ENVIRONMENT_OPTION,    "sound_environment_option"       },
	{ OPF_EXPLOSION_OPTION,            "explosion_option"               },
	{ OPF_AUDIO_VOLUME_OPTION,         "audio_volume_option"            },
	{ OPF_WEAPON_BANK_NUMBER,          "weapon_bank_number"             },
	{ OPF_MESSAGE_OR_STRING,           "message_or_string"              },
	{ OPF_BUILTIN_HUD_GAUGE,           "builtin_hud_gauge"              },
	{ OPF_DAMAGE_TYPE,                 "damage_type"                    },
	{ OPF_SHIP_EFFECT,                 "ship_effect"                    },
	{ OPF_ANIMATION_TYPE,              "animation_type"                 },
	{ OPF_MISSION_MOOD,                "mission_mood"                   },
	{ OPF_SHIP_FLAG,                   "ship_flag"                      },
	{ OPF_TEAM_COLOR,                  "team_color"                     },
	{ OPF_NEBULA_PATTERN,              "nebula_pattern"                 },
	{ OPF_SKYBOX_FLAGS,                "skybox_flags"                   },
	{ OPF_GAME_SND,                    "game_sound"                     },
	{ OPF_FIREBALL,                    "fireball"                       },
	{ OPF_SPECIES,                     "species"                        },
	{ OPF_LANGUAGE,                    "language"                       },
	{ OPF_FUNCTIONAL_WHEN_EVAL_TYPE,   "functional_when_eval_type"      },
	{ OPF_CONTAINER_NAME,              "container_name"                 },
	{ OPF_LIST_CONTAINER_NAME,         "list_container_name"            },
	{ OPF_MAP_CONTAINER_NAME,          "map_container_name"             },
	{ OPF_ANIMATION_NAME,              "animation_name"                 },
	{ OPF_CONTAINER_VALUE,             "container_value"                },
	{ OPF_DATA_OR_STR_CONTAINER,       "data_or_string_container"       },
	{ OPF_TRANSLATING_SUBSYSTEM,       "translating_subsystem"          },
	{ OPF_ANY_HUD_GAUGE,               "any_hud_gauge"                  },
	{ OPF_WING_FLAG,                   "wing_flag"                      },
	{ OPF_ASTEROID_TYPES,              "asteroid_type"                  },
	{ OPF_DEBRIS_TYPES,                "debris_type"                    },
	{ OPF_WING_FORMATION,              "wing_formation"                 },
	{ OPF_MOTION_DEBRIS,               "motion_debris"                  },
	{ OPF_TURRET_TYPE,                 "turret_type"                    },
	{ OPF_BOLT_TYPE,                   "bolt_type"                      },
	{ OPF_TRAITOR_OVERRIDE,            "traitor_override"               },
	{ OPF_LUA_GENERAL_ORDER,           "lua_general_order"              },
	{ OPF_CHILD_LUA_ENUM,              "child_lua_enum"                 },
	{ OPF_MISSION_CUSTOM_STRING,       "mission_custom_string"          },
	{ OPF_MESSAGE_TYPE,                "message_type"                   },
	{ OPF_PROP,                        "prop"                           },
	{ OPF_SHIP_PROP,                   "ship_or_prop"                   },
	{ OPF_PROP_CLASS_NAME,             "prop_class_name"                },
};

static const char *opf_to_string(int opf)
{
	for (const auto &p : s_opf_names)
		if (p.first == opf)
			return p.second;
	return "unknown";
}

// Reverse mapping: MCP argument type name → OPF_* constant.
// Returns -1 if the name is not recognized.
static int opf_from_string(const char *name)
{
	if (!name) return -1;
	for (const auto &p : s_opf_names)
		if (strcmp(p.second, name) == 0)
			return p.first;
	return -1;
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
		case SUBSYSTEM_SOLAR:         return "solar panel";
		case SUBSYSTEM_GAS_COLLECT:   return "gas collector";
		case SUBSYSTEM_ACTIVATION:    return "activation";
		case SUBSYSTEM_UNKNOWN:       return "unknown";
		default: return "unknown";
	}
}

// ---------------------------------------------------------------------------
// Tool schema registration
// ---------------------------------------------------------------------------

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
		add_string_prop(props, "ship_type", "Filter by ship type name (e.g. \"fighter\")");
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
			"speed, weapons, and per-bank weapon restrictions. See also get_ship_class_model_details "
			"for ship class information that depends on the 3D model.",
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
			"Get detailed stats for a specific weapon, including damage and damage-per-second (dps) against hulls, shields, and subsystems.",
			props, req);
	}

	// list_species
	register_tool(tools, "list_species",
		"List all species defined in the game (e.g. Terran, Vasudan, Shivan).",
		json_object());

	// list_iffs
	register_tool(tools, "list_iffs",
		"List all IFF (Identify Friend or Foe) definitions. IFFs define faction allegiances and determine who attacks whom.",
		json_object());

	// get_iff
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the IFF (e.g. \"Friendly\")");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "get_iff",
			"Get details for a specific IFF, including color, attack relationships, and flags.",
			props, req);
	}

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
			"max_args is -1 for variadic operators; for these, argument_types contains "
			"only the fixed argument group and variadic_argument_types contains the "
			"repeating argument group.",
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

	// list_sexp_argument_values
	{
		json_t *props = json_object();
		add_string_prop(props, "name",
			"Argument type name as returned by get_sexp_operator or get_sexp_argument_type "
			"(e.g. \"iff\", \"ship_flag\", \"skill_level\")");
		json_object_set_new(props, "node",
			json_pack("{s:s, s:s}", "type", "integer",
				"description", "Sexp_nodes[] index for context-filtered results "
				"(e.g. to get only subsystems of a specific ship). Omit for unfiltered results."));
		json_object_set_new(props, "arg_index",
			json_pack("{s:s, s:s}", "type", "integer",
				"description", "Argument position within the operator at 'node' (0-based). "
				"Used together with 'node' for context filtering."));
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "list_sexp_argument_values",
			"List the valid values for a SEXP argument type. "
			"Without the optional 'node' parameter, returns all possible values. "
			"With 'node', returns context-filtered values (e.g. only subsystems belonging "
			"to the ship specified in an earlier argument of the same SEXP operator). "
			"Some types depend on the current mission (e.g. 'ship' returns ships in the "
			"loaded mission).",
			props, req);
	}

	// get_ship_class_model_details
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship class (e.g. \"GTD Orion\")");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "get_ship_class_model_details",
			"Get 3D model details for a ship class, including subsystems, bounding box "
			"dimensions, docking bays, and navigation paths (for subsystem attack, docking, and "
			"fighter bay arrival/departure). Note: if the model is not already loaded, "
			"this tool may take several seconds to respond while the model is loaded "
			"into memory.",
			props, req);
	}

	// subsystem_names_compare
	{
		json_t *props = json_object();
		add_string_prop(props, "name1", "First subsystem name");
		add_string_prop(props, "name2", "Second subsystem name");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name1"));
		json_array_append_new(req, json_string("name2"));
		register_tool(tools, "subsystem_names_compare",
			"Compare the ordering of two subsystem names using the engine's special comparison that handles minor allowable variations (e.g. trailing 's'). "
			"Returns an integer: negative if name1 < name2, 0 if equal, positive if name1 > name2 (like strcmp). "
			"Use this for sorting; use subsystem_names_equal for simple equality checks. Always use one of these tools "
			"instead of common string comparison for subsystem names.",
			props, req);
	}

	// subsystem_names_equal
	{
		json_t *props = json_object();
		add_string_prop(props, "name1", "First subsystem name");
		add_string_prop(props, "name2", "Second subsystem name");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name1"));
		json_array_append_new(req, json_string("name2"));
		register_tool(tools, "subsystem_names_equal",
			"Check whether two subsystem names are considered equal using the engine's special comparison that handles minor allowable variations (e.g. trailing 's'). "
			"Returns a boolean. "
			"Use this for simple equality checks; use subsystem_names_compare for sorting. Always use one of these tools "
			"instead of common string comparison for subsystem names.",
			props, req);
	}

	// list_persona_types
	{
		register_tool(tools, "list_persona_types",
			"List all persona type strings (e.g. \"wingman\", \"support\", \"large\", \"command\").",
			json_object());
	}

	// list_personas
	{
		register_tool(tools, "list_personas",
			"List all available personas. Personas define who delivers a message "
			"(e.g. a wingman, support ship, or command). Returns each persona's name, "
			"type, and compatible species. Use persona names with create_message and "
			"update_message.",
			json_object());
	}

	// list_talking_heads
	{
		register_tool(tools, "list_talking_heads",
			"List all available talking head animations. Includes heads referenced by "
			"existing messages, hardcoded heads (unless disabled by mod), and custom "
			"heads defined in the mod table. Message talking heads are almost always "
			"assigned from this list, but on rare occasions can be unique.",
			json_object());
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
		json_object_set_new(item, "actively_pursues", build_array_with_field(st.ai_actively_pursues, Ship_types, &ship_type_info::name));
		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_get_ship_type(json_t *arguments)
{
	json_t *err = nullptr;
	const char *name = get_required_string(arguments, "name", &err);
	if (!name) return err;

	int idx = ship_type_name_lookup(name);
	if (idx < 0)
		return make_not_found_error("Ship type", name);

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

	json_object_set_new(obj, "ai_actively_pursues", build_array_with_field(st.ai_actively_pursues, Ship_types, &ship_type_info::name));
	json_object_set_new(obj, "ai_cripple_ignores", build_array_with_field(st.ai_cripple_ignores, Ship_types, &ship_type_info::name));

	return make_json_tool_result(obj);
}

static json_t *handle_list_ship_classes(json_t *arguments)
{
	// Optional filters
	const char *filter_species = get_optional_string(arguments, "species");
	const char *filter_type    = get_optional_string(arguments, "ship_type");

	int filter_species_idx = -1;
	if (filter_species) {
		filter_species_idx = species_info_lookup(filter_species);
		if (filter_species_idx < 0)
			return make_not_found_error("Species", filter_species);
	}

	int filter_type_idx = -1;
	if (filter_type) {
		filter_type_idx = ship_type_name_lookup(filter_type);
		if (filter_type_idx < 0)
			return make_not_found_error("Ship type", filter_type);
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
			json_object_set_new(item, "ship_type", json_string(Ship_types[sip.class_type].name));

		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

// Build a JSON array describing weapon banks. restriction_offset is the base index
// into sip.restricted_loadout_flag / allowed_bank_restricted_weapons for this bank set
// (0 for primary banks, MAX_SHIP_PRIMARY_BANKS for secondary banks).
static json_t *build_weapon_bank_array(const ship_info &sip, int num_banks,
	const int *bank_weapons, const int *bank_ammo_capacity, int restriction_offset)
{
	json_t *banks = json_array();
	for (int b = 0; b < num_banks; b++) {
		json_t *bank_obj = json_object();
		json_object_set_new(bank_obj, "index", json_integer(b));

		int wi = bank_weapons[b];
		if (wi >= 0 && wi < weapon_info_size())
			json_object_set_new(bank_obj, "default_weapon", json_string(Weapon_info[wi].name));
		else
			json_object_set_new(bank_obj, "default_weapon", json_string("(none)"));

		json_object_set_new(bank_obj, "capacity_in_ammo_units", json_integer(bank_ammo_capacity[b]));

		int ri = restriction_offset + b;
		std::array<std::tuple<int, const char*, const char*>, 2> restriction_types = { {
			{ REGULAR_WEAPON, "restricted", "allowed_weapons" },
			{ DOGFIGHT_WEAPON, "restricted_dogfight", "allowed_dogfight_weapons" }
		} };
		for (auto &rt : restriction_types) {
			auto flag = std::get<0>(rt);
			auto tag = std::get<1>(rt);
			auto bag = std::get<2>(rt);
			bool has_restriction = (ri < (int)sip.restricted_loadout_flag.size()) && (sip.restricted_loadout_flag[ri] & flag);
			json_object_set_new(bank_obj, tag, json_boolean(has_restriction));
			if (has_restriction && (ri < (int)sip.allowed_bank_restricted_weapons.size())) {
				json_t *rw = json_array();
				for (const auto &wf : sip.allowed_bank_restricted_weapons[ri].weapon_and_flags) {
					if ((wf.second & flag) && wf.first >= 0 && wf.first < weapon_info_size())
						json_array_append_new(rw, json_string(Weapon_info[wf.first].name));
				}
				json_object_set_new(bank_obj, bag, rw);
			}
		}

		json_array_append_new(banks, bank_obj);
	}
	return banks;
}

static json_t *handle_get_ship_class(json_t *arguments)
{
	json_t *err = nullptr;
	const char *name = get_required_string(arguments, "name", &err);
	if (!name) return err;

	int idx = ship_info_lookup(name);
	if (idx < 0)
		return make_not_found_error("Ship class", name);

	const auto &sip = Ship_info[idx];
	json_t *obj = json_object();

	// Identity
	json_object_set_new(obj, "name", json_string(sip.name));
	if (sip.has_display_name())
		json_object_set_new(obj, "display_name", json_string(sip.get_display_name()));

	if (sip.species >= 0 && sip.species < (int)Species_info.size())
		json_object_set_new(obj, "species", json_string(Species_info[sip.species].species_name));
	if (sip.class_type >= 0 && sip.class_type < (int)Ship_types.size())
		json_object_set_new(obj, "ship_type", json_string(Ship_types[sip.class_type].name));

	// flags
	json_object_set_new(obj, "allowed_for_player", json_boolean(sip.flags[Ship::Info_Flags::Player_ship]));

	// Tech room strings
	{
		json_t* tech = json_object();
		set_optional_string(tech, "role", sip.type_str.get());
		set_optional_string(tech, "manufacturer", sip.manufacturer_str.get());
		set_optional_string(tech, "description", sip.tech_desc.get());
		set_optional_string(tech, "armor", sip.armor_str.get());
		set_optional_string(tech, "maneuverability", sip.maneuverability_str.get());
		set_optional_string(tech, "ship_length", sip.ship_length.get());
		set_optional_string(tech, "gun_mounts", sip.gun_mounts.get());
		set_optional_string(tech, "missile_banks", sip.missile_banks.get());
		json_object_set_new(tech, "tech_lore", tech);
	}

	// Physics
	json_object_set_new(obj, "max_velocity", build_vec3_json(sip.max_vel));
	json_object_set_new(obj, "max_afterburner_speed", json_real(sip.afterburner_max_vel.xyz.z));
	json_object_set_new(obj, "max_rear_speed", json_real(sip.max_rear_vel));

	// Durability
	json_object_set_new(obj, "max_hull_strength", json_real(sip.max_hull_strength));
	json_object_set_new(obj, "max_shield_strength", json_real(sip.max_shield_strength));

	// Energy
	json_object_set_new(obj, "power_output", json_real(sip.power_output));
	json_object_set_new(obj, "max_weapon_reserve", json_real(sip.max_weapon_reserve));

	// Afterburner
	json_object_set_new(obj, "afterburner_fuel_capacity", json_real(sip.afterburner_fuel_capacity));

	// Weapons — per-bank details
	json_object_set_new(obj, "primary_banks",
		build_weapon_bank_array(sip, sip.num_primary_banks,
			sip.primary_bank_weapons, sip.primary_bank_ammo_capacity, 0));
	json_object_set_new(obj, "secondary_banks",
		build_weapon_bank_array(sip, sip.num_secondary_banks,
			sip.secondary_bank_weapons, sip.secondary_bank_ammo_capacity, MAX_SHIP_PRIMARY_BANKS));

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
		json_object_set_new(obj, "allowed_weapons", aw);
		json_object_set_new(obj, "allowed_dogfight_weapons", daw);
	}

	// Countermeasures
	if (sip.cmeasure_type >= 0 && sip.cmeasure_type < weapon_info_size())
		json_object_set_new(obj, "countermeasure_type", json_string(Weapon_info[sip.cmeasure_type].name));
	json_object_set_new(obj, "countermeasure_max", json_integer(sip.cmeasure_max));

	// Score
	json_object_set_new(obj, "kill_score", json_integer(sip.score));

	// Classification helpers
	auto size_classification = "unspecified";
	if (sip.is_huge_ship())
		size_classification = "huge";
	else if (sip.is_big_ship())
		size_classification = "big";
	else if (sip.is_small_ship())
		size_classification = "small";
	json_object_set_new(obj, "size_classification", json_string(size_classification));
	json_object_set_new(obj, "can_move_under_its_own_power", json_boolean(sip.is_flyable()));
	json_object_set_new(obj, "is_fighter_or_bomber", json_boolean(sip.is_fighter_bomber()));

	return make_json_tool_result(obj);
}

static json_t *handle_list_weapon_classes(json_t *arguments)
{
	// Optional category filter.  We use the semantic helpers (is_beam(),
	// is_secondary(), is_non_beam_primary()) instead of the raw subtype field,
	// because most beam weapons have subtype WP_LASER rather than WP_BEAM.
	enum { FILTER_NONE, FILTER_PRIMARY, FILTER_SECONDARY, FILTER_BEAM } filter = FILTER_NONE;
	const char *st = get_optional_string(arguments, "subtype");
	if (st) {
		if (stricmp(st, "primary") == 0)
			filter = FILTER_PRIMARY;
		else if (stricmp(st, "secondary") == 0)
			filter = FILTER_SECONDARY;
		else if (stricmp(st, "beam") == 0)
			filter = FILTER_BEAM;
		else
			return make_tool_result("Invalid subtype. Use \"primary\", \"secondary\", or \"beam\".", true);
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

		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_get_weapon_class(json_t *arguments)
{
	json_t *err = nullptr;
	const char *name = get_required_string(arguments, "name", &err);
	if (!name) return err;

	int idx = weapon_info_lookup(name);
	if (idx < 0)
		return make_not_found_error("Weapon class", name);

	const auto &wip = Weapon_info[idx];
	json_t *obj = json_object();

	// Identity
	json_object_set_new(obj, "name", json_string(wip.name));
	if (wip.has_display_name())
		json_object_set_new(obj, "display_name", json_string(wip.get_display_name()));
	json_object_set_new(obj, "subtype", json_string(weapon_category_str(wip)));

	// Loadout and Tech screen strings
	{
		json_t* loadout = json_object();
		set_optional_string(loadout, "title", wip.title);
		set_optional_string(loadout, "description", wip.desc.get());
		json_object_set_new(obj, "loadout_ui", loadout);

		json_t* tech = json_object();
		set_optional_string(tech, "title", wip.tech_title);
		set_optional_string(tech, "description", wip.tech_desc.get());
		json_object_set_new(obj, "tech_lore", tech);
	}

	// Performance — computed stats from weapon_get_stats
	{
		static const char *stat_keys[] = {
			"max_speed", "standard_range",
			"damage_hull", "dps_hull", "damage_shield", "dps_shield",
			"damage_subsystem", "dps_subsystem",
			"energy_consumption_rate", "fire_interval", "fire_rate",
			"reload_rate", "has_area_effect"
		};

		auto stats = weapon_get_stats(wip);
		for (const char *key : stat_keys) {
			auto it = stats.find(key);
			if (it == stats.end())
				continue;
			if (wip.is_beam() && !stricmp(key, "max_speed"))
				continue;
			if (auto *f = std::get_if<float>(&it->second))
				json_object_set_new(obj, key, json_real(*f));
			else if (auto *b = std::get_if<bool>(&it->second))
				json_object_set_new(obj, key, json_boolean(*b));
		}
	}

	// AI targeting ranges
	json_object_set_new(obj, "ai_max_targeting_range", json_real(wip.weapon_range));
	json_object_set_new(obj, "ai_min_targeting_range", json_real(wip.weapon_min_range));
	json_object_set_new(obj, "ai_preferred_range", json_real(wip.optimum_range));

	if (wip.cargo_size > 0.0f)
		json_object_set_new(obj, "size_in_ammo_units", json_real(wip.cargo_size));

	// Missile-specific
	if (wip.is_secondary()) {
		json_object_set_new(obj, "is_interceptable", json_boolean(wip.is_interceptable()));
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
	if (wip.wi_flags[Weapon::Info_Flags::Emp]) {
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
	json_object_set_new(obj, "allowed_for_player", json_boolean(wip.wi_flags[Weapon::Info_Flags::Player_allowed]));
	json_object_set_new(obj, "hurts_big_ships", json_boolean(wip.hurts_big_ships()));

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
		set_optional_string(item, "support_ship_class", sp.support_ship_name);

		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_list_iffs()
{
	json_t *arr = json_array();

	for (size_t i = 0; i < Iff_info.size(); i++) {
		json_t *item = json_object();
		json_object_set_new(item, "name", json_string(Iff_info[i].iff_name));
		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_get_iff(json_t *arguments)
{
	json_t *err = nullptr;
	const char *name = get_required_string(arguments, "name", &err);
	if (!name) return err;

	int idx = iff_lookup(name);
	if (idx < 0)
		return make_not_found_error("IFF", name);

	const auto &iff = Iff_info[idx];
	json_t *obj = json_object();

	// Identity
	json_object_set_new(obj, "name", json_string(iff.iff_name));

	// Color (RGB array from the non-bright variant)
	{
		color *c = iff_get_color(iff.color_index, 0);
		json_t *color_arr = json_array();
		json_array_append_new(color_arr, json_integer(c->red));
		json_array_append_new(color_arr, json_integer(c->green));
		json_array_append_new(color_arr, json_integer(c->blue));
		json_object_set_new(obj, "color", color_arr);
	}

	// Attacks
	{
		json_t *attacks = json_array();
		for (int j = 0; j < (int)Iff_info.size(); j++) {
			if (iff_x_attacks_y(idx, j))
				json_array_append_new(attacks, json_string(Iff_info[j].iff_name));
		}
		json_object_set_new(obj, "attacks", attacks);
	}

	// Flags
	json_object_set_new(obj, "support_allowed", json_boolean(iff.flags & IFFF_SUPPORT_ALLOWED));
	json_object_set_new(obj, "exempt_from_all_teams_at_war", json_boolean(iff.flags & IFFF_EXEMPT_FROM_ALL_TEAMS_AT_WAR));

	return make_json_tool_result(obj);
}

static json_t *handle_list_intel_entries()
{
	json_t *arr = json_array();

	for (int i = 0; i < intel_info_size(); i++) {
		const auto &entry = Intel_info[i];
		json_t *item = json_object();
		json_object_set_new(item, "name", json_string(entry.name));

		json_array_append_new(arr, item);
	}

	return make_json_tool_result(arr);
}

static json_t *handle_get_intel_entry(json_t *arguments)
{
	json_t *err = nullptr;
	const char *name = get_required_string(arguments, "name", &err);
	if (!name) return err;

	int idx = intel_info_lookup(name);
	if (idx < 0)
		return make_not_found_error("Intel entry", name);

	const auto &entry = Intel_info[idx];
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(entry.name));
	json_object_set_new(obj, "lore", json_string(entry.desc.c_str()));

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
	const char *filter_category    = get_optional_string(arguments, "category");
	const char *filter_subcategory = get_optional_string(arguments, "subcategory");
	const char *filter_search      = get_optional_string(arguments, "search");

	// Subcategory requires category (names can appear in multiple categories)
	if (filter_subcategory && !filter_category)
		return make_tool_result("The subcategory filter requires category to also be specified.", true);

	// Resolve category name to ID if filtering
	int filter_category_id = -1;
	if (filter_category) {
		int found_idx = find_item_with_string(op_menu, &op_menu_struct::name, filter_category);
		if (found_idx >= 0)
			filter_category_id = op_menu[found_idx].id;
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

// Populate "argument_types" (and "variadic_argument_types" for variadic ops) on obj.
// For variadic operators (max_args == INT_MAX), detects the repeating cycle and splits
// the fixed prefix from the cycle.  For non-variadic operators, lists all argument types.
static void build_argument_types_json(json_t *obj, int op_index, int min_args, int max_args)
{
	bool is_variadic = (max_args == INT_MAX);

	if (is_variadic) {
		// For variadic operators, separate the fixed prefix from the
		// repeating cycle.  We query enough positions to reliably detect
		// the cycle pattern, then verify it repeats into an extended range.
		//
		// We need at least 8 positions even when min_args is small (e.g. 0)
		// so that we have enough data to detect multi-element cycles.
		int query_count = std::max(min_args * 2, 8);

		// Collect types for positions 0..query_count-1
		SCP_vector<int> types;
		for (int a = 0; a < query_count; a++)
			types.push_back(query_operator_argument_type(op_index, a));

		// Query 2 * query_count additional positions to verify the cycle,
		// ensuring even the largest candidate cycle gets a second-period check.
		int extended_count = 2 * query_count;
		SCP_vector<int> extended_types;
		for (int a = 0; a < extended_count; a++)
			extended_types.push_back(query_operator_argument_type(op_index, query_count + a));

		// Find the cycle length.  The cycle is the smallest L (1..query_count)
		// where the pattern at the tail of types[] repeats into
		// extended_types[].  Specifically, the last L entries of types[]
		// must equal the first L entries of extended_types[], AND for any
		// additional repetitions within types[] the cycle must hold.
		int cycle_len = 0;
		for (int L = 1; L <= query_count; L++) {
			// The cycle covers positions [query_count - L .. query_count - 1].
			// Verify it repeats at [query_count .. query_count + L - 1].
			bool match = true;
			for (int i = 0; i < L; i++) {
				if (types[query_count - L + i] != extended_types[i]) {
					match = false;
					break;
				}
			}
			if (!match)
				continue;

			// Verify the cycle is self-consistent within types[] —
			// i.e., for the cycle region, type[a] == type[a - L]
			int prefix_len = query_count - L;
			for (int a = prefix_len + L; a < query_count; a++) {
				if (types[a] != types[a - L]) {
					match = false;
					break;
				}
			}
			if (!match)
				continue;

			// Verify a second period in extended_types to guard
			// against a coincidental single-period boundary match
			for (int i = 0; i < L; i++) {
				if (extended_types[i] != extended_types[L + i]) {
					match = false;
					break;
				}
			}
			if (!match)
				continue;

			cycle_len = L;
			break;
		}

		bool cycle_found = (cycle_len > 0);

		if (cycle_found) {
			int prefix_len = query_count - cycle_len;

			// The cycle may begin earlier than query_count - cycle_len.
			// Walk backwards in full-cycle steps while the prefix tail
			// matches the cycle.
			while (prefix_len >= cycle_len) {
				bool matches = true;
				for (int i = 0; i < cycle_len; i++) {
					if (types[prefix_len - cycle_len + i] != types[prefix_len + i]) {
						matches = false;
						break;
					}
				}
				if (!matches)
					break;
				prefix_len -= cycle_len;
			}

			// Fine-tune: the prefix may end partway through a cycle
			// period.  Trim one position at a time if it matches.
			while (prefix_len > 0 && types[prefix_len - 1] == types[prefix_len - 1 + cycle_len]) {
				prefix_len--;
			}

			// Fixed prefix (argument_types)
			json_t *arg_types = json_array();
			for (int a = 0; a < prefix_len; a++)
				json_array_append_new(arg_types, json_string(opf_to_string(types[a])));
			json_object_set_new(obj, "argument_types", arg_types);

			// Repeating group (variadic_argument_types) — emit exactly one cycle
			json_t *var_types = json_array();
			for (int a = prefix_len; a < prefix_len + cycle_len; a++)
				json_array_append_new(var_types, json_string(opf_to_string(types[a])));
			json_object_set_new(obj, "variadic_argument_types", var_types);
		} else {
			// No cycle detected — emit only the min_args types as argument_types
			json_t *arg_types = json_array();
			for (int a = 0; a < min_args; a++)
				json_array_append_new(arg_types, json_string(opf_to_string(types[a])));
			json_object_set_new(obj, "argument_types", arg_types);
		}
	} else {
		// Non-variadic: just list all argument types
		json_t *arg_types = json_array();
		for (int a = 0; a < max_args; a++) {
			int atype = query_operator_argument_type(op_index, a);
			if (atype == OPF_NONE)
				break;
			json_array_append_new(arg_types, json_string(opf_to_string(atype)));
		}
		json_object_set_new(obj, "argument_types", arg_types);
	}
}

static json_t *handle_get_sexp_operator(json_t *arguments)
{
	json_t *err = nullptr;
	const char *name = get_required_string(arguments, "name", &err);
	if (!name) return err;

	// Find operator by name
	int op_index = find_item_with_string(Operators, &sexp_oper::text, name);
	if (op_index < 0)
		return make_not_found_error("SEXP operator", name);

	const auto &op = Operators[op_index];
	json_t *obj = json_object();

	json_object_set_new(obj, "name", json_string(op.text.c_str()));
	json_object_set_new(obj, "min_args", json_integer(op.min));
	json_object_set_new(obj, "max_args", json_integer(op.max == INT_MAX ? -1 : op.max));

	// Category and subcategory
	int cat_id = get_category(op.value);
	json_object_set_new(obj, "category", json_string(mcp_category_name(cat_id)));

	int subcat_id = get_subcategory(op.value);
	if (subcat_id != OP_SUBCATEGORY_NONE) {
		// Find subcategory name in op_submenu
		int found_idx = find_item_with_field(op_submenu, &op_menu_struct::id, subcat_id);
		if (found_idx >= 0)
			json_object_set_new(obj, "subcategory", json_string(op_submenu[found_idx].name.c_str()));
	}

	// Dynamic SEXP flag
	json_object_set_new(obj, "is_dynamic", json_boolean(sexp::get_dynamic_sexp(op.value) != nullptr));

	// Return type
	int ret = query_operator_return_type(op.value);
	json_object_set_new(obj, "return_type", json_string(opr_to_string(ret)));

	// Help text
	int help_idx = find_item_with_field(Sexp_help, &sexp_help_struct::id, op.value);
	if (help_idx >= 0)
		json_object_set_new(obj, "help", json_string(Sexp_help[help_idx].help.c_str()));

	// Argument types
	build_argument_types_json(obj, op_index, op.min, op.max);

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
	const char *topic = get_optional_string(arguments, "topic");

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

// Linear search in a sentinel-terminated (t->name == nullptr) type-info table.
template<typename T>
static const T *find_type_info(const T *table, const char *name)
{
	for (const T *t = table; t->name; t++)
		if (stricmp(name, t->name) == 0)
			return t;
	return nullptr;
}

static json_t *handle_get_sexp_argument_type(json_t *arguments)
{
	const char *name = get_optional_string(arguments, "name");

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

	const opf_type_info *info = find_type_info(Opf_type_info, name);
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
	const char *name = get_optional_string(arguments, "name");

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
	const opr_type_info *info = find_type_info(Opr_type_info, name);
	if (!info)
		return make_tool_result("Unknown return type. Call get_sexp_return_type without arguments to list all types.", true);

	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(info->name));
	json_object_set_new(obj, "description", json_string(info->description));

	json_t *compat = json_array();
	if (info->compatible_with) {
		for (const char **c = info->compatible_with; *c; c++)
			json_array_append_new(compat, json_string(*c));
	}
	json_object_set_new(obj, "compatible_with", compat);

	return make_json_tool_result(obj);
}

// ---------------------------------------------------------------------------
// get_ship_class_model_details — requires model loading on main thread
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

static json_t *handle_get_ship_class_model_details(json_t *arguments)
{
	json_t *err = nullptr;
	const char *name = get_required_string(arguments, "name", &err);
	if (!name) return err;

	int sip_idx = ship_info_lookup(name);
	if (sip_idx < 0)
		return make_not_found_error("Ship class", name);

	// Check cache (use canonical name from Ship_info for consistent keys)
	const char *canonical_name = Ship_info[sip_idx].name;
	{
		std::lock_guard<std::mutex> lock(model_cache_mutex);
		auto it = model_details_cache.find(canonical_name);
		if (it != model_details_cache.end())
			return json_incref(it->second);
	}

	// Load the model if not already loaded
	const auto &sip = Ship_info[sip_idx];
	bool we_loaded_model = false;
	if (sip.model_num < 0) {
		json_t *load_result = mcp_execute_on_main_thread(McpToolId::LOAD_SHIP_MODEL, name);

		json_t *is_err = json_object_get(load_result, "isError");
		if (is_err && json_is_true(is_err))
			return load_result;
		json_decref(load_result);

		if (sip.model_num < 0)
			return make_tool_result("Model failed to load", true);
		we_loaded_model = true;
	}

	// Model is now loaded — read polymodel data
	polymodel *pm = model_get(sip.model_num);
	if (!pm)
		return make_tool_result("Model data unavailable", true);

	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(sip.name));

	// Bounding box dimensions
	json_object_set_new(obj, "mins", build_vec3_json(pm->mins));
	json_object_set_new(obj, "maxs", build_vec3_json(pm->maxs));

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
				json_array_append_new(positions, build_vec3_json(bay.pnt[s]));
				json_array_append_new(normals, build_vec3_json(bay.norm[s]));
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
			json_object_set_new(dock_obj, "spline_path_names", spline_names);

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

				json_object_set_new(vert_obj, "pos", build_vec3_json(vert.pos));

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

	// Subsystems (model is loaded at this point, so types are reliable)
	{
		json_t *subsys = json_array();
		for (int s = 0; s < sip.n_subsystems; s++) {
			const auto &ss = sip.subsystems[s];
			json_t *ss_obj = json_object();

			json_object_set_new(ss_obj, "name", json_string(ss.subobj_name));
			if (ss.type != SUBSYSTEM_NONE)
				json_object_set_new(ss_obj, "subsystem_type", json_string(subsystem_type_str(ss.type)));
			json_object_set_new(ss_obj, "max_hitpoints", json_real(ss.max_subsys_strength));

			// Turret-specific info
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
				json_object_set_new(ss_obj, "primary_weapons", t_pri);

				// Turret secondary weapons
				json_t *t_sec = json_array();
				for (int b = 0; b < MAX_SHIP_SECONDARY_BANKS; b++) {
					if (ss.secondary_banks[b] >= 0 && ss.secondary_banks[b] < weapon_info_size())
						json_array_append_new(t_sec, json_string(Weapon_info[ss.secondary_banks[b]].name));
				}
				json_object_set_new(ss_obj, "secondary_weapons", t_sec);
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

	json_t *result = make_json_tool_result(obj);

	// Cache the result for future calls
	{
		std::lock_guard<std::mutex> lock(model_cache_mutex);
		if (model_details_cache.find(canonical_name) == model_details_cache.end())
			model_details_cache.emplace(SCP_string(canonical_name), json_incref(result));
	}

	// Unload the model if we were the ones who loaded it
	if (we_loaded_model) {
		json_t *unload_result = mcp_execute_on_main_thread(McpToolId::UNLOAD_SHIP_MODEL, canonical_name);
		json_decref(unload_result);
	}

	return result;
}

// ---------------------------------------------------------------------------
// Subsystem name comparison
// ---------------------------------------------------------------------------

static json_t *handle_subsystem_names_compare(json_t *arguments)
{
	const char *name1 = get_optional_string(arguments, "name1");
	const char *name2 = get_optional_string(arguments, "name2");
	if (!name1 || !name2)
		return make_tool_result("Missing required parameters: name1 and name2", true);

	int cmp = subsystem_stricmp(name1, name2);

	json_t *result = make_tool_result(false, "Comparison result: %d (%s)", cmp, cmp == 0 ? "equal" : "not equal");

	json_t *sc = json_object();
	json_object_set_new(sc, "result", json_integer(cmp));
	json_object_set_new(result, "structuredContent", sc);

	return result;
}

static json_t *handle_subsystem_names_equal(json_t *arguments)
{
	const char *name1 = get_optional_string(arguments, "name1");
	const char *name2 = get_optional_string(arguments, "name2");
	if (!name1 || !name2)
		return make_tool_result("Missing required parameters: name1 and name2", true);

	bool equal = subsystem_stricmp(name1, name2) == 0;

	json_t *result = make_tool_result(equal
		? "The subsystem names are equal."
		: "The subsystem names are not equal.");

	json_t *sc = json_object();
	json_object_set_new(sc, "equal", json_boolean(equal));
	json_object_set_new(result, "structuredContent", sc);

	return result;
}

// ---------------------------------------------------------------------------
// Persona and talking head reference tools
// ---------------------------------------------------------------------------

static json_t *handle_list_persona_types()
{
	json_t *arr = json_array();
	for (int i = 0; i < MAX_PERSONA_TYPES; i++)
		json_array_append_new(arr, json_string(Persona_type_names[i]));

	return make_list_result("persona_types", arr);
}

static json_t *handle_list_personas()
{
	json_t *arr = json_array();

	for (const auto &p : Personas) {
		json_t *entry = json_object();
		json_object_set_new(entry, "name", json_string(p.name));

		// Derive persona_type from type flag bits
		const char *persona_type = "unknown";
		for (int j = 0; j < MAX_PERSONA_TYPES; j++) {
			if (p.flags & (1 << j)) {
				persona_type = Persona_type_names[j];
				break;
			}
		}
		json_object_set_new(entry, "persona_type", json_string(persona_type));

		// Decode species_bitfield into species name array
		json_t *species_arr = json_array();
		for (int j = 0; j < (int)Species_info.size(); j++) {
			if (p.species_bitfield & (1 << j))
				json_array_append_new(species_arr, json_string(Species_info[j].species_name));
		}
		json_object_set_new(entry, "species", species_arr);

		json_array_append_new(arr, entry);
	}

	return make_list_result("personas", arr);
}

static json_t *handle_list_talking_heads()
{
	// Collect unique head names, matching event_editor::OnInitDialog() logic
	SCP_vector<SCP_string> heads;
	auto maybe_add = [&](const char *name) {
		if (!SCP_vector_contains_lcase(heads, name))
			heads.push_back(name);
	};

	// Heads referenced by existing builtin messages
	for (int i = 0; i < Num_builtin_messages; i++) {
		if (Messages[i].avi_info.name)
			maybe_add(Messages[i].avi_info.name);
	}

	// Hardcoded heads (unless disabled by mod table)
	if (!Disable_hc_message_ani) {
		static const char *hardcoded_heads[] = {
			"Head-TP2", "Head-VC2", "Head-TP4", "Head-TP5", "Head-TP6",
			"Head-TP7", "Head-TP8", "Head-VP2", "Head-CM2", "Head-CM3",
			"Head-CM4", "Head-CM5", "Head-BSH"
		};
		for (const char *h : hardcoded_heads)
			maybe_add(h);
	}

	// Custom heads from mod table
	for (const auto &h : Custom_head_anis) {
		maybe_add(h.c_str());
	}

	json_t *arr = json_array();
	for (const auto &h : heads)
		json_array_append_new(arr, json_string(h.c_str()));

	return make_list_result("talking_heads", arr);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

static json_t *handle_list_sexp_argument_values(json_t *arguments)
{
	json_t *err = nullptr;
	const char *name = get_required_string(arguments, "name", &err);
	if (!name)
		return err;

	int opf = opf_from_string(name);
	if (opf < 0)
		return make_tool_result(
			"Unknown argument type. Call get_sexp_argument_type without arguments to list all types.", true);

	// Optional: node index for context-filtered results
	int parent_node = -1;
	int arg_index = -1;
	get_optional_integer(arguments, "node", &parent_node);
	get_optional_integer(arguments, "arg_index", &arg_index);

	// If context was requested and the forest is dirty, rebuild it first.
	if (parent_node >= 0 && mcp_sexp_forest_is_dirty()) {
		json_t *rebuild_result = mcp_execute_on_main_thread(McpToolId::REBUILD_SEXP_FOREST, "");
		json_decref(rebuild_result);
	}

	// Get the value list from the forest (or with no context if parent_node < 0)
	sexp_list_item *list = mcp_sexp_forest_get_listing(opf, parent_node, arg_index);

	json_t *values = json_array();
	for (sexp_list_item *item = list; item != nullptr; item = item->next)
		json_array_append_new(values, json_string(item->text.c_str()));

	if (list)
		list->destroy();

	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_string(name));
	json_object_set_new(obj, "values", values);
	return make_json_tool_result(obj);
}

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
	if (strcmp(tool_name, "list_iffs") == 0)
		return handle_list_iffs();
	if (strcmp(tool_name, "get_iff") == 0)
		return handle_get_iff(arguments);
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
	if (strcmp(tool_name, "list_sexp_argument_values") == 0)
		return handle_list_sexp_argument_values(arguments);
	if (strcmp(tool_name, "get_ship_class_model_details") == 0)
		return handle_get_ship_class_model_details(arguments);
	if (strcmp(tool_name, "subsystem_names_compare") == 0)
		return handle_subsystem_names_compare(arguments);
	if (strcmp(tool_name, "subsystem_names_equal") == 0)
		return handle_subsystem_names_equal(arguments);
	if (strcmp(tool_name, "list_persona_types") == 0)
		return handle_list_persona_types();
	if (strcmp(tool_name, "list_personas") == 0)
		return handle_list_personas();
	if (strcmp(tool_name, "list_talking_heads") == 0)
		return handle_list_talking_heads();

	return nullptr;  // not one of our tools
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void mcp_reference_tools_cleanup()
{
	// Called after mg_stop() — no mongoose threads running, no lock needed.
	for (auto &pair : model_details_cache)
		json_decref(pair.second);
	model_details_cache.clear();

	mcp_sexp_forest_cleanup();
}
