#include "stdafx.h"
#include "mcp_wings.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcpserver.h"

#include <jansson.h>
#include <cstring>

#include "missioneditor/common.h"     // FredWingSlotConfig, reassign_wing_slot, swap_wing_slots
#include "ship/ship.h"                // Wings, MAX_WINGS

#include "management.h"               // cur_wing, wing_objects

// ---------------------------------------------------------------------------
// Dialog conflict guard
// ---------------------------------------------------------------------------

static bool validate_dialog_for_wings(SCP_string &error_msg)
{
	return validate_single_dialog("wings", "wing", error_msg);
}

// ---------------------------------------------------------------------------
// Move / swap
// ---------------------------------------------------------------------------

// Sentinel matching the engine convention in code/scripting/api/libs/mission.cpp.
static constexpr int COUNT_WINGS = -1000;

// Same pattern as ship_slot_at_public_index (mcp_ships.cpp), adapted for wings.
// Wings aren't Objects, so the occupancy sentinel is wave_count > 0.
static int wing_slot_at_public_index(int index)
{
	int count = 0;
	for (int i = 0; i < MAX_WINGS; ++i) {
		if (Wings[i].wave_count == 0)
			continue;
		++count;
		if (count == index)
			return i;
	}
	return (index == COUNT_WINGS) ? count : -1;
}

static int wing_slot_count()
{
	return wing_slot_at_public_index(COUNT_WINGS);
}

static FredWingSlotConfig make_wing_slot_config()
{
	FredWingSlotConfig wcfg;
	wcfg.wing_objects = wing_objects;
	wcfg.cur_wing = &cur_wing;
	return wcfg;
}

static MoveSwapConfig make_wing_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "wing";
	cfg.count = wing_slot_count();
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_wings;
	cfg.get_name = [](int i) {
		return SCP_string(Wings[wing_slot_at_public_index(i)].name);
	};
	cfg.do_move = [](int from, int to) {
		// Walk the moving element via adjacent swaps.  After each swap the
		// sparse positions of *other* occupied slots are unchanged, so
		// wing_slot_at_public_index remains correct for the next step.
		auto wcfg = make_wing_slot_config();
		int step = (from < to) ? 1 : -1;
		for (int pos = from; pos != to; pos += step) {
			int a = wing_slot_at_public_index(pos);
			int b = wing_slot_at_public_index(pos + step);
			swap_wing_slots(a, b, wcfg);
		}
	};
	cfg.do_swap = [](int a, int b) {
		int sa = wing_slot_at_public_index(a);
		int sb = wing_slot_at_public_index(b);
		swap_wing_slots(sa, sb, make_wing_slot_config());
	};
	return cfg;
}

static void handle_move_wing(json_t *input, McpToolRequest *req)
{
	auto cfg = make_wing_move_swap_config();
	handle_generic_move(input, req, cfg);
}

static void handle_swap_wings(json_t *input, McpToolRequest *req)
{
	auto cfg = make_wing_move_swap_config();
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_wing_tools(json_t *tools)
{
	// move_wing
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"1-based index of the wing to move");
		add_integer_prop(props, "to_index",
			"1-based destination index");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_wing",
			"Move a wing from one list position to another.  Indices are 1-based.",
			props, req);
	}

	// swap_wings
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first wing");
		add_integer_prop(props, "index_b",
			"1-based index of the second wing");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_wings",
			"Swap two wings at the given list positions.  Indices are 1-based.",
			props, req);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_wing_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "move_wing") == 0) {
		handle_move_wing(input_json, req);
	} else if (strcmp(tool_name, "swap_wings") == 0) {
		handle_swap_wings(input_json, req);
	} else {
		return false;
	}
	return true;
}
