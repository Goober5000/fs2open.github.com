#include "stdafx.h"
#include "mcp_json.h"
#include "mcpserver.h"

#include <jansson.h>

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

json_t *make_json_tool_result(json_t *data)
{
	char *text = json_dumps(data, JSON_INDENT(2) | JSON_REAL_PRECISION(6));
	json_t *result = make_tool_result(text);
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
	char buf[64];
	snprintf(buf, sizeof(buf), "Missing required parameter: %s", param_name);
	return make_tool_result(buf, true);
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
