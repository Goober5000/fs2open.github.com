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

void add_string_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("string"));
	json_object_set_new(p, "description", json_string(description));
	json_object_set_new(props, name, p);
}

void add_integer_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("integer"));
	json_object_set_new(p, "description", json_string(description));
	json_object_set_new(props, name, p);
}

void add_number_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("number"));
	json_object_set_new(p, "description", json_string(description));
	json_object_set_new(props, name, p);
}

void add_bool_prop(json_t *props, const char *name, const char *description)
{
	json_t *p = json_object();
	json_object_set_new(p, "type", json_string("boolean"));
	json_object_set_new(p, "description", json_string(description));
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
	const char *value = nullptr;
	if (input) {
		json_t *v = json_object_get(input, param_name);
		if (v && json_is_string(v))
			value = json_string_value(v);
	}
	if (!value || !value[0]) {
		req->success = false;
		snprintf(req->result_message, sizeof(req->result_message),
			"Missing required parameter: %s", param_name);
		return nullptr;
	}
	return value;
}

const char *get_required_string(json_t *arguments, const char *param_name, json_t **error_out)
{
	json_t *val = arguments ? json_object_get(arguments, param_name) : nullptr;
	const char *str = (val && json_is_string(val)) ? json_string_value(val) : nullptr;
	if (!str || str[0] == '\0') {
		char buf[64];
		snprintf(buf, sizeof(buf), "Missing required parameter: %s", param_name);
		*error_out = make_tool_result(buf, true);
		return nullptr;
	}
	return str;
}
