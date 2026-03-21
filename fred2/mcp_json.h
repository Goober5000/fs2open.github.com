#ifndef _MCP_JSON_H
#define _MCP_JSON_H

#include <functional>
#include <jansson.h>

#include "globalincs/vmallocator.h"	// for SCP_string

struct McpToolRequest;  // full definition in mcpserver.h
struct vec3d;           // full definition in pstypes.h
struct matrix;          // full definition in pstypes.h

// Build an MCP tool result with a text content item.
json_t *make_tool_result(const char *text, bool is_error = false);

// Build an MCP tool result with a text content item, formatted with arguments.
json_t *make_tool_result(bool is_error, const char *format, ...);

// Build an MCP tool-result whose text content is pretty-printed JSON.
// Takes ownership of `data` (decrefs it after serializing).
json_t *make_json_tool_result(json_t *data);

// Build an MCP tool-result wrapping `arr` in {"<array_key>": arr, "count": N}.
// Takes ownership of `arr`.
json_t *make_list_result(const char *array_key, json_t *arr);

// Set a string field only if the source is non-null and non-empty.
void set_optional_string(json_t *obj, const char *key, const char *value);

// Schema property helpers for tool registration.
void add_string_prop(json_t *props, const char *name, const char *description);
void add_integer_prop(json_t *props, const char *name, const char *description);
void add_number_prop(json_t *props, const char *name, const char *description);
void add_bool_prop(json_t *props, const char *name, const char *description);
void add_vec3d_prop(json_t *props, const char *name, const char *description);
void add_matrix_prop(json_t *props, const char *name, const char *description);

// Register an MCP tool schema in the tools array.
void register_tool(json_t *tools, const char *name, const char *description,
	json_t *properties, json_t *required_arr = nullptr);

// Calls check_fn(); if it returns a conflict message, sets req->success=false and
// copies the message into req->result_message, then returns true. Caller should return.
bool set_conflict_error(McpToolRequest *req, std::function<const char *()> check_fn);

// Extracts a required string parameter from input JSON. Returns nullptr and sets
// req->success=false with an error message if the parameter is missing or empty.
const char *require_string_param(json_t *input, const char *param_name, McpToolRequest *req);

// Extracts required integer, number, or bool parameters from input JSON. Returns false and sets
// req->success=false with an error message if the parameter is missing or the wrong type.
bool require_integer_param(json_t *input, const char *param_name, McpToolRequest *req, int *out);
bool require_double_param(json_t *input, const char *param_name, McpToolRequest *req, double *out);
bool require_float_param(json_t *input, const char *param_name, McpToolRequest *req, float *out);
bool require_bool_param(json_t *input, const char *param_name, McpToolRequest *req, bool *out);
bool require_vec3d_param(json_t *input, const char *param_name, McpToolRequest *req, vec3d *out);
bool require_matrix_param(json_t *input, const char *param_name, McpToolRequest *req, matrix *out);

// Extracts a required string parameter from arguments JSON (for reference tools that
// return json_t* directly). Returns nullptr and sets *error_out to an error result
// if the parameter is missing or empty.
const char *get_required_string(json_t *arguments, const char *param_name, json_t **error_out);

// Extracts required integer, double, float, or bool parameters from arguments JSON (for reference
// tools that return json_t* directly). Returns false and sets *error_out to an error result
// if the parameter is missing or the wrong type.
bool get_required_integer(json_t *arguments, const char *param_name, json_t **error_out, int *out);
bool get_required_double(json_t *arguments, const char *param_name, json_t **error_out, double *out);
bool get_required_float(json_t *arguments, const char *param_name, json_t **error_out, float *out);
bool get_required_bool(json_t *arguments, const char *param_name, json_t **error_out, bool *out);
bool get_required_vec3d(json_t *arguments, const char *param_name, json_t **error_out, vec3d *out);
bool get_required_matrix(json_t *arguments, const char *param_name, json_t **error_out, matrix *out);

// Extracts an optional string parameter from arguments JSON (for reference tools that
// return json_t* directly). Returns nullptr if the parameter is missing or empty.
const char *get_optional_string(json_t *arguments, const char *param_name);

// Extracts optional integer, double, float, or bool parameters from arguments JSON (for reference
// tools that return json_t* directly). Returns true and sets *out if present, false if absent.
bool get_optional_integer(json_t *arguments, const char *param_name, int *out);
bool get_optional_double(json_t *arguments, const char *param_name, double *out);
bool get_optional_float(json_t *arguments, const char *param_name, float *out);
bool get_optional_bool(json_t *arguments, const char *param_name, bool *out);
bool get_optional_vec3d(json_t *arguments, const char *param_name, vec3d *out);
bool get_optional_matrix(json_t *arguments, const char *param_name, matrix *out);

// Builds a JSON {"x":..., "y":..., "z":...} object from a vec3d.
struct vec3d;
json_t *build_vec3d_json(const vec3d &v);

// Builds a JSON {"rvec":..., "uvec":..., "fvec":...} object from a matrix.
struct matrix;
json_t* build_matrix_json(const matrix& m);

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
