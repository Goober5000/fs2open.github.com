#include "stdafx.h"
#include "mcp_jump_node.h"
#include "mcp_mission_tools.h"
#include "mcp_app.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "mcp_array_utils.h"
#include "mcp_sexp_forest.h"

#include <jansson.h>
#include <cstring>

#include "globalincs/utility.h"

#include "jumpnode/jumpnode.h"
#include "object/object.h"
#include "model/model.h"
#include "parse/sexp.h"
#include "parse/parselo.h"
#include "management.h"
#include "fred.h"

// ---------------------------------------------------------------------------
// Dialog conflict guards
// ---------------------------------------------------------------------------

static bool validate_dialog_for_jump_nodes(SCP_string &error_msg)
{
	return validate_single_dialog("jump nodes", "jump node", error_msg);
}

// ---------------------------------------------------------------------------
// Jump node tools
// ---------------------------------------------------------------------------

static json_t *build_jump_node_json(const CJumpNode &jn, int index, bool include_details = false)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "name", json_safe_string(jn.GetName()));
	json_object_set_new(obj, "index", json_integer(index + 1));
	json_object_set_new(obj, "position", build_vec3d_json(*jn.GetPosition()));

	if (include_details) {
		if (jn.HasDisplayName())
			json_object_set_new(obj, "display_name", json_safe_string(jn.GetDisplayName()));
		if (jn.IsColored())
			json_object_set_new(obj, "color", build_color_json(jn.GetColor(), true));
		if (jn.IsSpecialModel()) {
			const char *model_filename = model_get(jn.GetModelNumber())->filename;
			set_optional_filename(obj, "model_filename", model_filename);
		}
		json_object_set_new(obj, "hidden", json_boolean(jn.IsHidden()));
		json_object_set_new(obj, "radius", json_real(jn.GetRadius()));
	}

	return obj;
}

static void handle_list_jump_nodes(json_t * /*input*/, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	json_t *arr = json_array();
	int index = 0;
	for (const auto &jn : Jump_nodes)
		json_array_append_new(arr, build_jump_node_json(jn, index++));

	req->result_json = make_json_tool_result(arr);
	req->success = true;
}

static void handle_get_jump_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int index = jumpnode_lookup(name);
	if (index >= 0) {
		req->result_json = make_json_tool_result(build_jump_node_json(Jump_nodes[index], index, true));
		req->success = true;
		return;
	}

	set_not_found_error(sink,"Jump node", name);
}

static void handle_create_jump_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true, NAME_LENGTH - 1);
	if (!name) return;

	auto pos = get_required_vec3d(input, "position", sink);
	if (!pos.has_value()) return;

	auto display_name = get_optional_string(input, "display_name", sink, NAME_LENGTH - 1);
	auto color_val    = get_optional_color(input, "color", sink);
	auto model_file   = get_optional_filename(input, "model_filename", sink, true, MAX_FILENAME_LEN - 1);
	auto show_polys   = get_optional_bool(input, "show_polys", sink);
	auto hidden       = get_optional_bool(input, "hidden", sink);
	auto insert_index = get_optional_integer(input, "index", sink);
	if (sink.has_error()) return;

	// Validate name against all object types
	if (!check_object_rename("jump node", name, sink)) return;

	// Validate insert index
	int target_index;
	if (!insert_index.has_value()) {
		target_index = (int)Jump_nodes.size();
	} else {
		if (!check_int_range(*insert_index, 1, (int)Jump_nodes.size() + 1, "index", sink))
			return;
		target_index = *insert_index - 1;
	}

	// Construct the jump node
	vec3d position = *pos;
	CJumpNode jnp(&position);
	jnp.SetName(name);

	if (display_name)
		jnp.SetDisplayName(display_name);
	if (color_val.has_value())
		jnp.SetAlphaColor(color_val->red, color_val->green, color_val->blue, color_val->alpha);
	if (model_file)
		jnp.SetModel(model_file, show_polys.has_value() && *show_polys);
	else if (show_polys.has_value() && *show_polys)
		jnp.SetModel(JN_DEFAULT_MODEL, true);
	if (hidden.has_value() && *hidden)
		jnp.SetVisibility(false);

	// Insert
	Jump_nodes.insert(Jump_nodes.begin() + target_index, std::move(jnp));
	obj_merge_created_list();

	Jumpnode_editor_dialog.initialize_data(1);
	mark_modified("MCP: create jump node %s", name);

	req->result_json = make_json_tool_result(build_jump_node_json(Jump_nodes[target_index], target_index, true));
	req->success = true;
}

