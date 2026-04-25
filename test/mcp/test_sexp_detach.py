"""All detach_sexp_node behavior tests in one place.

This file contains:

1. The 21 regression-pinned scenarios that used to live as a flat
   procedural script in test_detach_sexp_node.py.  Each section's
   individual `check()` calls have been promoted to standalone
   `suite.add()` entries so a failure tells you exactly which assertion
   broke.  Sections that have multiple checks share section-local state
   via a closure-captured dict, not via suite.ctx, so they don't pollute
   the namespace shared with other test files.

2. The three deeper regression tests for the shrink-mode rollback path,
   the unwrap parent-pointer path, and the consistent-detached_node-index
   path that previously lived in test_fred2_mcp.py phase 5.

Detach test categories covered:

- Case A: detaching the root of a mission-attached formula (event,
  goal); the handler swaps in a default replacement (do-nothing or true).
- Case B: detaching a free-standing tree root (delete vs. preserve;
  no-op rejection when there's nothing to detach).
- Case C: detaching an embedded node in a mission-attached formula,
  including the syntax-check rollback path that must leave the tree
  byte-identical when the splice would produce an invalid formula.
- Case D: detaching an embedded node in a free-standing tree, including
  the operator-atom retarget+unwrap path and the shrink mode that
  shifts subsequent siblings up by one position.
- Locked singleton rejection (Locked_sexp_true / Locked_sexp_false).
- Out-of-range and invalid-node rejection.
"""

from mcp_test_lib import (
    assert_equal,
    assert_error,
    assert_in,
    assert_success,
    assert_true,
    find_node_by_value,
    run_module_standalone,
    SkipTest,
    tool_data,
    tool_text,
    tree_values,
)


# ---------------------------------------------------------------------------
# Detach response shape helpers
# ---------------------------------------------------------------------------
# These are specific to the detach response format, so they live in this
# file rather than the shared library.

def get_detached_data(d):
    """Return the detached_node_data subobject if present, else None."""
    return d.get("detached_node_data")


def get_replacement_data(d):
    """Return the replacement_node_data subobject if present, else None."""
    return d.get("replacement_node_data")


def tree_signature(walk_nodes):
    """Snapshot a walk_sexp_tree result as a list of (node, value, parent,
    rest, first) tuples so two walks can be compared for byte-identical
    structure.  Used by the Case C rollback test."""
    return [(n["node"], n.get("value"), n.get("node_parent"), n.get("node_rest"),
             n.get("node_first")) for n in walk_nodes]


