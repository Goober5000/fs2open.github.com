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

// Abstracts over the two error-reporting conventions used by MCP tools.
// Mission tools write errors into McpToolRequest*; reference tools produce json_t*.
// Functions that take McpErrorSink& work with both conventions.
class McpErrorSink {
public:
	explicit McpErrorSink(McpToolRequest *req) : m_req(req), m_err(nullptr) {}
	explicit McpErrorSink(json_t **err) : m_req(nullptr), m_err(err) {}

	void set_error(const char *fmt, ...);
	bool has_error() const;

private:
	McpToolRequest *m_req;
	json_t **m_err;
};

// Build an MCP tool result with a text content item.
json_t *make_tool_result(const char *text, bool is_error = false);

// Build an MCP tool result with a text content item, formatted with arguments.
json_t *make_tool_result(bool is_error, const char *format, ...);
json_t *vmake_tool_result(bool is_error, const char *format, va_list args);

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

// Validation functions (unified via McpErrorSink)
bool check_string_length(const char *input, size_t max_len, const char *param_name, McpErrorSink &sink);
bool check_string_enum(const char *input, const SCP_vector<const char *> &values, const char *param_name, McpErrorSink &sink);
bool check_int_range(int input, int min, int max, const char *param_name, McpErrorSink &sink);
int check_lookup(const char *input, std::function<int(const char*)> lookup_fn, const char *param_name, McpErrorSink &sink);
int check_lookup(const char *input, const SCP_vector<const char*> &lookup_vec, const char *param_name, McpErrorSink &sink);

// Checks the given name against all entity types (ships, wings, waypoints, jump nodes, etc.)
// for conflicts.  The exclude parameters prevent matching against the entity being renamed.
// Returns true if the name is valid, or false with an error message.
bool check_name_conflict(const char *entity_type, const char *name, McpErrorSink &sink,
	int exclude_ship = -1, int exclude_wing = -1, int exclude_waypoint_list = -1, int exclude_jump_node = -1);

bool validate(std::function<const char *()> error_msg_fn, McpErrorSink &sink);
bool validate(std::function<bool(SCP_string&)> validate_fn, McpErrorSink &sink);

template<typename T>
bool validate(const T& input, std::function<bool(const T&, SCP_string&)> validate_fn, McpErrorSink &sink)
{
	SCP_string failure_msg;
	if (!validate_fn(input, failure_msg)) {
		sink.set_error("%s", failure_msg.c_str());
		return false;
	}
	return true;
}

// Extracts a required string parameter from input JSON. Returns nullptr and
// reports an error via sink if the parameter is missing, or empty and disallowed.
const char *get_required_string(json_t *input, const char *param_name, McpErrorSink &sink, bool disallow_empty);

// Extracts a required string parameter (which represents a filename) from input JSON.
// Returns nullptr and reports an error via sink if VALID_FNAME fails.
const char *get_required_filename(json_t *input, const char *param_name, McpErrorSink &sink);

// Extracts required typed parameters from input JSON.
// Returns std::nullopt and reports an error via sink if the parameter is missing or the wrong type.
std::optional<int> get_required_integer(json_t *input, const char *param_name, McpErrorSink &sink);
std::optional<double> get_required_double(json_t *input, const char *param_name, McpErrorSink &sink);
std::optional<float> get_required_float(json_t *input, const char *param_name, McpErrorSink &sink);
std::optional<bool> get_required_bool(json_t *input, const char *param_name, McpErrorSink &sink);
std::optional<vec3d> get_required_vec3d(json_t *input, const char *param_name, McpErrorSink &sink);
std::optional<matrix> get_required_matrix(json_t *input, const char *param_name, McpErrorSink &sink);
std::optional<color> get_required_color(json_t *input, const char *param_name, McpErrorSink &sink);

// Extracts an optional string parameter from arguments JSON.
// Returns nullptr if the parameter is missing; reports a type error via sink if present but not a string.
const char *get_optional_string(json_t *arguments, const char *param_name, McpErrorSink &sink, bool null_if_empty);

// Extracts an optional string parameter (that represents a filename) from arguments JSON.
// If the string doesn't satisfy VALID_FNAME, it returns null or empty, depending on null_if_invalid.
const char *get_optional_filename(json_t *arguments, const char *param_name, McpErrorSink &sink, bool null_if_invalid);

// Extracts optional typed parameters from arguments JSON.
// Returns std::nullopt if the parameter is missing; reports a type error via sink if present but the wrong type.
std::optional<int> get_optional_integer(json_t *arguments, const char *param_name, McpErrorSink &sink);
std::optional<double> get_optional_double(json_t *arguments, const char *param_name, McpErrorSink &sink);
std::optional<float> get_optional_float(json_t *arguments, const char *param_name, McpErrorSink &sink);
std::optional<bool> get_optional_bool(json_t *arguments, const char *param_name, McpErrorSink &sink);
std::optional<vec3d> get_optional_vec3d(json_t *arguments, const char *param_name, McpErrorSink &sink);
std::optional<matrix> get_optional_matrix(json_t *arguments, const char *param_name, McpErrorSink &sink);
std::optional<color> get_optional_color(json_t *arguments, const char *param_name, McpErrorSink &sink);

// Extracts an optional JSON array of strings.
// Reports an error via sink if the value is not an array, or if a non-string element is found.
std::optional<SCP_vector<SCP_string>> get_optional_string_array(json_t *arguments, const char *param_name, McpErrorSink &sink);

// Extracts a required JSON array of vec3d objects.  Returns true on success.
// If min_count > 0, the array must contain at least that many elements.
std::optional<SCP_vector<vec3d>> get_required_vec3d_array(json_t *input, const char *param_name, McpErrorSink &sink, int min_count = 0);

// Builds a JSON {"x":..., "y":..., "z":...} object from a vec3d.
json_t *build_vec3d_json(const vec3d &v);

// Builds a JSON {"rvec":..., "uvec":..., "fvec":...} object from a matrix.
json_t *build_matrix_json(const matrix &m);

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

// Reports "EntityType not found: name" via the error sink.
void set_not_found_error(McpErrorSink &sink, const char *entity_type, const char *name);

// Builds an MCP error result with "EntityType not found: name" text (for reference tools).
json_t *make_not_found_error(const char *entity_type, const char *name);

#endif // _MCP_JSON_H
