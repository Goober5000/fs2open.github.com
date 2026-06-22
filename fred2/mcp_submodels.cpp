#include "stdafx.h"
#include "mcp_submodels.h"
#include "mcp_mission_tools.h"        // lookup_ship
#include "mcp_json.h"
#include "mcpserver.h"

#include <jansson.h>
#include <cstring>
#include <optional>

#include "math/floating.h"            // fl_radians, fl_degrees
#include "math/vecmat.h"              // vm_closest_angle_to_matrix, vm_vec_dot
#include "model/model.h"             // polymodel(_instance), submodel helpers, MOVEMENT_AXIS_*
#include "ship/ship.h"               // Ships, Ship::Subsystem_Flags

#include "fred.h"                     // Update_window

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Map a MOVEMENT_AXIS_* id to a short label, or nullptr for MOVEMENT_AXIS_NONE.
static const char *movement_axis_name(int axis_id)
{
	switch (axis_id) {
		case MOVEMENT_AXIS_X:     return "x";
		case MOVEMENT_AXIS_Y:     return "y";
		case MOVEMENT_AXIS_Z:     return "z";
		case MOVEMENT_AXIS_OTHER: return "other";
		default:                  return nullptr;	// MOVEMENT_AXIS_NONE
	}
}

// Resolve a ship name to its live polymodel + polymodel_instance.
// Returns false (with error reported) if the ship or its model instance is missing.
static bool resolve_ship_model(const char *ship_name, McpErrorSink &sink,
	int &ship_idx_out, polymodel *&pm_out, polymodel_instance *&pmi_out)
{
	int ship_idx = lookup_ship(ship_name, sink);
	if (ship_idx < 0)
		return false;

	int instance_num = Ships[ship_idx].model_instance_num;
	if (instance_num < 0) {
		sink.set_error("Ship '%s' has no live model instance.", Ships[ship_idx].ship_name);
		return false;
	}

	polymodel_instance *pmi = model_get_instance(instance_num);
	if (pmi == nullptr) {
		sink.set_error("Ship '%s' has no live model instance.", Ships[ship_idx].ship_name);
		return false;
	}
	polymodel *pm = model_get(pmi->model_num);
	if (pm == nullptr) {
		sink.set_error("Ship '%s' has no loaded model.", Ships[ship_idx].ship_name);
		return false;
	}

	ship_idx_out = ship_idx;
	pm_out = pm;
	pmi_out = pmi;
	return true;
}

// Resolve a submodel name within a model to its (detail-0) index, or -1 with an error.
static int resolve_submodel(polymodel *pm, const char *submodel_name, const char *ship_name, McpErrorSink &sink)
{
	int idx = model_find_submodel_index(pm, submodel_name);
	if (idx < 0)
		sink.set_error("Submodel '%s' not found on ship '%s'.", submodel_name, ship_name);
	return idx;
}

