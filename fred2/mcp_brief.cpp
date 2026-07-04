#include "stdafx.h"
#include "mcp_brief.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "mcp_array_utils.h"
#include "mcp_sexp.h"
#include "mcp_sexp_forest.h"
#include "fredrender.h"

#include <jansson.h>
#include <climits>
#include <cstring>
#include <iterator>

#include "globalincs/utility.h"

#include "globalincs/alphacolors.h"
#include "graphics/2d.h"
#include "iff_defs/iff_defs.h"
#include "jumpnode/jumpnode.h"
#include "math/vecmat.h"
#include "mission/missionbriefcommon.h"
#include "mission/missionparse.h"
#include "object/object.h"
#include "object/waypoint.h"
#include "parse/parselo.h"
#include "parse/sexp.h"
#include "ship/ship.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_briefing(SCP_string &error_msg)
{
	return validate_single_dialog("briefing stages", "briefing", error_msg);
}

// ---------------------------------------------------------------------------
// Briefing stage tools
// ---------------------------------------------------------------------------

// Resolve optional "team" parameter to a briefing pointer.
// Defaults to Team 1. Rejects "none".
static briefing *get_briefing_for_team(json_t *input, McpErrorSink &sink)
{
	auto team_str = get_optional_string(input, "team", sink);
	if (sink.has_error()) return nullptr;
	int team_index = 0;  // default to Team 1

	if (team_str) {
		if (!check_string_enum(team_str, team_selector_enum_values, "team", sink))
			return nullptr;
		if (reject_team_none(team_str, "briefing", sink)) return nullptr;
		team_index = team_index_from_name(team_str);
	}

	return &Briefings[team_index];
}

// Restore a stage slot to its default state without losing its buffers.
// brief_stage::reset() nulls the icons/lines pointers, but in FRED every
// stage slot's buffers are pre-allocated at startup and must be preserved.
static void reset_stage_preserving_buffers(brief_stage &s)
{
	auto *icons = s.icons;
	auto *lines = s.lines;
	s.reset();
	s.icons = icons;
	s.lines = lines;
}

static json_t *build_brief_icon_json(const brief_icon &icon, int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "id", json_integer(icon.id));
	if (icon.type >= 0 && icon.type < MIN_BRIEF_ICONS)
		json_object_set_new(obj, "icon_type", json_string(Icon_names[icon.type]));
	if (icon.team >= 0 && icon.team < (int)Iff_info.size())
		json_object_set_new(obj, "iff", json_safe_string(Iff_info[icon.team].iff_name));
	if (icon.ship_class >= 0 && icon.ship_class < ship_info_size())
		json_object_set_new(obj, "ship_class", json_safe_string(Ship_info[icon.ship_class].name));
	json_object_set_new(obj, "position", build_vec3d_json(icon.pos));
	json_object_set_new(obj, "label", json_safe_string(icon.label));
	set_optional_string(obj, "closeup_label", icon.closeup_label, true);
	json_object_set_new(obj, "scale", json_integer(static_cast<int>(icon.scale_factor * 100.0f)));
	json_object_set_new(obj, "highlight", json_boolean((icon.flags & BI_HIGHLIGHT) != 0));
	json_object_set_new(obj, "mirror", json_boolean((icon.flags & BI_MIRROR_ICON) != 0));
	json_object_set_new(obj, "use_wing_icon", json_boolean((icon.flags & BI_USE_WING_ICON) != 0));
	json_object_set_new(obj, "use_cargo_icon", json_boolean((icon.flags & BI_USE_CARGO_ICON) != 0));
	return obj;
}

// Line endpoints are 1-based icon indices, matching the icons array.
static json_t *build_brief_line_json(const brief_line &line, int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "start_icon", json_integer(line.start_icon + 1));
	json_object_set_new(obj, "end_icon", json_integer(line.end_icon + 1));
	return obj;
}

static json_t *build_brief_stage_json(const brief_stage &stage, int index)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "text", json_safe_string(stage.text.c_str()));
	set_optional_filename(obj, "voice_filename", stage.voice);
	json_object_set_new(obj, "camera_position", build_vec3d_json(stage.camera_pos));
	json_object_set_new(obj, "camera_orientation", build_matrix_json(stage.camera_orient));
	json_object_set_new(obj, "camera_time", json_integer(stage.camera_time));
	json_object_set_new(obj, "cut_to_next", json_boolean((stage.flags & BS_FORWARD_CUT) != 0));
	json_object_set_new(obj, "cut_from_previous", json_boolean((stage.flags & BS_BACKWARD_CUT) != 0));
	json_object_set_new(obj, "draw_grid", json_boolean(stage.draw_grid));
	json_object_set_new(obj, "grid_color", build_color_json(stage.grid_color, true));
	json_object_set_new(obj, "formula", json_integer(stage.formula));

	json_t *icons = json_array();
	for (int i = 0; i < stage.num_icons; i++)
		json_array_append_new(icons, build_brief_icon_json(stage.icons[i], i));
	json_object_set_new(obj, "icons", icons);

	json_t *lines = json_array();
	for (int i = 0; i < stage.num_lines; i++)
		json_array_append_new(lines, build_brief_line_json(stage.lines[i], i));
	json_object_set_new(obj, "lines", lines);

	return obj;
}

static void handle_list_briefing_stages(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	json_t *arr = json_array();
	for (int i = 0; i < br->num_stages; i++)
		json_array_append_new(arr, build_brief_stage_json(br->stages[i], i));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_briefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, br->num_stages, "index", sink)) return;

	req->result_json = make_json_tool_result(build_brief_stage_json(br->stages[*index - 1], *index - 1));
	req->success = true;
}

