#include "stdafx.h"
#include "mcpserver.h"
#include "mcp_json.h"
#include "mcp_reference_tools.h"
#include "mcp_mission_tools.h"
#include "mongoose.h"

#include <jansson.h>
#include <cstring>

#include "mission/missionparse.h"
#include "mod_table/mod_table.h"
#include "mainfrm.h"
#include "globalincs/pstypes.h"

std::atomic<bool> mcp_fred_ready{false};

static struct mg_context *mcp_ctx = nullptr;

static const char *MCP_PROTOCOL_VERSION = "2025-06-18";

// ---------------------------------------------------------------------------
// JSON-RPC helpers
// ---------------------------------------------------------------------------

static void send_json_response(struct mg_connection *conn, json_t *response)
{
	char *body = json_dumps(response, JSON_COMPACT | JSON_REAL_PRECISION(6));
	size_t body_len = strlen(body);

	// Write header and body separately so that large responses are not
	// limited by mg_printf's internal buffer (8 KB on Windows).
	mg_printf(conn,
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/json\r\n"
		"Connection: close\r\n"
		"Content-Length: %d\r\n"
		"\r\n",
		(int)body_len);
	mg_write(conn, body, body_len);
	free(body);
}

static json_t *make_base_response(json_t *id)
{
	json_t *resp = json_object();
	json_object_set_new(resp, "jsonrpc", json_string("2.0"));
	if (id)
		json_object_set(resp, "id", id);
	else
		json_object_set_new(resp, "id", json_null());
	return resp;
}

static json_t *make_response(json_t *id, json_t *result)
{
	json_t *resp = make_base_response(id);
	json_object_set_new(resp, "result", result);
	return resp;
}

static json_t *make_error_response(json_t *id, int code, const char *message)
{
	json_t *resp = make_base_response(id);

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
	register_tool(tools, "get_server_info",
		"Returns information about the running FRED2 instance, including the currently loaded mission and active mod if applicable",
		nullptr);

	// Tool: load_mission
	{
		json_t *props = json_object();
		add_string_prop(props, "filepath", "Absolute path to the mission file (.fs2 extension)");
		json_t *req = json_array();
		json_array_append_new(req, json_string("filepath"));
		register_tool(tools, "load_mission", "Load a mission file into FRED2", props, req);
	}

	// Tool: save_mission
	{
		json_t *props = json_object();
		add_string_prop(props, "filepath", "Absolute path to save the mission file to");
		json_t *req = json_array();
		json_array_append_new(req, json_string("filepath"));
		register_tool(tools, "save_mission", "Save the current mission in standard (.fs2) format", props, req);
	}

	// Tool: new_mission
	register_tool(tools, "new_mission",
		"Create a new empty mission, replacing any currently loaded mission",
		nullptr);

	// Tool: get_mission_info
	register_tool(tools, "get_mission_info",
		"Returns metadata about the currently loaded mission (filename, title, author, notes, etc.)",
		nullptr);

	// Tool: get_ui_status
	register_tool(tools, "get_ui_status",
		"Returns the state of FRED2's UI windows: whether a modal dialog is blocking, and which modeless editor windows are open",
		nullptr);

	// Tool: get_timeout
	register_tool(tools, "get_timeout",
		"Returns the current timeout (in seconds) for MCP operations that run on the FRED2 UI thread",
		nullptr);

	// Tool: set_timeout
	{
		json_t *props = json_object();
		add_integer_prop(props, "seconds", "Timeout in seconds (1-300)");
		json_t *req = json_array();
		json_array_append_new(req, json_string("seconds"));
		register_tool(tools, "set_timeout",
			"Set the timeout (in seconds) for MCP operations that run on the FRED2 UI thread. Range: 1-300 seconds. Default: 10.",
			props, req);
	}

	// Reference/discovery tools (ships, weapons, species, SEXPs, intel)
	mcp_register_reference_tools(tools);

	// Mission CRUD tools (messages, etc.)
	mcp_register_mission_tools(tools);

	json_object_set_new(result, "tools", tools);

	return result;
}

// Timeout for PostMessage + WaitForSingleObject, in milliseconds
static std::atomic<DWORD> mcp_tool_timeout_ms{10000};  // default 10 seconds

// Clean up a heap-allocated McpToolRequest (event handle, json, struct)
static void mcp_cleanup_request(McpToolRequest *req)
{
	CloseHandle(req->completion_event);
	if (req->input_json)
		json_decref(req->input_json);
	if (req->result_json)
		json_decref(req->result_json);
	delete req;
}

