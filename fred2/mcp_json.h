#ifndef _MCP_JSON_H
#define _MCP_JSON_H

#include <functional>
#include <jansson.h>

struct McpToolRequest;  // full definition in mcpserver.h

// Build an MCP tool result with a text content item.
json_t *make_tool_result(const char *text, bool is_error = false);

// Build an MCP tool-result whose text content is pretty-printed JSON.
// Takes ownership of `data` (decrefs it after serializing).
json_t *make_json_tool_result(json_t *data);

// Set a string field only if the source is non-null and non-empty.
void set_optional_string(json_t *obj, const char *key, const char *value);

// Schema property helpers for tool registration.
void add_string_prop(json_t *props, const char *name, const char *description);
void add_integer_prop(json_t *props, const char *name, const char *description);
void add_number_prop(json_t *props, const char *name, const char *description);
void add_bool_prop(json_t *props, const char *name, const char *description);

// Register an MCP tool schema in the tools array.
void register_tool(json_t *tools, const char *name, const char *description,
	json_t *properties, json_t *required_arr = nullptr);

// Calls check_fn(); if it returns a conflict message, sets req->success=false and
// copies the message into req->result_message, then returns true. Caller should return.
bool set_conflict_error(McpToolRequest *req, std::function<const char *()> check_fn);

// Extracts a required string parameter from input JSON. Returns nullptr and sets
// req->success=false with an error message if the parameter is missing or empty.
const char *require_string_param(json_t *input, const char *param_name, McpToolRequest *req);

// Extracts a required string parameter from arguments JSON (for reference tools that
// return json_t* directly). Returns nullptr and sets *error_out to an error result
// if the parameter is missing or empty.
const char *get_required_string(json_t *arguments, const char *param_name, json_t **error_out);

// Extracts an optional string parameter from arguments JSON (for reference tools that
// return json_t* directly). Returns nullptr if the parameter is missing or empty.
const char *get_optional_string(json_t *arguments, const char *param_name);

#endif // _MCP_JSON_H