static void handle_create_briefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	if (br->num_stages >= MAX_BRIEF_STAGES) {
		sink.set_error("Cannot add more than %d briefing stages", MAX_BRIEF_STAGES);
		return;
	}

	auto text = get_required_string(input, "text", sink, false, MULTITEXT_LENGTH - 1);
	if (!text) return;

	auto voice             = get_optional_filename(input, "voice_filename", sink, false);
	auto camera_pos        = get_optional_vec3d(input, "camera_position", sink);
	auto camera_orient     = get_optional_matrix(input, "camera_orientation", sink);
	auto camera_time       = get_optional_integer(input, "camera_time", sink);
	auto cut_to_next       = get_optional_bool(input, "cut_to_next", sink);
	auto cut_from_previous = get_optional_bool(input, "cut_from_previous", sink);
	auto draw_grid         = get_optional_bool(input, "draw_grid", sink);
	auto grid_color        = get_optional_color(input, "grid_color", sink);
	auto formula           = get_optional_integer(input, "formula", sink);
	auto insert_index      = get_optional_integer(input, "index", sink);
	auto copy_from_prev    = get_optional_bool(input, "copy_from_previous", sink);
	if (sink.has_error()) return;
	if (formula.has_value() && !check_sexp_formula(*formula, OPR_BOOL, sink)) return;
	if (camera_time.has_value() && !check_int_range(*camera_time, 0, INT_MAX, "camera_time", sink)) return;

	int target;
	if (!insert_index.has_value()) {
		target = br->num_stages;
	} else {
		if (!check_int_range(*insert_index, 1, br->num_stages + 1, "index", sink))
			return;
		target = *insert_index - 1;
	}

	if (!array_insert_slot_swap(br->stages, br->num_stages, MAX_BRIEF_STAGES, target)) {
		sink.set_error("Cannot add more than %d briefing stages", MAX_BRIEF_STAGES);
		return;
	}

	// The slot arriving at target holds the former one-past-end element's
	// stale state; wipe it before filling it in.
	brief_stage &s = br->stages[target];
	reset_stage_preserving_buffers(s);

	// Determine the stage to inherit from: the stage preceding the insertion
	// point, or when inserting at the front, the stage that used to be first.
	int source = -1;
	if (copy_from_prev.value_or(true)) {
		if (target > 0)
			source = target - 1;
		else if (br->num_stages > 1)
			source = 1;
	}

	if (source >= 0) {
		// Copy camera, flags, grid settings, and icon/line contents.  Voice is
		// per-stage audio, and copying the formula root would alias the SEXP
		// tree between stages, so neither is inherited.
		const brief_stage &src = br->stages[source];
		s.camera_pos = src.camera_pos;
		s.camera_orient = src.camera_orient;
		s.camera_time = src.camera_time;
		s.flags = src.flags;
		s.draw_grid = src.draw_grid;
		s.grid_color = src.grid_color;
		s.num_icons = src.num_icons;
		memcpy(s.icons, src.icons, sizeof(brief_icon) * MAX_STAGE_ICONS);
		s.num_lines = src.num_lines;
		memcpy(s.lines, src.lines, sizeof(brief_line) * MAX_BRIEF_STAGE_LINES);
	} else {
		s.camera_pos = view_pos;
		s.camera_orient = view_orient;
		s.camera_time = 500;
	}

	// Explicit parameters override inherited/default values
	s.text = text;
	strcpy_s(s.voice, (voice && voice[0]) ? voice : "none");
	if (camera_pos.has_value())
		s.camera_pos = *camera_pos;
	if (camera_orient.has_value())
		s.camera_orient = *camera_orient;
	if (camera_time.has_value())
		s.camera_time = *camera_time;
	if (cut_to_next.has_value())
		s.flags = *cut_to_next ? (s.flags | BS_FORWARD_CUT) : (s.flags & ~BS_FORWARD_CUT);
	if (cut_from_previous.has_value())
		s.flags = *cut_from_previous ? (s.flags | BS_BACKWARD_CUT) : (s.flags & ~BS_BACKWARD_CUT);
	if (draw_grid.has_value())
		s.draw_grid = *draw_grid;
	if (grid_color.has_value())
		s.grid_color = *grid_color;

	if (formula.has_value()) {
		s.formula = *formula;
	} else {
		s.formula = Locked_sexp_true;
	}

	mcp_sexp_forest_mark_dirty({ s.formula });
	mark_modified("MCP: create briefing stage %d", target + 1);

	req->result_json = make_json_tool_result(build_brief_stage_json(br->stages[target], target));
	req->success = true;
}

static void handle_update_briefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, br->num_stages, "index", sink)) return;

	auto new_text          = get_optional_string(input, "text", sink, MULTITEXT_LENGTH - 1);
	auto new_voice         = get_optional_filename(input, "voice_filename", sink, false);
	auto new_camera_pos    = get_optional_vec3d(input, "camera_position", sink);
	auto new_camera_orient = get_optional_matrix(input, "camera_orientation", sink);
	auto new_camera_time   = get_optional_integer(input, "camera_time", sink);
	auto cut_to_next       = get_optional_bool(input, "cut_to_next", sink);
	auto cut_from_previous = get_optional_bool(input, "cut_from_previous", sink);
	auto new_draw_grid     = get_optional_bool(input, "draw_grid", sink);
	auto new_grid_color    = get_optional_color(input, "grid_color", sink);
	auto new_formula       = get_optional_integer(input, "formula", sink);
	if (sink.has_error()) return;
	if (new_formula.has_value() && !check_sexp_formula(*new_formula, OPR_BOOL, sink)) return;
	if (new_camera_time.has_value() && !check_int_range(*new_camera_time, 0, INT_MAX, "camera_time", sink)) return;

	brief_stage &s = br->stages[*index - 1];
	bool changed = false;

	if (new_text && s.text != new_text) {
		s.text = new_text;
		changed = true;
	}
	if (new_voice) {
		const char *effective_voice = new_voice[0] ? new_voice : "none";
		if (strcmp(s.voice, effective_voice) != 0) {
			strcpy_s(s.voice, effective_voice);
			changed = true;
		}
	}
	if (new_camera_pos.has_value() && !vm_vec_same(&s.camera_pos, &*new_camera_pos)) {
		s.camera_pos = *new_camera_pos;
		changed = true;
	}
	if (new_camera_orient.has_value() && !vm_matrix_same(&s.camera_orient, &*new_camera_orient)) {
		s.camera_orient = *new_camera_orient;
		changed = true;
	}
	if (new_camera_time.has_value() && s.camera_time != *new_camera_time) {
		s.camera_time = *new_camera_time;
		changed = true;
	}

	int flags = s.flags;
	if (cut_to_next.has_value())
		flags = *cut_to_next ? (flags | BS_FORWARD_CUT) : (flags & ~BS_FORWARD_CUT);
	if (cut_from_previous.has_value())
		flags = *cut_from_previous ? (flags | BS_BACKWARD_CUT) : (flags & ~BS_BACKWARD_CUT);
	if (flags != s.flags) {
		s.flags = flags;
		changed = true;
	}

	if (new_draw_grid.has_value() && s.draw_grid != *new_draw_grid) {
		s.draw_grid = *new_draw_grid;
		changed = true;
	}
	if (new_grid_color.has_value() && !gr_compare_color_values(s.grid_color, *new_grid_color)) {
		s.grid_color = *new_grid_color;
		changed = true;
	}
	if (new_formula.has_value() && replace_cue(s.formula, *new_formula)) {
		changed = true;
	}

	if (changed)
		mark_modified("MCP: update briefing stage %d", *index);

	req->result_json = make_json_tool_result(build_brief_stage_json(s, *index - 1));
	req->success = true;
}