def register(suite, client):

    # =====================================================================
    # Section 1: Detach+delete free-standing root (Case B)
    # =====================================================================
    def s01_freestanding_root_freed():
        r = client.call_tool("text_to_sexp", {"text": "( when ( true ) ( do-nothing ) )"})
        assert_success(r)
        node = tool_data(r)["node"]
        r = client.call_tool("detach_sexp_node", {"node": node, "delete": True})
        assert_success(r)
        d = tool_data(r)
        assert_true(d.get("detached_node") == node
                    and d.get("deleted") is True
                    and d.get("replacement_node") is None,
                    f"unexpected detach response: {d}")

    suite.add("sexp_detach_s01_freestanding_root_freed", s01_freestanding_root_freed)

    # =====================================================================
    # Section 2: Detach embedded node (default: preserved with placeholder)
    # =====================================================================
    s02 = {}

    def s02_setup_and_detach_embedded():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
                {"argument_type": "number", "argument_value": "3"},
            ],
        })
        assert_success(r)
        s02["root"] = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": s02["root"]})
        assert_success(r)
        s02["two_node"] = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
        r = client.call_tool("detach_sexp_node", {"node": s02["two_node"]})
        assert_success(r)
        s02["response"] = tool_data(r)
        dn = get_detached_data(s02["response"])
        repl = get_replacement_data(s02["response"])
        repl_val = repl.get("value", "") if repl else ""
        assert_true(dn is not None and dn.get("node") == s02["two_node"]
                    and s02["response"].get("deleted") is False
                    and repl_val == "<placeholder>",
                    f"detached={dn}, replacement={repl_val}")

    def s02_detached_node_still_accessible():
        if "two_node" not in s02:
            raise SkipTest("section 2 setup did not run")
        r = client.call_tool("get_sexp_node", {"node": s02["two_node"]})
        assert_success(r)

    def s02_cleanup():
        if "root" in s02:
            client.call_tool("detach_sexp_node", {"node": s02["root"], "delete": True})
        if "two_node" in s02:
            client.call_tool("detach_sexp_node", {"node": s02["two_node"], "delete": True})

    suite.add("sexp_detach_s02_embedded_node_preserved_with_placeholder", s02_setup_and_detach_embedded)
    suite.add("sexp_detach_s02_detached_node_still_accessible", s02_detached_node_still_accessible)
    suite.add("sexp_detach_s02_cleanup", s02_cleanup)

    # =====================================================================
    # Section 3: Detach root of event formula (Case A, OPR_NULL)
    # =====================================================================
    s03 = {}

    def s03_event_formula_replaced_with_do_nothing():
        r = client.call_tool("create_event", {"name": "test_evt_detach"})
        assert_success(r)
        s03["evt_formula"] = tool_data(r).get("formula")
        r = client.call_tool("detach_sexp_node", {"node": s03["evt_formula"]})
        assert_success(r)
        s03["response"] = tool_data(r)
        dn = get_detached_data(s03["response"])
        repl = get_replacement_data(s03["response"])
        repl_val = repl.get("value", "") if repl else ""
        assert_true(dn is not None and dn.get("node") == s03["evt_formula"]
                    and repl_val == "do-nothing",
                    f"detached={dn}, replacement={repl_val}")

    def s03_event_now_points_to_replacement_node():
        if "response" not in s03:
            raise SkipTest("section 3 setup did not run")
        r = client.call_tool("get_event", {"name": "test_evt_detach"})
        assert_success(r)
        new_formula = tool_data(r).get("formula")
        repl_node = s03["response"].get("replacement_node")
        assert_equal(new_formula, repl_node,
                     f"event formula should point to replacement node")

    def s03_cleanup():
        if "evt_formula" in s03:
            client.call_tool("detach_sexp_node", {"node": s03["evt_formula"], "delete": True})
        client.call_tool("delete_event", {"name": "test_evt_detach", "force": True})

    suite.add("sexp_detach_s03_event_formula_replaced_with_do_nothing", s03_event_formula_replaced_with_do_nothing)
    suite.add("sexp_detach_s03_event_now_points_to_replacement_node", s03_event_now_points_to_replacement_node)
    suite.add("sexp_detach_s03_cleanup", s03_cleanup)

    # =====================================================================
    # Section 4: Detach root of goal formula (Case A, OPR_BOOL)
    # =====================================================================
    s04 = {}

    def s04_goal_formula_replaced_with_true():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "not",
            "operator_arguments": [{"argument_type": "boolean", "argument_value": "true"}],
        })
        assert_success(r)
        custom_formula = tool_data(r)["node"]
        r = client.call_tool("create_goal", {
            "name": "test_goal_detach", "formula": custom_formula
        })
        assert_success(r)
        s04["goal_formula"] = tool_data(r).get("formula")
        r = client.call_tool("detach_sexp_node", {"node": s04["goal_formula"]})
        assert_success(r)
        d = tool_data(r)
        dn = get_detached_data(d)
        repl = get_replacement_data(d)
        repl_val = repl.get("value", "") if repl else ""
        assert_true(dn is not None and dn.get("node") == s04["goal_formula"]
                    and repl_val == "true",
                    f"detached={dn}, replacement={repl_val}")

    def s04_cleanup():
        if "goal_formula" in s04:
            client.call_tool("detach_sexp_node", {"node": s04["goal_formula"], "delete": True})
        client.call_tool("delete_goal", {"name": "test_goal_detach", "force": True})

    suite.add("sexp_detach_s04_goal_formula_replaced_with_true", s04_goal_formula_replaced_with_true)
    suite.add("sexp_detach_s04_cleanup", s04_cleanup)

    # =====================================================================
    # Section 5: Detach Locked_sexp_true embedded in event formula
    # =====================================================================
    # The default event formula `( when ( true ) ( do-nothing ) )` is built
    # by the parser, which reuses Locked_sexp_true for the inner "true"
    # node.  Detaching any locked singleton must always be rejected, even
    # when the singleton is embedded inside a larger tree -- this is
    # checked before any retargeting or splice happens, so the formula
    # must be left untouched.
    s05 = {}

    def s05_setup_find_embedded_true():
        r = client.call_tool("create_event", {"name": "test_evt_embed"})
        assert_success(r)
        s05["evt_formula"] = tool_data(r).get("formula")
        r = client.call_tool("walk_sexp_tree", {"node": s05["evt_formula"]})
        assert_success(r)
        s05["true_node"] = find_node_by_value(tool_data(r)["nodes"],
                                              "true", role="operator")["node"]

    def s05_embedded_locked_singleton_is_rejected():
        if "true_node" not in s05:
            raise SkipTest("section 5 setup did not run")
        r = client.call_tool("detach_sexp_node", {"node": s05["true_node"]})
        err_text = tool_text(r)
        assert_true(r.get("isError") and "singleton" in err_text.lower(),
                    err_text[:200])

    def s05_event_formula_unchanged_after_rejection():
        if "evt_formula" not in s05:
            raise SkipTest("section 5 setup did not run")
        r = client.call_tool("get_event", {"name": "test_evt_embed"})
        assert_success(r)
        assert_equal(tool_data(r).get("formula"), s05["evt_formula"],
                     "event formula should be unchanged after rejection")

    def s05_cleanup():
        client.call_tool("delete_event", {"name": "test_evt_embed", "force": True})

    suite.add("sexp_detach_s05_setup_find_embedded_true", s05_setup_find_embedded_true)
    suite.add("sexp_detach_s05_embedded_locked_singleton_is_rejected",
              s05_embedded_locked_singleton_is_rejected)
    suite.add("sexp_detach_s05_event_formula_unchanged_after_rejection",
              s05_event_formula_unchanged_after_rejection)
    suite.add("sexp_detach_s05_cleanup", s05_cleanup)

    # =====================================================================
    # Section 6: Detach Locked_sexp_true (rejection)
    # =====================================================================
    def s06_locked_true_rejected():
        r = client.call_tool("create_goal", {"name": "test_locked"})
        assert_success(r)
        try:
            locked_formula = tool_data(r).get("formula")
            r = client.call_tool("detach_sexp_node", {"node": locked_formula})
            err = tool_text(r).lower()
            assert_true(r.get("isError") and "singleton" in err,
                        tool_text(r)[:120])
        finally:
            client.call_tool("delete_goal", {"name": "test_locked", "force": True})

    suite.add("sexp_detach_s06_locked_true_rejected", s06_locked_true_rejected)

    # =====================================================================
    # Section 7: Detach SEXP_NOT_USED node (rejection)
    # =====================================================================
    def s07_out_of_range_node_rejected():
        r = client.call_tool("detach_sexp_node", {"node": 99999})
        assert_error(r)

    suite.add("sexp_detach_s07_out_of_range_node_rejected", s07_out_of_range_node_rejected)

    # =====================================================================
    # Section 8: Delete last arg -- placeholder preserved
    # =====================================================================
    # Build (+ 1 2 3) then delete the last arg "3".  After deletion:
    # (+ 1 2 <placeholder>).  Placeholder stays.
    s08 = {}

    def s08_last_arg_placeholder_inserted():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
                {"argument_type": "number", "argument_value": "3"},
            ],
        })
        assert_success(r)
        s08["plus_node"] = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": s08["plus_node"]})
        assert_success(r)
        three_node = find_node_by_value(tool_data(r)["nodes"], "3")["node"]
        r = client.call_tool("detach_sexp_node", {"node": three_node})
        assert_success(r)
        repl = get_replacement_data(tool_data(r))
        assert_true(repl is not None and repl.get("value") == "<placeholder>",
                    f"replacement={repl}")

    def s08_tree_has_one_two_placeholder():
        if "plus_node" not in s08:
            raise SkipTest("section 8 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s08["plus_node"]})
        assert_success(r)
        remaining = tree_values(tool_data(r)["nodes"])
        assert_equal(remaining, ["+", "1", "2", "<placeholder>"], str(remaining))

    def s08_cleanup():
        if "plus_node" in s08:
            client.call_tool("detach_sexp_node", {"node": s08["plus_node"], "delete": True})

    suite.add("sexp_detach_s08_last_arg_placeholder_inserted", s08_last_arg_placeholder_inserted)
    suite.add("sexp_detach_s08_tree_has_one_two_placeholder", s08_tree_has_one_two_placeholder)
    suite.add("sexp_detach_s08_cleanup", s08_cleanup)

    # =====================================================================
    # Section 9: Trailing placeholder cleanup -- middle arg with trailing
    # placeholders.
    # =====================================================================
    # Build (+ 1 2 3), delete "2".  Chain becomes (+ 1 <placeholder> 3).
    # "3" is not a placeholder, so <placeholder> stays.
    s09 = {}

    def s09_middle_arg_placeholder_inserted():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
                {"argument_type": "number", "argument_value": "3"},
            ],
        })
        assert_success(r)
        s09["plus_node"] = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": s09["plus_node"]})
        assert_success(r)
        two_node = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
        r = client.call_tool("detach_sexp_node", {"node": two_node})
        assert_success(r)
        repl = get_replacement_data(tool_data(r))
        assert_true(repl is not None and repl.get("value") == "<placeholder>",
                    f"replacement={repl}")

    def s09_tree_has_one_placeholder_three():
        if "plus_node" not in s09:
            raise SkipTest("section 9 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s09["plus_node"]})
        assert_success(r)
        remaining = tree_values(tool_data(r)["nodes"])
        assert_true("+" in remaining and "1" in remaining
                    and "<placeholder>" in remaining and "3" in remaining,
                    str(remaining))

    def s09_cleanup():
        if "plus_node" in s09:
            client.call_tool("detach_sexp_node", {"node": s09["plus_node"], "delete": True})

    suite.add("sexp_detach_s09_middle_arg_placeholder_inserted", s09_middle_arg_placeholder_inserted)
    suite.add("sexp_detach_s09_tree_has_one_placeholder_three", s09_tree_has_one_placeholder_three)
    suite.add("sexp_detach_s09_cleanup", s09_cleanup)

    # =====================================================================
    # Section 10: Delete all args -- placeholders preserved
    # =====================================================================
    # Build (+ 1 2), delete "1" then "2".  Both become placeholders.
    s10 = {}

    def s10_setup_and_delete_second_arg():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
            ],
        })
        assert_success(r)
        s10["plus_node"] = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": s10["plus_node"]})
        assert_success(r)
        nodes = tool_data(r)["nodes"]
        s10["one_node"] = find_node_by_value(nodes, "1")["node"]
        s10["two_node"] = find_node_by_value(nodes, "2")["node"]
        r = client.call_tool("detach_sexp_node", {"node": s10["two_node"]})
        assert_success(r)
        repl = get_replacement_data(tool_data(r))
        assert_true(repl is not None and repl.get("value") == "<placeholder>",
                    f"replacement={repl}")

    def s10_delete_first_arg():
        if "one_node" not in s10:
            raise SkipTest("section 10 setup did not run")
        r = client.call_tool("detach_sexp_node", {"node": s10["one_node"]})
        assert_success(r)
        repl = get_replacement_data(tool_data(r))
        assert_true(repl is not None and repl.get("value") == "<placeholder>",
                    f"replacement={repl}")

    def s10_tree_has_both_placeholders():
        if "plus_node" not in s10:
            raise SkipTest("section 10 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s10["plus_node"]})
        assert_success(r)
        remaining = tree_values(tool_data(r)["nodes"])
        assert_equal(remaining, ["+", "<placeholder>", "<placeholder>"], str(remaining))

    def s10_cleanup():
        if "plus_node" in s10:
            client.call_tool("detach_sexp_node", {"node": s10["plus_node"], "delete": True})

    suite.add("sexp_detach_s10_setup_and_delete_second_arg", s10_setup_and_delete_second_arg)
    suite.add("sexp_detach_s10_delete_first_arg", s10_delete_first_arg)
    suite.add("sexp_detach_s10_tree_has_both_placeholders", s10_tree_has_both_placeholders)
    suite.add("sexp_detach_s10_cleanup", s10_cleanup)

    # =====================================================================
    # Section 11: Shrink -- remove middle arg, siblings shift up
    # =====================================================================
    # Build (+ 1 2 3), shrink-delete "2".  Chain becomes (+ 1 3).
    s11 = {}

    def s11_shrink_middle_no_placeholder():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
                {"argument_type": "number", "argument_value": "3"},
            ],
        })
        assert_success(r)
        s11["plus_node"] = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": s11["plus_node"]})
        assert_success(r)
        two_node = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
        r = client.call_tool("detach_sexp_node", {"node": two_node, "shrink": True})
        assert_success(r)
        d = tool_data(r)
        assert_true(d.get("replacement_node") is None,
                    f"replacement_node={d.get('replacement_node')}")

    def s11_tree_has_one_three():
        if "plus_node" not in s11:
            raise SkipTest("section 11 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s11["plus_node"]})
        assert_success(r)
        remaining = tree_values(tool_data(r)["nodes"])
        assert_equal(remaining, ["+", "1", "3"], str(remaining))

    def s11_cleanup():
        if "plus_node" in s11:
            client.call_tool("detach_sexp_node", {"node": s11["plus_node"], "delete": True})

    suite.add("sexp_detach_s11_shrink_middle_no_placeholder", s11_shrink_middle_no_placeholder)
    suite.add("sexp_detach_s11_tree_has_one_three", s11_tree_has_one_three)
    suite.add("sexp_detach_s11_cleanup", s11_cleanup)

    # =====================================================================
    # Section 12: Shrink -- remove last arg
    # =====================================================================
    # Build (+ 1 2 3), shrink-delete "3".  Chain becomes (+ 1 2).
    s12 = {}

    def s12_shrink_last_arg():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
                {"argument_type": "number", "argument_value": "3"},
            ],
        })
        assert_success(r)
        s12["plus_node"] = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": s12["plus_node"]})
        assert_success(r)
        three_node = find_node_by_value(tool_data(r)["nodes"], "3")["node"]
        r = client.call_tool("detach_sexp_node", {"node": three_node, "shrink": True})
        assert_success(r)

    def s12_tree_has_one_two():
        if "plus_node" not in s12:
            raise SkipTest("section 12 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s12["plus_node"]})
        assert_success(r)
        remaining = tree_values(tool_data(r)["nodes"])
        assert_equal(remaining, ["+", "1", "2"], str(remaining))

    def s12_cleanup():
        if "plus_node" in s12:
            client.call_tool("detach_sexp_node", {"node": s12["plus_node"], "delete": True})

    suite.add("sexp_detach_s12_shrink_last_arg", s12_shrink_last_arg)
    suite.add("sexp_detach_s12_tree_has_one_two", s12_tree_has_one_two)
    suite.add("sexp_detach_s12_cleanup", s12_cleanup)

    # =====================================================================
    # Section 13: Shrink -- remove first arg
    # =====================================================================
    # Build (+ 1 2 3), shrink-delete "1".  Chain becomes (+ 2 3).
    s13 = {}

    def s13_shrink_first_arg():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
                {"argument_type": "number", "argument_value": "3"},
            ],
        })
        assert_success(r)
        s13["plus_node"] = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": s13["plus_node"]})
        assert_success(r)
        one_node = find_node_by_value(tool_data(r)["nodes"], "1")["node"]
        r = client.call_tool("detach_sexp_node", {"node": one_node, "shrink": True})
        assert_success(r)

    def s13_tree_has_two_three():
        if "plus_node" not in s13:
            raise SkipTest("section 13 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s13["plus_node"]})
        assert_success(r)
        remaining = tree_values(tool_data(r)["nodes"])
        assert_equal(remaining, ["+", "2", "3"], str(remaining))

    def s13_cleanup():
        if "plus_node" in s13:
            client.call_tool("detach_sexp_node", {"node": s13["plus_node"], "delete": True})

    suite.add("sexp_detach_s13_shrink_first_arg", s13_shrink_first_arg)
    suite.add("sexp_detach_s13_tree_has_two_three", s13_tree_has_two_three)
    suite.add("sexp_detach_s13_cleanup", s13_cleanup)

    # =====================================================================
    # Section 14: Shrink -- placeholder sibling preserved
    # =====================================================================
    # Build (+ 1 2 <placeholder>), shrink-delete "2".  After shrink:
    # (+ 1 <placeholder>).  Placeholder is not removed.
    s14 = {}

    def s14_shrink_removes_node_no_replacement():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
                {"argument_type": "string", "argument_value": "<placeholder>"},
            ],
        })
        assert_success(r)
        s14["plus_node"] = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": s14["plus_node"]})
        assert_success(r)
        two_node = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
        r = client.call_tool("detach_sexp_node", {"node": two_node, "shrink": True})
        assert_success(r)
        d = tool_data(r)
        assert_true(d.get("replacement_node") is None,
                    f"replacement_node={d.get('replacement_node')}")

    def s14_tree_has_one_placeholder():
        if "plus_node" not in s14:
            raise SkipTest("section 14 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s14["plus_node"]})
        assert_success(r)
        remaining = tree_values(tool_data(r)["nodes"])
        assert_equal(remaining, ["+", "1", "<placeholder>"], str(remaining))

    def s14_cleanup():
        if "plus_node" in s14:
            client.call_tool("detach_sexp_node", {"node": s14["plus_node"], "delete": True})

    suite.add("sexp_detach_s14_shrink_removes_node_no_replacement", s14_shrink_removes_node_no_replacement)
    suite.add("sexp_detach_s14_tree_has_one_placeholder", s14_tree_has_one_placeholder)
    suite.add("sexp_detach_s14_cleanup", s14_cleanup)

    # =====================================================================
    # Section 15: shrink=false is the same as default (placeholder)
    # =====================================================================
    s15 = {}

    def s15_shrink_false_inserts_placeholder():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
                {"argument_type": "number", "argument_value": "3"},
            ],
        })
        assert_success(r)
        s15["plus_node"] = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": s15["plus_node"]})
        assert_success(r)
        two_node = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
        r = client.call_tool("detach_sexp_node", {"node": two_node, "shrink": False})
        assert_success(r)
        repl = get_replacement_data(tool_data(r))
        assert_true(repl is not None and repl.get("value") == "<placeholder>",
                    f"replacement={repl}")

    def s15_cleanup():
        if "plus_node" in s15:
            client.call_tool("detach_sexp_node", {"node": s15["plus_node"], "delete": True})

    suite.add("sexp_detach_s15_shrink_false_inserts_placeholder", s15_shrink_false_inserts_placeholder)
    suite.add("sexp_detach_s15_cleanup", s15_cleanup)

    # =====================================================================
    # Section 16: Detach already-free-standing root without delete is rejected
    # =====================================================================
    # Detaching a node that is already a free-standing root, with delete=False
    # and without an operator-atom retarget, would otherwise be a silent no-op
    # (the node has nothing to be detached from).  The handler should report
    # this rather than pretending success.
    s16 = {}

    def s16_already_freestanding_rejected():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
            ],
        })
        assert_success(r)
        s16["plus_node"] = tool_data(r)["node"]
        r = client.call_tool("detach_sexp_node", {"node": s16["plus_node"]})
        err_text = tool_text(r)
        assert_true(r.get("isError") and "already the root" in err_text.lower(),
                    err_text[:200])

    def s16_rejected_root_still_accessible():
        if "plus_node" not in s16:
            raise SkipTest("section 16 setup did not run")
        r = client.call_tool("get_sexp_node", {"node": s16["plus_node"]})
        assert_success(r)

    def s16_cleanup():
        if "plus_node" in s16:
            client.call_tool("detach_sexp_node", {"node": s16["plus_node"], "delete": True})

    suite.add("sexp_detach_s16_already_freestanding_rejected", s16_already_freestanding_rejected)
    suite.add("sexp_detach_s16_rejected_root_still_accessible", s16_rejected_root_still_accessible)
    suite.add("sexp_detach_s16_cleanup", s16_cleanup)

    # =====================================================================
    # Section 17: Detach+delete free-standing root reports correct fields
    # =====================================================================
    # create_sexp_node returns the "+" operator atom directly with no list
    # wrapper, and the number args are bare atoms in its .rest chain.
    # Freeing the tree should release exactly 3 nodes: the operator + the
    # two number atoms.
    s17 = {}

    def s17_setup_and_response_fields():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
            ],
        })
        assert_success(r)
        s17["plus_node"] = tool_data(r)["node"]
        r = client.call_tool("detach_sexp_node",
                             {"node": s17["plus_node"], "delete": True})
        assert_success(r)
        s17["response"] = tool_data(r)
        d = s17["response"]
        assert_true(d.get("detached_node") == s17["plus_node"]
                    and d.get("deleted") is True
                    and get_detached_data(d) is None,
                    f"detached_node={d.get('detached_node')}, "
                    f"deleted={d.get('deleted')}, "
                    f"detached_node_data={get_detached_data(d)}")

    def s17_freed_count_exactly_three():
        if "response" not in s17:
            raise SkipTest("section 17 setup did not run")
        d = s17["response"]
        assert_equal(d.get("freed_count"), 3, f"freed_count={d.get('freed_count')}")

    def s17_deleted_root_not_in_use():
        if "plus_node" not in s17:
            raise SkipTest("section 17 setup did not run")
        r = client.call_tool("get_sexp_node", {"node": s17["plus_node"]})
        assert_error(r)

    suite.add("sexp_detach_s17_setup_and_response_fields", s17_setup_and_response_fields)
    suite.add("sexp_detach_s17_freed_count_exactly_three", s17_freed_count_exactly_three)
    suite.add("sexp_detach_s17_deleted_root_not_in_use", s17_deleted_root_not_in_use)

    # =====================================================================
    # Section 18: Case D retarget+unwrap (operator inside wrapper, no delete)
    # =====================================================================
    # Build a free-standing tree with a nested operator subexpression:
    #   ( when ( and ( true ) ( true ) ) ( do-nothing ) )
    # Detaching the embedded "and" operator atom retargets to its enclosing
    # list wrapper, splices the wrapper out of "when"'s argument chain
    # (Case D), and then unwraps the detached subtree so the freed wrapper
    # is gone and "and" becomes the new free-standing root with its
    # arguments reparented to it.
    s18 = {}

    def s18_setup_find_and_operator_and_wrapper():
        r = client.call_tool("text_to_sexp",
                             {"text": "( when ( and ( true ) ( true ) ) ( do-nothing ) )"})
        assert_success(r)
        s18["when_node"] = tool_data(r).get("node")
        r = client.call_tool("walk_sexp_tree", {"node": s18["when_node"]})
        assert_success(r)
        and_n = find_node_by_value(tool_data(r)["nodes"], "and", role="operator")
        s18["and_node"] = and_n["node"]
        s18["and_wrapper_idx"] = and_n.get("node_parent")
        assert_true(s18["and_wrapper_idx"] is not None and s18["and_wrapper_idx"] >= 0,
                    f"wrapper={s18['and_wrapper_idx']}")

    def s18_detach_response_reports_op_atom():
        if "and_node" not in s18:
            raise SkipTest("section 18 setup did not run")
        r = client.call_tool("detach_sexp_node", {"node": s18["and_node"]})
        assert_success(r)
        s18["response"] = tool_data(r)
        d = s18["response"]
        dn = get_detached_data(d)
        assert_true(d.get("detached_node") == s18["and_node"]
                    and dn is not None and dn.get("node") == s18["and_node"],
                    f"detached_node={d.get('detached_node')}, dn_data={dn}")

    def s18_detached_and_is_freestanding_root():
        if "response" not in s18:
            raise SkipTest("section 18 setup did not run")
        dn = get_detached_data(s18["response"])
        assert_true(dn is not None and dn.get("node_parent") == -1,
                    f"node_parent={dn.get('node_parent') if dn else None}")

    def s18_replacement_is_placeholder():
        if "response" not in s18:
            raise SkipTest("section 18 setup did not run")
        repl = get_replacement_data(s18["response"])
        assert_true(repl is not None and repl.get("value") == "<placeholder>",
                    f"replacement={repl}")

    def s18_original_wrapper_was_freed():
        if "and_wrapper_idx" not in s18:
            raise SkipTest("section 18 setup did not run")
        r = client.call_tool("get_sexp_node", {"node": s18["and_wrapper_idx"]})
        assert_error(r)

    def s18_detached_subtree_walks_to_and_true_true():
        if "and_node" not in s18:
            raise SkipTest("section 18 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s18["and_node"]})
        assert_success(r)
        # walk_sexp_tree also returns the empty-text list_wrapper entries;
        # filter them out so we can match the meaningful payload exactly.
        detached_walk = tree_values(tool_data(r)["nodes"], filter_empty=True)
        assert_equal(detached_walk, ["and", "true", "true"], str(detached_walk))

    def s18_original_tree_has_when_placeholder_do_nothing():
        if "when_node" not in s18:
            raise SkipTest("section 18 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s18["when_node"]})
        assert_success(r)
        remaining = tree_values(tool_data(r)["nodes"])
        assert_true("when" in remaining and "<placeholder>" in remaining
                    and "do-nothing" in remaining and "and" not in remaining,
                    str(remaining))

    def s18_cleanup():
        if "when_node" in s18:
            client.call_tool("detach_sexp_node", {"node": s18["when_node"], "delete": True})
        if "and_node" in s18:
            client.call_tool("detach_sexp_node", {"node": s18["and_node"], "delete": True})

    suite.add("sexp_detach_s18_setup_find_and_operator_and_wrapper",
              s18_setup_find_and_operator_and_wrapper)
    suite.add("sexp_detach_s18_detach_response_reports_op_atom",
              s18_detach_response_reports_op_atom)
    suite.add("sexp_detach_s18_detached_and_is_freestanding_root",
              s18_detached_and_is_freestanding_root)
    suite.add("sexp_detach_s18_replacement_is_placeholder",
              s18_replacement_is_placeholder)
    suite.add("sexp_detach_s18_original_wrapper_was_freed",
              s18_original_wrapper_was_freed)
    suite.add("sexp_detach_s18_detached_subtree_walks_to_and_true_true",
              s18_detached_subtree_walks_to_and_true_true)
    suite.add("sexp_detach_s18_original_tree_has_when_placeholder_do_nothing",
              s18_original_tree_has_when_placeholder_do_nothing)
    suite.add("sexp_detach_s18_cleanup", s18_cleanup)

    # =====================================================================
    # Section 19: Case D retarget+unwrap with delete=true
    # =====================================================================
    # Same setup as section 18, but the detached 'and' subtree is freed in
    # one step.  The 'and' subtree contains 4 freeable nodes: the wrapper
    # around 'and', the 'and' operator atom itself, and the two ( true )
    # wrappers.  The two Locked_sexp_true singletons are not freeable and
    # not counted.
    s19 = {}

    def s19_setup_and_detach_delete():
        r = client.call_tool("text_to_sexp",
                             {"text": "( when ( and ( true ) ( true ) ) ( do-nothing ) )"})
        assert_success(r)
        s19["when_node"] = tool_data(r).get("node")
        r = client.call_tool("walk_sexp_tree", {"node": s19["when_node"]})
        assert_success(r)
        s19["and_node"] = find_node_by_value(tool_data(r)["nodes"],
                                             "and", role="operator")["node"]
        r = client.call_tool("detach_sexp_node",
                             {"node": s19["and_node"], "delete": True})
        assert_success(r)
        s19["response"] = tool_data(r)
        d = s19["response"]
        assert_true(d.get("deleted") is True and get_detached_data(d) is None,
                    f"deleted={d.get('deleted')}, dn_data={get_detached_data(d)}")

    def s19_freed_count_is_four():
        if "response" not in s19:
            raise SkipTest("section 19 setup did not run")
        d = s19["response"]
        assert_equal(d.get("freed_count"), 4, f"freed_count={d.get('freed_count')}")

    def s19_deleted_and_node_not_in_use():
        if "and_node" not in s19:
            raise SkipTest("section 19 setup did not run")
        r = client.call_tool("get_sexp_node", {"node": s19["and_node"]})
        assert_error(r)

    def s19_original_tree_has_when_placeholder_do_nothing():
        if "when_node" not in s19:
            raise SkipTest("section 19 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s19["when_node"]})
        assert_success(r)
        remaining = tree_values(tool_data(r)["nodes"])
        assert_true("when" in remaining and "<placeholder>" in remaining
                    and "do-nothing" in remaining and "and" not in remaining,
                    str(remaining))

    def s19_cleanup():
        if "when_node" in s19:
            client.call_tool("detach_sexp_node", {"node": s19["when_node"], "delete": True})

    suite.add("sexp_detach_s19_setup_and_detach_delete", s19_setup_and_detach_delete)
    suite.add("sexp_detach_s19_freed_count_is_four", s19_freed_count_is_four)
    suite.add("sexp_detach_s19_deleted_and_node_not_in_use", s19_deleted_and_node_not_in_use)
    suite.add("sexp_detach_s19_original_tree_has_when_placeholder_do_nothing",
              s19_original_tree_has_when_placeholder_do_nothing)
    suite.add("sexp_detach_s19_cleanup", s19_cleanup)

    # =====================================================================
    # Section 20: Case D retarget+unwrap with shrink mode
    # =====================================================================
    # Build ( + 1 ( - 5 3 ) 2 ) and shrink-detach the embedded '-' operator.
    # Retargeting takes us to the inner list wrapper, shrink mode removes
    # it from '+'s argument chain without inserting a placeholder, and the
    # unwrap step leaves '-' as the root of a new free-standing tree.
    s20 = {}

    def s20_setup_find_minus_and_wrapper():
        r = client.call_tool("text_to_sexp", {"text": "( + 1 ( - 5 3 ) 2 )"})
        assert_success(r)
        s20["plus_node"] = tool_data(r).get("node")
        r = client.call_tool("walk_sexp_tree", {"node": s20["plus_node"]})
        assert_success(r)
        minus_n = find_node_by_value(tool_data(r)["nodes"], "-", role="operator")
        s20["minus_node"] = minus_n["node"]
        s20["minus_wrapper_idx"] = minus_n.get("node_parent")
        assert_true(s20["minus_wrapper_idx"] is not None and s20["minus_wrapper_idx"] >= 0,
                    f"wrapper={s20['minus_wrapper_idx']}")

    def s20_shrink_retarget_response_reports_minus():
        if "minus_node" not in s20:
            raise SkipTest("section 20 setup did not run")
        r = client.call_tool("detach_sexp_node",
                             {"node": s20["minus_node"], "shrink": True})
        assert_success(r)
        s20["response"] = tool_data(r)
        d = s20["response"]
        dn = get_detached_data(d)
        assert_true(d.get("detached_node") == s20["minus_node"]
                    and dn is not None and dn.get("node") == s20["minus_node"],
                    f"detached_node={d.get('detached_node')}, dn_data={dn}")

    def s20_shrink_no_replacement_node():
        if "response" not in s20:
            raise SkipTest("section 20 setup did not run")
        d = s20["response"]
        assert_true(d.get("replacement_node") is None,
                    f"replacement_node={d.get('replacement_node')}")

    def s20_detached_minus_is_freestanding_root():
        if "response" not in s20:
            raise SkipTest("section 20 setup did not run")
        dn = get_detached_data(s20["response"])
        assert_true(dn is not None and dn.get("node_parent") == -1,
                    f"node_parent={dn.get('node_parent') if dn else None}")

    def s20_original_minus_wrapper_was_freed():
        if "minus_wrapper_idx" not in s20:
            raise SkipTest("section 20 setup did not run")
        r = client.call_tool("get_sexp_node", {"node": s20["minus_wrapper_idx"]})
        assert_error(r)

    def s20_original_tree_shrank_to_one_two():
        if "plus_node" not in s20:
            raise SkipTest("section 20 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s20["plus_node"]})
        assert_success(r)
        remaining = tree_values(tool_data(r)["nodes"])
        assert_equal(remaining, ["+", "1", "2"], str(remaining))

    def s20_detached_subtree_walks_to_minus_five_three():
        if "minus_node" not in s20:
            raise SkipTest("section 20 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s20["minus_node"]})
        assert_success(r)
        detached_walk = tree_values(tool_data(r)["nodes"])
        assert_equal(detached_walk, ["-", "5", "3"], str(detached_walk))

    def s20_cleanup():
        if "plus_node" in s20:
            client.call_tool("detach_sexp_node", {"node": s20["plus_node"], "delete": True})
        if "minus_node" in s20:
            client.call_tool("detach_sexp_node", {"node": s20["minus_node"], "delete": True})

    suite.add("sexp_detach_s20_setup_find_minus_and_wrapper", s20_setup_find_minus_and_wrapper)
    suite.add("sexp_detach_s20_shrink_retarget_response_reports_minus",
              s20_shrink_retarget_response_reports_minus)
    suite.add("sexp_detach_s20_shrink_no_replacement_node", s20_shrink_no_replacement_node)
    suite.add("sexp_detach_s20_detached_minus_is_freestanding_root",
              s20_detached_minus_is_freestanding_root)
    suite.add("sexp_detach_s20_original_minus_wrapper_was_freed",
              s20_original_minus_wrapper_was_freed)
    suite.add("sexp_detach_s20_original_tree_shrank_to_one_two",
              s20_original_tree_shrank_to_one_two)
    suite.add("sexp_detach_s20_detached_subtree_walks_to_minus_five_three",
              s20_detached_subtree_walks_to_minus_five_three)
    suite.add("sexp_detach_s20_cleanup", s20_cleanup)

    # =====================================================================
    # Section 21: Case C syntax-error rollback in attached formula
    # =====================================================================
    # Build an event whose formula is `( when ( and ( true ) ( true ) )
    # ( do-nothing ) )` and try to detach the embedded 'and' operator.
    # The default mode would replace the inner list wrapper with a
    # <placeholder> string atom in the OPF_BOOL conditional position;
    # check_sexp_syntax rejects this with SEXP_CHECK_TYPE_MISMATCH, so
    # the handler must roll back and leave the event formula structurally
    # identical to before the call.
    s21 = {}

    def s21_setup_find_and_in_attached_formula():
        r = client.call_tool("text_to_sexp",
                             {"text": "( when ( and ( true ) ( true ) ) ( do-nothing ) )"})
        assert_success(r)
        formula_node = tool_data(r).get("node")
        r = client.call_tool("create_event",
                             {"name": "test_evt_rollback", "formula": formula_node})
        assert_success(r)
        s21["event_formula"] = tool_data(r).get("formula")
        r = client.call_tool("walk_sexp_tree", {"node": s21["event_formula"]})
        assert_success(r)
        nodes_before = tool_data(r)["nodes"]
        for n in nodes_before:
            if n.get("value") == "and" and n.get("role") == "operator":
                s21["and_node"] = n["node"]
                break
        assert_true("and_node" in s21, "could not find 'and' in attached formula")
        s21["sig_before"] = tree_signature(nodes_before)

    def s21_detach_rejected_with_syntax_error():
        if "and_node" not in s21:
            raise SkipTest("section 21 setup did not run")
        r = client.call_tool("detach_sexp_node", {"node": s21["and_node"]})
        err_text = tool_text(r)
        assert_true(r.get("isError") and "syntax" in err_text.lower(),
                    err_text[:200])

    def s21_event_formula_root_unchanged():
        if "event_formula" not in s21:
            raise SkipTest("section 21 setup did not run")
        r = client.call_tool("get_event", {"name": "test_evt_rollback"})
        assert_success(r)
        assert_equal(tool_data(r).get("formula"), s21["event_formula"],
                     "event formula root should be unchanged after rollback")

    def s21_tree_structure_byte_identical():
        if "sig_before" not in s21:
            raise SkipTest("section 21 setup did not run")
        r = client.call_tool("walk_sexp_tree", {"node": s21["event_formula"]})
        assert_success(r)
        sig_after = tree_signature(tool_data(r)["nodes"])
        assert_equal(sig_after, s21["sig_before"],
                     "tree structure should be byte-identical after rollback")

    def s21_cleanup():
        client.call_tool("delete_event", {"name": "test_evt_rollback", "force": True})

    suite.add("sexp_detach_s21_setup_find_and_in_attached_formula",
              s21_setup_find_and_in_attached_formula)
    suite.add("sexp_detach_s21_detach_rejected_with_syntax_error",
              s21_detach_rejected_with_syntax_error)
    suite.add("sexp_detach_s21_event_formula_root_unchanged",
              s21_event_formula_root_unchanged)
    suite.add("sexp_detach_s21_tree_structure_byte_identical",
              s21_tree_structure_byte_identical)
    suite.add("sexp_detach_s21_cleanup", s21_cleanup)

    # =====================================================================
    # Deeper regression tests originally in test_fred2_mcp.py phase 5
    # =====================================================================

    def test_detach_shrink_rollback_restores_last_sibling():
        """Bug regression: shrink-mode rollback fails when target_rest == -1.

        When detach_sexp_node is called with shrink=true on the last sibling
        of an attached formula's operator, removing that sibling violates
        the operator's min argument count, the syntax check fails, and the
        function is supposed to roll back. In the buggy version the
        rollback uses splice_replace_node(target_rest, n) where target_rest
        is -1 -- a no-op -- so the predecessor's rest pointer is left at -1
        and the original last sibling is permanently orphaned even though
        the API returns an error claiming a clean rollback.
        """
        # Default event formula is `(when (true) (do-nothing))`. `when`
        # requires at least 2 args (cond + 1 action), so removing
        # `do-nothing` violates min.
        r = client.call_tool("create_event", {"name": "test_shrink_rollback"})
        assert_success(r)
        evt_d = tool_data(r)
        formula_root = evt_d.get("formula")
        assert_true(formula_root is not None and formula_root >= 0,
                    f"event should have a formula root, got {formula_root}")

        try:
            r = client.call_tool("walk_sexp_tree", {"node": formula_root})
            assert_success(r)
            nodes = tool_data(r).get("nodes", [])
            do_nothing = None
            for n in nodes:
                if n.get("value") == "do-nothing" and n.get("role") == "operator":
                    do_nothing = n["node"]
                    break
            assert_true(do_nothing is not None,
                        f"could not find do-nothing operator in default event formula; "
                        f"walked {[(n.get('value'), n.get('role')) for n in nodes]}")

            # Shrink-detach the last action.  The retarget logic moves to
            # its enclosing list wrapper, whose .rest is -1, exercising the
            # bug path.  The syntax check on `when` should fail and the
            # operation should roll back, leaving the formula tree exactly
            # as it was.
            r = client.call_tool("detach_sexp_node",
                                 {"node": do_nothing, "shrink": True})
            assert_error(r)

            # The rollback must have restored do-nothing in the formula
            # tree.  In the buggy version, do-nothing is orphaned and
            # walk_sexp_tree cannot reach it from the formula root.
            r = client.call_tool("walk_sexp_tree", {"node": formula_root})
            assert_success(r)
            nodes = tool_data(r).get("nodes", [])
            values = [n.get("value") for n in nodes]
            assert_in("do-nothing", values,
                      f"shrink-mode rollback failed to restore do-nothing in the "
                      f"event formula; reachable nodes after failed detach = {values}")
        finally:
            client.call_tool("delete_event",
                             {"name": "test_shrink_rollback", "force": True})

    def test_detach_unwrap_clears_stale_sibling_parents():
        """Regression: unwrapping after detach must clear stale sibling parents.

        When the user passes an operator atom to detach_sexp_node, the
        handler retargets to the enclosing list wrapper, processes the
        detach, and then (on the !do_delete path) frees the wrapper.
        Before the fix, the operator atom's sibling wrappers still pointed
        at that (now-freed) wrapper slot — leaving a dangling reference.

        The detached subtree must match the parser's convention for a
        natively-parsed free-standing tree: wrappers directly under a
        top-level operator atom have parent = -1 (not the operator, and
        not the prior enclosing wrapper).

        This only manifests when the operator actually has an enclosing
        wrapper, which in parser-produced trees happens for *nested*
        sub-expressions, not top-level operators.  So the test nests
        `when` inside `and`.
        """
        r = client.call_tool("text_to_sexp", {
            "text": "( and ( when ( true ) ( do-nothing ) ) ( true ) )"
        })
        assert_success(r)
        formula_root = tool_data(r)["node"]

        when_op = None
        detached_root = None
        try:
            r = client.call_tool("walk_sexp_tree", {"node": formula_root})
            assert_success(r)
            nodes = tool_data(r).get("nodes", [])
            for n in nodes:
                if n.get("value") == "when" and n.get("role") == "operator":
                    when_op = n["node"]
                    break
            assert_true(when_op is not None,
                        "could not find nested `when` operator")

            # Detach the `when` atom (preserve, not delete).  The handler
            # retargets to the enclosing wrapper, splices it out of `and`'s
            # rest chain, then unwraps back to the operator atom.
            r = client.call_tool("detach_sexp_node", {"node": when_op})
            assert_success(r)
            d = tool_data(r)
            detached_root = d.get("detached_node")
            assert_equal(detached_root, when_op,
                         "preserved detach should report the operator atom index")

            # Walk the new free-standing subtree and check a sibling's
            # parent.  Per parser convention for a natively-parsed free
            # tree, wrappers directly under a top-level operator atom have
            # parent = -1.  In the buggy version, parent still pointed at
            # the freed enclosing wrapper (neither -1 nor detached_root).
            r = client.call_tool("walk_sexp_tree", {"node": detached_root})
            assert_success(r)
            nodes = tool_data(r).get("nodes", [])
            sibling = None
            for n in nodes:
                if n["node"] != detached_root and n.get("role") == "list_wrapper":
                    sibling = n
                    break
            assert_true(sibling is not None,
                        f"could not find a sibling list_wrapper under {detached_root}; "
                        f"walked {[(n['node'], n.get('role')) for n in nodes]}")

            parent = sibling.get("node_parent")
            assert_equal(parent, -1,
                         f"sibling node {sibling['node']} should have parent=-1 "
                         f"(matching parser convention for a free-standing tree); "
                         f"got parent={parent}")
        finally:
            if detached_root is not None:
                client.call_tool("detach_sexp_node",
                                 {"node": detached_root, "delete": True})
            client.call_tool("detach_sexp_node",
                             {"node": formula_root, "delete": True})

    def test_detach_reports_consistent_node_when_retargeted():
        """Bug regression: detached_node response differs between delete=true and delete=false.

        When the user passes an operator atom that gets retargeted to its
        enclosing wrapper, the !do_delete path reverts `n` back to the
        original operator atom before building the response, so the user
        sees the index they passed in.  The do_delete path skips that
        revert, so the response reports the (now-freed) wrapper index
        instead -- a different number than the user's input.

        As with the parent-pointer test, the retargeting only fires when
        the operator actually has an enclosing wrapper, which means the
        target `when` must be a nested sub-expression.
        """
        def build_and_find_nested_when():
            r = client.call_tool("text_to_sexp", {
                "text": "( and ( when ( true ) ( do-nothing ) ) ( true ) )"
            })
            assert_success(r)
            root = tool_data(r)["node"]
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            for n in tool_data(r).get("nodes", []):
                if n.get("value") == "when" and n.get("role") == "operator":
                    return root, n["node"]
            raise AssertionError("could not find nested `when` operator")

        # delete=false case: report the operator atom index via the unwrap path.
        root_preserved, when_op_preserved = build_and_find_nested_when()
        detached_preserved = None
        try:
            r = client.call_tool("detach_sexp_node",
                                 {"node": when_op_preserved, "delete": False})
            assert_success(r)
            preserved_reported = tool_data(r).get("detached_node")
            detached_preserved = preserved_reported
        finally:
            if detached_preserved is not None:
                client.call_tool("detach_sexp_node",
                                 {"node": detached_preserved, "delete": True})
            client.call_tool("detach_sexp_node",
                             {"node": root_preserved, "delete": True})

        # delete=true case: should report the same operator atom index.
        root_deleted, when_op_deleted = build_and_find_nested_when()
        try:
            r = client.call_tool("detach_sexp_node",
                                 {"node": when_op_deleted, "delete": True})
            assert_success(r)
            deleted_reported = tool_data(r).get("detached_node")
        finally:
            client.call_tool("detach_sexp_node",
                             {"node": root_deleted, "delete": True})

        # Both calls passed an operator atom; both should report it back.
        assert_equal(preserved_reported, when_op_preserved,
                     "delete=false should report the operator atom index")
        assert_equal(deleted_reported, when_op_deleted,
                     "delete=true should report the operator atom index "
                     "(buggy version reports the wrapper index instead)")

    suite.add("sexp_detach_shrink_rollback_restores_last_sibling",
              test_detach_shrink_rollback_restores_last_sibling)
    suite.add("sexp_detach_unwrap_clears_stale_sibling_parents",
              test_detach_unwrap_clears_stale_sibling_parents)
    suite.add("sexp_detach_reports_consistent_node_when_retargeted",
              test_detach_reports_consistent_node_when_retargeted)

    # =================================================================
    # Target resolution: singleton rejection, argument index, entity mode
    # =================================================================

    def test_detach_singleton_without_index_is_rejected():
        """Targeting a locked singleton directly should be rejected."""
        r = client.call_tool("text_to_sexp", {"text": "( when ( true ) ( do-nothing ) )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            nodes = tool_data(r)["nodes"]
            true_node = None
            for n in nodes:
                if n["value"] == "true" and n["role"] == "operator":
                    true_node = n["node"]
                    break
            assert_true(true_node is not None, "Should find a true operator in the tree")

            r = client.call_tool("detach_sexp_node", {"node": true_node})
            assert_error(r)
            assert_in("singleton", tool_text(r).lower())
            assert_in("argument_index", tool_text(r))
        finally:
            client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_detach_singleton_via_argument_index():
        """Using argument_index to address a singleton should work."""
        r = client.call_tool("text_to_sexp", {"text": "( when ( true ) ( do-nothing ) )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            # The when operator (the root) has argument 1 = the condition (true),
            # argument 2 = the action (do-nothing).
            # Detach argument 1 (the true boolean wrapper) via index.
            r = client.call_tool("detach_sexp_node",
                                 {"node": root, "argument_index": 1, "delete": True})
            assert_success(r)
            d = tool_data(r)
            assert_true(d.get("deleted"), "Should have deleted the detached node")

            # The tree should still have a placeholder where true was.
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            values = tree_values(tool_data(r)["nodes"])
            assert_in("<placeholder>", values)
        finally:
            client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_detach_argument_index_out_of_range():
        """Out-of-range argument index should be rejected."""
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            r = client.call_tool("detach_sexp_node",
                                 {"node": root, "argument_index": 99})
            assert_error(r)
            assert_in("out of range", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_detach_argument_index_on_non_operator():
        """argument_index with a non-operator target should be rejected."""
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "42",
        })
        assert_success(r)
        node = tool_data(r)["node"]
        try:
            r = client.call_tool("detach_sexp_node",
                                 {"node": node, "argument_index": 1})
            assert_error(r)
            assert_in("operator", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node", {"node": node, "delete": True})

    def test_detach_argument_index_below_minimum():
        """Non-positive argument index should be rejected (1-based)."""
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            r = client.call_tool("detach_sexp_node",
                                 {"node": root, "argument_index": 0})
            assert_error(r)
            assert_in("1-based", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_detach_last_arg_via_argument_index():
        """Detach the last argument of a multi-arg operator via arg_idx.
        Exercises the resolver walk landing exactly on the final rest slot."""
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 3 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            # Detach arg 3 (the "3") — the last argument.
            r = client.call_tool("detach_sexp_node",
                                 {"node": root, "argument_index": 3,
                                  "delete": True})
            assert_success(r)
            d = tool_data(r)
            assert_true(d.get("deleted"), "should have deleted the detached node")
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            values = tree_values(tool_data(r)["nodes"], filter_empty=True)
            # "3" is gone; placeholder replaces the last arg slot.
            assert_equal(values, ["+", "1", "2", "<placeholder>"])
        finally:
            client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_detach_entity_mode():
        """Entity-mode detach should replace the formula with a default."""
        r = client.call_tool("create_event", {"name": "detach_ent_test"})
        assert_success(r)
        evt_formula = tool_data(r)["formula"]
        detached_node = None
        try:
            r = client.call_tool("detach_sexp_node", {
                "entity_type": "event",
                "entity_id": "detach_ent_test",
            })
            assert_success(r)
            d = tool_data(r)
            detached_node = d.get("detached_node")
            assert_equal(d["detached_node"], evt_formula,
                         "detached_node should be the old formula root")
            assert_true(d.get("replacement_node") is not None,
                        "Should have a replacement node")
        finally:
            if detached_node is not None:
                client.call_tool("detach_sexp_node",
                                 {"node": detached_node, "delete": True})
            client.call_tool("delete_event", {"name": "detach_ent_test", "force": True})

    def test_detach_entity_mode_with_delete():
        """Entity-mode detach with delete=true should free the old formula."""
        r = client.call_tool("create_event", {"name": "detach_ent_del"})
        assert_success(r)
        try:
            r = client.call_tool("detach_sexp_node", {
                "entity_type": "event",
                "entity_id": "detach_ent_del",
                "delete": True,
            })
            assert_success(r)
            d = tool_data(r)
            assert_true(d.get("deleted"), "Should have deleted")
            assert_true(d.get("freed_count", 0) > 0, "Should have freed nodes")
        finally:
            client.call_tool("delete_event", {"name": "detach_ent_del", "force": True})

    def test_detach_entity_and_node_mutually_exclusive():
        """Passing both node and entity_type should error."""
        r = client.call_tool("detach_sexp_node", {
            "node": 0,
            "entity_type": "event",
            "entity_id": "foo",
        })
        assert_error(r)
        assert_in("exactly one", tool_text(r).lower())

    def test_detach_entity_tag_with_node_mode_rejected():
        """Passing entity_tag with node (no entity_type) should
        error; entity_tag is only valid in entity mode."""
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            r = client.call_tool("detach_sexp_node", {
                "node": root,
                "entity_tag": "arrival_cue",
            })
            assert_error(r)
            assert_in("entity_tag", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_detach_entity_id_with_node_mode_rejected():
        """Passing entity_id with node (no entity_type) should
        error; entity_id is only valid in entity mode."""
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            r = client.call_tool("detach_sexp_node", {
                "node": root,
                "entity_id": "phantom_entity",
            })
            assert_error(r)
            assert_in("entity_id", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    suite.add("sexp_detach_singleton_without_index_is_rejected",
              test_detach_singleton_without_index_is_rejected)
    suite.add("sexp_detach_singleton_via_argument_index",
              test_detach_singleton_via_argument_index)
    suite.add("sexp_detach_argument_index_out_of_range",
              test_detach_argument_index_out_of_range)
    suite.add("sexp_detach_argument_index_on_non_operator",
              test_detach_argument_index_on_non_operator)
    suite.add("sexp_detach_argument_index_below_minimum",
              test_detach_argument_index_below_minimum)
    suite.add("sexp_detach_last_arg_via_argument_index",
              test_detach_last_arg_via_argument_index)
    def test_detach_entity_default_formula_rejected():
        """Detaching a goal whose formula is already the default (true) should
        error rather than silently no-op."""
        r = client.call_tool("create_goal", {"name": "detach_default_test"})
        assert_success(r)
        try:
            r = client.call_tool("detach_sexp_node", {
                "entity_type": "goal",
                "entity_id": "detach_default_test",
            })
            assert_error(r)
            assert_in("already the default", tool_text(r).lower())
        finally:
            client.call_tool("delete_goal", {
                "name": "detach_default_test", "force": True,
            })

    suite.add("sexp_detach_entity_mode",
              test_detach_entity_mode)
    suite.add("sexp_detach_entity_mode_with_delete",
              test_detach_entity_mode_with_delete)
    suite.add("sexp_detach_entity_default_formula_rejected",
              test_detach_entity_default_formula_rejected)
    suite.add("sexp_detach_entity_and_node_mutually_exclusive",
              test_detach_entity_and_node_mutually_exclusive)
    suite.add("sexp_detach_entity_tag_with_node_mode_rejected",
              test_detach_entity_tag_with_node_mode_rejected)
    suite.add("sexp_detach_entity_id_with_node_mode_rejected",
              test_detach_entity_id_with_node_mode_rejected)

    # =================================================================
    # Direct wrapper targeting: unwrap even without retargeting
    # =================================================================

    def test_detach_direct_wrapper_target_unwraps():
        """When the user passes a LIST wrapper's index directly (no
        retargeting path), the detach handler still unwraps it so the
        resulting free-standing tree has a bare operator atom at the root
        (matching the parser's convention for top-level operators).

        Regression: previously the unwrap only fired when retargeting
        happened (n != original_n), so direct wrapper targets left a
        wrapper-rooted tree behind, violating the invariant.
        """
        r = client.call_tool("text_to_sexp",
                             {"text": "( when ( and ( true ) ( true ) ) ( do-nothing ) )"})
        assert_success(r)
        root = tool_data(r)["node"]
        detached_root = None
        try:
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            # Find the 'and' operator, then its enclosing wrapper.  Target
            # the wrapper directly so retargeting does not trigger.
            and_n = find_node_by_value(tool_data(r)["nodes"], "and", role="operator")
            wrapper_idx = and_n.get("node_parent")
            assert_true(wrapper_idx is not None and wrapper_idx >= 0,
                        f"wrapper={wrapper_idx}")

            r = client.call_tool("detach_sexp_node", {"node": wrapper_idx})
            assert_success(r)
            d = tool_data(r)
            # The reported detached_node should be the inner 'and' atom,
            # not the freed wrapper.
            assert_equal(d.get("detached_node"), and_n["node"],
                         f"detached_node={d.get('detached_node')}, expected and={and_n['node']}")
            dn = get_detached_data(d)
            assert_true(dn is not None and dn.get("role") == "operator"
                        and dn.get("value") == "and" and dn.get("node_parent") == -1,
                        f"detached_node_data={dn}")
            detached_root = d.get("detached_node")

            # The wrapper index should no longer be in use.
            r = client.call_tool("get_sexp_node", {"node": wrapper_idx})
            assert_error(r)
        finally:
            if detached_root is not None:
                client.call_tool("detach_sexp_node",
                                 {"node": detached_root, "delete": True})
            client.call_tool("detach_sexp_node",
                             {"node": root, "delete": True})

    suite.add("sexp_detach_direct_wrapper_target_unwraps",
              test_detach_direct_wrapper_target_unwraps)


if __name__ == "__main__":
    run_module_standalone(register, "SEXP detach behavior tests")
