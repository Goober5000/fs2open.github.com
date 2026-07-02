#include "stdafx.h"
#include "mcp_tool_registry.h"
#include "mcp_app.h"
#include "mcp_reference_tools.h"
#include "mcp_mission_info.h"
#include "mcp_fiction_viewer.h"
#include "mcp_cmd_brief.h"
#include "mcp_brief.h"
#include "mcp_debrief.h"
#include "mcp_messages.h"
#include "mcp_sexp.h"
#include "mcp_events.h"
#include "mcp_goals.h"
#include "mcp_waypoints.h"
#include "mcp_jump_node.h"
#include "mcp_ships.h"
#include "mcp_submodels.h"
#include "mcp_wings.h"
#include "mcp_reinforcements.h"
#include "mcp_loadout.h"
#include "mcp_mission_tools.h"

#include <cstring>

#include "globalincs/pstypes.h"
#include "globalincs/vmallocator.h"

// ---------------------------------------------------------------------------
// Canonical area order == client-visible tools/list order.
// Do not reorder — clients and tests see this order.
// ---------------------------------------------------------------------------

static const McpToolArea s_areas[] = {
	{ "app",            mcp_app_tool_defs,            mcp_app_tool_def_count            },
	{ "reference",      mcp_reference_tool_defs,      mcp_reference_tool_def_count      },
	{ "mission_info",   mcp_mission_info_tool_defs,   mcp_mission_info_tool_def_count   },
	{ "fiction_viewer", mcp_fiction_viewer_tool_defs, mcp_fiction_viewer_tool_def_count },
	{ "cmd_brief",      mcp_cmd_brief_tool_defs,      mcp_cmd_brief_tool_def_count      },
	{ "brief",          mcp_brief_tool_defs,          mcp_brief_tool_def_count          },
	{ "debrief",        mcp_debrief_tool_defs,        mcp_debrief_tool_def_count        },
	{ "messages",       mcp_message_tool_defs,        mcp_message_tool_def_count        },
	{ "sexp",           mcp_sexp_tool_defs,           mcp_sexp_tool_def_count           },
	{ "events",         mcp_event_tool_defs,          mcp_event_tool_def_count          },
	{ "goals",          mcp_goal_tool_defs,           mcp_goal_tool_def_count           },
	{ "waypoints",      mcp_waypoint_tool_defs,       mcp_waypoint_tool_def_count       },
	{ "jump_nodes",     mcp_jump_node_tool_defs,      mcp_jump_node_tool_def_count      },
	{ "ships",          mcp_ship_tool_defs,           mcp_ship_tool_def_count           },
	{ "submodels",      mcp_submodel_tool_defs,       mcp_submodel_tool_def_count       },
	{ "wings",          mcp_wing_tool_defs,           mcp_wing_tool_def_count           },
	{ "reinforcements", mcp_reinforcement_tool_defs,  mcp_reinforcement_tool_def_count  },
	{ "loadout",        mcp_loadout_tool_defs,        mcp_loadout_tool_def_count        },
	{ "mission_misc",   mcp_mission_misc_tool_defs,   mcp_mission_misc_tool_def_count   },
};

static SCP_unordered_map<SCP_string, const McpToolDef *> s_tool_lookup;

void mcp_tool_registry_init()
{
	if (!s_tool_lookup.empty())
		return;		// idempotent (server restart)

	for (const auto &area : s_areas) {
		for (size_t i = 0; i < area.count; ++i) {
			const McpToolDef *def = &area.tools[i];
			Assertion(def->name != nullptr && def->register_fn != nullptr,
				"MCP tool table entry " SIZE_T_ARG " in area '%s' is incomplete", i, area.area_name);
			Assertion((def->worker_handler != nullptr) != (def->main_handler != nullptr),
				"MCP tool '%s' must have exactly one handler", def->name);
			auto inserted = s_tool_lookup.emplace(SCP_string(def->name), def);
			Assertion(inserted.second, "Duplicate MCP tool name '%s' (area '%s')",
				def->name, area.area_name);
		}
	}

#ifndef NDEBUG
	// Catch drift between the table entry's name and the name literal inside
	// its register function: each register_fn must register exactly one tool
	// whose "name" matches the table entry.
	for (const auto &area : s_areas) {
		for (size_t i = 0; i < area.count; ++i) {
			const McpToolDef *def = &area.tools[i];
			json_t *scratch = json_array();
			def->register_fn(scratch);
			Assertion(json_array_size(scratch) == 1,
				"register_fn for '%s' registered " SIZE_T_ARG " tools", def->name, json_array_size(scratch));
			const char *reg_name = json_string_value(json_object_get(json_array_get(scratch, 0), "name"));
			Assertion(reg_name != nullptr && strcmp(reg_name, def->name) == 0,
				"register_fn for '%s' registered '%s'", def->name, reg_name ? reg_name : "<null>");
			json_decref(scratch);
		}
	}
#endif
}

const McpToolDef *mcp_find_tool(const char *name)
{
	auto it = s_tool_lookup.find(SCP_string(name));
	return (it == s_tool_lookup.end()) ? nullptr : it->second;
}

void mcp_register_all_tools(json_t *tools)
{
	for (const auto &area : s_areas)
		for (size_t i = 0; i < area.count; ++i)
			area.tools[i].register_fn(tools);
}