static void handle_delete_briefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, br->num_stages, "index", sink)) return;

	free_cue(br->stages[*index - 1].formula);

	array_remove_slot_swap(br->stages, br->num_stages, *index - 1);

	// The removed stage was parked at the one-past-end slot; wipe its
	// leftovers so stale text/icons can't resurface.
	reset_stage_preserving_buffers(br->stages[br->num_stages]);

	mark_modified("MCP: delete briefing stage %d", *index);
	sprintf(req->result_message,
		"Deleted briefing stage %d", *index);
	req->success = true;
}

static MoveSwapConfig make_briefing_move_swap_config(briefing *br)
{
	MoveSwapConfig cfg;
	cfg.entity_name = "briefing stage";
	cfg.count = br->num_stages;
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_briefing;
	cfg.get_name = [](int index) {
		SCP_string name;
		sprintf(name, "briefing stage %d", index);
		return name;
	};
	cfg.do_move = [br](int from, int to) {
		array_move_element_swap(br->stages, br->num_stages, from - 1, to - 1);
	};
	cfg.do_swap = [br](int a, int b) {
		swap(br->stages[a - 1], br->stages[b - 1]);
	};
	return cfg;
}

static void handle_move_briefing_stage(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	auto cfg = make_briefing_move_swap_config(br);
	handle_generic_move(input, req, cfg);
}

