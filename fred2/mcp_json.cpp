#include "stdafx.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "parse/parselo.h"

#include <jansson.h>
#include "globalincs/pstypes.h"
#include "graphics/2d.h"
#include "management.h"

bool McpErrorSink::has_error() const
{
	return m_has_error;
}

void McpErrorSink::set_error(const char *fmt, ...)
{
	// always keep the first error
	if (m_has_error)
		return;

	m_has_error = true;

	va_list args;
	va_start(args, fmt);

	if (m_req) {
		m_req->success = false;
		vsprintf(m_req->result_message, fmt, args);
	} else if (m_err) {
		if (*m_err)
			json_decref(*m_err);
		*m_err = vmake_tool_result(true, fmt, args);
	}

	va_end(args);
}

json_t *make_tool_result(const char *text, bool is_error)
{
	json_t *result = json_object();
	json_t *content = json_array();
	json_t *item = json_object();
	json_object_set_new(item, "type", json_string("text"));
	json_object_set_new(item, "text", json_string(text));
	json_array_append_new(content, item);
	json_object_set_new(result, "content", content);
	if (is_error)
		json_object_set_new(result, "isError", json_true());
	return result;
}

json_t *make_tool_result(bool is_error, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	auto result = vmake_tool_result(is_error, format, args);
	va_end(args);
	return result;
}

json_t *vmake_tool_result(bool is_error, const char *format, va_list args)
{
	SCP_string buf;
	vsprintf(buf, format, args);
	return make_tool_result(buf.c_str(), is_error);
}

json_t *make_json_tool_result(json_t *data)
{
	char *text = json_dumps(data, JSON_INDENT(2) | JSON_REAL_PRECISION(6));
	json_t *result;
	if (text) {
		result = make_tool_result(text);
		free(text);
	} else {
		result = make_tool_result("json_dumps failed to return text!", true);
	}
	json_decref(data);
	return result;
}

void set_optional_string(json_t *obj, const char *key, const char *value, bool omit_if_empty)
{
	if (value && (value[0] || !omit_if_empty))
		json_object_set_new(obj, key, json_string(value));
}

void set_optional_filename(json_t *obj, const char *key, const char *value)
{
	if (value && VALID_FNAME(value))
		json_object_set_new(obj, key, json_string(value));
}

static void add_typed_prop(json_t *props, const char *name, const char *description, const char *type)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string(type));
	json_object_set_new(p, "description", json_string(description));
	json_object_set_new(props, name, p);
}

void add_string_prop(json_t *props, const char *name, const char *description)
	{ add_typed_prop(props, name, description, "string"); }

void add_integer_prop(json_t *props, const char *name, const char *description)
	{ add_typed_prop(props, name, description, "integer"); }

void add_number_prop(json_t *props, const char *name, const char *description)
	{ add_typed_prop(props, name, description, "number"); }

void add_bool_prop(json_t *props, const char *name, const char *description)
	{ add_typed_prop(props, name, description, "boolean"); }

void add_string_enum_prop(json_t *props, const char *name, const char *description, const SCP_vector<const char *> &allowed_values)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("string"));
	json_object_set_new(p, "description", json_string(description));
	json_t *arr = json_array();
	for (const char *v : allowed_values)
		json_array_append_new(arr, json_string(v));
	json_object_set_new(p, "enum", arr);
	json_object_set_new(props, name, p);
}

void add_string_array_prop(json_t *props, const char *name, const char *description, const SCP_vector<const char *> &allowed_values)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("array"));
	json_object_set_new(p, "description", json_string(description));
	json_t *items = json_object();
	json_object_set_new(items, "type", json_string("string"));
	json_t *arr = json_array();
	for (const char *v : allowed_values)
		json_array_append_new(arr, json_string(v));
	json_object_set_new(items, "enum", arr);
	json_object_set_new(p, "items", items);
	json_object_set_new(props, name, p);
}

