#include "stdafx.h"
#include "mcpserver.h"
#include "mongoose.h"

#include <jansson.h>
#include <cstring>

#include "mission/missionparse.h"
#include "mainfrm.h"
#include "globalincs/pstypes.h"

static struct mg_context *mcp_ctx = nullptr;

static const char *MCP_PROTOCOL_VERSION = "2025-06-18";

// ---------------------------------------------------------------------------
// JSON-RPC helpers
// ---------------------------------------------------------------------------

static void send_json_response(struct mg_connection *conn, json_t *response)
{
	char *body = json_dumps(response, JSON_COMPACT);
	mg_printf(conn,
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/json\r\n"
		"Connection: close\r\n"
		"Content-Length: %d\r\n"
		"\r\n"
		"%s",
		(int)strlen(body), body);
	free(body);
}

static json_t *make_response(json_t *id, json_t *result)
{
	json_t *resp = json_object();
	json_object_set_new(resp, "jsonrpc", json_string("2.0"));
	if (id)
		json_object_set(resp, "id", id);
	else
		json_object_set_new(resp, "id", json_null());
	json_object_set_new(resp, "result", result);
	return resp;
}

static json_t *make_error_response(json_t *id, int code, const char *message)
{
	json_t *resp = json_object();
	json_object_set_new(resp, "jsonrpc", json_string("2.0"));
	if (id)
		json_object_set(resp, "id", id);
	else
		json_object_set_new(resp, "id", json_null());

	json_t *err = json_object();
	json_object_set_new(err, "code", json_integer(code));
	json_object_set_new(err, "message", json_string(message));
	json_object_set_new(resp, "error", err);
	return resp;
}

// ---------------------------------------------------------------------------
// MCP method handlers
// ---------------------------------------------------------------------------

static json_t *handle_initialize(json_t * /*params*/)
{
	json_t *result = json_object();
	json_object_set_new(result, "protocolVersion", json_string(MCP_PROTOCOL_VERSION));

	json_t *capabilities = json_object();
	json_object_set_new(capabilities, "tools", json_object());
	json_object_set_new(result, "capabilities", capabilities);

	json_t *server_info = json_object();
	json_object_set_new(server_info, "name", json_string("FRED2"));
	json_object_set_new(server_info, "version", json_string("1.0.0"));
	json_object_set_new(result, "serverInfo", server_info);

	return result;
}

static json_t *handle_tools_list(json_t * /*params*/)
{
	json_t *result = json_object();
	json_t *tools = json_array();

	// Tool: get_server_info
	json_t *tool = json_object();
	json_object_set_new(tool, "name", json_string("get_server_info"));
	json_object_set_new(tool, "description",
		json_string("Returns basic information about the running FRED2 instance"));

	json_t *schema = json_object();
	json_object_set_new(schema, "type", json_string("object"));
	json_object_set_new(schema, "properties", json_object());
	json_object_set_new(tool, "inputSchema", schema);

	json_array_append_new(tools, tool);

	// Tool: load_mission
	{
		json_t *t = json_object();
		json_object_set_new(t, "name", json_string("load_mission"));
		json_object_set_new(t, "description",
			json_string("Load a mission file into FRED2"));

		json_t *s = json_object();
		json_object_set_new(s, "type", json_string("object"));
		json_t *props = json_object();
		json_t *fp = json_object();
		json_object_set_new(fp, "type", json_string("string"));
		json_object_set_new(fp, "description", json_string("Absolute path to the mission file (.fs2 or .json)"));
		json_object_set_new(props, "filepath", fp);
		json_object_set_new(s, "properties", props);
		json_t *req = json_array();
		json_array_append_new(req, json_string("filepath"));
		json_object_set_new(s, "required", req);
		json_object_set_new(t, "inputSchema", s);

		json_array_append_new(tools, t);
	}

	// Tool: save_mission
	{
		json_t *t = json_object();
		json_object_set_new(t, "name", json_string("save_mission"));
		json_object_set_new(t, "description",
			json_string("Save the current mission in standard (.fs2) format"));

		json_t *s = json_object();
		json_object_set_new(s, "type", json_string("object"));
		json_t *props = json_object();
		json_t *fp = json_object();
		json_object_set_new(fp, "type", json_string("string"));
		json_object_set_new(fp, "description", json_string("Absolute path to save the mission file to"));
		json_object_set_new(props, "filepath", fp);
		json_object_set_new(s, "properties", props);
		json_t *req = json_array();
		json_array_append_new(req, json_string("filepath"));
		json_object_set_new(s, "required", req);
		json_object_set_new(t, "inputSchema", s);

		json_array_append_new(tools, t);
	}

	// Tool: save_mission_json
	{
		json_t *t = json_object();
		json_object_set_new(t, "name", json_string("save_mission_json"));
		json_object_set_new(t, "description",
			json_string("Save the current mission in JSON format"));

		json_t *s = json_object();
		json_object_set_new(s, "type", json_string("object"));
		json_t *props = json_object();
		json_t *fp = json_object();
		json_object_set_new(fp, "type", json_string("string"));
		json_object_set_new(fp, "description", json_string("Absolute path to save the JSON mission file to"));
		json_object_set_new(props, "filepath", fp);
		json_object_set_new(s, "properties", props);
		json_t *req = json_array();
		json_array_append_new(req, json_string("filepath"));
		json_object_set_new(s, "required", req);
		json_object_set_new(t, "inputSchema", s);

		json_array_append_new(tools, t);
	}

	json_object_set_new(result, "tools", tools);

	return result;
}

