#ifndef _MCP_SEXP_H
#define _MCP_SEXP_H

#include <jansson.h>
#include "mcp_tool_registry.h"
#include "parse/sexp.h"   // for sexp_opr_t used by check_sexp_formula

class McpErrorSink;
struct McpToolRequest;

// SEXP tools (sexp_to_text, text_to_sexp, get_sexp_node, walk_sexp_tree,
// find_sexp_text, detach/attach/move/swap/create/update_sexp_node,
// get_sexp_formula_info, and the five sexp_variable tools).
extern const McpToolDef mcp_sexp_tool_defs[];
extern const size_t mcp_sexp_tool_def_count;

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
int parse_sexp_text(const char *text, const char *source);

#endif // _MCP_SEXP_H
