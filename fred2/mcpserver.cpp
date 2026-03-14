#include "stdafx.h"
#include "mcpserver.h"
#include "mongoose.h"

static struct mg_context *mcp_ctx = nullptr;

static void *mcp_request_handler(enum mg_event event, struct mg_connection *conn)
{
	if (event == MG_NEW_REQUEST) {
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
			"<p>The server is running. MCP protocol support is not yet implemented.</p>\n"
			"</body>\n"
			"</html>\n");
		return (void *)"";  // non-NULL = request handled
	}

	return nullptr;
}

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
