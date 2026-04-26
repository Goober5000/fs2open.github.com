"""SEXP-variable-reference-inside-tree tests.

Covers the interaction between SEXP variables (managed by
create/update/delete_sexp_variable) and SEXP nodes that reference them
via the SEXP_FLAG_VARIABLE bit.  The variable-CRUD tool surface itself
is covered in test_crud.py; this file focuses specifically on:

  * Building a node that references a variable (numeric_variable /
    string_variable value_type)
  * text_to_sexp / sexp_to_text round-trip with variable atoms
  * delete_sexp_variable refusing to drop a referenced variable unless
    force=true, and resetting references to <placeholder> when forced
  * update_sexp_variable rename propagating to every referencing node

Each scenario owns its setup and uses a try/finally cleanup list, so a
failure in one scenario does not leak state into the next.
"""

from mcp_test_lib import (
    assert_equal,
    assert_error,
    assert_in,
    assert_is_list,
    assert_success,
    assert_true,
    find_node_by_value,
    run_module_standalone,
    tool_data,
    tool_text,
)


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


def _list_var_names(client):
    r = client.call_tool("list_sexp_variables")
    assert_success(r)
    d = tool_data(r)
    assert_is_list(d)
    return [v.get("name") for v in d]


def register(suite, client):

    # ----- Scenario 1: create_sexp_node with @varname argument ---------------

    def test_create_node_with_variable_arg():
        trees, vars_ = [], []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "tsv_score",
                "default_value": "0",
                "variable_type": "number"
            })
            assert_success(r)
            vars_.append("tsv_score")

            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "@tsv_score"},
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            nodes = _walk_nodes(client, root)
            ref = find_node_by_value(nodes, "tsv_score", role="argument")
            assert_equal(ref.get("value_type"), "numeric_variable",
                         "variable atom should be numeric_variable")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 2: unknown variable reference is rejected ----------------

    def test_create_node_with_unknown_variable():
        baseline = _list_var_names(client)

        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "@tsv_phantom"},
                {"argument_type": "number", "argument_value": "1"},
            ],
        })
        assert_error(r)
        assert_in("Unknown SEXP variable", tool_text(r))

        # Variable list must be unchanged (no side effects from the failed call)
        assert_equal(_list_var_names(client), baseline,
                     "list_sexp_variables changed across a failed create_sexp_node")

    # ----- Scenario 3: text_to_sexp / sexp_to_text round-trip ----------------

    def test_text_to_sexp_roundtrip_with_variable():
        trees, vars_ = [], []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "tsv_score",
                "default_value": "0",
                "variable_type": "number"
            })
            assert_success(r)
            vars_.append("tsv_score")

            r = client.call_tool("text_to_sexp",
                                 {"text": "( + @tsv_score[0] 1 )"})
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            nodes = _walk_nodes(client, root)
            ref = find_node_by_value(nodes, "tsv_score", role="argument")
            assert_equal(ref.get("value_type"), "numeric_variable",
                         "parsed variable atom should be numeric_variable")

            r = client.call_tool("sexp_to_text", {"node": root})
            assert_success(r)
            text = tool_text(r)
            assert_in("@tsv_score[", text,
                      "sexp_to_text should emit @varname[ ... ] syntax")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 4: string variable in a tree -----------------------------

    def test_string_variable_in_tree():
        trees, vars_ = [], []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "tsv_msg",
                "default_value": "hello",
                "variable_type": "string"
            })
            assert_success(r)
            vars_.append("tsv_msg")

            # string-equals takes OPF_STRING args; a string variable passes
            # through is_arg_type_compatible's "data OPF accepts strings" branch.
            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "string-equals",
                "operator_arguments": [
                    {"argument_type": "string", "argument_value": "@tsv_msg"},
                    {"argument_type": "string", "argument_value": "hello"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            nodes = _walk_nodes(client, root)
            ref = find_node_by_value(nodes, "tsv_msg", role="argument")
            assert_equal(ref.get("value_type"), "string_variable",
                         "variable atom should be string_variable")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 5: delete refused without force when referenced ---------

    def test_delete_refused_without_force():
        trees, vars_ = [], []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "tsv_score",
                "default_value": "0",
                "variable_type": "number"
            })
            assert_success(r)
            vars_.append("tsv_score")

            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "@tsv_score"},
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            r = client.call_tool("delete_sexp_variable", {"name": "tsv_score"})
            assert_error(r)
            assert_in("force=true", tool_text(r),
                      "error message should mention force=true")

            # Variable must still exist after the refused delete
            assert_in("tsv_score", _list_var_names(client),
                      "variable should still be present after refused delete")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 6: delete with force resets references -------------------

    def test_delete_with_force_resets_references():
        trees, vars_ = [], []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "tsv_score",
                "default_value": "0",
                "variable_type": "number"
            })
            assert_success(r)
            vars_.append("tsv_score")

            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "@tsv_score"},
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            r = client.call_tool("delete_sexp_variable",
                                 {"name": "tsv_score", "force": True})
            assert_success(r)
            vars_.remove("tsv_score")  # already gone

            # Reference node text is now <placeholder> and the
            # SEXP_FLAG_VARIABLE bit has been cleared.  The subtype is
            # left as SEXP_ATOM_NUMBER, so value_type reads as
            # "numeric_literal" — a known oddity (a non-numeric string in
            # a number-typed atom) tracked separately from this test.
            nodes = _walk_nodes(client, root)
            ref = find_node_by_value(nodes, "<placeholder>", role="argument")
            assert_equal(ref.get("value_type"), "numeric_literal",
                         "after force-delete the variable bit should be cleared")

            assert_true("tsv_score" not in _list_var_names(client),
                        "variable should be gone after force-delete")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 7: rename propagates to references -----------------------

    def test_rename_propagates_to_references():
        trees, vars_ = [], []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "tsv_score",
                "default_value": "0",
                "variable_type": "number"
            })
            assert_success(r)
            vars_.append("tsv_score")

            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "@tsv_score"},
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            r = client.call_tool("update_sexp_variable", {
                "name": "tsv_score",
                "new_name": "tsv_points",
            })
            assert_success(r)
            vars_.remove("tsv_score")
            vars_.append("tsv_points")

            nodes = _walk_nodes(client, root)
            ref = find_node_by_value(nodes, "tsv_points", role="argument")
            assert_equal(ref.get("value_type"), "numeric_variable",
                         "renamed reference should still be a numeric_variable")

            r = client.call_tool("sexp_to_text", {"node": root})
            assert_success(r)
            assert_in("@tsv_points[", tool_text(r),
                      "sexp_to_text should emit the renamed variable")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    tests = [
        ("sexp_variables_create_node_with_variable_arg",
         test_create_node_with_variable_arg),
        ("sexp_variables_create_node_with_unknown_variable",
         test_create_node_with_unknown_variable),
        ("sexp_variables_text_to_sexp_roundtrip",
         test_text_to_sexp_roundtrip_with_variable),
        ("sexp_variables_string_variable_in_tree",
         test_string_variable_in_tree),
        ("sexp_variables_delete_refused_without_force",
         test_delete_refused_without_force),
        ("sexp_variables_delete_with_force_resets_references",
         test_delete_with_force_resets_references),
        ("sexp_variables_rename_propagates_to_references",
         test_rename_propagates_to_references),
    ]
    for name, func in tests:
        suite.add(name, func)


if __name__ == "__main__":
    run_module_standalone(register, "SEXP variable-reference tests")
