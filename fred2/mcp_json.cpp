#include "stdafx.h"
#include "mcp_json.h"
#include "mcpserver.h"
#include "parse/parselo.h"

#include <jansson.h>
#include "globalincs/pstypes.h"

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
	SCP_string buf;

	va_list args;
	va_start(args, format);
	vsprintf(buf, format, args);
	va_end(args);

	return make_tool_result(buf.c_str(), is_error);
}

json_t *make_json_tool_result(json_t *data)
{
	char *text = json_dumps(data, JSON_INDENT(2) | JSON_REAL_PRECISION(6));
	json_t *result;
	if (text) {
		result = make_tool_result(text);
	} else {
		result = make_tool_result("json_dumps failed to return text!", true);
	}
	free(text);
	json_decref(data);
	return result;
}

void set_optional_string(json_t *obj, const char *key, const char *value)
{
	if (value && value[0] != '\0')
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

static void set_missing_param_error(McpToolRequest *req, const char *param_name)
{
	req->success = false;
	snprintf(req->result_message, sizeof(req->result_message),
		"Missing required parameter: %s", param_name);
}

static json_t *make_missing_param_error(const char *param_name)
{
	return make_tool_result(true, "Missing required parameter: %s", param_name);
}

bool set_conflict_error(McpToolRequest *req, std::function<const char *()> check_fn)
{
	const char *conflict = check_fn();
	if (!conflict)
		return false;
	req->success = false;
	strncpy(req->result_message, conflict, sizeof(req->result_message) - 1);
	req->result_message[sizeof(req->result_message) - 1] = '\0';
	return true;
}

const char *require_string_param(json_t *input, const char *param_name, McpToolRequest *req)
{
	const char *value = get_optional_string(input, param_name);
	if (!value || !value[0]) {
		set_missing_param_error(req, param_name);
		return nullptr;
	}
	return value;
}

bool require_integer_param(json_t *input, const char *param_name, McpToolRequest *req, int *out)
{
	if (get_optional_integer(input, param_name, out))
		return true;
	set_missing_param_error(req, param_name);
	return false;
}

bool require_double_param(json_t *input, const char *param_name, McpToolRequest *req, double *out)
{
	if (get_optional_double(input, param_name, out))
		return true;
	set_missing_param_error(req, param_name);
	return false;
}

bool require_float_param(json_t *input, const char *param_name, McpToolRequest *req, float *out)
{
	if (get_optional_float(input, param_name, out))
		return true;
	set_missing_param_error(req, param_name);
	return false;
}

bool require_bool_param(json_t *input, const char *param_name, McpToolRequest *req, bool *out)
{
	if (get_optional_bool(input, param_name, out))
		return true;
	set_missing_param_error(req, param_name);
	return false;
}

const char *get_required_string(json_t *arguments, const char *param_name, json_t **error_out)
{
	const char *str = get_optional_string(arguments, param_name);
	if (!str || str[0] == '\0') {
		*error_out = make_missing_param_error(param_name);
		return nullptr;
	}
	return str;
}

bool get_required_integer(json_t *arguments, const char *param_name, json_t **error_out, int *out)
{
	if (get_optional_integer(arguments, param_name, out))
		return true;
	*error_out = make_missing_param_error(param_name);
	return false;
}

bool get_required_double(json_t *arguments, const char *param_name, json_t **error_out, double *out)
{
	if (get_optional_double(arguments, param_name, out))
		return true;
	*error_out = make_missing_param_error(param_name);
	return false;
}

bool get_required_float(json_t *arguments, const char *param_name, json_t **error_out, float *out)
{
	if (get_optional_float(arguments, param_name, out))
		return true;
	*error_out = make_missing_param_error(param_name);
	return false;
}

bool get_required_bool(json_t *arguments, const char *param_name, json_t **error_out, bool *out)
{
	if (get_optional_bool(arguments, param_name, out))
		return true;
	*error_out = make_missing_param_error(param_name);
	return false;
}

const char *get_optional_string(json_t *arguments, const char *param_name)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	return (val && json_is_string(val)) ? json_string_value(val) : nullptr;
}