void add_object_array_prop(json_t *props, const char *name, const char *description,
	json_t *item_properties, json_t *item_required)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("array"));
	json_object_set_new(p, "description", json_string(description));
	json_t *items = json_object();
	json_object_set_new(items, "type", json_string("object"));
	json_object_set_new(items, "properties", item_properties);
	if (item_required)
		json_object_set_new(items, "required", item_required);
	json_object_set_new(p, "items", items);
	json_object_set_new(props, name, p);
}

void register_tool(json_t *tools, const char *name, const char *description,
	json_t *properties, json_t *required_arr)
{
	json_t *tool = json_object();
	json_object_set_new(tool, "name", json_string(name));
	json_object_set_new(tool, "description", json_string(description));

	json_t *schema = json_object();
	json_object_set_new(schema, "type", json_string("object"));
	json_object_set_new(schema, "properties", properties ? properties : json_object());
	if (required_arr)
		json_object_set_new(schema, "required", required_arr);
	json_object_set_new(tool, "inputSchema", schema);

	json_array_append_new(tools, tool);
}

void register_tool_with_required_string(json_t *tools, const char *tool_name, const char *description,
	const char *param_name, const char *param_desc)
{
	json_t *props = json_object();
	add_string_prop(props, param_name, param_desc);
	json_t *req = json_array();
	json_array_append_new(req, json_string(param_name));
	register_tool(tools, tool_name, description, props, req);
}

static void set_missing_param_error(McpErrorSink &sink, const char *param_name)
{
	sink.set_error("Missing required parameter, or parameter is the wrong type: %s", param_name);
}

bool check_string_length(const char *input, size_t max_len, const char *param_name, McpErrorSink &sink)
{
	Assertion(input != nullptr, "check_string_length called with null input for param '%s'", param_name);
	size_t len = strlen(input);
	if (len > max_len) {
		sink.set_error("Parameter '%s' is too long (length=" SIZE_T_ARG "; max=" SIZE_T_ARG ")", param_name, len, max_len);
		return false;
	}
	return true;
}

static SCP_string format_string_enum_error(const char *input, const SCP_vector<const char *> &values, const char *param_name)
{
	SCP_string msg;
	sprintf(msg, "Parameter '%s' has invalid value '%s'. Must be one of:", param_name, input);
	for (const char *v : values) {
		msg += " \"";
		msg += v;
		msg += "\",";
	}
	if (!values.empty())
		msg.pop_back();  // remove trailing comma
	return msg;
}

bool check_string_enum(const char *input, const SCP_vector<const char *> &values, const char *param_name, McpErrorSink &sink)
{
	Assertion(input != nullptr, "check_string_enum called with null input for param '%s'", param_name);
	for (const char *v : values)
		if (stricmp(input, v) == 0)
			return true;
	SCP_string msg = format_string_enum_error(input, values, param_name);
	sink.set_error("%s", msg.c_str());
	return false;
}

bool check_int_range(int input, int min, int max, const char *param_name, McpErrorSink &sink)
{
	if (input < min || input > max) {
		sink.set_error("Parameter '%s' is out of range (value=%d; min=%d; max=%d)",
			param_name, input, min, max);
		return false;
	}
	return true;
}

int check_lookup(const char *input, std::function<int(const char*)> lookup_fn, const char *param_name, McpErrorSink &sink)
{
	Assertion(input != nullptr, "check_lookup called with null input for param '%s'", param_name);
	int result = lookup_fn(input);
	if (result < 0) {
		sink.set_error("Parameter '%s' could not be found in the list of allowed values (value=%s)",
			param_name, input);
	}
	return result;
}

int check_lookup(const char *input, const SCP_vector<const char*> &lookup_vec, const char *param_name, McpErrorSink &sink)
{
	Assertion(input != nullptr, "check_lookup called with null input for param '%s'", param_name);
	int count = sz2i(lookup_vec.size());
	int result = -1;
	for (int i = 0; i < count; i++) {
		if (!stricmp(input, lookup_vec[i])) {
			result = i;
			break;
		}
	}
	if (result < 0) {
		SCP_string msg = format_string_enum_error(input, lookup_vec, param_name);
		sink.set_error("%s", msg.c_str());
	}
	return result;
}

