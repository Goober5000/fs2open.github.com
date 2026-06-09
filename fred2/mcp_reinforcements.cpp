#include "stdafx.h"
#include "mcp_reinforcements.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcpserver.h"

#include <jansson.h>
#include <cstring>

#include "globalincs/utility.h"

#include "ship/ship.h"
#include "management.h"          // set_reinforcement

// ---------------------------------------------------------------------------
// Dialog conflict guard
// ---------------------------------------------------------------------------

static bool validate_dialog_for_reinforcements(SCP_string &error_msg)
{
	return validate_single_dialog("reinforcements", "reinforcement", error_msg)
		&& validate_single_dialog("ships", "ship", error_msg)
		&& validate_single_dialog("wings", "wing", error_msg);
}

// ---------------------------------------------------------------------------
// FRED dialog ranges (mirrored from reinforcementeditordlg.cpp)
// ---------------------------------------------------------------------------

static constexpr int REINFORCEMENT_USES_MIN          = 1;
static constexpr int REINFORCEMENT_USES_MAX          = 99;
static constexpr int REINFORCEMENT_ARRIVAL_DELAY_MIN = 0;
static constexpr int REINFORCEMENT_ARRIVAL_DELAY_MAX = 1000;

// ---------------------------------------------------------------------------
// JSON builders
// ---------------------------------------------------------------------------

static json_t *build_reinforcement_json(const reinforcements &r)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_safe_string(r.name));
	json_object_set_new(obj, "uses", json_integer(r.uses));
	json_object_set_new(obj, "arrival_delay", json_integer(r.arrival_delay));
	return obj;
}

// ---------------------------------------------------------------------------
// Eligibility resolution
//
// Mirrors the FRED reinforcement picker (reinforcementeditordlg.cpp):
//   - ships in a wing are ineligible (the wing is the reinforcement instead)
//   - wings are eligible only if all member ships share a team
// ---------------------------------------------------------------------------

enum class reinforcement_kind { ship, wing };

static bool resolve_reinforcement_target(const char *name, reinforcement_kind &out_kind,
	int &out_index, McpErrorSink &sink)
{
	// Try ship first; suppress lookup_ship's "Ship not found" error if it misses,
	// so we can transparently fall through to wings.
	{
		json_t *suppressed = nullptr;
		McpErrorSink ship_sink(&suppressed);
		int ship_idx = lookup_ship(name, ship_sink);
		if (suppressed)
			json_decref(suppressed);
		if (ship_idx >= 0) {
			if (Ships[ship_idx].wingnum >= 0) {
				sink.set_error("Ship '%s' is in wing '%s' — make the wing a reinforcement instead.",
					name, Wings[Ships[ship_idx].wingnum].name);
				return false;
			}
			out_kind  = reinforcement_kind::ship;
			out_index = ship_idx;
			return true;
		}
	}

	int wing_idx = wing_name_lookup(name);
	if (wing_idx >= 0) {
		out_kind  = reinforcement_kind::wing;
		out_index = wing_idx;
		return true;
	}

	sink.set_error("Reinforcement target not found: '%s' "
		"(must name a ship not in a wing, or a wing)", name);
	return false;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void handle_list_reinforcements(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_reinforcements, sink)) return;

	json_t *arr = json_array();
	for (const auto &r : Reinforcements)
		json_array_append_new(arr, build_reinforcement_json(r));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_reinforcement(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_reinforcements, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int idx = find_item_with_string(Reinforcements, &reinforcements::name, name);
	if (idx < 0) {
		set_not_found_error(sink, "Reinforcement", name);
		return;
	}

	req->result_json = make_json_tool_result(build_reinforcement_json(Reinforcements[idx]));
	req->success = true;
}

