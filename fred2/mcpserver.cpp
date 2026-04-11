#include "stdafx.h"
#include "mcpserver.h"
#include "mcp_json.h"
#include "mcp_app.h"
#include "mcp_reference_tools.h"
#include "mcp_mission_tools.h"
#include "mongoose.h"

#include <jansson.h>
#include <cstring>
#include <thread>

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
	if (body == nullptr) {
		const char *fallback = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"JSON serialization failed\"}}";
		mg_printf(conn,
			"HTTP/1.1 500 Internal Server Error\r\n"
			"Content-Type: application/json\r\n"
			"Connection: close\r\n"
			"Content-Length: %d\r\n"
			"\r\n%s",
			(int)strlen(fallback), fallback);
		return;
	}
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

static json_t *handle_initialize(json_t * /*params*/, int &error_code, SCP_string &error_msg)
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

static json_t *handle_tools_list(json_t * /*params*/, int &error_code, SCP_string &error_msg)
{
	json_t *result = json_object();
	json_t *tools = json_array();

	// App-level tools (server info, UI status, timeout, mission lifecycle)
	mcp_register_app_tools(tools);

	// Reference/discovery tools (ships, weapons, species, SEXPs, intel)
	mcp_register_reference_tools(tools);

	// Mission CRUD tools (messages, etc.)
	mcp_register_mission_tools(tools);

	json_object_set_new(result, "tools", tools);

	return result;
}

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
	DWORD timeout_ms = mcp_get_tool_timeout_ms();
	DWORD wait_result = WaitForSingleObject(req->completion_event, timeout_ms);

	if (wait_result == WAIT_OBJECT_0) {
		// Normal completion — extract result before releasing
		json_t *result;
		if (req->result_json) {
			result = req->result_json;
			req->result_json = nullptr;  // take ownership
		} else {
			result = make_tool_result(req->result_message.c_str(), !req->success);
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

	return make_tool_result(true,
		"Operation timed out (%us). A modal dialog may be blocking the FRED2 UI. "
		"Use get_ui_status to check.", timeout_ms / 1000);
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
	req->result_json = nullptr;
	req->completion_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!req->completion_event) {
		delete req;
		return make_tool_result("Failed to create completion event", true);
	}
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
	req->result_json = nullptr;
	req->completion_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!req->completion_event) {
		delete req;
		return make_tool_result("Failed to create completion event", true);
	}
	req->refcount.store(2, std::memory_order_relaxed);

	if (!::PostMessage(Fred_main_wnd->m_hWnd, WM_MCP_TOOL_CALL, 0, (LPARAM)req)) {
		mcp_cleanup_request(req);
		return make_tool_result("Failed to post tool call to FRED2 main thread", true);
	}

	return mcp_wait_for_result(req);
}

static json_t *handle_tools_call(json_t *params, int &error_code, SCP_string &error_msg)
{
	const char *tool_name;
	{
		json_t *err = nullptr;
		McpErrorSink sink(&err);
		tool_name = get_optional_string(params, "name", sink);
		if (err) {
			error_code = -32602;
			error_msg = "Invalid params";
			json_decref(err);
			return nullptr;
		}
	}

	if (!tool_name) {
		error_code = -32602;
		error_msg = "tool_name must be specified";
		return nullptr;
	}

	// Try app-level tools (timeout/server info/UI status/mission lifecycle)
	{
		json_t *app_result = mcp_route_app_tool(tool_name, params);
		if (app_result)
			return app_result;
	}

	// All tools below require FRED2 to be fully initialized
	if (!mcp_fred_ready.load())
		return make_tool_result("FRED2 is still initializing. Please wait and try again.", true);

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
	int error_code = 0;
	SCP_string error_msg;

	if (strcmp(method, "initialize") == 0) {
		result = handle_initialize(params, error_code, error_msg);
	} else if (strcmp(method, "tools/list") == 0) {
		result = handle_tools_list(params, error_code, error_msg);
	} else if (strcmp(method, "tools/call") == 0) {
		result = handle_tools_call(params, error_code, error_msg);
	} else {
		error_code = -32601;
		error_msg = "Method not found";
	}

	if (result && (error_code == 0) && (error_msg.empty())) {
		resp = make_response(id, result);
	} else {
		if (result)
			json_decref(result);
		if (error_msg.empty()) {
			error_msg = "Unknown error";
		} else if (error_code == 0) {
			error_code = -32000;	// server error
		}
		resp = make_error_response(id, error_code, error_msg.c_str());
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
		if (strcmp(info->uri, "/") == 0 || strcmp(info->uri, "/mcp") == 0) {
			serve_html_status(conn);
			return (void *)"";
		}
		// Fall through to 404 for other GET paths
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

	SCP_string listen_addr;
	sprintf(listen_addr, "127.0.0.1:%d", Mcp_server_port);

	const char *options[] = {
		"listening_ports", listen_addr.c_str(),
		"num_threads", "2",
		nullptr
	};

	mcp_ctx = mg_start(mcp_request_handler, nullptr, options);
	if (mcp_ctx == nullptr) {
		Warning(LOCATION, "MCP server failed to start on 127.0.0.1:%d (port may be in use)", Mcp_server_port);
		return;
	}
	mprintf(("MCP server listening on 127.0.0.1:%d\n", Mcp_server_port));
	mcp_fred_ready.store(true);

	mcp_reference_tools_init();
}

void mcp_server_stop()
{
	if (mcp_ctx == nullptr) {
		mcp_reference_tools_cleanup();
		return;
	}

	// Reject new tool calls immediately
	mcp_fred_ready.store(false);

	// mg_stop() blocks until all worker threads exit, but a worker may be
	// blocked in WaitForSingleObject waiting for the main thread to process
	// its WM_MCP_TOOL_CALL.  To avoid a bounded deadlock (up to the tool
	// timeout), run mg_stop on a background thread and pump messages here.
	struct mg_context *ctx = mcp_ctx;
	mcp_ctx = nullptr;

	std::atomic<bool> stop_done{false};
	std::thread stop_thread([ctx, &stop_done]() {
		mg_stop(ctx);
		stop_done.store(true, std::memory_order_release);
	});

	// Pump messages so any in-flight WM_MCP_TOOL_CALL requests get processed,
	// allowing their worker threads to unblock and exit.
	while (!stop_done.load(std::memory_order_acquire)) {
		DWORD result = MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
		if (result == WAIT_OBJECT_0) {
			MSG msg;
			while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
	}

	stop_thread.join();
	mcp_reference_tools_cleanup();
}

bool mcp_server_is_running()
{
	return mcp_ctx != nullptr;
}