static void handle_update_jump_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	auto new_name     = get_optional_string(input, "new_name", sink, NAME_LENGTH - 1);
	auto new_pos      = get_optional_vec3d(input, "position", sink);
	auto display_name = get_optional_string(input, "display_name", sink, NAME_LENGTH - 1);
	auto color_val    = get_optional_color(input, "color", sink);
	auto model_file   = get_optional_filename(input, "model_filename", sink, true, MAX_FILENAME_LEN - 1);
	auto show_polys   = get_optional_bool(input, "show_polys", sink);
	auto hidden       = get_optional_bool(input, "hidden", sink);
	if (sink.has_error()) return;

	// Find the jump node
	int index = jumpnode_lookup(name);
	if (index < 0) {
		set_not_found_error(sink,"Jump node", name);
		return;
	}
	auto &jn = Jump_nodes[index];

	bool changed = false;

	// Rename (with SEXP reference update)
	if (new_name && stricmp(jn.GetName(), new_name) != 0) {
		if (!check_object_rename("jump node", new_name, sink, -1, -1, -1, index)) return;
		update_sexp_references(jn.GetName(), new_name, OPF_JUMP_NODE_NAME);
		mcp_sexp_forest_mark_dirty();
		jn.SetName(new_name);
		changed = true;
	}

	// Position
	if (new_pos.has_value()) {
		const vec3d *cur = jn.GetPosition();
		if (!fl_equal(cur->xyz.x, new_pos->xyz.x) || !fl_equal(cur->xyz.y, new_pos->xyz.y) || !fl_equal(cur->xyz.z, new_pos->xyz.z)) {
			Objects[jn.GetSCPObjectNumber()].pos = *new_pos;
			changed = true;
		}
	}

	// Display name
	if (display_name) {
		if (!display_name[0]) {
			// Empty string clears display name
			if (jn.HasDisplayName()) {
				jn.SetDisplayName("");
				changed = true;
			}
		} else if (strcmp(jn.GetDisplayName(), display_name) != 0) {
			jn.SetDisplayName(display_name);
			changed = true;
		}
	}

	// Color
	if (color_val.has_value()) {
		const color &c = jn.GetColor();
		if (c.red != color_val->red || c.green != color_val->green ||
			c.blue != color_val->blue || c.alpha != color_val->alpha) {
			jn.SetAlphaColor(color_val->red, color_val->green, color_val->blue, color_val->alpha);
			changed = true;
		}
	}

	// Model
	if (model_file) {
		jn.SetModel(model_file, show_polys.has_value() && *show_polys);
		changed = true;
	} else if (show_polys.has_value()) {
		// Reload current model with new show_polys setting
		if (jn.IsSpecialModel()) {
			const char *cur_model = model_get(jn.GetModelNumber())->filename;
			jn.SetModel(cur_model, *show_polys);
		} else {
			jn.SetModel(JN_DEFAULT_MODEL, *show_polys);
		}
		changed = true;
	}

	// Hidden
	if (hidden.has_value() && jn.IsHidden() != *hidden) {
		jn.SetVisibility(!*hidden);
		changed = true;
	}

	if (changed) {
		Jumpnode_editor_dialog.initialize_data(1);
		mark_modified("MCP: update jump node %s", jn.GetName());
	}

	req->result_json = make_json_tool_result(build_jump_node_json(jn, index, true));
	req->success = true;
}

static void handle_delete_jump_node(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	if (!validate(validate_dialog_for_jump_nodes, sink)) return;

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	// Find the jump node
	int index = jumpnode_lookup(name);
	if (index < 0 ) {
		set_not_found_error(sink,"Jump node", name);
		return;
	}

	auto force = get_optional_bool(input, "force", sink);
	if (sink.has_error()) return;

	if (!check_and_report_sexp_refs(sexp_ref_type::NON_OBJECT, "Jump node", name, force, sink))
		return;

	// Invalidate SEXP references
	char buf[NAME_LENGTH + 4];
	snprintf(buf, sizeof(buf), "<%s>", name);
	update_sexp_references(name, buf, OPF_JUMP_NODE_NAME);
	mcp_sexp_forest_mark_dirty();

	// Must follow the same pattern as management.cpp delete_object() to avoid
	// orphaning the OBJ_JUMP_NODE object slot.  The CJumpNode move-assignment
	// operator does not call obj_delete on the overwritten m_objnum, so a
	// naive vector::erase on a non-last element silently leaks the object.
	int objnum = Jump_nodes[index].GetSCPObjectNumber();
	Objects[objnum].type = OBJ_NONE;          // fool destructor into skipping obj_delete
	Jump_nodes.erase(Jump_nodes.begin() + index);
	Objects[objnum].type = OBJ_JUMP_NODE;     // restore for obj_delete
	unmark_object(objnum);
	obj_delete(objnum);                        // free the object slot

	Jumpnode_editor_dialog.initialize_data(1);
	mark_modified("MCP: delete jump node %s", name);

	sprintf(req->result_message,
		"Deleted jump node: %s", name);
	req->success = true;
}

// Move/swap config for jump nodes
static MoveSwapConfig make_jump_node_move_swap_config()
{
	MoveSwapConfig cfg;
	cfg.entity_name = "jump node";
	cfg.count = (int)Jump_nodes.size();
	cfg.one_based = true;
	cfg.validate_dialog = validate_dialog_for_jump_nodes;
	cfg.get_name = [](int i) {
		return Jump_nodes[i - 1].GetName();
	};
	cfg.do_move = [](int from, int to) {
		array_move_element(Jump_nodes, from - 1, to - 1);
		resort_jump_nodes_in_obj_used_list();
	};
	cfg.do_swap = [](int a, int b) {
		std::swap(Jump_nodes[a - 1], Jump_nodes[b - 1]);
		resort_jump_nodes_in_obj_used_list();
	};
	return cfg;
}

