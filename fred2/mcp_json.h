#ifndef _MCP_JSON_H
#define _MCP_JSON_H

#include <functional>
#include <initializer_list>
#include <optional>
#include <jansson.h>

#include "globalincs/vmallocator.h"	// for SCP_string

struct McpToolRequest;  // full definition in mcpserver.h
struct vec3d;           // full definition in pstypes.h
struct matrix;          // full definition in pstypes.h
struct color;           // full definition in graphics/2d.h

// Build an MCP tool result with a text content item.
json_t *make_tool_result(const char *text, bool is_error = false);

// Build an MCP tool result with a text content item, formatted with arguments.
json_t *make_tool_result(bool is_error, const char *format, ...);

// Build an MCP tool-result whose text content is pretty-printed JSON.
// Takes ownership of `data` (decrefs it after serializing).
json_t *make_json_tool_result(json_t *data);

// Set a string field only if the source is non-null and, optionally, non-empty.
void set_optional_string(json_t *obj, const char *key, const char *value, bool omit_if_empty);

// Set a string field (that represents a filename) only if VALID_FNAME is true.
void set_optional_filename(json_t *obj, const char *key, const char *value);

// Schema property helpers for tool registration.
void add_string_prop(json_t *props, const char *name, const char *description);
void add_integer_prop(json_t *props, const char *name, const char *description);
void add_number_prop(json_t *props, const char *name, const char *description);
void add_bool_prop(json_t *props, const char *name, const char *description);
void add_string_enum_prop(json_t *props, const char *name, const char *description, const SCP_vector<const char *> &allowed_values);
void add_string_array_prop(json_t *props, const char *name, const char *description, const SCP_vector<const char *> &allowed_values);
void add_object_array_prop(json_t *props, const char *name, const char *description,
	json_t *item_properties, json_t *item_required = nullptr);
void add_vec3d_prop(json_t *props, const char *name, const char *description);
void add_vec3d_array_prop(json_t *props, const char *name, const char *description);
void add_matrix_prop(json_t *props, const char *name, const char *description);
void add_color_prop(json_t *props, const char *name, const char *description);

// Register an MCP tool schema in the tools array.
void register_tool(json_t *tools, const char *name, const char *description,
	json_t *properties, json_t *required_arr = nullptr);

// Shorthand: register a tool whose only parameter is a single required string.
void register_tool_with_required_string(json_t *tools, const char *tool_name, const char *description,
	const char *param_name, const char *param_desc);

// Various validation functions
bool check_string_length(const char *input, size_t max_len, const char *param_name, McpToolRequest *req);
bool check_string_length(const char *input, size_t max_len, const char *param_name, json_t **error_out);
bool check_string_enum(const char *input, const SCP_vector<const char *> &values, const char *param_name, McpToolRequest *req);
bool check_string_enum(const char *input, const SCP_vector<const char *> &values, const char *param_name, json_t **error_out);
bool check_int_range(int input, int min, int max, const char *param_name, McpToolRequest *req);
bool check_int_range(int input, int min, int max, const char *param_name, json_t **error_out);
int check_lookup(const char *input, std::function<int(const char*)> lookup_fn, const char *param_name, McpToolRequest *req);
int check_lookup(const char *input, std::function<int(const char*)> lookup_fn, const char *param_name, json_t **error_out);
int check_lookup(const char *input, const SCP_vector<const char*> &lookup_vec, const char *param_name, McpToolRequest *req);
int check_lookup(const char *input, const SCP_vector<const char*> &lookup_vec, const char *param_name, json_t **error_out);

// Checks the given name against all entity types (ships, wings, waypoints, jump nodes, etc.)
// for conflicts.  The exclude parameters prevent matching against the entity being renamed.
// Returns true if the name is valid, or false with an error message on req.
bool check_name_conflict(const char *entity_type, const char *name, McpToolRequest *req,
	int exclude_ship = -1, int exclude_wing = -1, int exclude_waypoint_list = -1, int exclude_jump_node = -1);

bool validate(std::function<const char *()> error_msg_fn, McpToolRequest *req);
bool validate(std::function<bool(SCP_string&)> validate_fn, McpToolRequest *req);

template<typename T>
bool validate(const T& input, std::function<bool(const T&, SCP_string&)> validate_fn, McpToolRequest *req)
{
	SCP_string failure_msg;
	if (!validate_fn(input, failure_msg)) {
		req->success = false;
		strncpy(req->result_message, failure_msg.c_str(), sizeof(req->result_message) - 1);
		req->result_message[sizeof(req->result_message) - 1] = '\0';
		return false;
	}
	return true;
}

template<typename T>
bool validate(const T& input, std::function<bool(const T&, SCP_string&)> validate_fn, json_t **error_out)
{
	SCP_string failure_msg;
	if (!validate_fn(input, failure_msg)) {
		*error_out = make_tool_result(failure_msg.c_str(), true);
		return false;
	}
	return true;
}

// Extracts a required string parameter from input JSON. Returns nullptr and sets
// req->success=false with an error message if the parameter is missing, or empty and disallowed.
const char *get_required_string(json_t *input, const char *param_name, McpToolRequest *req, bool disallow_empty);

// Extracts a required string parameter (which represents a filename) from input JSON. Returns nullptr and sets
// req->success=false with an error message if VALID_FNAME fails.
const char *get_required_filename(json_t *input, const char *param_name, McpToolRequest *req);