static void handle_swap_briefing_stages(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	auto cfg = make_briefing_move_swap_config(br);
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Briefing icon tools
// ---------------------------------------------------------------------------

static const SCP_vector<const char *> &icon_type_enum_values()
{
	static const SCP_vector<const char *> values(Icon_names, Icon_names + MIN_BRIEF_ICONS);
	return values;
}

// Resolve the required "stage" parameter to a 0-based stage index.
// Returns -1 and sets an error on failure.
static int get_stage_param(json_t *input, const briefing *br, McpErrorSink &sink)
{
	auto stage = get_required_integer(input, "stage", sink);
	if (!stage.has_value()) return -1;
	if (!check_int_range(*stage, 1, br->num_stages, "stage", sink)) return -1;
	return *stage - 1;
}

// Find the icon with the given id in a stage, or -1.
static int find_icon_in_stage(const brief_stage &s, int id)
{
	if (id >= 0)
		for (int i = 0; i < s.num_icons; i++)
			if (s.icons[i].id == id)
				return i;
	return -1;
}

// Find the first navbuoy ship class, used for waypoint and jump node icons
// (which have no real ship class).  Returns -1 if the mod has none.
static int first_navbuoy_class()
{
	for (auto it = Ship_info.cbegin(); it != Ship_info.cend(); ++it)
		if (it->flags[Ship::Info_Flags::Navbuoy])
			return (int)std::distance(Ship_info.cbegin(), it);
	return -1;
}

// Pick the icon type for a ship the way the briefing editor's Make Icon
// button does for a single (non-wing) ship with no docked cargo.
static int icon_type_for_ship(const ship_info *sip)
{
	if (sip->flags[Ship::Info_Flags::Knossos_device])
		return ICON_KNOSSOS_DEVICE;
	if (sip->flags[Ship::Info_Flags::Corvette])
		return ICON_CORVETTE;
	if (sip->flags[Ship::Info_Flags::Gas_miner])
		return ICON_GAS_MINER;
	if (sip->flags[Ship::Info_Flags::Supercap])
		return ICON_SUPERCAP;
	if (sip->flags[Ship::Info_Flags::Sentrygun])
		return ICON_SENTRYGUN;
	if (sip->flags[Ship::Info_Flags::Awacs])
		return ICON_AWACS;
	if (sip->flags[Ship::Info_Flags::Cargo])
		return ICON_CARGO;
	if (sip->flags[Ship::Info_Flags::Support])
		return ICON_SUPPORT_SHIP;
	if (sip->flags[Ship::Info_Flags::Fighter])
		return ICON_FIGHTER;
	if (sip->flags[Ship::Info_Flags::Bomber])
		return ICON_BOMBER;
	if (sip->flags[Ship::Info_Flags::Freighter])
		return ICON_FREIGHTER_NO_CARGO;
	if (sip->flags[Ship::Info_Flags::Cruiser])
		return ICON_CRUISER;
	if (sip->flags[Ship::Info_Flags::Transport])
		return ICON_TRANSPORT;
	if (sip->flags[Ship::Info_Flags::Capital] || sip->flags[Ship::Info_Flags::Drydock])
		return ICON_CAPITAL;
	if (sip->flags[Ship::Info_Flags::Navbuoy])
		return ICON_WAYPOINT;
	return ICON_ASTEROID_FIELD;
}

struct derived_icon_props
{
	vec3d pos = vmd_zero_vector;
	int type = -1;
	int team = 0;         // IFF index
	int ship_class = -1;
	SCP_string label;
};

// Derive icon properties from a named mission entity, the way the briefing
// editor's Make Icon button does.  Tries ships, then waypoints (either an
// individual "List:n" waypoint or a list name, which uses the first point),
// then jump nodes.
static bool derive_icon_from_source(const char *source, derived_icon_props &out, McpErrorSink &sink)
{
	int ship = ship_name_lookup(source, 1);
	if (ship >= 0) {
		out.pos = Objects[Ships[ship].objnum].pos;
		out.team = Ships[ship].team;
		out.ship_class = Ships[ship].ship_info_index;
		out.type = icon_type_for_ship(&Ship_info[out.ship_class]);
		out.label = Ships[ship].ship_name;
		return true;
	}

	waypoint *wpt = find_matching_waypoint(source);
	if (!wpt) {
		waypoint_list *wp_list = find_matching_waypoint_list(source);
		if (wp_list && !wp_list->get_waypoints().empty())
			wpt = &wp_list->get_waypoints().front();
	}
	if (wpt) {
		out.pos = *wpt->get_pos();
		out.type = ICON_WAYPOINT;
		out.ship_class = first_navbuoy_class();
		out.label = wpt->get_parent_list()->get_name();
		return true;
	}

	CJumpNode *jnp = jumpnode_get_by_name(source);
	if (jnp) {
		out.pos = jnp->GetSCPObject()->pos;
		out.type = ICON_JUMP_NODE;
		out.ship_class = first_navbuoy_class();
		out.label = jnp->GetName();
		return true;
	}

	sink.set_error("No ship, waypoint, or jump node named '%s'", source);
	return false;
}

static void handle_create_briefing_icon(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	int stage_idx = get_stage_param(input, br, sink);
	if (stage_idx < 0) return;

	brief_stage &s = br->stages[stage_idx];
	if (s.num_icons >= MAX_STAGE_ICONS) {
		sink.set_error("Cannot add more than %d icons to a briefing stage", MAX_STAGE_ICONS);
		return;
	}

	auto source         = get_optional_string(input, "source", sink);
	auto position       = get_optional_vec3d(input, "position", sink);
	auto type_str       = get_optional_string(input, "icon_type", sink);
	auto iff_str        = get_optional_string(input, "iff", sink);
	auto class_str      = get_optional_string(input, "ship_class", sink);
	auto label          = get_optional_string(input, "label", sink, MAX_LABEL_LEN - 1);
	auto closeup_label  = get_optional_string(input, "closeup_label", sink, MAX_LABEL_LEN - 1);
	auto scale          = get_optional_integer(input, "scale", sink);
	auto highlight      = get_optional_bool(input, "highlight", sink);
	auto mirror         = get_optional_bool(input, "mirror", sink);
	auto use_wing_icon  = get_optional_bool(input, "use_wing_icon", sink);
	auto use_cargo_icon = get_optional_bool(input, "use_cargo_icon", sink);
	auto id             = get_optional_integer(input, "id", sink);
	auto propagate      = get_optional_bool(input, "propagate", sink);
	if (sink.has_error()) return;

	// Resolve enums up-front so we fail before mutation
	int type_idx = -1;
	if (type_str) {
		type_idx = check_lookup(type_str, icon_type_enum_values(), "icon_type", sink);
		if (type_idx < 0) return;
	}
	int iff_idx = -1;
	if (iff_str) {
		iff_idx = check_lookup(iff_str, iff_lookup, "iff", sink);
		if (iff_idx < 0) return;
	}
	int class_idx = -1;
	if (class_str) {
		class_idx = check_lookup(class_str, ship_info_lookup, "ship_class", sink);
		if (class_idx < 0) return;
	}
	if (scale.has_value() && !check_int_range(*scale, 1, INT_MAX, "scale", sink)) return;
	if (id.has_value() && !check_int_range(*id, 0, INT_MAX, "id", sink)) return;

	derived_icon_props derived;
	if (source) {
		if (!derive_icon_from_source(source, derived, sink))
			return;
	} else {
		if (!position.has_value()) {
			sink.set_error("The position parameter is required when source is not given");
			return;
		}
		if (!type_str) {
			sink.set_error("The icon_type parameter is required when source is not given");
			return;
		}
	}

	// Explicit parameters override derived values
	if (position.has_value())
		derived.pos = *position;
	if (type_str)
		derived.type = type_idx;
	if (iff_str)
		derived.team = iff_idx;
	if (class_str)
		derived.ship_class = class_idx;
	if (label)
		derived.label = label;

	// Icon id: default a fresh one; explicit ids must be unique in this stage
	int icon_id;
	if (id.has_value()) {
		if (find_icon_in_stage(s, *id) >= 0) {
			sink.set_error("Icon id %d is already used in stage %d", *id, stage_idx + 1);
			return;
		}
		icon_id = *id;
		if (icon_id >= Cur_brief_id)
			Cur_brief_id = icon_id + 1;
	} else {
		icon_id = Cur_brief_id++;
	}

	brief_icon &icon = s.icons[s.num_icons++];
	memset(&icon, 0, sizeof(icon));
	icon.modelnum = -1;
	icon.model_instance_num = -1;
	icon.bitmap_id = -1;
	icon.id = icon_id;
	icon.pos = derived.pos;
	icon.type = derived.type;
	icon.team = derived.team;
	icon.ship_class = derived.ship_class;
	strcpy_s(icon.label, derived.label.c_str());
	if (closeup_label)
		strcpy_s(icon.closeup_label, closeup_label);
	icon.scale_factor = scale.has_value() ? *scale / 100.0f : 1.0f;
	if (highlight.value_or(false))
		icon.flags |= BI_HIGHLIGHT;
	if (mirror.value_or(false))
		icon.flags |= BI_MIRROR_ICON;
	if (use_wing_icon.value_or(false))
		icon.flags |= BI_USE_WING_ICON;
	if (use_cargo_icon.value_or(false))
		icon.flags |= BI_USE_CARGO_ICON;

	// Copy the icon into every later stage that has room and doesn't already
	// contain the id, as the briefing editor does when "change locally" is off
	if (propagate.value_or(true)) {
		for (int t = stage_idx + 1; t < br->num_stages; t++) {
			brief_stage &later = br->stages[t];
			if (later.num_icons >= MAX_STAGE_ICONS)
				continue;
			if (find_icon_in_stage(later, icon.id) >= 0)
				continue;
			later.icons[later.num_icons++] = icon;
		}
	}

	mark_modified("MCP: create briefing icon in stage %d", stage_idx + 1);

	req->result_json = make_json_tool_result(build_brief_icon_json(icon, s.num_icons - 1));
	req->success = true;
}

static void handle_update_briefing_icon(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	int stage_idx = get_stage_param(input, br, sink);
	if (stage_idx < 0) return;
	brief_stage &s = br->stages[stage_idx];

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, s.num_icons, "index", sink)) return;

	auto position       = get_optional_vec3d(input, "position", sink);
	auto type_str       = get_optional_string(input, "icon_type", sink);
	auto iff_str        = get_optional_string(input, "iff", sink);
	auto class_str      = get_optional_string(input, "ship_class", sink);
	auto label          = get_optional_string(input, "label", sink, MAX_LABEL_LEN - 1);
	auto closeup_label  = get_optional_string(input, "closeup_label", sink, MAX_LABEL_LEN - 1);
	auto scale          = get_optional_integer(input, "scale", sink);
	auto highlight      = get_optional_bool(input, "highlight", sink);
	auto mirror         = get_optional_bool(input, "mirror", sink);
	auto use_wing_icon  = get_optional_bool(input, "use_wing_icon", sink);
	auto use_cargo_icon = get_optional_bool(input, "use_cargo_icon", sink);
	auto new_id         = get_optional_integer(input, "id", sink);
	auto propagate      = get_optional_bool(input, "propagate", sink);
	if (sink.has_error()) return;

	int type_idx = -1;
	if (type_str) {
		type_idx = check_lookup(type_str, icon_type_enum_values(), "icon_type", sink);
		if (type_idx < 0) return;
	}
	int iff_idx = -1;
	if (iff_str) {
		iff_idx = check_lookup(iff_str, iff_lookup, "iff", sink);
		if (iff_idx < 0) return;
	}
	int class_idx = -1;
	if (class_str) {
		class_idx = check_lookup(class_str, ship_info_lookup, "ship_class", sink);
		if (class_idx < 0) return;
	}
	if (scale.has_value() && !check_int_range(*scale, 1, INT_MAX, "scale", sink)) return;
	if (new_id.has_value() && !check_int_range(*new_id, 0, INT_MAX, "id", sink)) return;

	brief_icon &icon = s.icons[*index - 1];
	bool do_propagate = propagate.value_or(true);
	int old_id = icon.id;

	// Validate an id change before mutating anything
	if (new_id.has_value() && *new_id != old_id) {
		int other = find_icon_in_stage(s, *new_id);
		if (other >= 0 && other != *index - 1) {
			sink.set_error("Icon id %d is already used in stage %d", *new_id, stage_idx + 1);
			return;
		}
		if (do_propagate) {
			for (int t = stage_idx + 1; t < br->num_stages; t++) {
				if (find_icon_in_stage(br->stages[t], *new_id) >= 0) {
					sink.set_error("Icon id %d is already used in stage %d. "
						"Pass propagate=false to change the id only in this stage.", *new_id, t + 1);
					return;
				}
			}
		}
	}

	bool changed = false;
	bool prop_pos = false, prop_type = false, prop_team = false, prop_class = false;
	bool prop_label = false, prop_closeup = false, prop_id = false;

	if (position.has_value() && !vm_vec_same(&icon.pos, &*position)) {
		icon.pos = *position;
		prop_pos = changed = true;
	}
	if (type_str && icon.type != type_idx) {
		icon.type = type_idx;
		prop_type = changed = true;
	}
	if (iff_str && icon.team != iff_idx) {
		icon.team = iff_idx;
		prop_team = changed = true;
	}
	if (class_str && icon.ship_class != class_idx) {
		icon.ship_class = class_idx;
		prop_class = changed = true;
	}
	if (label && strcmp(icon.label, label) != 0) {
		strcpy_s(icon.label, label);
		prop_label = changed = true;
	}
	if (closeup_label && strcmp(icon.closeup_label, closeup_label) != 0) {
		strcpy_s(icon.closeup_label, closeup_label);
		prop_closeup = changed = true;
	}
	if (new_id.has_value() && *new_id != old_id) {
		icon.id = *new_id;
		if (*new_id >= Cur_brief_id)
			Cur_brief_id = *new_id + 1;
		prop_id = changed = true;
	}

	// Scale and the display flags are stage-local in FRED and stay local here
	if (scale.has_value()) {
		float sf = *scale / 100.0f;
		if (icon.scale_factor != sf) {
			icon.scale_factor = sf;
			changed = true;
		}
	}
	int flags = icon.flags;
	if (highlight.has_value())
		flags = *highlight ? (flags | BI_HIGHLIGHT) : (flags & ~BI_HIGHLIGHT);
	if (mirror.has_value())
		flags = *mirror ? (flags | BI_MIRROR_ICON) : (flags & ~BI_MIRROR_ICON);
	if (use_wing_icon.has_value())
		flags = *use_wing_icon ? (flags | BI_USE_WING_ICON) : (flags & ~BI_USE_WING_ICON);
	if (use_cargo_icon.has_value())
		flags = *use_cargo_icon ? (flags | BI_USE_CARGO_ICON) : (flags & ~BI_USE_CARGO_ICON);
	if (flags != icon.flags) {
		icon.flags = flags;
		changed = true;
	}

	// Apply the identity-carrying changes to same-id icons in later stages,
	// as the briefing editor does when "change locally" is off
	if (do_propagate && (prop_pos || prop_type || prop_team || prop_class || prop_label || prop_closeup || prop_id)) {
		for (int t = stage_idx + 1; t < br->num_stages; t++) {
			int z = find_icon_in_stage(br->stages[t], old_id);
			if (z < 0)
				continue;
			brief_icon &other = br->stages[t].icons[z];
			if (prop_pos)
				other.pos = icon.pos;
			if (prop_type)
				other.type = icon.type;
			if (prop_team)
				other.team = icon.team;
			if (prop_class)
				other.ship_class = icon.ship_class;
			if (prop_label)
				strcpy_s(other.label, icon.label);
			if (prop_closeup)
				strcpy_s(other.closeup_label, icon.closeup_label);
			if (prop_id)
				other.id = icon.id;
		}
	}

	if (changed)
		mark_modified("MCP: update briefing icon %d in stage %d", *index, stage_idx + 1);

	req->result_json = make_json_tool_result(build_brief_icon_json(icon, *index - 1));
	req->success = true;
}