static void handle_move_jump_node(json_t *input, McpToolRequest *req)
{
	auto cfg = make_jump_node_move_swap_config();
	handle_generic_move(input, req, cfg);
}

static void handle_swap_jump_nodes(json_t *input, McpToolRequest *req)
{
	auto cfg = make_jump_node_move_swap_config();
	handle_generic_swap(input, req, cfg);
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_jump_node_tools(json_t *tools)
{
	// -----------------------------------------------------------------------
	// Jump node tools
	// -----------------------------------------------------------------------

	// list_jump_nodes
	register_tool(tools, "list_jump_nodes",
		"List all jump nodes in the mission. Returns each node's name, "
		"index, and position.",
		json_object());

	// get_jump_node
	register_tool_with_required_string(tools, "get_jump_node",
		"Get full details of a jump node by name, including position, "
		"display name, color, model file, hidden state, and radius.",
		"name", "Name of the jump node to retrieve");

	// create_jump_node
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Unique name for the jump node");
		add_vec3d_prop(props, "position", "World position of the jump node");
		add_string_prop(props, "display_name",
			"Display name shown to the player (if different from name)");
		add_color_prop(props, "color",
			"Custom RGBA display color. If omitted, defaults to green (0,255,0,255).");
		add_string_prop(props, "model_filename",
			"Model filename (POF). Defaults to \"" JN_DEFAULT_MODEL "\".");
		add_bool_prop(props, "show_polys",
			"If true, render as solid model instead of wireframe. Default false.");
		add_bool_prop(props, "hidden",
			"If true, the jump node is hidden from rendering. Default false.");
		add_integer_prop(props, "index",
			"Position to insert the jump node (1 = first). If omitted, appends to the end.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("position"));
		register_tool(tools, "create_jump_node",
			"Create a new jump node at a given position. Jump nodes are subspace "
			"navigation points that ships can depart through.",
			props, req);
	}

	// update_jump_node
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the jump node to update");
		add_string_prop(props, "new_name", "New name for the jump node");
		add_vec3d_prop(props, "position", "New world position");
		add_string_prop(props, "display_name",
			"New display name (empty string to clear)");
		add_color_prop(props, "color", "New RGBA display color");
		add_string_prop(props, "model_filename",
			"New model filename (POF)");
		add_bool_prop(props, "show_polys",
			"If true, render as solid model instead of wireframe");
		add_bool_prop(props, "hidden",
			"If true, the jump node is hidden from rendering");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "update_jump_node",
			"Update properties of an existing jump node. Only specified fields "
			"are changed; omitted fields are left unchanged. Renaming updates "
			"SEXP references automatically.",
			props, req);
	}

	// delete_jump_node
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the jump node to delete");
		add_bool_prop(props, "force",
			"If true, delete even if the jump node is referenced in SEXPs "
			"(references will be invalidated). Default false.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		register_tool(tools, "delete_jump_node",
			"Delete a jump node from the mission. Fails if the node is "
			"referenced in SEXPs unless force=true.",
			props, req);
	}

	// move_jump_node
	{
		json_t *props = json_object();
		add_integer_prop(props, "from_index",
			"1-based index of the jump node to move");
		add_integer_prop(props, "to_index",
			"1-based target index");
		json_t *req = json_array();
		json_array_append_new(req, json_string("from_index"));
		json_array_append_new(req, json_string("to_index"));
		register_tool(tools, "move_jump_node",
			"Move a jump node from one list position to another. "
			"Indices are 1-based.",
			props, req);
	}

	// swap_jump_nodes
	{
		json_t *props = json_object();
		add_integer_prop(props, "index_a",
			"1-based index of the first jump node");
		add_integer_prop(props, "index_b",
			"1-based index of the second jump node");
		json_t *req = json_array();
		json_array_append_new(req, json_string("index_a"));
		json_array_append_new(req, json_string("index_b"));
		register_tool(tools, "swap_jump_nodes",
			"Swap two jump nodes at the given positions. "
			"Indices are 1-based.",
			props, req);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_jump_node_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_jump_nodes") == 0) {
		handle_list_jump_nodes(input_json, req);
	} else if (strcmp(tool_name, "get_jump_node") == 0) {
		handle_get_jump_node(input_json, req);
	} else if (strcmp(tool_name, "create_jump_node") == 0) {
		handle_create_jump_node(input_json, req);
	} else if (strcmp(tool_name, "update_jump_node") == 0) {
		handle_update_jump_node(input_json, req);
	} else if (strcmp(tool_name, "delete_jump_node") == 0) {
		handle_delete_jump_node(input_json, req);
	} else if (strcmp(tool_name, "move_jump_node") == 0) {
		handle_move_jump_node(input_json, req);
	} else if (strcmp(tool_name, "swap_jump_nodes") == 0) {
		handle_swap_jump_nodes(input_json, req);
	} else {
		return false;
	}
	return true;
}
