#ifndef _MCP_MISSION_TOOLS_H
#define _MCP_MISSION_TOOLS_H

#include <jansson.h>
#include <functional>
#include <optional>
#include "globalincs/vmallocator.h"

struct McpToolRequest;
class McpErrorSink;
enum class sexp_ref_type;
class anchor_t;

// Sets the mission as modified and autosaves it.
void mark_modified(const char *fmt, ...);

// Append mission CRUD tool schemas to the tools array used by tools/list.
void mcp_register_mission_tools(json_t *tools);

// Try to route a mission tool call.  Returns a json_t* result if the tool
// name matched one of the mission tools (marshaled to main thread), or
// nullptr if the name is not recognized.
json_t *mcp_route_mission_tool(const char *tool_name, json_t *arguments);

// Main-thread handler for MISSION_TOOL calls.
// Called from OnMcpToolCall when req->tool == McpToolId::MISSION_TOOL.
void mcp_handle_mission_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

// ---------------------------------------------------------------------------
// Shared helpers for entity submodules (messages, sexp, mission_info, etc.)
// ---------------------------------------------------------------------------

// Returns true if deletion should proceed, false if blocked by references.
// Handles the common pattern: check SEXP refs unless force is set, report error if found.
bool check_and_report_sexp_refs(sexp_ref_type ref_type, const char *entity_label,
	const char *name, std::optional<bool> force, McpErrorSink &sink);

extern const SCP_vector<const char *> team_enum_values;

const char *team_name_from_index(int multi_team);
int team_index_from_name(const char *name);

// Returns true (and sets sink error) if team_str equals "none"; rejects "none"
// for entities that don't allow it (e.g. command briefings, debriefings).
bool reject_team_none(const char *team_str, const char *entity_name, McpErrorSink &sink);

// ---------------------------------------------------------------------------
// Arrival/departure location enums (shared by ships and wings)
//
// Built once from the engine's Arrival_location_names / Departure_location_names
// arrays so handlers can pass them to check_lookup and add_string_enum_prop
// without duplicating the literal strings.
// ---------------------------------------------------------------------------

extern const SCP_vector<const char *> arrival_location_enum_values;
extern const SCP_vector<const char *> departure_location_enum_values;

// ---------------------------------------------------------------------------
// Anchor encoding (shared by ships and wings)
//
// Wings and ships both store arrival/departure targets as anchor_t.  The
// MCP-facing representation is a single string -- either a ship name or a
// special token like "<any friendly>".
// ---------------------------------------------------------------------------

// Resolve a target name to an anchor.  Empty/null name => anchor_t::invalid().
// Returns true on success, false (with error set) on unknown name.
bool resolve_target_name_to_anchor(const char *name, anchor_t &out, McpErrorSink &sink);

// Decode an anchor value back to a human-readable name.  Returns an empty
// string for invalid anchors or unresolved references.
SCP_string anchor_to_name(anchor_t anchor);

// ---------------------------------------------------------------------------
// SEXP cue replacement (shared by ships and wings)
//
// Frees the previous user-allocated tree (skipping the Locked_sexp_true/false
// sentinels), assigns the new node, and marks the SEXP forest dirty.
// ---------------------------------------------------------------------------

void replace_cue(int &cue_slot, int new_cue);

// Configuration for entity-specific move/swap behavior.
// Lambdas encapsulate offsets, annotation updates, and array access.
struct MoveSwapConfig
{
	const char *entity_name = nullptr;
	int count = 0;
	bool one_based = false;		// if true, user-facing indices start at 1 instead of 0
	std::function<bool(SCP_string&)> validate_dialog = nullptr;
	std::function<SCP_string(int)> get_name = nullptr;	// copy-return isn't ideal, but certain use cases work better with it and the MCP isn't performance-critical
	std::function<void(int, int)> do_move = nullptr;
	std::function<void(int, int)> do_swap = nullptr;

	int min_index() const { return one_based ? 1 : 0; }
	int max_index() const { return one_based ? count : count - 1; }
};

void handle_generic_move(json_t *input, McpToolRequest *req, const MoveSwapConfig &cfg);
void handle_generic_swap(json_t *input, McpToolRequest *req, const MoveSwapConfig &cfg);

#endif // _MCP_MISSION_TOOLS_H