bool check_object_rename(const char *object_type, const char *name, McpErrorSink &sink,
	int exclude_ship, int exclude_wing, int exclude_waypoint_list, int exclude_jump_node)
{
	SCP_string conflict = check_name_conflict(object_type, name, exclude_ship, exclude_wing,
		exclude_waypoint_list, exclude_jump_node);
	if (!conflict.empty()) {
		sink.set_error("%s", conflict.c_str());
		return false;
	}
	return true;
}

bool check_general_rename(const char *new_name, const char *current_name,
	std::function<bool(const char*)> name_exists, const char *entity_label, McpErrorSink &sink)
{
	if (!check_string_length(new_name, NAME_LENGTH - 1, "new_name", sink))
		return false;
	if (!new_name[0]) {
		sink.set_error("%s name cannot be blank", entity_label);
		return false;
	}
	if (stricmp(current_name, new_name) != 0 && name_exists(new_name)) {
		sink.set_error("%s '%s' already exists", entity_label, new_name);
		return false;
	}
	return true;
}

bool validate(std::function<const char *()> error_msg_fn, McpErrorSink &sink)
{
	auto failure_msg = error_msg_fn();
	if (failure_msg) {
		sink.set_error("%s", failure_msg);
		return false;
	}
	return true;
}

bool validate(std::function<bool(SCP_string&)> validate_fn, McpErrorSink &sink)
{
	SCP_string failure_msg;
	if (!validate_fn(failure_msg)) {
		sink.set_error("%s", failure_msg.c_str());
		return false;
	}
	return true;
}

const char *get_required_string(json_t *input, const char *param_name, McpErrorSink &sink, bool disallow_empty, size_t max_len)
{
	auto value = get_optional_string(input, param_name, sink, max_len);
	if (!value) {
		set_missing_param_error(sink, param_name);
		return nullptr;
	}
	if (!value[0] && disallow_empty) {
		sink.set_error("Required parameter must not be empty: %s", param_name);
		return nullptr;
	}
	return value;
}

const char *get_required_filename(json_t *input, const char *param_name, McpErrorSink &sink, bool disallow_invalid, size_t max_len)
{
	auto value = get_required_string(input, param_name, sink, false, max_len);
	if (!value)
		return nullptr;
	if (!VALID_FNAME(value)) {
		if (disallow_invalid) {
			sink.set_error("Required parameter must be a valid file name: %s", param_name);
			return nullptr;
		}
		return "";
	}
	return value;
}

std::optional<int> get_required_integer(json_t *input, const char *param_name, McpErrorSink &sink)
{
	auto item = get_optional_integer(input, param_name, sink);
	if (item.has_value())
		return *item;
	set_missing_param_error(sink, param_name);
	return std::nullopt;
}

std::optional<double> get_required_double(json_t *input, const char *param_name, McpErrorSink &sink)
{
	auto item = get_optional_double(input, param_name, sink);
	if (item.has_value())
		return *item;
	set_missing_param_error(sink, param_name);
	return std::nullopt;
}

std::optional<float> get_required_float(json_t *input, const char *param_name, McpErrorSink &sink)
{
	auto item = get_optional_float(input, param_name, sink);
	if (item.has_value())
		return *item;
	set_missing_param_error(sink, param_name);
	return std::nullopt;
}

std::optional<bool> get_required_bool(json_t *input, const char *param_name, McpErrorSink &sink)
{
	auto item = get_optional_bool(input, param_name, sink);
	if (item.has_value())
		return *item;
	set_missing_param_error(sink, param_name);
	return std::nullopt;
}

