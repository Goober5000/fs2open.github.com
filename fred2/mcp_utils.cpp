#include "stdafx.h"
#include "mcp_utils.h"

#include <algorithm>
#include <cstring>

#include "globalincs/utility.h"   // for stringcost, stringcost_tolower_equal
#include "parse/sexp.h"

// Compute the fuzzy match cost of search_str against candidate.
// Returns SCP_string::npos if no match within threshold.
size_t fuzzy_match_cost(const char *candidate, const SCP_string &search_str, size_t max_length)
{
	SCP_string candidate_str(candidate);
	size_t threshold = max_length * max_length * 3;
	size_t cost = stringcost(candidate_str, search_str, max_length, stringcost_tolower_equal);
	if (cost == SCP_string::npos || cost >= threshold)
		return SCP_string::npos;
	return cost;
}

// Fuzzy-search a pre-filtered candidate list and return matches sorted by cost.
// candidates: (original_index, candidate_name) pairs.
// max_name_length: pass 0 to auto-compute from candidates.
// When search is empty, all candidates pass with cost 0 (original order preserved).
SCP_vector<std::pair<size_t, size_t>> fuzzy_search_and_sort(
	const SCP_vector<std::pair<size_t, const char *>> &candidates,
	const char *search, size_t max_name_length)
{
	SCP_vector<std::pair<size_t, size_t>> matches;
	bool has_search = search && search[0];

	if (has_search) {
		SCP_string search_str(search);
		if (max_name_length == 0) {
			for (const auto &c : candidates) {
				if (c.second) {
					size_t len = strlen(c.second);
					if (len > max_name_length)
						max_name_length = len;
				}
			}
		}
		for (const auto &c : candidates) {
			if (!c.second)
				continue;
			size_t cost = fuzzy_match_cost(c.second, search_str, max_name_length);
			if (cost != SCP_string::npos)
				matches.emplace_back(c.first, cost);
		}
		std::sort(matches.begin(), matches.end(),
			[](const auto &a, const auto &b) { return a.second < b.second; });
	} else {
		for (const auto &c : candidates)
			matches.emplace_back(c.first, 0);
	}

	return matches;
}

// ---------------------------------------------------------------------------
// OPF / OPR enum-to-string helpers
// ---------------------------------------------------------------------------

const std::pair<int, const char *> Opf_types_to_names[] = {
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

const char *opf_to_name(int opf)
{
	for (const auto &p : Opf_types_to_names)
		if (p.first == opf)
			return p.second;
	return "unknown";
}

int opf_from_name(const char *name)
{
	if (!name) return -1;
	for (const auto &p : Opf_types_to_names)
		if (strcmp(p.second, name) == 0)
			return p.first;
	return -1;
}

// ---------------------------------------------------------------------------
// SEXP argument and return type metadata
// ---------------------------------------------------------------------------

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

const opf_type_info Opf_type_info[] = {
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
	{ "ship_with_bay",   "A ship that has a hangar bay subsystem", ac_ship, nullptr },
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

// ---------------------------------------------------------------------------
// Other SEXP utility functions
// ---------------------------------------------------------------------------

const char *mcp_get_sexp_category_name(int category_id)
{
	if (category_id == OP_CATEGORY_NONE)
		return "Uncategorized";
	return get_category_name(category_id);
}

const char *mcp_get_sexp_category_description(int category_id)
{
	switch (category_id) {
		case OP_CATEGORY_OBJECTIVE:    return "Conditions for mission goals (is-destroyed-delay, is-departed-delay, etc.)";
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

// ---------------------------------------------------------------------------
// SEXP return type metadata
// ---------------------------------------------------------------------------

static const char *compat_number[]    = { "number", "positive_number", nullptr };
static const char *compat_positive[]  = { "positive_number", "number", nullptr };
static const char *compat_boolean[]   = { "boolean", nullptr };
static const char *compat_null[]      = { nullptr };
static const char *compat_ai_goal[]   = { "ai_goal", nullptr };
static const char *compat_flexible[]  = { "flexible_argument", nullptr };
static const char *compat_ambiguous[] = { "ambiguous", nullptr };
static const char *compat_string[]    = { nullptr };

const opr_type_info Opr_type_info[] = {
	{ OPR_NUMBER,            "number",            "Returns any integer (positive or negative)", compat_number },
	{ OPR_POSITIVE,          "positive_number",   "Returns a non-negative integer (>= 0)", compat_positive },
	{ OPR_BOOL,              "boolean",           "Returns true or false", compat_boolean },
	{ OPR_NULL,              "void",              "Action operator that does not return a value", compat_null },
	{ OPR_AI_GOAL,           "ai_goal",           "Returns an AI goal (used as argument to ai-goals operators)", compat_ai_goal },
	{ OPR_STRING,            "string",            "Returns a string value", compat_string },
	{ OPR_AMBIGUOUS,         "ambiguous",         "Return type depends on variable type (used with variables)", compat_ambiguous },
	{ OPR_FLEXIBLE_ARGUMENT, "flexible_argument", "Flexible argument return (used with when-argument operators)", compat_flexible },
	{ OPR_NONE,              "none",              "No return type", compat_null },

	// sentinel
	{ OPR_NONE, nullptr, nullptr, nullptr }
};

const char *opr_to_name(int opr_value)
{
    for (const opr_type_info *t = Opr_type_info; t->name; t++) {
        if (t->opr_value == opr_value)
            return t->name;
    }
    return "unknown";
}