static void handle_set_reinforcement(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_reinforcements, sink)) return;

	auto name = get_required_string(input, "name", sink, true, NAME_LENGTH - 1);
	if (!name) return;

	auto enable        = get_optional_bool(input, "enable", sink);
	auto new_uses      = get_optional_integer(input, "uses", sink);
	auto new_delay     = get_optional_integer(input, "arrival_delay", sink);
	if (sink.has_error()) return;

	if (new_uses.has_value() &&
		!check_int_range(*new_uses, REINFORCEMENT_USES_MIN, REINFORCEMENT_USES_MAX, "uses", sink))
		return;
	if (new_delay.has_value() &&
		!check_int_range(*new_delay, REINFORCEMENT_ARRIVAL_DELAY_MIN, REINFORCEMENT_ARRIVAL_DELAY_MAX, "arrival_delay", sink))
		return;

	bool removing = enable.has_value() && !*enable;

	// Resolve the entity.  Required even when removing, so we report a clear
	// "target not found" error instead of silently no-op'ing on typos.
	reinforcement_kind kind = reinforcement_kind::ship;
	int entity_idx = -1;
	if (!resolve_reinforcement_target(name, kind, entity_idx, sink))
		return;

	int existing = find_item_with_string(Reinforcements, &reinforcements::name, name);

	if (removing) {
		if (existing < 0) {
			sink.set_error("Reinforcement '%s' is not currently set.", name);
			return;
		}
		set_reinforcement(name, 0);
		mark_modified("MCP: clear reinforcement %s", name);

		sprintf(req->result_message, "Reinforcement removed: %s", name);
		req->success = true;
		return;
	}

	// Creating or updating.  For wings, mirror the FRED dialog's team-mixing check
	// on the create path (existing entries are left alone — disturbing the team
	// composition after-the-fact is a separate concern).
	bool creating = (existing < 0);
	if (creating && kind == reinforcement_kind::wing) {
		if (wing_has_conflicting_teams(entity_idx)) {
			sink.set_error("Wing '%s' has members from conflicting teams and cannot be a reinforcement.", name);
			return;
		}
	}

	if (creating)
		set_reinforcement(name, 1);

	// Re-lookup after the engine helper may have inserted at the back.
	int idx = find_item_with_string(Reinforcements, &reinforcements::name, name);
	Assertion(idx >= 0, "set_reinforcement did not insert a Reinforcements entry for '%s'", name);

	// Compare against current values so redundant calls don't autosave.
	bool changed = creating;
	if (new_uses.has_value() && Reinforcements[idx].uses != *new_uses) {
		Reinforcements[idx].uses = *new_uses;
		changed = true;
	}
	if (new_delay.has_value() && Reinforcements[idx].arrival_delay != *new_delay) {
		Reinforcements[idx].arrival_delay = *new_delay;
		changed = true;
	}

	if (changed)
		mark_modified("MCP: set reinforcement %s", name);

	req->result_json = make_json_tool_result(build_reinforcement_json(Reinforcements[idx]));
	req->success = true;
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_reinforcement_tools(json_t *tools)
{
	// list_reinforcements
	register_tool(tools, "list_reinforcements",
		"List all reinforcement entries in the mission. Each entry names a ship "
		"(not in a wing) or a whole wing that can be summoned via the squad-message "
		"menu. Returns each entry's name, uses, and arrival_delay.",
		json_object());

	// get_reinforcement
	register_tool_with_required_string(tools, "get_reinforcement",
		"Get the reinforcement entry for a named ship or wing. "
		"Returns name, uses, and arrival_delay.",
		"name", "Name of the ship or wing whose reinforcement entry to retrieve");

	// set_reinforcement
	{
		json_t *props = json_object();
		add_string_prop(props, "name",
			"Ship or wing name. A ship may only be a reinforcement if it is not "
			"part of a wing (if it is, designate the wing instead). A wing may "
			"only be a reinforcement if all its member ships share a team.");
		add_bool_prop(props, "enable",
			"Whether the named entity should be a reinforcement. Omit or set to "
			"true to create or keep the entry (also applies optional uses / "
			"arrival_delay). Set to false to remove the entry; uses and "
			"arrival_delay are then ignored.");
		add_integer_prop(props, "uses",
			"Number of times this reinforcement can be summoned (1-99). "
			"For wing reinforcements this becomes the wing's num_waves at mission "
			"load. For ship reinforcements the field is stored but functionally "
			"inert (a ship always arrives once). Omit to leave unchanged.");
		add_integer_prop(props, "arrival_delay",
			"Seconds between summoning and arrival (0-1000). Omit to leave unchanged.");
		json_t *required = json_array();
		json_array_append_new(required, json_string("name"));
		register_tool(tools, "set_reinforcement",
			"Create, update, or remove a reinforcement entry for a ship or wing. "
			"Adding an entry also sets the ship/wing's reinforcement flag; "
			"removing one clears it. Use this in preference to toggling the "
			"reinforcement flag through update_ship / update_wing.",
			props, required);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_reinforcement_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_reinforcements") == 0) {
		handle_list_reinforcements(input_json, req);
	} else if (strcmp(tool_name, "get_reinforcement") == 0) {
		handle_get_reinforcement(input_json, req);
	} else if (strcmp(tool_name, "set_reinforcement") == 0) {
		handle_set_reinforcement(input_json, req);
	} else {
		return false;
	}
	return true;
}