const char *get_optional_string(json_t *arguments, const char *param_name, McpErrorSink &sink, size_t max_len)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (val && json_is_string(val)) {
		auto str = json_string_value(val);
		if (str) {
			if (max_len == SIZE_MAX || check_string_length(str, max_len, param_name, sink))
				return str;
		}
	} else if (val && !json_is_null(val)) {
		sink.set_error("Parameter '%s' must be a string", param_name);
	}
	return nullptr;
}

const char *get_optional_filename(json_t *arguments, const char *param_name, McpErrorSink &sink, bool disallow_invalid, size_t max_len)
{
	auto value = get_optional_string(arguments, param_name, sink, max_len);
	if (!value)
		return nullptr;
	if (!VALID_FNAME(value)) {
		if (disallow_invalid) {
			sink.set_error("Optional parameter, if specified, must be a valid file name: %s", param_name);
			return nullptr;
		}
		return "";
	}
	return value;
}

std::optional<int> get_optional_integer(json_t *arguments, const char *param_name, McpErrorSink &sink)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (val && json_is_integer(val)) {
		json_int_t raw = json_integer_value(val);
		if (raw < INT_MIN || raw > INT_MAX) {
			sink.set_error("Parameter '%s' is out of 32-bit integer range", param_name);
			return std::nullopt;
		}
		return (int)raw;
	}
	if (val && !json_is_null(val)) {
		sink.set_error("Parameter '%s' must be an integer", param_name);
	}
	return std::nullopt;
}

std::optional<double> get_optional_double(json_t *arguments, const char *param_name, McpErrorSink &sink)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (val && json_is_number(val)) {
		return json_number_value(val);
	}
	if (val && !json_is_null(val)) {
		sink.set_error("Parameter '%s' must be a number", param_name);
	}
	return std::nullopt;
}

std::optional<float> get_optional_float(json_t *arguments, const char *param_name, McpErrorSink &sink)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (val && json_is_number(val)) {
		return (float)json_number_value(val);
	}
	if (val && !json_is_null(val)) {
		sink.set_error("Parameter '%s' must be a number", param_name);
	}
	return std::nullopt;
}

std::optional<bool> get_optional_bool(json_t *arguments, const char *param_name, McpErrorSink &sink)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (val && json_is_boolean(val)) {
		return json_is_true(val);
	}
	if (val && !json_is_null(val)) {
		sink.set_error("Parameter '%s' must be a boolean", param_name);
	}
	return std::nullopt;
}

std::optional<SCP_vector<SCP_string>> get_optional_string_array(json_t *arguments, const char *param_name, McpErrorSink &sink)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (val && json_is_array(val)) {
		SCP_vector<SCP_string> result;
		size_t index;
		json_t *item;
		json_array_foreach(val, index, item) {
			if (json_is_string(item)) {
				result.push_back(json_string_value(item));
			} else {
				sink.set_error("'%s' array element " SIZE_T_ARG " is not a string", param_name, index);
				return std::nullopt;
			}
		}
		return result;
	}
	if (val && !json_is_null(val)) {
		sink.set_error("Parameter '%s' must be an array", param_name);
	}
	return std::nullopt;
}

static json_t *make_vec3d_schema()
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("object"));
	json_t *sub = json_object();
	for (const char *axis : {"x", "y", "z"}) {
		json_t *np = json_object();
		json_object_set_new(np, "type", json_string("number"));
		json_object_set_new(sub, axis, np);
	}
	json_object_set_new(p, "properties", sub);
	json_t *req = json_array();
	json_array_append_new(req, json_string("x"));
	json_array_append_new(req, json_string("y"));
	json_array_append_new(req, json_string("z"));
	json_object_set_new(p, "required", req);
	return p;
}

static std::optional<vec3d> parse_vec3d_json(json_t *obj)
{
	if (!obj || !json_is_object(obj))
		return std::nullopt;
	json_t *x = json_object_get(obj, "x");
	json_t *y = json_object_get(obj, "y");
	json_t *z = json_object_get(obj, "z");
	if (!x || !json_is_number(x) || !y || !json_is_number(y) || !z || !json_is_number(z))
		return std::nullopt;
	return vec3d{ (float)json_number_value(x), (float)json_number_value(y), (float)json_number_value(z) };
}

