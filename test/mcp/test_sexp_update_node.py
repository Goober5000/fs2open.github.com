"""Tests for update_sexp_node.

Covers in-place mutation of SEXP nodes:
  * Operator rename (happy path and error paths)
  * Argument value and type changes (literals, variable references)
  * Boolean wrapper toggle (true <-> false)
  * Structural error rejections (out-of-range, not-in-use, locked node,
    non-boolean list wrapper)
  * Rollback when a syntax error would result in a mission-attached formula

Each scenario owns its setup and uses try/finally cleanup, so a failure in
one scenario does not leak state into the next.
"""

from mcp_test_lib import (
    assert_equal,
    assert_error,
    assert_in,
    assert_success,
    assert_true,
    find_node_by_value,
    run_module_standalone,
    tool_data,
    tool_text,
)


# ---------------------------------------------------------------------------
# Private helpers
# ---------------------------------------------------------------------------

def _walk_nodes(client, root):
    """Call walk_sexp_tree and return its 'nodes' list."""
    r = client.call_tool("walk_sexp_tree", {"node": root})
    assert_success(r)
    d = tool_data(r)
    return d["nodes"] if isinstance(d, dict) and "nodes" in d else d


def _safe_detach(client, node):
    try:
        client.call_tool("detach_sexp_node", {"node": node, "delete": True})
    except Exception:
        pass


def _safe_delete_var(client, name):
    try:
        client.call_tool("delete_sexp_variable", {"name": name, "force": True})
    except Exception:
        pass


def _safe_delete_event(client, name):
    try:
        client.call_tool("delete_event", {"name": name})
    except Exception:
        pass


def _find_node_or_none(nodes, value, role=None):
    """find_node_by_value but returns None instead of raising when absent."""
    try:
        return find_node_by_value(nodes, value, role=role)
    except AssertionError:
        return None


def _find_bool_wrapper(nodes, bool_value):
    """Find the list_wrapper whose first child is the named boolean operator.

    bool_value should be "true" or "false".  Returns the node dict, or None.
    """
    locked_op = _find_node_or_none(nodes, bool_value, role="operator")
    if locked_op is None:
        return None
    locked_idx = locked_op["node"]
    for n in nodes:
        if n.get("role") == "list_wrapper" and n.get("node_first") == locked_idx:
            return n
    return None


# ---------------------------------------------------------------------------
# Test registration
# ---------------------------------------------------------------------------

