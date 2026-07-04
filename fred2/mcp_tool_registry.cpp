#include "stdafx.h"
#include "mcp_tool_registry.h"
#include "mcp_app.h"
#include "mcp_messages.h"

#include <cstring>

#include "globalincs/pstypes.h"
#include "globalincs/vmallocator.h"

// ---------------------------------------------------------------------------
// Canonical area order == client-visible tools/list order.
// Do not reorder — clients and tests see this order.
//
// During the registry migration, areas are added here as they are converted;
// unconverted areas are still registered/dispatched by the legacy cascade in
// mcpserver.cpp / mcp_mission_tools.cpp.
// ---------------------------------------------------------------------------

static const McpToolArea s_areas[] = {
	{ "app",      mcp_app_tool_defs,     mcp_app_tool_def_count     },
	{ "messages", mcp_message_tool_defs, mcp_message_tool_def_count },
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