json_t *build_vec3d_json(const vec3d &v)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "x", json_real(v.xyz.x));
	json_object_set_new(obj, "y", json_real(v.xyz.y));
	json_object_set_new(obj, "z", json_real(v.xyz.z));
	return obj;
}

void add_vec3d_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = make_vec3d_schema();
	json_object_set_new(p, "description", json_string(description));
	json_object_set_new(props, name, p);
}

void add_vec3d_array_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("array"));
	json_object_set_new(p, "items", make_vec3d_schema());
	json_object_set_new(p, "description", json_string(description));
	json_object_set_new(props, name, p);
}

std::optional<SCP_vector<vec3d>> get_required_vec3d_array(json_t *input, const char *param_name, McpErrorSink &sink, int min_count)
{
	json_t *val = input ? json_object_get(input, param_name) : nullptr;
	if (!val) {
		set_missing_param_error(sink, param_name);
		return std::nullopt;
	}
	if (!json_is_array(val)) {
		sink.set_error("Parameter '%s' must be an array", param_name);
		return std::nullopt;
	}

	size_t arr_size = json_array_size(val);
	if (min_count > 0 && (int)arr_size < min_count) {
		sink.set_error("Parameter '%s' must contain at least %d element(s), got %d",
			param_name, min_count, (int)arr_size);
		return std::nullopt;
	}

	SCP_vector<vec3d> out;
	out.reserve(arr_size);
	size_t index;
	json_t *item;
	json_array_foreach(val, index, item) {
		auto v = parse_vec3d_json(item);
		if (!v.has_value()) {
			sink.set_error("Parameter '%s[%d]' is not a valid {x, y, z} object", param_name, (int)index);
			return std::nullopt;
		}
		out.push_back(*v);
	}

	return out;
}

static json_t *make_matrix_schema()
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("object"));
	json_t *sub = json_object();
	json_object_set_new(sub, "rvec", make_vec3d_schema());
	json_object_set_new(sub, "uvec", make_vec3d_schema());
	json_object_set_new(sub, "fvec", make_vec3d_schema());
	json_object_set_new(p, "properties", sub);
	json_t *req = json_array();
	json_array_append_new(req, json_string("rvec"));
	json_array_append_new(req, json_string("uvec"));
	json_array_append_new(req, json_string("fvec"));
	json_object_set_new(p, "required", req);
	return p;
}

json_t *build_matrix_json(const matrix &m)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "rvec", build_vec3d_json(m.vec.rvec));
	json_object_set_new(obj, "uvec", build_vec3d_json(m.vec.uvec));
	json_object_set_new(obj, "fvec", build_vec3d_json(m.vec.fvec));
	return obj;
}

void add_matrix_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = make_matrix_schema();
	json_object_set_new(p, "description", json_string(description));
	json_object_set_new(props, name, p);
}

static json_t *make_color_schema()
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("object"));
	json_t *sub = json_object();
	for (const char *ch : {"red", "green", "blue", "alpha"}) {
		json_t *np = json_object();
		json_object_set_new(np, "type", json_string("integer"));
		json_object_set_new(sub, ch, np);
	}
	json_object_set_new(p, "properties", sub);
	json_t *req = json_array();
	json_array_append_new(req, json_string("red"));
	json_array_append_new(req, json_string("green"));
	json_array_append_new(req, json_string("blue"));
	json_object_set_new(p, "required", req);
	return p;
}

static int clamp_color_channel(int val)
{
	if (val < 0) return 0;
	if (val > 255) return 255;
	return val;
}

