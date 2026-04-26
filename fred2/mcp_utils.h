#ifndef _MCP_UTILS_H
#define _MCP_UTILS_H

#include <algorithm>
#include <utility>

#include "globalincs/vmallocator.h"
#include "parse/sexp.h"

// ---------------------------------------------------------------------------
// Fuzzy search
// ---------------------------------------------------------------------------

// Compute the fuzzy match cost of search_str against candidate.
// Returns SCP_string::npos if no match within threshold.
size_t fuzzy_match_cost(const char *candidate, const SCP_string &search_str, size_t max_length);

// Fuzzy-search a pre-filtered candidate list and return matches sorted by cost.
// candidates: (original_index, candidate_name) pairs.
// max_name_length: pass 0 to auto-compute from candidates.
// When search is empty, all candidates pass with cost 0 (original order preserved).
SCP_vector<std::pair<size_t, size_t>> fuzzy_search_and_sort(
	const SCP_vector<std::pair<size_t, const char *>> &candidates,
	const char *search, size_t max_name_length = 0);

// Overload with custom cost function for multi-field or non-standard matching.
// get_cost(index) should return fuzzy cost or SCP_string::npos to skip.
// When search is empty, all indices [0, count) pass with cost 0.
template <typename GetCost>
SCP_vector<std::pair<size_t, size_t>> fuzzy_search_and_sort(
	size_t count, const char *search, GetCost get_cost)
{
	SCP_vector<std::pair<size_t, size_t>> matches;
	bool has_search = search && search[0];

	if (has_search) {
		for (size_t i = 0; i < count; i++) {
			size_t cost = get_cost(i);
			if (cost != SCP_string::npos)
				matches.emplace_back(i, cost);
		}
		std::sort(matches.begin(), matches.end(),
			[](const auto &a, const auto &b) { return a.second < b.second; });
	} else {
		for (size_t i = 0; i < count; i++)
			matches.emplace_back(i, 0);
	}

	return matches;
}

// ---------------------------------------------------------------------------
// SEXPs
// ---------------------------------------------------------------------------

// Metadata for a SEXP argument type (OPF_*)
struct opf_type_info {
	const char *name;        // string from opf_to_name
	const char *description; // what this type accepts
	const char **accepts;    // null-terminated list of value categories
	const char *notes;       // optional extra context (may be nullptr)
};

extern const opf_type_info Opf_type_info[];

// Metadata for a SEXP return type (OPR_*)
struct opr_type_info {
	sexp_opr_t opr_value;
	const char *name;
	const char *description;
	const char **compatible_with; // null-terminated list of argument type names this can satisfy
};

extern const opr_type_info Opr_type_info[];

// Look up the human-readable name for an OPR_* return type constant.
const char *opr_to_name(int opr_value);

// Look up the human-readable name for an OPF_* argument type constant.
const char *opf_to_name(int opf);

// Reverse mapping: MCP name → OPF_* constant.
// Returns -1 if the name is not recognized.
int opf_from_name(const char *name);

const char *mcp_get_sexp_category_name(int category_id);

const char *mcp_get_sexp_category_description(int category_id);


#endif // _MCP_UTILS_H
