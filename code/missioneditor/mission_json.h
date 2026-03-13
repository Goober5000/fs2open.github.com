#ifndef MISSION_JSON_H
#define MISSION_JSON_H
#pragma once

#include "globalincs/pstypes.h"
#include "math/vecmat.h"

struct mission;
struct json_t;
struct FredSaveConfig;

namespace mission_json {

// JSON format version -- increment when the schema changes in an incompatible way
constexpr int FORMAT_VERSION = 1;

const auto MINIMUM_FSO_VERSION = gameversion::version(25, 0);

// Save the current in-memory mission state to a JSON file.
// Returns 0 on success, negative on error.
int save_mission(const char* pathname, const FredSaveConfig& config);

// Load a mission from a JSON file into the provided mission struct.
// flags: same MPF_* flags used by parse_mission().
// Returns true on success.
bool load(const char* pathname, mission* pm, int flags);

// --- JSON helpers for common FSO types ---

json_t* vec3d_to_json(const vec3d& v);
vec3d   json_to_vec3d(const json_t* obj);

json_t* matrix_to_json(const matrix& m);
matrix  json_to_matrix(const json_t* obj);

json_t* angles_to_json(const angles& a);
angles  json_to_angles(const json_t* obj);

// Convert a SEXP tree rooted at node_index to its text form and return as json_string.
// Returns json_null() if node_index < 0.
json_t* sexp_to_json(int node_index);

// Parse a SEXP from a JSON string value and return the root node index.
// Returns -1 on failure or if the json value is null.
int json_to_sexp(const json_t* val);

// Helpers for reading JSON values with defaults
int         json_get_int(const json_t* obj, const char* key, int default_val = 0);
float       json_get_float(const json_t* obj, const char* key, float default_val = 0.0f);
const char* json_get_string(const json_t* obj, const char* key, const char* default_val = "");
bool        json_get_bool(const json_t* obj, const char* key, bool default_val = false);

} // namespace mission_json

#endif // MISSION_JSON_H