static void handle_delete_briefing_icon(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	int stage_idx = get_stage_param(input, br, sink);
	if (stage_idx < 0) return;
	brief_stage &s = br->stages[stage_idx];

	auto index = get_required_integer(input, "index", sink);
	if (!index.has_value()) return;
	if (!check_int_range(*index, 1, s.num_icons, "index", sink)) return;

	int num = *index - 1;  // 0-based

	// Remove any lines that reference the icon being deleted
	int i = s.num_lines;
	while (i--) {
		if (s.lines[i].start_icon == num || s.lines[i].end_icon == num) {
			s.num_lines--;
			for (int l = i; l < s.num_lines; l++)
				s.lines[l] = s.lines[l + 1];
		}
	}

	// Fix the indexes of lines that reference the icons being shifted down
	for (i = 0; i < s.num_lines; i++) {
		if (s.lines[i].start_icon > num)
			s.lines[i].start_icon--;
		if (s.lines[i].end_icon > num)
			s.lines[i].end_icon--;
	}

	array_remove_slot(s.icons, s.num_icons, num);

	mark_modified("MCP: delete briefing icon %d in stage %d", *index, stage_idx + 1);
	sprintf(req->result_message,
		"Deleted briefing icon %d from stage %d", *index, stage_idx + 1);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Briefing line tools
// ---------------------------------------------------------------------------

// Find the line with the given 0-based endpoints (order-insensitive), or -1.
static int find_line_in_stage(const brief_stage &s, int icon_a, int icon_b)
{
	for (int i = 0; i < s.num_lines; i++) {
		if ((s.lines[i].start_icon == icon_a && s.lines[i].end_icon == icon_b) ||
			(s.lines[i].start_icon == icon_b && s.lines[i].end_icon == icon_a))
			return i;
	}
	return -1;
}

static void handle_create_briefing_line(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	int stage_idx = get_stage_param(input, br, sink);
	if (stage_idx < 0) return;
	brief_stage &s = br->stages[stage_idx];

	auto start_icon = get_required_integer(input, "start_icon", sink);
	if (!start_icon.has_value()) return;
	auto end_icon = get_required_integer(input, "end_icon", sink);
	if (!end_icon.has_value()) return;
	if (!check_int_range(*start_icon, 1, s.num_icons, "start_icon", sink)) return;
	if (!check_int_range(*end_icon, 1, s.num_icons, "end_icon", sink)) return;

	if (*start_icon == *end_icon) {
		sink.set_error("A line cannot connect an icon to itself");
		return;
	}
	if (s.num_lines >= MAX_BRIEF_STAGE_LINES) {
		sink.set_error("Cannot add more than %d lines to a briefing stage", MAX_BRIEF_STAGE_LINES);
		return;
	}
	if (find_line_in_stage(s, *start_icon - 1, *end_icon - 1) >= 0) {
		sink.set_error("A line between icons %d and %d already exists in stage %d",
			*start_icon, *end_icon, stage_idx + 1);
		return;
	}

	brief_line &line = s.lines[s.num_lines++];
	line.start_icon = *start_icon - 1;
	line.end_icon = *end_icon - 1;

	mark_modified("MCP: create briefing line in stage %d", stage_idx + 1);

	req->result_json = make_json_tool_result(build_brief_line_json(line, s.num_lines - 1));
	req->success = true;
}

static void handle_delete_briefing_line(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_briefing, sink)) return;

	auto *br = get_briefing_for_team(input, sink);
	if (!br) return;

	int stage_idx = get_stage_param(input, br, sink);
	if (stage_idx < 0) return;
	brief_stage &s = br->stages[stage_idx];

	auto index      = get_optional_integer(input, "index", sink);
	auto start_icon = get_optional_integer(input, "start_icon", sink);
	auto end_icon   = get_optional_integer(input, "end_icon", sink);
	if (sink.has_error()) return;

	// Exactly one addressing form: a line index, or a complete endpoint pair
	bool have_pair = start_icon.has_value() || end_icon.has_value();
	if (index.has_value() == have_pair) {
		sink.set_error("Specify either index or the start_icon/end_icon pair, but not both");
		return;
	}

	int target;
	if (index.has_value()) {
		if (!check_int_range(*index, 1, s.num_lines, "index", sink)) return;
		target = *index - 1;
	} else {
		if (!start_icon.has_value() || !end_icon.has_value()) {
			sink.set_error("Both start_icon and end_icon are required when deleting by endpoints");
			return;
		}
		if (!check_int_range(*start_icon, 1, s.num_icons, "start_icon", sink)) return;
		if (!check_int_range(*end_icon, 1, s.num_icons, "end_icon", sink)) return;
		target = find_line_in_stage(s, *start_icon - 1, *end_icon - 1);
		if (target < 0) {
			sink.set_error("No line between icons %d and %d in stage %d",
				*start_icon, *end_icon, stage_idx + 1);
			return;
		}
	}

	array_remove_slot(s.lines, s.num_lines, target);

	mark_modified("MCP: delete briefing line %d in stage %d", target + 1, stage_idx + 1);
	sprintf(req->result_message,
		"Deleted briefing line %d from stage %d", target + 1, stage_idx + 1);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

