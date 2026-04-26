#ifndef _MCP_UTILS_H
#define _MCP_UTILS_H

#include <algorithm>
#include <utility>

#include "globalincs/vmallocator.h"

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

#endif // _MCP_UTILS_H
