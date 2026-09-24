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
  * variable references typed by the variable's declared type: quoting
    that contradicts it is flagged as a syntax error, and node tools
    reject an argument_type that contradicts it

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


def _safe_delete_event(client, name):
    try:
        client.call_tool("delete_event", {"name": name})
    except Exception:
        pass


VARIABLE_TYPE_MISMATCH_TEXT = "does not match the variable's type"


def _create_typed_vars(client, vars_):
    """Create one string and one number variable for the type-mismatch scenarios."""
    for name, value, var_type in (("tsv_str", "unset", "string"), ("tsv_num", "0", "number")):
        r = client.call_tool("create_sexp_variable", {
            "name": name,
            "default_value": value,
            "variable_type": var_type
        })
        assert_success(r)
        vars_.append(name)


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

            # Reference node text is now <placeholder>, SEXP_FLAG_VARIABLE
            # is cleared, and the subtype is normalized to SEXP_ATOM_STRING
            # so value_type reads as "string_literal" rather than the
            # misleading "numeric_literal".
            nodes = _walk_nodes(client, root)
            ref = find_node_by_value(nodes, "<placeholder>", role="argument")
            assert_equal(ref.get("value_type"), "string_literal",
                         "after force-delete the subtype should be normalized to string")

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

    # ----- Scenario 8: create accepts leading '@' on name --------------------

    def test_create_variable_accepts_at_prefix():
        vars_ = []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "@tsv_at_create",
                "default_value": "0",
                "variable_type": "number",
            })
            assert_success(r)
            vars_.append("tsv_at_create")

            # The stored canonical name is bare.  list_sexp_variables and
            # get_sexp_variable both report it without the '@'.
            assert_in("tsv_at_create", _list_var_names(client),
                      "variable should be stored bare after stripping '@'")
            assert_true("@tsv_at_create" not in _list_var_names(client),
                        "stored name should not retain the '@' prefix")

            r = client.call_tool("get_sexp_variable", {"name": "tsv_at_create"})
            assert_success(r)
            assert_equal(tool_data(r).get("name"), "tsv_at_create",
                         "get by bare name should return the variable created with '@'")
        finally:
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 9: get accepts leading '@' on name -----------------------

    def test_get_variable_accepts_at_prefix():
        vars_ = []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "tsv_at_get",
                "default_value": "0",
                "variable_type": "number",
            })
            assert_success(r)
            vars_.append("tsv_at_get")

            r_bare = client.call_tool("get_sexp_variable", {"name": "tsv_at_get"})
            assert_success(r_bare)
            r_at = client.call_tool("get_sexp_variable", {"name": "@tsv_at_get"})
            assert_success(r_at)
            assert_equal(tool_data(r_at).get("name"), tool_data(r_bare).get("name"),
                         "get by '@name' and 'name' should return the same variable")
        finally:
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 10: update accepts leading '@' on name and new_name ------

    def test_update_variable_accepts_at_prefix():
        vars_ = []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "tsv_at_update",
                "default_value": "0",
                "variable_type": "number",
            })
            assert_success(r)
            vars_.append("tsv_at_update")

            # Update the default_value via '@' lookup
            r = client.call_tool("update_sexp_variable", {
                "name": "@tsv_at_update",
                "default_value": "42",
            })
            assert_success(r)
            r = client.call_tool("get_sexp_variable", {"name": "tsv_at_update"})
            assert_success(r)
            assert_equal(tool_data(r).get("default_value"), "42",
                         "update by '@name' should change default_value")

            # Rename via '@' on both name and new_name
            r = client.call_tool("update_sexp_variable", {
                "name": "@tsv_at_update",
                "new_name": "@tsv_at_renamed",
            })
            assert_success(r)
            vars_.remove("tsv_at_update")
            vars_.append("tsv_at_renamed")

            names = _list_var_names(client)
            assert_in("tsv_at_renamed", names,
                      "renamed variable should appear bare in list")
            assert_true("@tsv_at_renamed" not in names,
                        "renamed name should not retain the '@' prefix")
            assert_true("tsv_at_update" not in names,
                        "old name should be gone after rename")
        finally:
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 11: delete accepts leading '@' on name -------------------

    def test_delete_variable_accepts_at_prefix():
        vars_ = []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "tsv_at_delete",
                "default_value": "0",
                "variable_type": "number",
            })
            assert_success(r)
            vars_.append("tsv_at_delete")

            r = client.call_tool("delete_sexp_variable", {"name": "@tsv_at_delete"})
            assert_success(r)
            vars_.remove("tsv_at_delete")

            assert_true("tsv_at_delete" not in _list_var_names(client),
                        "variable should be gone after delete by '@name'")
        finally:
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 12: unquoted string variable in text_to_sexp -------------

    def test_text_to_sexp_unquoted_string_variable():
        trees, vars_ = [], []
        try:
            _create_typed_vars(client, vars_)

            r = client.call_tool("text_to_sexp", {
                "text": '( when ( true ) ( modify-variable @tsv_str "saved" ) )'
            })
            assert_success(r)
            d = tool_data(r)
            trees.append(d["node"])

            nodes = _walk_nodes(client, d["node"])
            ref = find_node_by_value(nodes, "tsv_str", role="argument")
            assert_equal(ref.get("value_type"), "string_variable",
                         "node type should come from the declared type, not the quoting")

            err = d.get("syntax_error")
            assert_true(err is not None, "contradicting quoting should be a syntax error")
            assert_in(VARIABLE_TYPE_MISMATCH_TEXT, err.get("error_message", ""))
            assert_equal(err.get("bad_node_text"), "tsv_str")

            assert_in('"@tsv_str[', d.get("parsed_text", ""),
                      "round-tripped text should quote the string variable")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 13: quoted number variable in text_to_sexp ---------------

    def test_text_to_sexp_quoted_number_variable():
        trees, vars_ = [], []
        try:
            _create_typed_vars(client, vars_)

            r = client.call_tool("text_to_sexp", {
                "text": '( when ( true ) ( modify-variable "@tsv_num" 5 ) )'
            })
            assert_success(r)
            d = tool_data(r)
            trees.append(d["node"])

            nodes = _walk_nodes(client, d["node"])
            ref = find_node_by_value(nodes, "tsv_num", role="argument")
            assert_equal(ref.get("value_type"), "numeric_variable",
                         "node type should come from the declared type, not the quoting")

            err = d.get("syntax_error")
            assert_true(err is not None, "contradicting quoting should be a syntax error")
            assert_in(VARIABLE_TYPE_MISMATCH_TEXT, err.get("error_message", ""))
            assert_equal(err.get("bad_node_text"), "tsv_num")

            parsed = d.get("parsed_text", "")
            assert_in("@tsv_num[", parsed)
            assert_true('"@tsv_num[' not in parsed,
                        "round-tripped text should not quote the number variable")

            # The mismatch keeps the formula from being attached
            r = client.call_tool("create_event", {"name": "tsv_mismatch_evt", "formula": d["node"]})
            assert_error(r)
            assert_in("syntax error", tool_text(r))
        finally:
            _safe_delete_event(client, "tsv_mismatch_evt")
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 14: correct quoting parses cleanly -----------------------

    def test_text_to_sexp_correct_variable_quoting():
        trees, vars_ = [], []
        try:
            _create_typed_vars(client, vars_)

            for text in ('( when ( true ) ( modify-variable "@tsv_str" "saved" ) )',
                         '( when ( true ) ( modify-variable @tsv_num 5 ) )'):
                r = client.call_tool("text_to_sexp", {"text": text})
                assert_success(r)
                d = tool_data(r)
                trees.append(d["node"])
                assert_true(d.get("syntax_error") is None,
                            f"correctly quoted variable should not be a syntax error: {d.get('syntax_error')}")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 15: update_sexp_node clears a parsed mismatch ------------

    def test_update_node_fixes_parsed_mismatch():
        trees, vars_, events = [], [], []
        try:
            _create_typed_vars(client, vars_)

            r = client.call_tool("text_to_sexp", {
                "text": '( when ( true ) ( modify-variable "@tsv_num" 5 ) )'
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            # Rewriting the reference with the matching argument_type clears the mismatch
            ref = find_node_by_value(_walk_nodes(client, root), "tsv_num", role="argument")
            r = client.call_tool("update_sexp_node", {
                "node": ref["node"],
                "argument_type": "number",
                "argument_value": "@tsv_num",
            })
            assert_success(r)

            r = client.call_tool("create_event", {"name": "tsv_fixed_evt", "formula": root})
            assert_success(r)
            events.append("tsv_fixed_evt")
            trees.remove(root)  # owned by the event now
        finally:
            for e in events:
                _safe_delete_event(client, e)
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 16: node tools reject a contradicting argument_type ------

    def test_node_tools_reject_contradicting_argument_type():
        trees, vars_ = [], []
        try:
            _create_typed_vars(client, vars_)

            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "string-equals",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "@tsv_str"},
                    {"argument_type": "string", "argument_value": "saved"},
                ],
            })
            assert_error(r)
            assert_in("is a string variable", tool_text(r))

            r = client.call_tool("create_sexp_node", {
                "role": "operator",
                "operator_name": "+",
                "operator_arguments": [
                    {"argument_type": "number", "argument_value": "@tsv_num"},
                    {"argument_type": "number", "argument_value": "1"},
                ],
            })
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            ref = find_node_by_value(_walk_nodes(client, root), "tsv_num", role="argument")
            r = client.call_tool("update_sexp_node", {
                "node": ref["node"],
                "argument_type": "string",
                "argument_value": "@tsv_num",
            })
            assert_error(r)
            assert_in("is a number variable", tool_text(r))

            ref = find_node_by_value(_walk_nodes(client, root), "tsv_num", role="argument")
            assert_equal(ref.get("value_type"), "numeric_variable",
                         "a rejected update should leave the node unchanged")
        finally:
            for n in trees:
                _safe_detach(client, n)
            for v in vars_:
                _safe_delete_var(client, v)

    # ----- Scenario 17: changing a variable's type retypes references --------

    def test_type_change_retypes_references():
        trees, vars_ = [], []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "tsv_retype",
                "default_value": "0",
                "variable_type": "number"
            })
            assert_success(r)
            vars_.append("tsv_retype")

            r = client.call_tool("text_to_sexp", {"text": "( modify-variable @tsv_retype 5 )"})
            assert_success(r)
            root = tool_data(r)["node"]
            trees.append(root)

            r = client.call_tool("update_sexp_variable", {
                "name": "tsv_retype",
                "variable_type": "string",
            })
            assert_success(r)

            ref = find_node_by_value(_walk_nodes(client, root), "tsv_retype", role="argument")
            assert_equal(ref.get("value_type"), "string_variable",
                         "reference should follow the variable's new type")

            r = client.call_tool("sexp_to_text", {"node": root})
            assert_success(r)
            assert_in('"@tsv_retype[', tool_text(r),
                      "sexp_to_text should now quote the reference")
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
        ("sexp_variables_create_accepts_at_prefix",
         test_create_variable_accepts_at_prefix),
        ("sexp_variables_get_accepts_at_prefix",
         test_get_variable_accepts_at_prefix),
        ("sexp_variables_update_accepts_at_prefix",
         test_update_variable_accepts_at_prefix),
        ("sexp_variables_delete_accepts_at_prefix",
         test_delete_variable_accepts_at_prefix),
        ("sexp_variables_text_to_sexp_unquoted_string_variable",
         test_text_to_sexp_unquoted_string_variable),
        ("sexp_variables_text_to_sexp_quoted_number_variable",
         test_text_to_sexp_quoted_number_variable),
        ("sexp_variables_text_to_sexp_correct_variable_quoting",
         test_text_to_sexp_correct_variable_quoting),
        ("sexp_variables_update_node_fixes_parsed_mismatch",
         test_update_node_fixes_parsed_mismatch),
        ("sexp_variables_node_tools_reject_contradicting_argument_type",
         test_node_tools_reject_contradicting_argument_type),
        ("sexp_variables_type_change_retypes_references",
         test_type_change_retypes_references),
    ]
    for name, func in tests:
        suite.add(name, func)


if __name__ == "__main__":
    run_module_standalone(register, "SEXP variable-reference tests")