static const char *brief_team_desc =
	"Which team's briefing to operate on (\"Team 1\" or \"Team 2\"). "
	"Defaults to \"Team 1\". \"none\" is not valid for briefings.";

static void register_list_briefing_stages(json_t *tools)
{
	json_t *props = json_object();
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	register_tool(tools, "list_briefing_stages",
		"List all briefing stages for a team. Returns each stage's index, "
		"text, voice, camera view, cut flags, grid settings, and SEXP "
		"formula root node.",
		props);
}

static void register_get_briefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index",
		"1-based index of the stage to retrieve");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "get_briefing_stage",
		"Get full details of a briefing stage by index.",
		props, req);
}

static void register_create_briefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_string_prop(props, "text", "The briefing text displayed during this stage");
	add_string_prop(props, "voice_filename",
		"Voice audio filename (wav/ogg). Defaults to empty (no voice).");
	add_vec3d_prop(props, "camera_position",
		"Camera position for this stage's view of the briefing map. "
		"Defaults to the inherited or current editor viewpoint.");
	add_matrix_prop(props, "camera_orientation",
		"Camera orientation for this stage's view of the briefing map. "
		"Defaults to the inherited or current editor viewpoint.");
	add_integer_prop(props, "camera_time",
		"Time in milliseconds for the camera to move to this stage's view. "
		"Defaults to the inherited value, or 500.");
	add_bool_prop(props, "cut_to_next",
		"Cut instantly to the next stage instead of moving the camera. Defaults to false.");
	add_bool_prop(props, "cut_from_previous",
		"Cut instantly from the previous stage instead of moving the camera. Defaults to false.");
	add_bool_prop(props, "draw_grid",
		"Whether the briefing map grid is drawn during this stage. Defaults to true.");
	add_color_prop(props, "grid_color",
		"Color of the briefing map grid. Defaults to the standard grid color.");
	add_integer_prop(props, "formula", "Root node of the SEXP formula used for this stage. "
		"Defaults to true (stage always shown).");
	add_integer_prop(props, "index",
		"Position to insert the stage (1 = first). If omitted, appends to the end.");
	add_bool_prop(props, "copy_from_previous",
		"Inherit camera view, cut flags, grid settings, icons, and lines from "
		"the stage preceding the insertion point (or the following stage when "
		"inserting at the front), as the FRED editor does. Voice and formula "
		"are never inherited. Defaults to true. Explicit parameters always "
		"override inherited values.");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("text"));
	register_tool(tools, "create_briefing_stage",
		"Create a new briefing stage. Briefing stages are shown on the briefing "
		"map before a mission starts; each stage has text, an optional voice, a "
		"camera view, and a SEXP formula controlling whether it is displayed. "
		"Maximum " SCP_TOKEN_TO_STR(MAX_BRIEF_STAGES) " stages per team.",
		props, req);
}