// Build an MCP tool result with a text content item
static json_t *make_tool_result(const char *text, bool is_error = false)
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

// Marshal a tool call to the main MFC thread via SendMessage and return the result
static json_t *execute_on_main_thread(McpToolId tool, const char *filepath)
{
	if (!Fred_main_wnd || !Fred_main_wnd->m_hWnd)
		return make_tool_result("FRED2 main window is not available", true);

	McpToolRequest req = {};
	req.tool = tool;
	strncpy(req.filepath, filepath, sizeof(req.filepath) - 1);
	req.filepath[sizeof(req.filepath) - 1] = '\0';
	req.success = false;
	req.result_message[0] = '\0';

	// Normalize path separators for the FreeSpace engine
	for (char *p = req.filepath; *p; ++p) {
		if (*p == '/' || *p == '\\')
			*p = DIR_SEPARATOR_CHAR;
	}

	::SendMessage(Fred_main_wnd->m_hWnd, WM_MCP_TOOL_CALL, 0, (LPARAM)&req);

	return make_tool_result(req.result_message, !req.success);
}

static json_t *handle_tools_call(json_t *params)
{
	const char *tool_name = nullptr;
	json_t *name_val = json_object_get(params, "name");
	if (name_val && json_is_string(name_val))
		tool_name = json_string_value(name_val);

	if (!tool_name)
		return nullptr;  // caller will send error

	if (strcmp(tool_name, "get_server_info") == 0) {
		char info[256];
		const char *mission = (Mission_filename[0] != '\0') ? Mission_filename : "(none)";
		snprintf(info, sizeof(info), "FRED2 MCP Server is running. Mission: %s", mission);
		return make_tool_result(info);
	}

	if (strcmp(tool_name, "load_mission") == 0 ||
		strcmp(tool_name, "save_mission") == 0 ||
		strcmp(tool_name, "save_mission_json") == 0)
	{
		// Extract filepath from arguments
		json_t *arguments = json_object_get(params, "arguments");
		json_t *fp_val = arguments ? json_object_get(arguments, "filepath") : nullptr;
		const char *filepath = (fp_val && json_is_string(fp_val)) ? json_string_value(fp_val) : nullptr;

		if (!filepath || filepath[0] == '\0')
			return make_tool_result("Missing required parameter: filepath", true);

		McpToolId tool;
		if (strcmp(tool_name, "load_mission") == 0)
			tool = McpToolId::LOAD_MISSION;
		else if (strcmp(tool_name, "save_mission") == 0)
			tool = McpToolId::SAVE_MISSION;
		else
			tool = McpToolId::SAVE_MISSION_JSON;

		return execute_on_main_thread(tool, filepath);
	}

	return make_tool_result("Unknown tool", true);
}

// ---------------------------------------------------------------------------
// HTTP request handler
// ---------------------------------------------------------------------------

