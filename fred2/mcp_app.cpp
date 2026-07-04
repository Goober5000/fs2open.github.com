#include "stdafx.h"
#include "mcp_app.h"
#include "mcpserver.h"
#include "mcp_json.h"
#include "mcp_mission_tools.h"
#include "mcp_tool_registry.h"

#include "FRED.h"
#include "MainFrm.h"
#include "FREDDoc.h"
#include "FREDView.h"
#include "Management.h"
#include "fredrender.h"
#include "missioneditor/missionsave.h"

#include "mission/missionparse.h"
#include "mod_table/mod_table.h"

#include <jansson.h>
#include <atomic>
#include <cstring>

// ---------------------------------------------------------------------------
// Timeout state (shared with mcpserver.cpp via mcp_get_tool_timeout_ms)
// ---------------------------------------------------------------------------

static std::atomic<DWORD> s_mcp_tool_timeout_ms{10000};  // default 10 seconds

DWORD mcp_get_tool_timeout_ms()
{
	return s_mcp_tool_timeout_ms.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Main-thread helpers (moved from mainfrm.cpp)
// ---------------------------------------------------------------------------

static void handle_get_server_info(json_t * /*input*/, McpToolRequest *req)
{
	json_t *info = json_object();
	json_object_set_new(info, "status", json_string("running"));
	json_object_set_new(info, "hint", json_string("Use get_mod_info for mod details, get_mission_info for mission details, "
		"and get_ui_status for UI state. Use list_reference_notes and get_reference_note for domain-specific knowledge."));

	// Mission context (if loaded)
	if (Mission_filename[0] != '\0') {
		SCP_string full_name;
		sprintf(full_name, "%s%s", Mission_filename, FS_MISSION_FILE_EXT);
		json_object_set_new(info, "mission_filename", json_safe_string(full_name.c_str()));
		json_object_set_new(info, "mission_title", json_safe_string(The_mission.name.c_str()));
	}

	// Mod context (if available)
	set_optional_string(info, "mod_title", Mod_title.c_str(), true);
	set_optional_string(info, "mod_version", Mod_version.c_str(), true);
	json_object_set_new(info, "supports_unicode", json_boolean(Unicode_text_mode));

	req->result_json = make_json_tool_result(info);
	req->success = true;
}

static void handle_new_mission(json_t * /*input*/, McpToolRequest *req)
{
	if (!validate_no_dialogs_open(req->result_message)) {
		req->success = false;
		return;
	}

	create_new_mission();
	if (Fred_view_wnd)
		Fred_view_wnd->Invalidate();
	FREDDoc_ptr->SetTitle("Untitled");
	FREDDoc_ptr->SetModifiedFlag(FALSE);
	req->success = true;
	req->result_message = "New empty mission created";
}

static const char *set_mission_filename_from_path(const char *pathname)
{
	auto sep_ch = strrchr(pathname, '\\');
	if (!sep_ch)
		sep_ch = strrchr(pathname, '/');
	auto filename = (sep_ch != nullptr) ? (sep_ch + 1) : pathname;
	auto len = strlen(filename);

	// drop extension
	auto ext_ch = strrchr(filename, '.');
	if (ext_ch != nullptr)
		len = ext_ch - filename;
	if (len >= 80)
		len = 79;
	strncpy(Mission_filename, filename, len);
	Mission_filename[len] = 0;

	return (ext_ch != nullptr) ? ext_ch : "";
}

static void handle_load_mission(McpToolRequest *req)
{
	clean_up_selections();
	auto ext = set_mission_filename_from_path(req->filepath);

	if (FREDDoc_ptr->load_mission(req->filepath)) {
		CString title;
		title.Format("%s%s", Mission_filename, ext);
		FREDDoc_ptr->autosave("nothing");
		FREDDoc_ptr->SetTitle((LPCTSTR)title);
		FREDDoc_ptr->SetModifiedFlag(FALSE);
		Undo_count = 0;
		if (Fred_view_wnd)
			Fred_view_wnd->Invalidate();
		req->success = true;
		sprintf(req->result_message,
			"Mission loaded successfully: %s", Mission_filename);
	} else {
		Mission_filename[0] = '\0';
		req->success = false;
		sprintf(req->result_message,
			"Failed to load mission: %s", req->filepath);
	}
}

static void handle_save_mission(McpToolRequest *req, MissionFormat format)
{
	auto ext = set_mission_filename_from_path(req->filepath);

	Fred_mission_save save;
	save.set_save_format(format);
	save.set_always_save_display_names(Always_save_display_names);
	save.set_view_pos(view_pos);
	save.set_view_orient(view_orient);
	save.set_fred_alt_names(Fred_alt_names);
	save.set_fred_callsigns(Fred_callsigns);

	if (save.save_mission_file(req->filepath) == 0) {
		CString title;
		title.Format("%s%s", Mission_filename, ext);
		FREDDoc_ptr->SetTitle((LPCTSTR)title);
		FREDDoc_ptr->SetModifiedFlag(FALSE);
		req->success = true;
		sprintf(req->result_message,
			"Mission saved successfully: %s", req->filepath);
	} else {
		req->success = false;
		sprintf(req->result_message,
			"Failed to save mission: %s", req->filepath);
	}
}

// Shared front half of the load/save tool handlers: extract "filepath" from
// input into req->filepath with separator normalization, and check that no
// editor dialogs are open.  Returns false with the error already set in req.
static bool extract_mission_filepath(json_t *input, McpToolRequest *req)
{
	McpErrorSink sink(req);
	auto filepath = get_required_string(input, "filepath", sink, true, MAX_PATH_LEN - 1);
	if (!filepath)
		return false;

	strncpy(req->filepath, filepath, sizeof(req->filepath) - 1);
	req->filepath[sizeof(req->filepath) - 1] = '\0';
	for (char *p = req->filepath; *p; ++p) {
		if (*p == '/' || *p == '\\')
			*p = DIR_SEPARATOR_CHAR;
	}

	if (!validate_no_dialogs_open(req->result_message)) {
		req->success = false;
		return false;
	}
	return true;
}

static void handle_load_mission_tool(json_t *input, McpToolRequest *req)
{
	if (!extract_mission_filepath(input, req))
		return;
	handle_load_mission(req);
}

static void handle_save_mission_tool(json_t *input, McpToolRequest *req)
{
	if (!extract_mission_filepath(input, req))
		return;
	handle_save_mission(req, MissionFormat::STANDARD);
}

// ---------------------------------------------------------------------------
// Worker-thread handlers (run directly on the mongoose thread; must work
// even before mcp_fred_ready)
// ---------------------------------------------------------------------------

static json_t *worker_get_timeout(json_t * /*arguments*/)
{
	DWORD seconds = s_mcp_tool_timeout_ms.load(std::memory_order_relaxed) / 1000;
	json_t *result = make_tool_result(false, "timeout (seconds): %u", seconds);
	json_t *sc = json_object();
	json_object_set_new(sc, "seconds", json_integer(seconds));
	json_object_set_new(result, "structuredContent", sc);
	return result;
}

static json_t *worker_set_timeout(json_t *arguments)
{
	json_t *err = nullptr;
	McpErrorSink sink(&err);
	auto seconds = get_required_integer(arguments, "seconds", sink);
	if (!seconds.has_value())
		return err;
	if (!check_int_range(*seconds, 1, 300, "seconds", sink))
		return err;

	s_mcp_tool_timeout_ms.store((DWORD)(*seconds * 1000), std::memory_order_relaxed);
	json_t *result = make_tool_result(false, "timeout (seconds): %d", *seconds);
	json_t *sc = json_object();
	json_object_set_new(sc, "seconds", json_integer(*seconds));
	json_object_set_new(result, "structuredContent", sc);
	return result;
}

static void handle_get_ui_status(json_t * /*input*/, McpToolRequest *req)
{
	SCP_string buf;

	// Check if a modal dialog is blocking the main window
	bool modal_active = !Fred_main_wnd->IsWindowEnabled();
	sprintf_concat(buf, "modal_dialog_active: %s\n", modal_active ? "true" : "false");

	if (!modal_active) {
		buf += "open_editors:";

		bool any_open = false;
		for (size_t i = 0; i < g_editor_info_count; ++i) {
			auto &info = g_editor_info[i];
			auto wnd = info.getCWndPtr();
			if (wnd && wnd->IsWindowVisible()) {
				sprintf_concat(buf, " %s,", info.editor_name);
				any_open = true;
			}
		}

		if (!any_open) {
			sprintf_concat(buf, " none");
		} else {
			// Remove trailing comma
			if (!buf.empty() && buf.back() == ',')
				buf.pop_back();
		}
	}

	req->success = true;
	req->result_message = std::move(buf);
}

bool validate_single_dialog(const char *items_to_modify, const char *dialog_key, SCP_string &error_msg)
{
	for (size_t i = 0; i < g_editor_info_count; ++i) {
		auto &info = g_editor_info[i];
		if (dialog_key && info.editor_key && !stricmp(dialog_key, info.editor_key)) {
			auto wnd = info.getCWndPtr();
			if (wnd && wnd->IsWindowVisible()) {
				sprintf(error_msg, "Cannot work with %s while the %s is open. "
					"Close it first, or use get_ui_status to check which editors are open.", items_to_modify, info.editor_name);
				return false;
			}
			return true;
		}
	}
	Assertion(false, "dialog key '%s' not found!", dialog_key ? dialog_key : "<nullptr>");
	return false;
}

bool validate_no_dialogs_open(SCP_string &error_msg)
{
	SCP_string open_list;
	for (size_t i = 0; i < g_editor_info_count; ++i) {
		auto &info = g_editor_info[i];
		auto wnd = info.getCWndPtr();
		if (wnd && wnd->IsWindowVisible()) {
			if (!open_list.empty())
				open_list += ", ";
			open_list += info.editor_name;
		}
	}
	if (!open_list.empty()) {
		sprintf(error_msg, "Cannot perform this operation while editor dialogs are open: %s. "
			"Close them first, or use get_ui_status to check which editors are open.", open_list.c_str());
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static void register_get_server_info(json_t *tools)
{
	register_tool(tools, "get_server_info",
		"Returns information about the running FRED2 instance, including whether Unicode is supported, and the currently loaded mission and active mod if applicable",
		nullptr);
}

static void register_new_mission(json_t *tools)
{
	register_tool(tools, "new_mission",
		"Create a new empty mission, replacing any currently loaded mission",
		nullptr);
}

static void register_load_mission(json_t *tools)
{
	register_tool_with_required_string(tools, "load_mission",
		"Load a mission file into FRED2",
		"filepath", "Absolute path to the mission file (.fs2 extension)");
}

static void register_save_mission(json_t *tools)
{
	register_tool_with_required_string(tools, "save_mission",
		"Save the current mission in standard (.fs2) format",
		"filepath", "Absolute path to save the mission file to");
}

static void register_get_ui_status(json_t *tools)
{
	register_tool(tools, "get_ui_status",
		"Returns the state of FRED2's UI windows: whether a modal dialog is blocking, and which modeless editor windows are open",
		nullptr);
}

static void register_get_timeout(json_t *tools)
{
	register_tool(tools, "get_timeout",
		"Returns the current timeout (in seconds) for MCP operations that run on the FRED2 UI thread",
		nullptr);
}

static void register_set_timeout(json_t *tools)
{
	json_t *props = json_object();
	add_integer_prop(props, "seconds", "Timeout in seconds (1-300)");
	json_t *req = json_array();
	json_array_append_new(req, json_string("seconds"));
	register_tool(tools, "set_timeout",
		"Set the timeout (in seconds) for MCP operations that run on the FRED2 UI thread. Range: 1-300 seconds. Default: 10.",
		props, req);
}

// ---------------------------------------------------------------------------
// Tool table
// ---------------------------------------------------------------------------

const McpToolDef mcp_app_tool_defs[] = {
	{ "get_server_info", register_get_server_info, nullptr,            handle_get_server_info,   false },
	{ "new_mission",     register_new_mission,     nullptr,            handle_new_mission,       false },
	{ "load_mission",    register_load_mission,    nullptr,            handle_load_mission_tool, false },
	{ "save_mission",    register_save_mission,    nullptr,            handle_save_mission_tool, false },
	{ "get_ui_status",   register_get_ui_status,   nullptr,            handle_get_ui_status,     false },
	{ "get_timeout",     register_get_timeout,     worker_get_timeout, nullptr,                  true  },
	{ "set_timeout",     register_set_timeout,     worker_set_timeout, nullptr,                  true  },
};
const size_t mcp_app_tool_def_count = sizeof(mcp_app_tool_defs) / sizeof(mcp_app_tool_defs[0]);
