#ifndef _MCP_SEXP_H
#define _MCP_SEXP_H

#include <jansson.h>
#include "parse/sexp.h"   // for sexp_opr_t used by check_sexp_formula

class McpErrorSink;
struct McpToolRequest;

// Append SEXP tool schemas (sexp_to_text, text_to_sexp, get_sexp_node,
// walk_sexp_tree, detach/create/update_sexp_node, get_sexp_formula_info,
// and the five sexp_variable tools) to the tools array.  Called from
// mcp_register_mission_tools in mcp_mission_tools.cpp.
void mcp_register_sexp_tools(json_t *tools);

// Try to dispatch a SEXP tool call on the main thread.  Returns true if the
// tool name matched and was handled; false to let the caller fall through.
// Called from mcp_handle_mission_tool.
bool mcp_handle_sexp_tool(const char *tool_name, json_t *input_json, McpToolRequest *req);

// Validate that `node` is a well-formed SEXP formula root of the expected
// return type.  Shared with non-SEXP handlers (create/update_event,
// create/update_goal, create/update_fiction_viewer_stage,
// create/update_debriefing_stage) that need to accept a formula root from
// the client and reject invalid ones.
bool check_sexp_formula(int node, sexp_opr_t expected_return_type, McpErrorSink &sink);

// Parse a SEXP expression from text and return the root node index, or a
// negative value on failure.  Shared with non-SEXP handlers that need to
// materialize a default formula (e.g. handle_create_event uses this to build
// "( when ( true ) ( do-nothing ) )").
int parse_sexp_text(const char *text);

#endif // _MCP_SEXP_H
