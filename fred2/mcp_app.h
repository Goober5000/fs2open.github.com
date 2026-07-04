#ifndef _MCP_APP_H
#define _MCP_APP_H

#include <jansson.h>
#include <windows.h>
#include "globalincs/pstypes.h"
#include "mcp_tool_registry.h"

struct McpToolRequest;

// App-level tools: get_server_info, get_ui_status, get_timeout, set_timeout,
// load_mission, save_mission, new_mission.
extern const McpToolDef mcp_app_tool_defs[];
extern const size_t mcp_app_tool_def_count;

// Current timeout (in milliseconds) for main-thread tool marshaling.
// Used by mcpserver.cpp's wait logic.
DWORD mcp_get_tool_timeout_ms();

// Returns false (with error_msg) if the specified modeless editor dialog
// (identified by its editor_key in g_editor_info) is currently visible.
// Used by validate_dialog_for_* helpers in the various tool units.
bool validate_single_dialog(const char *items_to_modify, const char *dialog_key, SCP_string &error_msg);

// Returns false (with error_msg) if any editor dialog is open.
// Used by LOAD_MISSION / SAVE_MISSION / NEW_MISSION to prevent data loss.
bool validate_no_dialogs_open(SCP_string &error_msg);

#endif // _MCP_APP_H