// Wait for a previously posted McpToolRequest to complete, then extract and return the result.
// Handles timeout and reference-counted cleanup in both the normal and timeout paths.
static json_t *mcp_wait_for_result(McpToolRequest *req)
{
	DWORD timeout_ms = mcp_tool_timeout_ms.load(std::memory_order_relaxed);
	DWORD wait_result = WaitForSingleObject(req->completion_event, timeout_ms);

	if (wait_result == WAIT_OBJECT_0) {
		// Normal completion — extract result before releasing
		json_t *result;
		if (req->result_json) {
			result = req->result_json;
			req->result_json = nullptr;  // take ownership
		} else {
			result = make_tool_result(req->result_message, !req->success);
		}

		// Release our reference; if handler already released, we clean up
		if (req->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
			mcp_cleanup_request(req);

		return result;
	}

	// Timeout — release our reference
	if (req->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
		// Handler already finished between timeout and our fetch_sub
		mcp_cleanup_request(req);
	}
	// else: handler still running and will clean up when it finishes

	char timeout_msg[256];
	snprintf(timeout_msg, sizeof(timeout_msg),
		"Operation timed out (%us). A modal dialog may be blocking the FRED2 UI. "
		"Use get_ui_status to check.", timeout_ms / 1000);
	return make_tool_result(timeout_msg, true);
}

// Marshal a tool call to the main MFC thread with a configurable timeout.
// Uses PostMessage so the mongoose thread is not blocked indefinitely
// if the UI thread handler triggers a modal dialog.
json_t *mcp_execute_on_main_thread(McpToolId tool, const char *param)
{
	if (!Fred_main_wnd || !Fred_main_wnd->m_hWnd)
		return make_tool_result("FRED2 main window is not available", true);

	// Heap-allocate because the request may outlive this stack frame on timeout
	auto *req = new McpToolRequest();
	req->tool = tool;
	strncpy(req->filepath, param, sizeof(req->filepath) - 1);
	req->filepath[sizeof(req->filepath) - 1] = '\0';
	req->input_json = nullptr;
	req->success = false;
	req->result_message[0] = '\0';
	req->result_json = nullptr;
	req->completion_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	req->refcount.store(2, std::memory_order_relaxed);

	// Normalize path separators for the FreeSpace engine
	for (char *p = req->filepath; *p; ++p) {
		if (*p == '/' || *p == '\\')
			*p = DIR_SEPARATOR_CHAR;
	}

	if (!::PostMessage(Fred_main_wnd->m_hWnd, WM_MCP_TOOL_CALL, 0, (LPARAM)req)) {
		// PostMessage failed — we are the sole owner
		mcp_cleanup_request(req);
		return make_tool_result("Failed to post tool call to FRED2 main thread", true);
	}

	return mcp_wait_for_result(req);
}

// Called by the UI thread handler after filling results.
// Signals the completion event, then releases the handler's reference.
void mcp_signal_completion(McpToolRequest *req)
{
	// Signal first so the caller can wake up before we decrement
	SetEvent(req->completion_event);

	// Release handler's reference; if caller already timed out, we clean up
	if (req->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1)
		mcp_cleanup_request(req);
}

// Overload for MISSION_TOOL: passes tool_name in filepath (no path normalization)
// and structured JSON arguments via input_json.
json_t *mcp_execute_on_main_thread(McpToolId tool, const char *tool_name, json_t *input_json)
{
	if (!Fred_main_wnd || !Fred_main_wnd->m_hWnd)
		return make_tool_result("FRED2 main window is not available", true);

	auto *req = new McpToolRequest();
	req->tool = tool;
	strncpy(req->filepath, tool_name, sizeof(req->filepath) - 1);
	req->filepath[sizeof(req->filepath) - 1] = '\0';
	// No path separator normalization — filepath holds a tool name, not a path
	req->input_json = input_json ? json_incref(input_json) : nullptr;
	req->success = false;
	req->result_message[0] = '\0';
	req->result_json = nullptr;
	req->completion_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	req->refcount.store(2, std::memory_order_relaxed);

	if (!::PostMessage(Fred_main_wnd->m_hWnd, WM_MCP_TOOL_CALL, 0, (LPARAM)req)) {
		mcp_cleanup_request(req);
		return make_tool_result("Failed to post tool call to FRED2 main thread", true);
	}

	return mcp_wait_for_result(req);
}

static json_t *handle_tools_call(json_t *params)
{
	const char *tool_name = get_optional_string(params, "name");

	if (!tool_name)
		return nullptr;  // caller will send error

	if (strcmp(tool_name, "get_server_info") == 0) {
		SCP_string server_info = "FRED2 MCP Server is running. ";
		if (!mcp_fred_ready.load())
			server_info += "FRED2 is initializing. ";
		server_info += "Use get_mod_info for mod details, get_mission_info for mission details, and get_ui_status for UI state.";
		return mcp_execute_on_main_thread(McpToolId::GET_SERVER_INFO, server_info.c_str());
	}

	if (strcmp(tool_name, "get_timeout") == 0) {
		char buf[64];
		snprintf(buf, sizeof(buf), "timeout_seconds: %u", mcp_tool_timeout_ms.load(std::memory_order_relaxed) / 1000);
		return make_tool_result(buf);
	}

	if (strcmp(tool_name, "set_timeout") == 0) {
		json_t *arguments = json_object_get(params, "arguments");
		json_t *err = nullptr;
		int seconds;
		if (!get_required_integer(arguments, "seconds", &err, &seconds))
			return err;
		if (seconds < 1 || seconds > 300)
			return make_tool_result("Timeout must be between 1 and 300 seconds", true);

		mcp_tool_timeout_ms.store((DWORD)(seconds * 1000), std::memory_order_relaxed);

		char buf[64];
		snprintf(buf, sizeof(buf), "timeout_seconds: %d", seconds);
		return make_tool_result(buf);
	}

	// All tools below require FRED2 to be fully initialized
	if (!mcp_fred_ready.load())
		return make_tool_result("FRED2 is still initializing. Please wait and try again.", true);

	if (strcmp(tool_name, "load_mission") == 0 ||
		strcmp(tool_name, "save_mission") == 0)
	{
		// Extract filepath from arguments
		json_t *arguments = json_object_get(params, "arguments");
		json_t *err = nullptr;
		const char *filepath = get_required_string(arguments, "filepath", &err);
		if (!filepath) return err;

		McpToolId tool;
		if (strcmp(tool_name, "load_mission") == 0)
			tool = McpToolId::LOAD_MISSION;
		else
			tool = McpToolId::SAVE_MISSION;

		return mcp_execute_on_main_thread(tool, filepath);
	}

	if (strcmp(tool_name, "new_mission") == 0) {
		return mcp_execute_on_main_thread(McpToolId::NEW_MISSION, "");
	}

	if (strcmp(tool_name, "get_mission_info") == 0) {
		return mcp_execute_on_main_thread(McpToolId::GET_MISSION_INFO, "");
	}

	if (strcmp(tool_name, "get_ui_status") == 0) {
		return mcp_execute_on_main_thread(McpToolId::GET_UI_STATUS, "");
	}

	// Try mission CRUD tools (marshaled to main thread)
	{
		json_t *arguments = json_object_get(params, "arguments");
		json_t *mission_result = mcp_route_mission_tool(tool_name, arguments);
		if (mission_result)
			return mission_result;
	}

	// Try reference/discovery tools
	{
		json_t *arguments = json_object_get(params, "arguments");
		json_t *ref_result = mcp_handle_reference_tool(tool_name, arguments);
		if (ref_result)
			return ref_result;
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

static const size_t MAX_POST_BODY = 256 * 1024;  // 256 KB

static void handle_mcp_post(struct mg_connection *conn)
{
	// Read POST body dynamically — CRUD tools may send larger payloads
	SCP_string body;
	char chunk[4096];
	int n;
	while ((n = mg_read(conn, chunk, sizeof(chunk))) > 0) {
		body.append(chunk, n);
		if (body.size() > MAX_POST_BODY) {
			json_t *resp = make_error_response(nullptr, -32700, "Request body too large");
			send_json_response(conn, resp);
			json_decref(resp);
			return;
		}
	}
	if (body.empty()) {
		json_t *resp = make_error_response(nullptr, -32700, "Empty request body");
		send_json_response(conn, resp);
		json_decref(resp);
		return;
	}

	// Parse JSON
	json_error_t error;
	json_t *request = json_loads(body.c_str(), 0, &error);
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

	char listen_addr[32];
	snprintf(listen_addr, sizeof(listen_addr), "127.0.0.1:%d", Mcp_server_port);

	const char *options[] = {
		"listening_ports", listen_addr,
		"num_threads", "2",
		nullptr
	};

	mcp_ctx = mg_start(mcp_request_handler, nullptr, options);
	mprintf(("MCP server listening on 127.0.0.1:%d\n", Mcp_server_port));
}

void mcp_server_stop()
{
	if (mcp_ctx != nullptr) {
		mg_stop(mcp_ctx);
		mcp_ctx = nullptr;
	}
	mcp_reference_tools_cleanup();
}

bool mcp_server_is_running()
{
	return mcp_ctx != nullptr;
}