static void register_update_briefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index",
		"1-based index of the stage to update");
	add_string_prop(props, "text", "New briefing text for this stage");
	add_string_prop(props, "voice_filename",
		"New voice audio filename. Empty string clears the voice.");
	add_vec3d_prop(props, "camera_position",
		"New camera position for this stage's view of the briefing map");
	add_matrix_prop(props, "camera_orientation",
		"New camera orientation for this stage's view of the briefing map");
	add_integer_prop(props, "camera_time",
		"New time in milliseconds for the camera to move to this stage's view");
	add_bool_prop(props, "cut_to_next",
		"Cut instantly to the next stage instead of moving the camera");
	add_bool_prop(props, "cut_from_previous",
		"Cut instantly from the previous stage instead of moving the camera");
	add_bool_prop(props, "draw_grid",
		"Whether the briefing map grid is drawn during this stage");
	add_color_prop(props, "grid_color",
		"New color of the briefing map grid");
	add_integer_prop(props, "formula", "Root node of the SEXP formula used for this stage.");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "update_briefing_stage",
		"Update properties of an existing briefing stage. Only specified "
		"fields are changed; omitted fields are left unchanged.",
		props, req);
}

static void register_delete_briefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index",
		"1-based index of the stage to delete");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "delete_briefing_stage",
		"Delete a briefing stage, including its icons and lines. Frees its "
		"SEXP formula. Remaining stages are shifted down.",
		props, req);
}

static void register_move_briefing_stage(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "from_index",
		"Current 1-based index of the stage");
	add_integer_prop(props, "to_index",
		"Target 1-based index to move the stage to");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("from_index"));
	json_array_append_new(req, json_string("to_index"));
	register_tool(tools, "move_briefing_stage",
		"Move a briefing stage from one position to another. "
		"Indices are 1-based.",
		props, req);
}

static void register_swap_briefing_stages(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "index_a",
		"1-based index of the first stage");
	add_integer_prop(props, "index_b",
		"1-based index of the second stage");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("index_a"));
	json_array_append_new(req, json_string("index_b"));
	register_tool(tools, "swap_briefing_stages",
		"Swap two briefing stages at the given positions. "
		"Indices are 1-based.",
		props, req);
}

static const char *icon_iff_desc =
	"IFF affiliation of the icon (e.g. \"Friendly\", \"Hostile\"), which "
	"determines its color. Use list_iffs to see valid names. Note: this is "
	"unrelated to the \"team\" parameter, which selects whose briefing to edit.";

static void register_create_briefing_icon(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "stage",
		"1-based index of the stage to add the icon to");
	add_string_prop(props, "source",
		"Name of a ship, waypoint, or jump node in the mission to derive the "
		"icon from, like the FRED editor's Make Icon button: position, label, "
		"IFF, ship class, and icon type are taken from the entity. Waypoints "
		"may be an individual point (\"List name:1\") or a list name (uses the "
		"first point). Explicit parameters override derived values.");
	add_vec3d_prop(props, "position",
		"Position of the icon on the briefing map. Required if source is not given.");
	add_string_enum_prop(props, "icon_type",
		"Icon type, which determines the symbol drawn on the briefing map. "
		"Required if source is not given.",
		icon_type_enum_values());
	add_string_prop(props, "iff", icon_iff_desc);
	add_string_prop(props, "ship_class",
		"Ship class of the icon, used for the closeup view. Defaults to none.");
	add_string_prop(props, "label",
		"Label displayed next to the icon. Defaults to empty (or the source entity's name).");
	add_string_prop(props, "closeup_label",
		"Label displayed in the closeup view. If empty, the ship class name is used.");
	add_integer_prop(props, "scale",
		"Icon scale in percent. Defaults to 100.");
	add_bool_prop(props, "highlight",
		"Highlight the icon when the stage is shown. Defaults to false.");
	add_bool_prop(props, "mirror",
		"Mirror the icon so it points the other way. Defaults to false.");
	add_bool_prop(props, "use_wing_icon",
		"Use the wing variant of the icon (only for ship classes that define one). Defaults to false.");
	add_bool_prop(props, "use_cargo_icon",
		"Use the cargo variant of the icon (only for ship classes that define one). Defaults to false.");
	add_integer_prop(props, "id",
		"Icon id linking the same icon across stages (for animation continuity). "
		"Defaults to a freshly assigned id. Must be unique within the stage.");
	add_bool_prop(props, "propagate",
		"Also copy the icon into every later stage that doesn't already contain "
		"its id, as the FRED editor does. Defaults to true.");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("stage"));
	register_tool(tools, "create_briefing_icon",
		"Add an icon to a briefing stage. Icons mark ships, waypoints, and jump "
		"nodes on the briefing map. Either derive the icon from a mission entity "
		"via source, or supply position and type explicitly. "
		"Maximum " SCP_TOKEN_TO_STR(MAX_STAGE_ICONS) " icons per stage.",
		props, req);
}