// Build the shared per-submodel entry (used by both list and get).  Does not
// include the ship name; callers add that where appropriate.
static json_t *build_submodel_json(const polymodel *pm, const polymodel_instance *pmi, int sm_idx)
{
	const bsp_info *sm = &pm->submodel[sm_idx];
	const submodel_instance *smi = &pmi->submodel[sm_idx];

	bool rotates = sm->rotation_axis_id != MOVEMENT_AXIS_NONE;
	bool translates = sm->translation_axis_id != MOVEMENT_AXIS_NONE;

	json_t *obj = json_object();
	// Absent fields (no parent, no movement axis) are omitted rather than emitted
	// as null, matching the convention used by the other MCP tools.
	json_object_set_new(obj, "submodel", json_safe_string(sm->name));
	if (sm->parent >= 0)
		json_object_set_new(obj, "parent", json_safe_string(pm->submodel[sm->parent].name));

	json_object_set_new(obj, "rotates", json_boolean(rotates));
	if (rotates)
		json_object_set_new(obj, "rotation_axis", json_string(movement_axis_name(sm->rotation_axis_id)));
	json_object_set_new(obj, "translates", json_boolean(translates));
	if (translates)
		json_object_set_new(obj, "translation_axis", json_string(movement_axis_name(sm->translation_axis_id)));

	json_object_set_new(obj, "blown_off", json_boolean(smi->blown_off));

	// basic: scalar angle/offset, only present when a movement axis exists.
	if (rotates || translates) {
		json_t *basic = json_object();
		if (rotates)
			json_object_set_new(basic, "angle_degrees", json_real(fl_degrees(smi->cur_angle)));
		if (translates)
			json_object_set_new(basic, "offset_meters", json_real(smi->cur_offset));
		json_object_set_new(obj, "basic", basic);
	}

	// advanced: full canonical orientation and offset, always present.
	json_t *advanced = json_object();
	json_object_set_new(advanced, "orientation", build_matrix_json(smi->canonical_orient));
	json_object_set_new(advanced, "offset", build_vec3d_json(smi->canonical_offset));
	json_object_set_new(obj, "advanced", advanced);

	return obj;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void handle_list_ship_submodels(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;

	int ship_idx;
	polymodel *pm;
	polymodel_instance *pmi;
	if (!resolve_ship_model(name, sink, ship_idx, pm, pmi))
		return;

	// Collect every submodel index that is a lower-LOD "mirror" of another
	// submodel; those are filtered from the listing (callers only ever name
	// the detail-0 representative).
	SCP_set<int> lod_copies;
	for (int i = 0; i < pm->n_models; i++) {
		const bsp_info *sm = &pm->submodel[i];
		for (int d = 0; d < sm->num_details; d++)
			lod_copies.insert(sm->details[d]);
	}

	json_t *result = json_object();
	json_object_set_new(result, "ship", json_safe_string(Ships[ship_idx].ship_name));

	json_t *arr = json_array();
	for (int i = 0; i < pm->n_models; i++) {
		if (lod_copies.count(i))
			continue;
		json_array_append_new(arr, build_submodel_json(pm, pmi, i));
	}
	json_object_set_new(result, "submodels", arr);

	req->result_json = make_json_tool_result(result);
	req->success = true;
}

static void handle_get_ship_submodel(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;
	auto submodel_name = get_required_string(input, "submodel", sink, true);
	if (!submodel_name) return;

	int ship_idx;
	polymodel *pm;
	polymodel_instance *pmi;
	if (!resolve_ship_model(name, sink, ship_idx, pm, pmi))
		return;

	int sm_idx = resolve_submodel(pm, submodel_name, Ships[ship_idx].ship_name, sink);
	if (sm_idx < 0) return;

	json_t *obj = build_submodel_json(pm, pmi, sm_idx);
	json_object_set_new(obj, "ship", json_safe_string(Ships[ship_idx].ship_name));
	req->result_json = make_json_tool_result(obj);
	req->success = true;
}

static void handle_update_ship_submodel(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);

	auto name = get_required_string(input, "name", sink, true);
	if (!name) return;
	auto submodel_name = get_required_string(input, "submodel", sink, true);
	if (!submodel_name) return;

	int ship_idx;
	polymodel *pm;
	polymodel_instance *pmi;
	if (!resolve_ship_model(name, sink, ship_idx, pm, pmi))
		return;

	int sm_idx = resolve_submodel(pm, submodel_name, Ships[ship_idx].ship_name, sink);
	if (sm_idx < 0) return;

	bsp_info *sm = &pm->submodel[sm_idx];
	submodel_instance *smi = &pmi->submodel[sm_idx];

	// Treat an explicit JSON null the same as an omitted field.
	json_t *basic = json_object_get(input, "basic");
	json_t *advanced = json_object_get(input, "advanced");
	bool has_basic = basic != nullptr && json_is_object(basic);
	bool has_advanced = advanced != nullptr && json_is_object(advanced);

	if (basic != nullptr && !json_is_object(basic)) {
		sink.set_error("'basic' must be an object with angle_degrees and/or offset_meters.");
		return;
	}
	if (advanced != nullptr && !json_is_object(advanced)) {
		sink.set_error("'advanced' must be an object with orientation and/or offset.");
		return;
	}
	if (has_basic && has_advanced) {
		sink.set_error("Provide either 'basic' or 'advanced', not both.");
		return;
	}

	// --- Parse and validate everything before mutating ---
	std::optional<float> angle_deg, offset_m;
	std::optional<matrix> orient;
	std::optional<vec3d> offset_vec;

	if (has_basic) {
		angle_deg = get_optional_float(basic, "angle_degrees", sink);
		if (sink.has_error()) return;
		offset_m = get_optional_float(basic, "offset_meters", sink);
		if (sink.has_error()) return;

		if (angle_deg.has_value() && sm->rotation_axis_id == MOVEMENT_AXIS_NONE) {
			sink.set_error("Submodel '%s' has no rotation axis; use the advanced 'orientation' field instead.", sm->name);
			return;
		}
		if (offset_m.has_value() && sm->translation_axis_id == MOVEMENT_AXIS_NONE) {
			sink.set_error("Submodel '%s' has no translation axis; use the advanced 'offset' field instead.", sm->name);
			return;
		}
	}
	if (has_advanced) {
		orient = get_optional_matrix(advanced, "orientation", sink);
		if (sink.has_error()) return;
		offset_vec = get_optional_vec3d(advanced, "offset", sink);
		if (sink.has_error()) return;
	}

	auto blown = get_optional_bool(input, "blown_off", sink);
	if (sink.has_error()) return;

	// --- Apply ---
	bool changed = false;

	if (angle_deg.has_value()) {
		smi->cur_angle = fl_radians(*angle_deg);
		smi->prev_angle = smi->cur_angle;
		submodel_canonicalize_rotation(sm, smi, true);
		changed = true;
	}
	if (offset_m.has_value()) {
		smi->cur_offset = *offset_m;
		smi->prev_offset = smi->cur_offset;
		submodel_canonicalize_translation(sm, smi);
		changed = true;
	}
	if (orient.has_value()) {
		smi->canonical_prev_orient = smi->canonical_orient;
		smi->canonical_orient = *orient;
		// Keep the scalar angle in sync where the submodel has a defined axis;
		// otherwise leave it at zero (canonical_orient is authoritative for rendering).
		if (sm->rotation_axis_id != MOVEMENT_AXIS_NONE)
			vm_closest_angle_to_matrix(&smi->canonical_orient, &sm->rotation_axis, &smi->cur_angle);
		else
			smi->cur_angle = 0.0f;
		smi->prev_angle = smi->cur_angle;
		changed = true;
	}
	if (offset_vec.has_value()) {
		smi->canonical_prev_offset = smi->canonical_offset;
		smi->canonical_offset = *offset_vec;
		// Recover the signed scalar offset along the submodel's translation axis (the inverse
		// of submodel_canonicalize_translation).  Leave it at zero when the submodel has no
		// defined axis; canonical_offset is authoritative for rendering.
		switch (sm->translation_axis_id) {
			case MOVEMENT_AXIS_X:     smi->cur_offset = offset_vec->xyz.x; break;
			case MOVEMENT_AXIS_Y:     smi->cur_offset = offset_vec->xyz.y; break;
			case MOVEMENT_AXIS_Z:     smi->cur_offset = offset_vec->xyz.z; break;
			case MOVEMENT_AXIS_OTHER: smi->cur_offset = vm_vec_dot(&*offset_vec, &sm->translation_axis); break;
			default:                  smi->cur_offset = 0.0f; break;	// MOVEMENT_AXIS_NONE
		}
		smi->prev_offset = smi->cur_offset;
		changed = true;
	}
	if (blown.has_value() && smi->blown_off != *blown) {
		smi->blown_off = *blown;
		changed = true;
	}

	if (changed) {
		// Propagate the pose to all lower LODs and update destroyed-replacement
		// (next_form) bookkeeping.  No subsystem flags apply here.
		flagset<Ship::Subsystem_Flags> empty;
		model_replicate_submodel_instance(pm, pmi, sm_idx, empty);

		// Refresh the viewport.  Deliberately NOT mark_modified(): submodel-instance
		// pose is transient runtime state and is not saved to the mission file.
		Update_window = 1;
	}

	json_t *obj = build_submodel_json(pm, pmi, sm_idx);
	json_object_set_new(obj, "ship", json_safe_string(Ships[ship_idx].ship_name));
	req->result_json = make_json_tool_result(obj);
	req->success = true;
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void mcp_register_submodel_tools(json_t *tools)
{
	// list_ship_submodels
	register_tool_with_required_string(tools, "list_ship_submodels",
		"List a ship's submodels (subobjects) and their current instance pose. Lower-LOD "
		"duplicate submodels are omitted. Each entry has the submodel name, parent submodel "
		"(omitted for root submodels), whether it rotates/translates and on which axis, blown_off "
		"state, and the current pose in both 'basic' (angle_degrees/offset_meters; omitted when the "
		"submodel has no movement axis) and 'advanced' (full orientation matrix + offset vector) "
		"forms. NOTE: submodel pose is transient editor "
		"state for viewing configurations; it is not saved to the mission file.",
		"name", "Name of the ship");

	// get_ship_submodel
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship.");
		add_string_prop(props, "submodel",
			"Submodel (subobject) name, case-insensitive. Use list_ship_submodels to enumerate names.");
		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("submodel"));
		register_tool(tools, "get_ship_submodel",
			"Get one submodel's current instance pose: rotation/translation axis metadata, "
			"blown_off state, and the pose in both 'basic' (angle_degrees/offset_meters; omitted "
			"when the submodel has no corresponding movement axis) and 'advanced' (orientation "
			"matrix + offset vector) forms.",
			props, req);
	}

	// update_ship_submodel
	{
		json_t *props = json_object();
		add_string_prop(props, "name", "Name of the ship.");
		add_string_prop(props, "submodel",
			"Submodel (subobject) name, case-insensitive.");

		json_t *basic_props = json_object();
		add_number_prop(basic_props, "angle_degrees",
			"Rotation angle in degrees about the submodel's defined rotation axis. Normalized to "
			"[0, 360). Rejected if the submodel has no rotation axis (use advanced.orientation).");
		add_number_prop(basic_props, "offset_meters",
			"Linear displacement in meters along the submodel's defined translation axis. Rejected "
			"if the submodel has no translation axis (use advanced.offset).");
		add_object_prop(props, "basic",
			"Simple axis-constrained pose. Provide angle_degrees and/or offset_meters (each "
			"optional). Mutually exclusive with 'advanced'.", basic_props);

		json_t *adv_props = json_object();
		add_matrix_prop(adv_props, "orientation",
			"Full submodel orientation matrix (rvec/uvec/fvec), relative to the submodel's default "
			"(unrotated) orientation; the identity matrix means unrotated. Allows arbitrary posing of "
			"any submodel, including those without a defined rotation axis.");
		add_vec3d_prop(adv_props, "offset",
			"Full submodel translation offset vector, relative to the submodel's default offset from "
			"its parent; the zero vector means unmoved.");
		add_object_prop(props, "advanced",
			"Arbitrary pose. Provide orientation and/or offset (each optional). Mutually exclusive "
			"with 'basic'.", adv_props);

		add_bool_prop(props, "blown_off",
			"Whether this submodel is in its blown-off (destroyed) state and hidden from rendering. "
			"Optional; independent of basic/advanced.");

		json_t *req = json_array();
		json_array_append_new(req, json_string("name"));
		json_array_append_new(req, json_string("submodel"));
		register_tool(tools, "update_ship_submodel",
			"Set a ship submodel's instance pose for viewing configurations in the editor. Provide "
			"either a 'basic' (angle_degrees/offset_meters) or an 'advanced' (orientation/offset) "
			"object, not both; all pose fields are optional and applied as a partial update. May "
			"also toggle blown_off. The change is propagated to all LODs and the viewport is "
			"refreshed. NOTE: this is transient editor state and is NOT saved to the mission file.",
			props, req);
	}
}

// ---------------------------------------------------------------------------
// Main-thread dispatch
// ---------------------------------------------------------------------------

bool mcp_handle_submodel_tool(const char *tool_name, json_t *input_json, McpToolRequest *req)
{
	if (strcmp(tool_name, "list_ship_submodels") == 0) {
		handle_list_ship_submodels(input_json, req);
	} else if (strcmp(tool_name, "get_ship_submodel") == 0) {
		handle_get_ship_submodel(input_json, req);
	} else if (strcmp(tool_name, "update_ship_submodel") == 0) {
		handle_update_ship_submodel(input_json, req);
	} else {
		return false;
	}
	return true;
}