static std::optional<color> parse_color_json(json_t *obj)
{
	if (!obj || !json_is_object(obj))
		return std::nullopt;
	json_t *jr = json_object_get(obj, "red");
	json_t *jg = json_object_get(obj, "green");
	json_t *jb = json_object_get(obj, "blue");
	if (!jr || !json_is_integer(jr) || !jg || !json_is_integer(jg) || !jb || !json_is_integer(jb))
		return std::nullopt;

	int r = clamp_color_channel((int)json_integer_value(jr));
	int g = clamp_color_channel((int)json_integer_value(jg));
	int b = clamp_color_channel((int)json_integer_value(jb));

	color c;

	// Alpha is optional
	json_t *ja = json_object_get(obj, "alpha");
	if (ja && json_is_integer(ja)) {
		int a = clamp_color_channel((int)json_integer_value(ja));
		gr_init_alphacolor(&c, r, g, b, a);
	} else {
		gr_init_color(&c, r, g, b);
	}

	return c;
}

json_t *build_color_json(const color &c, bool include_alpha)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "red", json_integer(c.red));
	json_object_set_new(obj, "green", json_integer(c.green));
	json_object_set_new(obj, "blue", json_integer(c.blue));
	if (include_alpha)
		json_object_set_new(obj, "alpha", json_integer(c.alpha));
	return obj;
}

void add_color_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = make_color_schema();
	json_object_set_new(p, "description", json_string(description));
	json_object_set_new(props, name, p);
}

std::optional<vec3d> get_optional_vec3d(json_t *arguments, const char *param_name, McpErrorSink &sink)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	auto result = parse_vec3d_json(val);
	if (!result.has_value() && val && !json_is_null(val)) {
		sink.set_error("Parameter '%s' must be a vec3d object", param_name);
	}
	return result;
}

std::optional<matrix> get_optional_matrix(json_t *arguments, const char *param_name, McpErrorSink &sink)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (!val)
		return std::nullopt;
	if (!json_is_object(val)) {
		if (!json_is_null(val))
			sink.set_error("Parameter '%s' must be a matrix object", param_name);
		return std::nullopt;
	}
	auto rvec = parse_vec3d_json(json_object_get(val, "rvec"));
	auto uvec = parse_vec3d_json(json_object_get(val, "uvec"));
	auto fvec = parse_vec3d_json(json_object_get(val, "fvec"));
	if (!rvec.has_value() || !uvec.has_value() || !fvec.has_value()) {
		sink.set_error("Parameter '%s' must be a matrix object", param_name);
		return std::nullopt;
	}
	return matrix{ *rvec, *uvec, *fvec };
}

std::optional<vec3d> get_required_vec3d(json_t *input, const char *param_name, McpErrorSink &sink)
{
	auto item = get_optional_vec3d(input, param_name, sink);
	if (item.has_value())
		return *item;
	set_missing_param_error(sink, param_name);
	return std::nullopt;
}

std::optional<matrix> get_required_matrix(json_t *input, const char *param_name, McpErrorSink &sink)
{
	auto item = get_optional_matrix(input, param_name, sink);
	if (item.has_value())
		return *item;
	set_missing_param_error(sink, param_name);
	return std::nullopt;
}

std::optional<color> get_optional_color(json_t *arguments, const char *param_name, McpErrorSink &sink)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	auto result = parse_color_json(val);
	if (!result.has_value() && val && !json_is_null(val)) {
		sink.set_error("Parameter '%s' must be a color object", param_name);
	}
	return result;
}

std::optional<color> get_required_color(json_t *input, const char *param_name, McpErrorSink &sink)
{
	auto item = get_optional_color(input, param_name, sink);
	if (item.has_value())
		return *item;
	set_missing_param_error(sink, param_name);
	return std::nullopt;
}

void set_not_found_error(McpErrorSink &sink, const char *entity_type, const char *name)
{
	sink.set_error("%s not found: '%s'", entity_type, name);
}

json_t *make_not_found_error(const char *entity_type, const char *name)
{
	return make_tool_result(true, "%s not found: '%s'", entity_type, name);
}