static void register_update_briefing_icon(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "stage",
		"1-based index of the stage containing the icon");
	add_integer_prop(props, "index",
		"1-based index of the icon within the stage");
	add_vec3d_prop(props, "position",
		"New position of the icon on the briefing map");
	add_string_enum_prop(props, "icon_type",
		"New icon type", icon_type_enum_values());
	add_string_prop(props, "iff", icon_iff_desc);
	add_string_prop(props, "ship_class", "New ship class of the icon");
	add_string_prop(props, "label", "New label. Empty string clears it.");
	add_string_prop(props, "closeup_label",
		"New closeup label. Empty string clears it (the ship class name is then used).");
	add_integer_prop(props, "scale", "New icon scale in percent");
	add_bool_prop(props, "highlight", "Highlight the icon when the stage is shown");
	add_bool_prop(props, "mirror", "Mirror the icon so it points the other way");
	add_bool_prop(props, "use_wing_icon", "Use the wing variant of the icon");
	add_bool_prop(props, "use_cargo_icon", "Use the cargo variant of the icon");
	add_integer_prop(props, "id",
		"New icon id. Must be unique within the stage (and, when propagating, "
		"not used in any later stage).");
	add_bool_prop(props, "propagate",
		"Also apply position, type, iff, ship_class, label, closeup_label, and "
		"id changes to icons with the same id in later stages, as the FRED "
		"editor does. Scale and the display flags are always stage-local. "
		"Defaults to true.");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("stage"));
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "update_briefing_icon",
		"Update properties of an existing briefing icon. Only specified "
		"fields are changed; omitted fields are left unchanged.",
		props, req);
}

static void register_delete_briefing_icon(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "stage",
		"1-based index of the stage containing the icon");
	add_integer_prop(props, "index",
		"1-based index of the icon to delete");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("stage"));
	json_array_append_new(req, json_string("index"));
	register_tool(tools, "delete_briefing_icon",
		"Delete an icon from a briefing stage. Lines connected to the icon are "
		"removed and remaining icons are shifted down. Only affects the given "
		"stage; same-id icons in other stages are untouched.",
		props, req);
}

static void register_create_briefing_line(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "stage",
		"1-based index of the stage to add the line to");
	add_integer_prop(props, "start_icon",
		"1-based index of the icon where the line starts");
	add_integer_prop(props, "end_icon",
		"1-based index of the icon where the line ends");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("stage"));
	json_array_append_new(req, json_string("start_icon"));
	json_array_append_new(req, json_string("end_icon"));
	register_tool(tools, "create_briefing_line",
		"Draw a line between two icons on the briefing map. Lines are unordered: "
		"at most one line can exist between any two icons, and endpoint order "
		"does not matter. Lines are local to the stage. "
		"Maximum " SCP_TOKEN_TO_STR(MAX_BRIEF_STAGE_LINES) " lines per stage.",
		props, req);
}

static void register_delete_briefing_line(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "stage",
		"1-based index of the stage containing the line");
	add_integer_prop(props, "index",
		"1-based index of the line to delete. Specify either this or the "
		"start_icon/end_icon pair, but not both.");
	add_integer_prop(props, "start_icon",
		"1-based icon index of one line endpoint (order does not matter)");
	add_integer_prop(props, "end_icon",
		"1-based icon index of the other line endpoint (order does not matter)");
	add_string_enum_prop(props, "team", brief_team_desc, team_selector_enum_values);
	json_t *req = json_array();
	json_array_append_new(req, json_string("stage"));
	register_tool(tools, "delete_briefing_line",
		"Delete a line between two icons, addressed either by its 1-based line "
		"index or by its endpoint pair (in either order). Remaining lines are "
		"shifted down.",
		props, req);
}

// ---------------------------------------------------------------------------
// Tool table
// ---------------------------------------------------------------------------

const McpToolDef mcp_brief_tool_defs[] = {
	{ "list_briefing_stages",  register_list_briefing_stages,  nullptr, handle_list_briefing_stages,  false },
	{ "get_briefing_stage",    register_get_briefing_stage,    nullptr, handle_get_briefing_stage,    false },
	{ "create_briefing_stage", register_create_briefing_stage, nullptr, handle_create_briefing_stage, false },
	{ "update_briefing_stage", register_update_briefing_stage, nullptr, handle_update_briefing_stage, false },
	{ "delete_briefing_stage", register_delete_briefing_stage, nullptr, handle_delete_briefing_stage, false },
	{ "move_briefing_stage",   register_move_briefing_stage,   nullptr, handle_move_briefing_stage,   false },
	{ "swap_briefing_stages",  register_swap_briefing_stages,  nullptr, handle_swap_briefing_stages,  false },
	{ "create_briefing_icon",  register_create_briefing_icon,  nullptr, handle_create_briefing_icon,  false },
	{ "update_briefing_icon",  register_update_briefing_icon,  nullptr, handle_update_briefing_icon,  false },
	{ "delete_briefing_icon",  register_delete_briefing_icon,  nullptr, handle_delete_briefing_icon,  false },
	{ "create_briefing_line",  register_create_briefing_line,  nullptr, handle_create_briefing_line,  false },
	{ "delete_briefing_line",  register_delete_briefing_line,  nullptr, handle_delete_briefing_line,  false },
};
const size_t mcp_brief_tool_def_count = sizeof(mcp_brief_tool_defs) / sizeof(mcp_brief_tool_defs[0]);
