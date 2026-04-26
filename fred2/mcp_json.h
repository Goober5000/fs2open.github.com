#ifndef _MCP_JSON_H
#define _MCP_JSON_H

#include <functional>
#include <initializer_list>
#include <optional>
#include <utility>
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
	explicit McpErrorSink() : m_req(nullptr), m_err(nullptr), m_has_error(false) {}	// dev/null sink
	explicit McpErrorSink(McpToolRequest *req) : m_req(req), m_err(nullptr), m_has_error(false) {}
	explicit McpErrorSink(json_t **err) : m_req(nullptr), m_err(err), m_has_error(false) {}

	void set_error(const char *fmt, ...);
	bool has_error() const;

private:
	McpToolRequest *m_req;
	json_t **m_err;
	bool m_has_error;
};

// Build an MCP tool result with a text content item.
json_t *make_tool_result(const char *text, bool is_error = false);

// Build an MCP tool result with a text content item, formatted with arguments.
json_t *make_tool_result(bool is_error, const char *format, ...);
json_t *vmake_tool_result(bool is_error, const char *format, va_list args);

// Extract the human-readable text from a tool-result JSON object produced by
// vmake_tool_result.  Returns nullptr if the structure does not match.
const char *extract_tool_result_text(json_t *tool_result);

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

// Build a schema_extras object with a branch constraint (e.g. "oneOf" or "anyOf")
// between one or more groups of required fields.
json_t *build_branch_required_fields(const char *branchType, const SCP_vector<SCP_vector<const char *>> &groups);

// Build {"allOf": [{<branchType>: [<group_a>]}, {<branchType>: [<group_b>]}, ...]}
// for tools that need multiple independent branch constraints (e.g. one oneOf
// for source_* and another for target_*).  Each entry in per_branch_groups is
// the equivalent of one build_branch_required_fields call's `groups` argument.
json_t *build_branch_required_fields_allof(const char *branchType,
	const SCP_vector<SCP_vector<SCP_vector<const char *>>> &per_branch_groups);

// Build a schema_extras object with an "allOf" of if/then constraints
// expressing: for each entry {value, required_fields}, if `property_name` is
// present and equals `value`, then the listed fields are required.  Useful
// for discriminator-style parameters (e.g. a `role` field where different
// values require different companion parameters).
json_t *build_conditional_required_fields(const char *property_name,
	const SCP_vector<std::pair<const char *, SCP_vector<const char *>>> &value_to_required);

// Build a schema_extras object with a "dependencies" constraint expressing
// conditional presence: for each pair {dependent_field, required_fields}, if
// dependent_field is present in the input then every name in required_fields
// must also be present.  Useful for constraints like "entity_tag is only valid
// with target_entity_type".
json_t *build_dependencies_extras(
	const SCP_vector<std::pair<const char *, SCP_vector<const char *>>> &deps);

// Merge the key-value pairs from `source` into `dest`, taking ownership of
// `source` (it is decref'd).  Returns `dest` so the result can be passed
// directly as register_tool's schema_extras argument.
json_t *merge_schema_extras(json_t *dest, json_t *source);

// Register an MCP tool schema in the tools array.  If schema_extras is
// provided, its keys are merged into the inputSchema (e.g. {"oneOf": [...]} or
// {"anyOf": [...]}).  Ownership of schema_extras transfers to this function.
void register_tool(json_t *tools, const char *name, const char *description,
	json_t *properties, json_t *required_arr = nullptr,
	json_t *schema_extras = nullptr);

// Shorthand: register a tool whose only parameter is a single required string.
void register_tool_with_required_string(json_t *tools, const char *tool_name, const char *description,
	const char *param_name, const char *param_desc);

// Validation functions (unified via McpErrorSink)
bool check_string_length(const char *input, size_t max_len, const char *param_name, McpErrorSink &sink);
bool check_string_enum(const char *input, const SCP_vector<const char *> &values, const char *param_name, McpErrorSink &sink);
bool check_int_range(int input, int min, int max, const char *param_name, McpErrorSink &sink);
int check_lookup(const char *input, std::function<int(const char*)> lookup_fn, const char *param_name, McpErrorSink &sink);
int check_lookup(const char *input, const SCP_vector<const char*> &lookup_vec, const char *param_name, McpErrorSink &sink);

// Checks the given name against all object types (ships, wings, waypoints, jump nodes, etc.)
// for conflicts.  The exclude parameters prevent matching against the object being renamed.
// Returns true if the name is valid, or false with an error message.
bool check_object_rename(const char *object_type, const char *name, McpErrorSink &sink,
	int exclude_ship = -1, int exclude_wing = -1, int exclude_waypoint_list = -1, int exclude_jump_node = -1);

// Validates a rename: checks length, non-empty, and duplicate via name_exists callback.
// Returns true if valid, false with error via sink if not.
bool check_general_rename(const char *new_name, const char *current_name,
	std::function<bool(const char*)> name_exists, const char *entity_label, McpErrorSink &sink);

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

// Extracts a required string parameter from input JSON.
// Returns nullptr and reports an error via sink if the parameter is missing, has the wrong type,
// exceeds max_len, or (with disallow_empty=true) is the empty string.  Returns a non-null string
// otherwise.
const char *get_required_string(json_t *input, const char *param_name, McpErrorSink &sink, bool disallow_empty, size_t max_len = SIZE_MAX);

// Extracts a required string parameter (which represents a filename) from input JSON.
// Returns nullptr and reports an error via sink if the parameter is missing, has the wrong type,
// or exceeds max_len. Returns an empty string if VALID_FNAME (empty, "none", or "<none>") is
// false. Returns a non-null filename otherwise.
const char *get_required_filename(json_t *input, const char *param_name, McpErrorSink &sink, bool disallow_invalid, size_t max_len = SIZE_MAX);

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
// Returns nullptr if the parameter is missing; returns nullptr and reports an error via sink if
// it is JSON null, the wrong type, or exceeds max_len. Returns a non-null string otherwise.
const char *get_optional_string(json_t *arguments, const char *param_name, McpErrorSink &sink, size_t max_len = SIZE_MAX);

// Extracts an optional string parameter (which represents a filename) from input JSON.
// Returns nullptr if the parameter is missing; returns nullptr and reports an error via sink if
// it is JSON null, the wrong type, or exceeds max_len. Returns an empty string if VALID_FNAME
// (empty, "none", or "<none>") is false. Returns a non-null filename otherwise.
const char *get_optional_filename(json_t *arguments, const char *param_name, McpErrorSink &sink, bool disallow_invalid, size_t max_len = SIZE_MAX);

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

// Compute max string length for a named field across a jansson array.
size_t json_array_max_string_length(json_t *arr, const char *field);

#endif // _MCP_JSON_H
