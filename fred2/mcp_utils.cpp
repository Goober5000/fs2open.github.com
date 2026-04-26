#include "stdafx.h"
#include "mcp_utils.h"

#include <algorithm>
#include <cstring>

#include "globalincs/utility.h"   // for stringcost, stringcost_tolower_equal

// Compute the fuzzy match cost of search_str against candidate.
// Returns SCP_string::npos if no match within threshold.
size_t fuzzy_match_cost(const char *candidate, const SCP_string &search_str, size_t max_length)
{
	SCP_string candidate_str(candidate);
	size_t threshold = max_length * max_length * 3;
	size_t cost = stringcost(candidate_str, search_str, max_length, stringcost_tolower_equal);
	if (cost == SCP_string::npos || cost >= threshold)
		return SCP_string::npos;
	return cost;
}

// Fuzzy-search a pre-filtered candidate list and return matches sorted by cost.
// candidates: (original_index, candidate_name) pairs.
// max_name_length: pass 0 to auto-compute from candidates.
// When search is empty, all candidates pass with cost 0 (original order preserved).
SCP_vector<std::pair<size_t, size_t>> fuzzy_search_and_sort(
	const SCP_vector<std::pair<size_t, const char *>> &candidates,
	const char *search, size_t max_name_length)
{
	SCP_vector<std::pair<size_t, size_t>> matches;
	bool has_search = search && search[0];

	if (has_search) {
		SCP_string search_str(search);
		if (max_name_length == 0) {
			for (const auto &c : candidates) {
				if (c.second) {
					size_t len = strlen(c.second);
					if (len > max_name_length)
						max_name_length = len;
				}
			}
		}
		for (const auto &c : candidates) {
			if (!c.second)
				continue;
			size_t cost = fuzzy_match_cost(c.second, search_str, max_name_length);
			if (cost != SCP_string::npos)
				matches.emplace_back(c.first, cost);
		}
		std::sort(matches.begin(), matches.end(),
			[](const auto &a, const auto &b) { return a.second < b.second; });
	} else {
		for (const auto &c : candidates)
			matches.emplace_back(c.first, 0);
	}

	return matches;
}
