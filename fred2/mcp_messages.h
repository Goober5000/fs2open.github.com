#ifndef _MCP_MESSAGES_H
#define _MCP_MESSAGES_H

#include <jansson.h>
#include "mcp_tool_registry.h"

struct McpToolRequest;

// Message tools (list/get/create/update/delete/move/swap).
extern const McpToolDef mcp_message_tool_defs[];
extern const size_t mcp_message_tool_def_count;

#endif // _MCP_MESSAGES_H
