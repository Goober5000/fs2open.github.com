#pragma once

#include "scripting_doc.h"

struct json_t;

namespace scripting {

void output_json_doc(const ScriptingDocumentation& doc, const SCP_string& filename);

// Build the scripting API documentation as a jansson JSON tree.
// Caller takes ownership and must call json_decref() when done.
json_t* build_json_doc(const ScriptingDocumentation& doc);

}
