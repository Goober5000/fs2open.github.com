#ifndef _MCP_REFERENCE_TOOLS_H
#define _MCP_REFERENCE_TOOLS_H

#include <jansson.h>

// Append reference/discovery tool schemas to the tools array used by tools/list.
void mcp_register_reference_tools(json_t *tools);

// Try to handle a reference tool call.  Returns a json_t* result if the tool
// name matched one of the reference tools, or nullptr if the name is not
// recognized (so the caller can fall through to other handlers).
json_t *mcp_handle_reference_tool(const char *tool_name, json_t *arguments);

// Release cached model detail results.
// Must be called after all mongoose threads have stopped (i.e., after mg_stop()).
void mcp_reference_tools_cleanup();

// Look up the human-readable name for an OPR_* return type constant.
const char *get_opr_type_name(int opr_value);

// Look up the human-readable name for an OPF_* argument type constant.
const char *opf_to_string(int opf);

#endif // _MCP_REFERENCE_TOOLS_H
