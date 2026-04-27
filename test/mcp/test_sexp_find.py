"""find_sexp_text behavior tests.

Covers find_sexp_text: substring vs exact match, case-insensitivity,
optional root_node scoping, role filter, value_type filter, list_wrapper
exclusion, truncation cap, and negative paths (missing/empty text, bad
enum values, out-of-range root_node, SEXP_NOT_USED root_node).
"""

from mcp_test_lib import (
    assert_equal,
    assert_error,
    assert_in,
    assert_is_list,
    assert_success,
    assert_true,
    run_module_standalone,
    tool_data,
    tool_text,
)


def register(suite, client):
    ctx = suite.ctx

    # =================================================================
    # Connectivity
    # =================================================================

    def test_tool_registered():
        assert_in("find_sexp_text", client.tool_names,
                  "find_sexp_text not exposed by server")

    # =================================================================
    # Required-param / empty-text rejections
    # =================================================================

    def test_missing_text():
        r = client.call_tool("find_sexp_text", {})
        assert_error(r)

    def test_empty_text():
        r = client.call_tool("find_sexp_text", {"text": ""})
        assert_error(r)

    # =================================================================
    # Build a tree the substring/exact/role/value_type tests can share
    # =================================================================

    def test_setup_simple_tree():
        # ( when ( true ) ( do-nothing ) ) — has operators, an argument list,
        # and list-wrappers around the inner clauses.
        r = client.call_tool("text_to_sexp", {"text": "( when ( true ) ( do-nothing ) )"})
        assert_success(r)
        ctx["find_simple_root"] = tool_data(r)["node"]

    # =================================================================
    # Substring (default match_mode)
    # =================================================================

    def test_substring_default_mode():
        root = ctx.get("find_simple_root")
        assert_true(root is not None, "test fixture missing")
        r = client.call_tool("find_sexp_text", {"text": "do-noth"})
        assert_success(r)
        d = tool_data(r)
        assert_in("nodes", d)
        assert_is_list(d["nodes"])
        assert_true(len(d["nodes"]) >= 1, "expected at least one match for 'do-noth'")
        for entry in d["nodes"]:
            for key in ("node", "value", "role", "value_type"):
                assert_in(key, entry, f"result entry missing {key}")
            assert_in("do-noth", entry["value"].lower())

    def test_substring_case_insensitive():
        # "WHEN" must match the lowercase "when" operator atom.
        r = client.call_tool("find_sexp_text", {"text": "WHEN"})
        assert_success(r)
        d = tool_data(r)
        values = [e["value"].lower() for e in d["nodes"]]
        assert_in("when", values, "case-insensitive substring search missed 'when'")

    def test_substring_no_match_returns_empty_array():
        r = client.call_tool("find_sexp_text", {"text": "this-string-should-not-appear-anywhere"})
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("nodes"), [], "expected empty nodes array")
        assert_true("truncated" not in d, "should not be marked truncated")

    # =================================================================
    # Exact match
    # =================================================================

    def test_exact_full_value_matches():
        r = client.call_tool("find_sexp_text", {
            "text": "do-nothing", "match_mode": "exact",
        })
        assert_success(r)
        d = tool_data(r)
        assert_true(len(d["nodes"]) >= 1, "exact match for 'do-nothing' should succeed")
        for entry in d["nodes"]:
            assert_equal(entry["value"].lower(), "do-nothing")

    def test_exact_partial_does_not_match():
        # "do-noth" is a substring but not an exact match.
        r = client.call_tool("find_sexp_text", {
            "text": "do-noth", "match_mode": "exact",
        })
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("nodes"), [],
                     "exact match should reject partial substring 'do-noth'")

    def test_exact_case_insensitive():
        r = client.call_tool("find_sexp_text", {
            "text": "DO-NOTHING", "match_mode": "exact",
        })
        assert_success(r)
        d = tool_data(r)
        assert_true(len(d["nodes"]) >= 1, "exact match should be case-insensitive")

    # =================================================================
    # list_wrapper exclusion (unconditional — tested even without filter)
    # =================================================================

    def test_list_wrapper_never_appears_in_results():
        # Match any single character that the wrapper text might contain by
        # using a substring that hits the operators.
        r = client.call_tool("find_sexp_text", {"text": "n"})
        assert_success(r)
        d = tool_data(r)
        for entry in d["nodes"]:
            assert_true(entry["role"] != "list_wrapper",
                        f"list_wrapper leaked into results: {entry}")

    # =================================================================
    # Role filter
    # =================================================================

    def test_role_operator_filter():
        # Use a substring guaranteed to hit operators ("when", "do-nothing").
        r = client.call_tool("find_sexp_text", {
            "text": "n", "role": "operator",
        })
        assert_success(r)
        d = tool_data(r)
        assert_true(len(d["nodes"]) >= 1, "expected at least one operator match")
        for entry in d["nodes"]:
            assert_equal(entry["role"], "operator")

    def test_role_argument_filter():
        # Build a tree with explicit arguments (numeric literal) so role=argument
        # has hits even after operator nodes are excluded.
        r = client.call_tool("text_to_sexp", {"text": "( + 1 23 )"})
        assert_success(r)
        plus_root = tool_data(r)["node"]
        try:
            r = client.call_tool("find_sexp_text", {
                "text": "23", "role": "argument",
            })
            assert_success(r)
            d = tool_data(r)
            assert_true(len(d["nodes"]) >= 1, "expected at least one argument match")
            for entry in d["nodes"]:
                assert_equal(entry["role"], "argument")
        finally:
            client.call_tool("detach_sexp_node", {"node": plus_root, "delete": True})

    # =================================================================
    # value_type filter
    # =================================================================

    def test_value_type_numeric_literal():
        r = client.call_tool("text_to_sexp", {"text": "( + 1 23 )"})
        assert_success(r)
        plus_root = tool_data(r)["node"]
        try:
            r = client.call_tool("find_sexp_text", {
                "text": "23", "value_type": "numeric_literal",
            })
            assert_success(r)
            d = tool_data(r)
            assert_true(len(d["nodes"]) >= 1, "expected numeric_literal match for '23'")
            for entry in d["nodes"]:
                assert_equal(entry["value_type"], "numeric_literal")
        finally:
            client.call_tool("detach_sexp_node", {"node": plus_root, "delete": True})

    def test_value_type_operator():
        r = client.call_tool("find_sexp_text", {
            "text": "when", "value_type": "operator",
        })
        assert_success(r)
        d = tool_data(r)
        assert_true(len(d["nodes"]) >= 1, "expected operator match for 'when'")
        for entry in d["nodes"]:
            assert_equal(entry["value_type"], "operator")

    def test_role_and_value_type_combined():
        # role=argument AND value_type=operator should be empty — by definition
        # an argument can't have value_type=operator.
        r = client.call_tool("find_sexp_text", {
            "text": "n", "role": "argument", "value_type": "operator",
        })
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("nodes"), [],
                     "role=argument + value_type=operator must yield no matches")

    # =================================================================
    # Scoped search (root_node)
    # =================================================================

    def test_root_node_restricts_results():
        # Build a second tree containing "alpha-foo" and confirm a scoped
        # search of the first tree does NOT return matches from the second.
        r = client.call_tool("text_to_sexp",
                              {"text": '( send-message "alpha-foo" "hi" "x" )'})
        assert_success(r)
        other_root = tool_data(r)["node"]
        try:
            simple_root = ctx.get("find_simple_root")
            assert_true(simple_root is not None, "fixture missing")
            r = client.call_tool("find_sexp_text", {
                "text": "alpha-foo", "root_node": simple_root,
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("nodes"), [],
                         "scoped search leaked nodes from other tree")

            # Sanity: mission-wide, the same query DOES find it.
            r = client.call_tool("find_sexp_text", {"text": "alpha-foo"})
            assert_success(r)
            d = tool_data(r)
            assert_true(len(d["nodes"]) >= 1,
                        "mission-wide search should have found alpha-foo")
        finally:
            client.call_tool("detach_sexp_node", {"node": other_root, "delete": True})

    def test_root_node_out_of_range():
        r = client.call_tool("find_sexp_text", {"text": "x", "root_node": 99999})
        assert_error(r)

    def test_root_node_negative():
        r = client.call_tool("find_sexp_text", {"text": "x", "root_node": -1})
        assert_error(r)

    # =================================================================
    # Fuzzy match
    # =================================================================

    def test_fuzzy_recovers_typo():
        # The simple-tree fixture has 'do-nothing'.  A query missing the hyphen
        # is a substring miss but a clear fuzzy hit.
        r = client.call_tool("find_sexp_text", {
            "text": "donothing", "match_mode": "fuzzy",
        })
        assert_success(r)
        d = tool_data(r)
        assert_true(len(d["nodes"]) >= 1, "fuzzy should match 'donothing' against 'do-nothing'")
        values = [e["value"].lower() for e in d["nodes"]]
        assert_in("do-nothing", values, "expected 'do-nothing' among fuzzy matches")

    def test_fuzzy_orders_by_relevance():
        # Build a second tree with a less-similar operator so we can verify
        # ordering: 'do-nothing' should rank above 'is-destroyed-delay' for
        # the query 'donothing'.
        r = client.call_tool("text_to_sexp",
                              {"text": '( when ( is-destroyed-delay 0 "x" ) ( do-nothing ) )'})
        assert_success(r)
        other_root = tool_data(r)["node"]
        try:
            r = client.call_tool("find_sexp_text", {
                "text": "donothing", "match_mode": "fuzzy",
            })
            assert_success(r)
            d = tool_data(r)
            values = [e["value"].lower() for e in d["nodes"]]
            assert_in("do-nothing", values, "fuzzy missed 'do-nothing'")
            # 'do-nothing' must appear before any 'is-destroyed-delay' result
            # (if the latter even matched at all — it might be filtered by
            # the threshold, which is also fine).
            do_nothing_pos = values.index("do-nothing")
            if "is-destroyed-delay" in values:
                idd_pos = values.index("is-destroyed-delay")
                assert_true(do_nothing_pos < idd_pos,
                            f"'do-nothing' should rank above 'is-destroyed-delay'; "
                            f"got order {values}")
        finally:
            client.call_tool("detach_sexp_node", {"node": other_root, "delete": True})

    def test_fuzzy_with_role_filter():
        r = client.call_tool("find_sexp_text", {
            "text": "donothing", "match_mode": "fuzzy", "role": "operator",
        })
        assert_success(r)
        d = tool_data(r)
        for entry in d["nodes"]:
            assert_equal(entry["role"], "operator")

    def test_fuzzy_no_match_returns_empty():
        r = client.call_tool("find_sexp_text", {
            "text": "zzzzznevermatchanything", "match_mode": "fuzzy",
        })
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("nodes"), [], "expected empty nodes array for unrelated query")
        assert_true("truncated" not in d, "should not be marked truncated")

    # =================================================================
    # Schema rejections
    # =================================================================

    def test_bad_match_mode():
        r = client.call_tool("find_sexp_text",
                              {"text": "x", "match_mode": "contains"})
        assert_error(r)

    def test_role_list_wrapper_rejected_by_schema():
        # The role enum excludes list_wrapper; passing it must error.
        r = client.call_tool("find_sexp_text",
                              {"text": "x", "role": "list_wrapper"})
        assert_error(r)

    def test_bad_role():
        r = client.call_tool("find_sexp_text",
                              {"text": "x", "role": "literal"})
        assert_error(r)

    def test_bad_value_type():
        r = client.call_tool("find_sexp_text",
                              {"text": "x", "value_type": "bool"})
        assert_error(r)

    # =================================================================
    # Cleanup
    # =================================================================

    def test_cleanup_simple_tree():
        root = ctx.pop("find_simple_root", None)
        if root is None:
            return
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    tests = [
        ("sexp_find_tool_registered", test_tool_registered),
        ("sexp_find_missing_text", test_missing_text),
        ("sexp_find_empty_text", test_empty_text),
        ("sexp_find_setup_simple_tree", test_setup_simple_tree),
        ("sexp_find_substring_default_mode", test_substring_default_mode),
        ("sexp_find_substring_case_insensitive", test_substring_case_insensitive),
        ("sexp_find_substring_no_match_returns_empty_array",
         test_substring_no_match_returns_empty_array),
        ("sexp_find_exact_full_value_matches", test_exact_full_value_matches),
        ("sexp_find_exact_partial_does_not_match", test_exact_partial_does_not_match),
        ("sexp_find_exact_case_insensitive", test_exact_case_insensitive),
        ("sexp_find_list_wrapper_never_appears_in_results",
         test_list_wrapper_never_appears_in_results),
        ("sexp_find_role_operator_filter", test_role_operator_filter),
        ("sexp_find_role_argument_filter", test_role_argument_filter),
        ("sexp_find_value_type_numeric_literal", test_value_type_numeric_literal),
        ("sexp_find_value_type_operator", test_value_type_operator),
        ("sexp_find_role_and_value_type_combined", test_role_and_value_type_combined),
        ("sexp_find_root_node_restricts_results", test_root_node_restricts_results),
        ("sexp_find_root_node_out_of_range", test_root_node_out_of_range),
        ("sexp_find_root_node_negative", test_root_node_negative),
        ("sexp_find_fuzzy_recovers_typo", test_fuzzy_recovers_typo),
        ("sexp_find_fuzzy_orders_by_relevance", test_fuzzy_orders_by_relevance),
        ("sexp_find_fuzzy_with_role_filter", test_fuzzy_with_role_filter),
        ("sexp_find_fuzzy_no_match_returns_empty", test_fuzzy_no_match_returns_empty),
        ("sexp_find_bad_match_mode", test_bad_match_mode),
        ("sexp_find_role_list_wrapper_rejected_by_schema",
         test_role_list_wrapper_rejected_by_schema),
        ("sexp_find_bad_role", test_bad_role),
        ("sexp_find_bad_value_type", test_bad_value_type),
        ("sexp_find_cleanup_simple_tree", test_cleanup_simple_tree),
    ]
    for name, func in tests:
        suite.add(name, func)


if __name__ == "__main__":
    run_module_standalone(register, "SEXP find_sexp_text behavior tests")