bool get_optional_integer(json_t *arguments, const char *param_name, int *out)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (val && json_is_integer(val)) {
		*out = (int)json_integer_value(val);
		return true;
	}
	return false;
}

bool get_optional_double(json_t *arguments, const char *param_name, double *out)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (val && json_is_number(val)) {
		*out = json_number_value(val);
		return true;
	}
	return false;
}

bool get_optional_float(json_t *arguments, const char *param_name, float *out)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (val && json_is_number(val)) {
		*out = (float)json_number_value(val);
		return true;
	}
	return false;
}

bool get_optional_bool(json_t *arguments, const char *param_name, bool *out)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (val && json_is_boolean(val)) {
		*out = json_is_true(val);
		return true;
	}
	return false;
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

static bool parse_vec3d_json(json_t *obj, vec3d *out)
{
	if (!obj || !json_is_object(obj))
		return false;
	json_t *x = json_object_get(obj, "x");
	json_t *y = json_object_get(obj, "y");
	json_t *z = json_object_get(obj, "z");
	if (!x || !json_is_number(x) || !y || !json_is_number(y) || !z || !json_is_number(z))
		return false;
	out->xyz.x = (float)json_number_value(x);
	out->xyz.y = (float)json_number_value(y);
	out->xyz.z = (float)json_number_value(z);
	return true;
}

json_t *build_vec3d_json(const vec3d &v)
{
	json_t *obj = json_object();
	json_object_set_new(obj, "x", json_real(v.xyz.x));
	json_object_set_new(obj, "y", json_real(v.xyz.y));
	json_object_set_new(obj, "z", json_real(v.xyz.z));
	return obj;
}

json_t* build_matrix_json(const matrix& m)
{
	json_t* obj = json_object();
	json_object_set_new(obj, "rvec", build_vec3d_json(m.vec.rvec));
	json_object_set_new(obj, "uvec", build_vec3d_json(m.vec.uvec));
	json_object_set_new(obj, "fvec", build_vec3d_json(m.vec.fvec));
	return obj;
}

void add_vec3d_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = make_vec3d_schema();
	json_object_set_new(p, "description", json_string(description));
	json_object_set_new(props, name, p);
}

void add_matrix_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("object"));
	json_object_set_new(p, "description", json_string(description));
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
	json_object_set_new(props, name, p);
}

bool get_optional_vec3d(json_t *arguments, const char *param_name, vec3d *out)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	return parse_vec3d_json(val, out);
}

bool get_optional_matrix(json_t *arguments, const char *param_name, matrix *out)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	if (!val || !json_is_object(val))
		return false;
	return parse_vec3d_json(json_object_get(val, "rvec"), &out->vec.rvec)
		&& parse_vec3d_json(json_object_get(val, "uvec"), &out->vec.uvec)
		&& parse_vec3d_json(json_object_get(val, "fvec"), &out->vec.fvec);
}

bool get_required_vec3d(json_t *arguments, const char *param_name, json_t **error_out, vec3d *out)
{
	if (get_optional_vec3d(arguments, param_name, out))
		return true;
	*error_out = make_missing_param_error(param_name);
	return false;
}

bool get_required_matrix(json_t *arguments, const char *param_name, json_t **error_out, matrix *out)
{
	if (get_optional_matrix(arguments, param_name, out))
		return true;
	*error_out = make_missing_param_error(param_name);
	return false;
}

bool require_vec3d_param(json_t *input, const char *param_name, McpToolRequest *req, vec3d *out)
{
	if (get_optional_vec3d(input, param_name, out))
		return true;
	set_missing_param_error(req, param_name);
	return false;
}

bool require_matrix_param(json_t *input, const char *param_name, McpToolRequest *req, matrix *out)
{
	if (get_optional_matrix(input, param_name, out))
		return true;
	set_missing_param_error(req, param_name);
	return false;
}

void set_not_found_error(McpToolRequest *req, const char *entity_type, const char *name)
{
	req->success = false;
	snprintf(req->result_message, sizeof(req->result_message), "%s not found: %s", entity_type, name);
}

json_t *make_not_found_error(const char *entity_type, const char *name)
{
	return make_tool_result(true, "%s not found: %s", entity_type, name);
}