static void serve_html_status(struct mg_connection *conn)
{
	mg_printf(conn,
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/html\r\n"
		"Connection: close\r\n"
		"\r\n"
		"<!DOCTYPE html>\n"
		"<html>\n"
		"<head><title>FRED2 MCP Server</title></head>\n"
		"<body>\n"
		"<h1>FRED2 MCP Server</h1>\n"
		"<p>The MCP server is running.</p>\n"
		"<p>Endpoint: <code>POST /mcp</code></p>\n"
		"<p>Protocol version: %s</p>\n"
		"</body>\n"
		"</html>\n",
		MCP_PROTOCOL_VERSION);
}

static void handle_mcp_post(struct mg_connection *conn)
{
	// Read POST body
	char body[8192];
	int body_len = mg_read(conn, body, sizeof(body) - 1);
	if (body_len <= 0) {
		json_t *resp = make_error_response(nullptr, -32700, "Empty request body");
		send_json_response(conn, resp);
		json_decref(resp);
		return;
	}
	body[body_len] = '\0';

	// Parse JSON
	json_error_t error;
	json_t *request = json_loads(body, 0, &error);
	if (!request) {
		json_t *resp = make_error_response(nullptr, -32700, "Parse error");
		send_json_response(conn, resp);
		json_decref(resp);
		return;
	}

	// Extract fields
	json_t *id = json_object_get(request, "id");
	json_t *method_val = json_object_get(request, "method");
	json_t *params = json_object_get(request, "params");

	const char *method = json_is_string(method_val) ? json_string_value(method_val) : nullptr;
	if (!method) {
		json_t *resp = make_error_response(id, -32600, "Invalid request: missing method");
		send_json_response(conn, resp);
		json_decref(resp);
		json_decref(request);
		return;
	}

	// Notifications (no id) — process but don't respond
	if (!id) {
		// notifications/initialized — nothing to do
		json_decref(request);
		// Send 202 Accepted with no body
		mg_printf(conn,
			"HTTP/1.1 202 Accepted\r\n"
			"Content-Length: 0\r\n"
			"Connection: close\r\n"
			"\r\n");
		return;
	}

	// Dispatch method
	json_t *result = nullptr;
	json_t *resp = nullptr;

	if (strcmp(method, "initialize") == 0) {
		result = handle_initialize(params);
	} else if (strcmp(method, "tools/list") == 0) {
		result = handle_tools_list(params);
	} else if (strcmp(method, "tools/call") == 0) {
		result = handle_tools_call(params);
	}

	if (result) {
		resp = make_response(id, result);
	} else {
		resp = make_error_response(id, -32601, "Method not found");
	}

	send_json_response(conn, resp);
	json_decref(resp);
	json_decref(request);
}

static void *mcp_request_handler(enum mg_event event, struct mg_connection *conn)
{
	if (event != MG_NEW_REQUEST)
		return nullptr;

	const struct mg_request_info *info = mg_get_request_info(conn);

	if (strcmp(info->request_method, "GET") == 0) {
		serve_html_status(conn);
		return (void *)"";
	}

	if (strcmp(info->request_method, "POST") == 0 && strcmp(info->uri, "/mcp") == 0) {
		handle_mcp_post(conn);
		return (void *)"";
	}

	// 404 for anything else
	mg_printf(conn,
		"HTTP/1.1 404 Not Found\r\n"
		"Content-Type: text/plain\r\n"
		"Connection: close\r\n"
		"\r\n"
		"Not Found\n");
	return (void *)"";
}

// ---------------------------------------------------------------------------
// Server lifecycle
// ---------------------------------------------------------------------------

void mcp_server_start()
{
	if (mcp_ctx != nullptr)
		return;

	const char *options[] = {
		"listening_ports", "127.0.0.1:8080",
		"num_threads", "2",
		nullptr
	};

	mcp_ctx = mg_start(mcp_request_handler, nullptr, options);
}

void mcp_server_stop()
{
	if (mcp_ctx != nullptr) {
		mg_stop(mcp_ctx);
		mcp_ctx = nullptr;
	}
}

bool mcp_server_is_running()
{
	return mcp_ctx != nullptr;
}