def register(suite, client):

    # -----------------------------------------------------------------------
    # Section 1: Operator update
    # -----------------------------------------------------------------------

    def test_operator_rename():
        trees = []
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "1"},
                    {"argument_type": "number", "argument_value": "2"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            nodes = _walk_nodes(client, root)
            op_entry = find_node_by_value(nodes, "+", role="operator")
            assert_true(op_entry is not None, "should find '+' operator in tree")
            op_node = op_entry["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": op_node,
                "operator_name": "-",
            })
            assert_success(r2)
            d = tool_data(r2)
            assert_equal(d.get("value"), "-",
                         "returned node value should be '-' after rename")
            assert_equal(d.get("role"), "operator",
                         "role should remain 'operator' after rename")

            nodes2 = _walk_nodes(client, root)
            assert_true(find_node_by_value(nodes2, "-", role="operator") is not None,
                        "walked tree should contain '-' operator after rename")
            assert_true(_find_node_or_none(nodes2, "+", role="operator") is None,
                        "'+' should no longer appear in tree after rename")
        finally:
            for n in trees:
                _safe_detach(client, n)

    def test_operator_unknown_name():
        trees = []
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "1"},
                    {"argument_type": "number", "argument_value": "2"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            nodes = _walk_nodes(client, root)
            op_node = find_node_by_value(nodes, "+", role="operator")["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": op_node,
                "operator_name": "zzz_no_such_op",
            })
            assert_error(r2)
            assert_in("Unknown SEXP operator", tool_text(r2))

            # Tree should be unchanged
            nodes2 = _walk_nodes(client, root)
            assert_true(find_node_by_value(nodes2, "+", role="operator") is not None,
                        "'+' should still be present after failed update")
        finally:
            for n in trees:
                _safe_detach(client, n)

    def test_operator_missing_name_param():
        trees = []
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            op_node = find_node_by_value(_walk_nodes(client, root),
                                         "+", role="operator")["node"]

            # operator_name field omitted entirely
            r2 = client.call_tool("update_sexp_node", {"node": op_node})
            assert_error(r2)
        finally:
            for n in trees:
                _safe_detach(client, n)

    # -----------------------------------------------------------------------
    # Section 2: Argument literal update
    # -----------------------------------------------------------------------

    def test_argument_update_number_value():
        trees = []
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "1"},
                    {"argument_type": "number", "argument_value": "2"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            atom = find_node_by_value(_walk_nodes(client, root), "1", role="argument")
            assert_true(atom is not None, "should find '1' argument node")
            atom_node = atom["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": atom_node,
                "argument_type": "number",
                "argument_value": "99",
            })
            assert_success(r2)
            d = tool_data(r2)
            assert_equal(d.get("value"), "99",
                         "updated atom should have value '99'")
            assert_equal(d.get("value_type"), "numeric_literal",
                         "value_type should be numeric_literal")
        finally:
            for n in trees:
                _safe_detach(client, n)

    def test_argument_update_string_value():
        trees = []
        try:
            # string-equals accepts OPF_STRING arguments
            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "string-equals",
                "operator_arguments": [
                    {"argument_type": "string", "argument_value": "hello"},
                    {"argument_type": "string", "argument_value": "world"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            atom = find_node_by_value(_walk_nodes(client, root),
                                      "hello", role="argument")
            assert_true(atom is not None, "should find 'hello' argument node")
            atom_node = atom["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": atom_node,
                "argument_type": "string",
                "argument_value": "changed",
            })
            assert_success(r2)
            d = tool_data(r2)
            assert_equal(d.get("value"), "changed", "value should be 'changed'")
            assert_equal(d.get("value_type"), "string_literal",
                         "value_type should be string_literal")
        finally:
            for n in trees:
                _safe_detach(client, n)

    def test_argument_reject_non_integer_number():
        trees = []
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            atom_node = find_node_by_value(_walk_nodes(client, root),
                                           "1", role="argument")["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": atom_node,
                "argument_type": "number",
                "argument_value": "not_a_number",
            })
            assert_error(r2)
            assert_in("not a valid number", tool_text(r2))
        finally:
            for n in trees:
                _safe_detach(client, n)

    def test_argument_missing_value_param():
        trees = []
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            atom_node = find_node_by_value(_walk_nodes(client, root),
                                           "1", role="argument")["node"]

            # argument_value omitted
            r2 = client.call_tool("update_sexp_node", {
                "node": atom_node,
                "argument_type": "number",
            })
            assert_error(r2)
        finally:
            for n in trees:
                _safe_detach(client, n)

    # -----------------------------------------------------------------------
    # Section 3: Variable references
    # -----------------------------------------------------------------------

    def test_literal_to_variable_reference():
        trees, vars_ = [], []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "usn_x",
                "default_value": "0",
                "variable_type": "number",
            })
            assert_success(r)
            vars_.append("usn_x")

            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "1"},
                    {"argument_type": "number", "argument_value": "2"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            atom_node = find_node_by_value(_walk_nodes(client, root),
                                           "1", role="argument")["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": atom_node,
                "argument_type": "number",
                "argument_value": "@usn_x",
            })
            assert_success(r2)
            d = tool_data(r2)
            assert_equal(d.get("value"), "usn_x",
                         "node text should be the variable name without @")
            assert_equal(d.get("value_type"), "numeric_variable",
                         "value_type should be numeric_variable")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    def test_variable_reference_to_literal():
        trees, vars_ = [], []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "usn_y",
                "default_value": "5",
                "variable_type": "number",
            })
            assert_success(r)
            vars_.append("usn_y")

            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "@usn_y"},
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            var_atom = find_node_by_value(_walk_nodes(client, root),
                                          "usn_y", role="argument")
            assert_true(var_atom is not None, "should find the usn_y variable atom")
            atom_node = var_atom["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": atom_node,
                "argument_type": "number",
                "argument_value": "42",
            })
            assert_success(r2)
            d = tool_data(r2)
            assert_equal(d.get("value"), "42", "value should be literal '42'")
            assert_equal(d.get("value_type"), "numeric_literal",
                         "SEXP_FLAG_VARIABLE should be cleared; value_type should be numeric_literal")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    def test_unknown_variable_reference():
        trees = []
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            atom_node = find_node_by_value(_walk_nodes(client, root),
                                           "1", role="argument")["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": atom_node,
                "argument_type": "number",
                "argument_value": "@usn_does_not_exist",
            })
            assert_error(r2)
            assert_in("Unknown SEXP variable", tool_text(r2))
        finally:
            for n in trees:
                _safe_detach(client, n)

    # -----------------------------------------------------------------------
    # Section 4: Boolean wrapper toggle
    # -----------------------------------------------------------------------

    def test_boolean_wrapper_true_to_false():
        trees = []
        try:
            r = client.call_tool("text_to_sexp",
                                 {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            nodes = _walk_nodes(client, root)
            wrapper = _find_bool_wrapper(nodes, "true")
            assert_true(wrapper is not None,
                        "should find a (true) boolean wrapper in the tree")
            wrapper_node = wrapper["node"]
            original_first = wrapper["node_first"]

            r2 = client.call_tool("update_sexp_node", {
                "node": wrapper_node,
                "argument_type": "boolean",
                "argument_value": "false",
            })
            assert_success(r2)
            d = tool_data(r2)
            assert_true(d.get("node_first") != original_first,
                        "node_first should change when toggling true->false")

            nodes2 = _walk_nodes(client, root)
            assert_true(_find_bool_wrapper(nodes2, "false") is not None,
                        "after toggle, should find a (false) boolean wrapper")
            assert_true(_find_bool_wrapper(nodes2, "true") is None,
                        "should no longer find a (true) boolean wrapper")
        finally:
            for n in trees:
                _safe_detach(client, n)

    def test_boolean_wrapper_false_to_true():
        trees = []
        try:
            r = client.call_tool("text_to_sexp",
                                 {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            nodes = _walk_nodes(client, root)
            wrapper = _find_bool_wrapper(nodes, "true")
            wrapper_node = wrapper["node"]

            # First set to false
            r_false = client.call_tool("update_sexp_node", {
                "node": wrapper_node,
                "argument_type": "boolean",
                "argument_value": "false",
            })
            assert_success(r_false)

            # Now toggle back to true
            r2 = client.call_tool("update_sexp_node", {
                "node": wrapper_node,
                "argument_type": "boolean",
                "argument_value": "true",
            })
            assert_success(r2)

            nodes3 = _walk_nodes(client, root)
            assert_true(_find_bool_wrapper(nodes3, "true") is not None,
                        "after false->true toggle, should find a (true) boolean wrapper")
            assert_true(_find_bool_wrapper(nodes3, "false") is None,
                        "should no longer find a (false) boolean wrapper")
        finally:
            for n in trees:
                _safe_detach(client, n)

    def test_boolean_wrapper_invalid_value():
        trees = []
        try:
            r = client.call_tool("text_to_sexp",
                                 {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            wrapper_node = _find_bool_wrapper(
                _walk_nodes(client, root), "true")["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": wrapper_node,
                "argument_type": "boolean",
                "argument_value": "yes",
            })
            assert_error(r2)
            assert_in('"true" or "false"', tool_text(r2))
        finally:
            for n in trees:
                _safe_detach(client, n)

    def test_boolean_type_on_non_boolean_atom():
        trees = []
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            atom_node = find_node_by_value(_walk_nodes(client, root),
                                           "1", role="argument")["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": atom_node,
                "argument_type": "boolean",
                "argument_value": "true",
            })
            assert_error(r2)
            assert_in("Cannot change a non-boolean argument to boolean", tool_text(r2))
        finally:
            for n in trees:
                _safe_detach(client, n)

    def test_non_boolean_type_on_boolean_wrapper():
        trees = []
        try:
            r = client.call_tool("text_to_sexp",
                                 {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            wrapper_node = _find_bool_wrapper(
                _walk_nodes(client, root), "true")["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": wrapper_node,
                "argument_type": "number",
                "argument_value": "1",
            })
            assert_error(r2)
            assert_in("Cannot change a boolean argument", tool_text(r2))
        finally:
            for n in trees:
                _safe_detach(client, n)

    # -----------------------------------------------------------------------
    # Section 5: Structural error paths
    # -----------------------------------------------------------------------

    def test_node_out_of_range_negative():
        r = client.call_tool("update_sexp_node", {
            "node": -1,
            "operator_name": "-",
        })
        assert_error(r)

    def test_node_not_in_use():
        # Allocate a node then free it; the stale index should be rejected.
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "do-nothing",
        })
        assert_success(r)
        node = tool_data(r)["node"]

        client.call_tool("detach_sexp_node", {"node": node, "delete": True})

        r2 = client.call_tool("update_sexp_node", {
            "node": node,
            "operator_name": "-",
        })
        assert_error(r2)
        assert_in("not in use", tool_text(r2))

    def test_non_boolean_list_wrapper_rejected():
        trees = []
        try:
            # ( when ( true ) ( do-nothing ) ) contains two inner list_wrappers:
            # the boolean ( true ) one and the non-boolean ( do-nothing ) one.
            r = client.call_tool("text_to_sexp",
                                 {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            nodes = _walk_nodes(client, root)
            # Locate the do-nothing operator atom, then find its list_wrapper parent
            do_nothing_op = find_node_by_value(nodes, "do-nothing", role="operator")
            do_nothing_idx = do_nothing_op["node"]
            wrapper = None
            for n in nodes:
                if n.get("role") == "list_wrapper" and n.get("node_first") == do_nothing_idx:
                    wrapper = n
                    break
            assert_true(wrapper is not None,
                        "should find a non-boolean list_wrapper in tree")
            wrapper_node = wrapper["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": wrapper_node,
                "argument_type": "number",
                "argument_value": "5",
            })
            assert_error(r2)
            assert_in("not supported", tool_text(r2))
        finally:
            for n in trees:
                _safe_detach(client, n)

    def test_locked_boolean_node_rejected():
        trees = []
        try:
            r = client.call_tool("text_to_sexp",
                                 {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            # The "true" operator atom IS Locked_sexp_true; direct updates must fail.
            locked_op = find_node_by_value(_walk_nodes(client, root),
                                           "true", role="operator")
            assert_true(locked_op is not None,
                        "should find the locked 'true' operator node in the walk")
            locked_node = locked_op["node"]

            r2 = client.call_tool("update_sexp_node", {
                "node": locked_node,
                "operator_name": "-",
            })
            assert_error(r2)
            assert_in("locked boolean operator", tool_text(r2))
        finally:
            for n in trees:
                _safe_detach(client, n)

    # -----------------------------------------------------------------------
    # Section 6: Rollback on attached formula
    # -----------------------------------------------------------------------

    def test_rollback_on_syntax_error_attached():
        # Strategy: attach (when (= 1 1) (do-nothing)) to an event.
        # "=" returns OPR_BOOL, satisfying when's OPF_BOOL first-arg slot.
        # Renaming "=" to "+" makes the sub-expression return OPR_NUMBER,
        # which fails check_sexp_syntax for that slot -> rollback.
        events = []
        orphan_trees = []
        try:
            r = client.call_tool("create_event", {"name": "usn_rollback_evt"})
            assert_success(r)
            events.append("usn_rollback_evt")

            r = client.call_tool("text_to_sexp",
                                 {"text": "( when ( = 1 1 ) ( do-nothing ) )"})
            assert_success(r)
            formula = tool_data(r)["node"]
            orphan_trees.append(formula)  # freed by event deletion if attach succeeds

            # Attach formula to the event; displaced default formula is deleted.
            r = client.call_tool("attach_sexp_node", {
                "source_node": formula,
                "target_entity_type": "event",
                "target_entity_id": "usn_rollback_evt",
                "delete_displaced": True,
            })
            assert_success(r)
            orphan_trees.remove(formula)  # now owned by the event

            # Find the "=" operator inside the attached formula
            nodes = _walk_nodes(client, formula)
            eq_op = find_node_by_value(nodes, "=", role="operator")
            assert_true(eq_op is not None,
                        "should find '=' operator in the attached formula")
            eq_node = eq_op["node"]

            # Attempt to rename "=" to "+": produces OPR_NUMBER where OPF_BOOL expected
            r2 = client.call_tool("update_sexp_node", {
                "node": eq_node,
                "operator_name": "+",
            })
            assert_error(r2)
            assert_in("syntax error", tool_text(r2))

            # Rollback: "=" should be restored; "+" must not appear
            nodes2 = _walk_nodes(client, formula)
            assert_true(find_node_by_value(nodes2, "=", role="operator") is not None,
                        "'=' should be restored after rollback")
            assert_true(_find_node_or_none(nodes2, "+", role="operator") is None,
                        "'+' must not appear in the formula after rollback")
        finally:
            for name in events:
                _safe_delete_event(client, name)
            for n in orphan_trees:
                _safe_detach(client, n)

    # -----------------------------------------------------------------------
    # Registration
    # -----------------------------------------------------------------------

    tests = [
        ("sexp_update_node_operator_rename",           test_operator_rename),
        ("sexp_update_node_operator_unknown_name",     test_operator_unknown_name),
        ("sexp_update_node_operator_missing_name",     test_operator_missing_name_param),
        ("sexp_update_node_arg_number_value",          test_argument_update_number_value),
        ("sexp_update_node_arg_string_value",          test_argument_update_string_value),
        ("sexp_update_node_arg_reject_non_integer",    test_argument_reject_non_integer_number),
        ("sexp_update_node_arg_missing_value",         test_argument_missing_value_param),
        ("sexp_update_node_literal_to_variable",       test_literal_to_variable_reference),
        ("sexp_update_node_variable_to_literal",       test_variable_reference_to_literal),
        ("sexp_update_node_unknown_variable",          test_unknown_variable_reference),
        ("sexp_update_node_bool_true_to_false",        test_boolean_wrapper_true_to_false),
        ("sexp_update_node_bool_false_to_true",        test_boolean_wrapper_false_to_true),
        ("sexp_update_node_bool_invalid_value",        test_boolean_wrapper_invalid_value),
        ("sexp_update_node_bool_type_on_non_bool",     test_boolean_type_on_non_boolean_atom),
        ("sexp_update_node_non_bool_type_on_bool",     test_non_boolean_type_on_boolean_wrapper),
        ("sexp_update_node_out_of_range",              test_node_out_of_range_negative),
        ("sexp_update_node_not_in_use",                test_node_not_in_use),
        ("sexp_update_node_non_bool_wrapper_rejected", test_non_boolean_list_wrapper_rejected),
        ("sexp_update_node_locked_bool_rejected",      test_locked_boolean_node_rejected),
        ("sexp_update_node_rollback_on_syntax_error",  test_rollback_on_syntax_error_attached),
    ]
    for name, func in tests:
        suite.add(name, func)


if __name__ == "__main__":
    run_module_standalone(register, "update_sexp_node tests")