// Extracts required integer, number, or bool parameters from input JSON. Returns false and sets
// req->success=false with an error message if the parameter is missing or the wrong type.
std::optional<int> get_required_integer(json_t *input, const char *param_name, McpToolRequest *req);
std::optional<double> get_required_double(json_t *input, const char *param_name, McpToolRequest *req);
std::optional<float> get_required_float(json_t *input, const char *param_name, McpToolRequest *req);
std::optional<bool> get_required_bool(json_t *input, const char *param_name, McpToolRequest *req);
std::optional<vec3d> get_required_vec3d(json_t *input, const char *param_name, McpToolRequest *req);
std::optional<matrix> get_required_matrix(json_t *input, const char *param_name, McpToolRequest *req);
std::optional<color> get_required_color(json_t *input, const char *param_name, McpToolRequest *req);

// Extracts a required string parameter from arguments JSON (for reference tools that
// return json_t* directly). Returns nullptr and sets *error_out to an error result
// if the parameter is missing, or empty and disallowed.
const char *get_required_string(json_t *arguments, const char *param_name, json_t **error_out, bool disallow_empty);

// Extracts required integer, double, float, or bool parameters from arguments JSON (for reference
// tools that return json_t* directly). Returns false and sets *error_out to an error result
// if the parameter is missing or the wrong type.
std::optional<int> get_required_integer(json_t *arguments, const char *param_name, json_t **error_out);
std::optional<double> get_required_double(json_t *arguments, const char *param_name, json_t **error_out);
std::optional<float> get_required_float(json_t *arguments, const char *param_name, json_t **error_out);
std::optional<bool> get_required_bool(json_t *arguments, const char *param_name, json_t **error_out);
std::optional<vec3d> get_required_vec3d(json_t *arguments, const char *param_name, json_t **error_out);
std::optional<matrix> get_required_matrix(json_t *arguments, const char *param_name, json_t **error_out);
std::optional<color> get_required_color(json_t *arguments, const char *param_name, json_t **error_out);

// Extracts an optional string parameter from arguments JSON (for reference tools that
// return json_t* directly). Returns nullptr if the parameter is missing or omitted.
const char *get_optional_string(json_t *arguments, const char *param_name, bool null_if_empty);

// Extracts an optional string parameter (that represents a filename) from arguments JSON (for reference tools that
// return json_t* directly). If the string doesn't satisfy VALID_FNAME, it returns null or empty, depending on the last parameter.
const char *get_optional_filename(json_t *arguments, const char *param_name, bool null_if_invalid);

// Extracts optional integer, double, float, or bool parameters from arguments JSON (for reference
// tools that return json_t* directly).
std::optional<int> get_optional_integer(json_t *arguments, const char *param_name);
std::optional<double> get_optional_double(json_t *arguments, const char *param_name);
std::optional<float> get_optional_float(json_t *arguments, const char *param_name);
std::optional<bool> get_optional_bool(json_t *arguments, const char *param_name);
std::optional<vec3d> get_optional_vec3d(json_t *arguments, const char *param_name);
std::optional<matrix> get_optional_matrix(json_t *arguments, const char *param_name);
std::optional<color> get_optional_color(json_t *arguments, const char *param_name);

// Extracts an optional JSON array of strings.
std::optional<SCP_vector<SCP_string>> get_optional_string_array(json_t *arguments, const char *param_name);

// Extracts a required JSON array of vec3d objects.  Returns true on success.
// If min_count > 0, the array must contain at least that many elements.
bool get_required_vec3d_array(json_t *input, const char *param_name,
	SCP_vector<vec3d> &out, McpToolRequest *req, int min_count = 0);

// Builds a JSON {"x":..., "y":..., "z":...} object from a vec3d.
json_t *build_vec3d_json(const vec3d &v);

// Builds a JSON {"rvec":..., "uvec":..., "fvec":...} object from a matrix.
json_t* build_matrix_json(const matrix& m);

// Builds a JSON {"red":..., "green":..., "blue":..., "alpha":..., "range":"0-255"}
// object from a color.  Alpha is included only if include_alpha is true.
json_t *build_color_json(const color &c, bool include_alpha = false);

// Build a JSON string array from a list of indices, using a C-string-compatible field.
template<typename VECTOR1_T, typename VECTOR2_T, typename ITEM_T, typename FIELD_T>
json_t *build_array_with_field(const VECTOR1_T& index_vector, const VECTOR2_T& item_vector, FIELD_T ITEM_T::* field)
{
	json_t *arr = json_array();
	for (int idx: index_vector)
		if (idx >= 0 && idx < static_cast<int>(item_vector.size()))
			json_array_append_new(arr, json_string(item_vector[idx].*field));
	return arr;
}

// Build a JSON string array from a list of indices, using a SCP_string field.
template<typename VECTOR1_T, typename VECTOR2_T, typename ITEM_T>
json_t *build_array_with_field(const VECTOR1_T& index_vector, const VECTOR2_T& item_vector, SCP_string ITEM_T::* field)
{
	json_t *arr = json_array();
	for (int idx: index_vector)
		if (idx >= 0 && idx < static_cast<int>(item_vector.size()))
			json_array_append_new(arr, json_string((item_vector[idx].*field).c_str()));
	return arr;
}

// Sets req->success=false and formats "EntityType not found: name" into result_message.
void set_not_found_error(McpToolRequest *req, const char *entity_type, const char *name);

// Builds an MCP error result with "EntityType not found: name" text (for reference tools).
json_t *make_not_found_error(const char *entity_type, const char *name);

#endif // _MCP_JSON_H
