"""All attach_sexp_node behavior tests.

Covers:
- Source validation (not in use, locked, not a root, already attached, out of range).
- Entity mode (Case A'): replacing an entity's formula with a free-standing subtree.
- Node-relative replace mode (Cases C'/D'): splicing a subtree into an embedded position.
- Node-relative insert-before/after modes: expanding a sibling chain.
- Rollback on syntax-check failure for mission-attached trees.
- Round-trip pairing with detach_sexp_node.
"""

from mcp_test_lib import (
    assert_equal,
    assert_error,
    assert_in,
    assert_success,
    assert_true,
    run_module_standalone,
    SkipTest,
    tool_data,
    tool_text,
)


def tree_signature(walk_nodes):
    """Snapshot a walk_sexp_tree result for structural comparison."""
    return [(n["node"], n.get("value"), n.get("node_parent"), n.get("node_rest"),
             n.get("node_first")) for n in walk_nodes]


def tree_values(walk_nodes, filter_empty=False):
    """Extract values list from a walk, optionally filtering empty wrapper entries."""
    vals = [n["value"] for n in walk_nodes]
    if filter_empty:
        vals = [v for v in vals if v]
    return vals


def register(suite, client):

    # =================================================================
    # Source validation
    # =================================================================

    def test_source_out_of_range():
        r = client.call_tool("attach_sexp_node", {"source_node": 99999, "target_node": 0})
        assert_error(r)

    def test_source_not_in_use():
        # Allocate and immediately free a node to get a NOT_USED index.
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "42",
        })
        assert_success(r)
        freed_node = tool_data(r)["node"]
        client.call_tool("detach_sexp_node", {"node": freed_node, "delete": True})
        r = client.call_tool("attach_sexp_node", {"source_node": freed_node, "target_node": 0})
        assert_error(r)
        assert_in("not in use", tool_text(r).lower())

    def test_source_locked():
        # Create a goal to get a Locked_sexp_true reference.
        r = client.call_tool("create_goal", {"name": "attach_locked_test"})
        assert_success(r)
        locked_node = tool_data(r).get("formula")
        r = client.call_tool("attach_sexp_node", {"source_node": locked_node, "target_node": 0})
        assert_error(r)
        assert_in("locked", tool_text(r).lower())
        client.call_tool("delete_goal", {"name": "attach_locked_test"})

    def test_source_not_a_root():
        # Create (+ 1 2), pass the "1" node as source.
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
        r = client.call_tool("walk_sexp_tree", {"node": root})
        assert_success(r)
        one_node = None
        for n in tool_data(r)["nodes"]:
            if n.get("value") == "1":
                one_node = n["node"]
                break
        assert_true(one_node is not None, "could not find '1' node")
        r = client.call_tool("attach_sexp_node", {"source_node": one_node, "target_node": root})
        assert_error(r)
        assert_in("free-standing root", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_source_already_attached():
        # Create an event — its formula root is attached to the event.
        r = client.call_tool("create_event", {"name": "attach_src_test"})
        assert_success(r)
        formula = tool_data(r).get("formula")
        # Try to attach the event's formula somewhere else.
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "99",
        })
        assert_success(r)
        target = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {"source_node": formula, "target_node": target})
        assert_error(r)
        assert_in("attached", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"node": target, "delete": True})
        client.call_tool("delete_event", {"name": "attach_src_test"})

    def test_must_specify_target():
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "1",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {"source_node": src})
        assert_error(r)
        assert_in("exactly one", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"node": src, "delete": True})

    suite.add("sexp_attach_source_out_of_range", test_source_out_of_range)
    suite.add("sexp_attach_source_not_in_use", test_source_not_in_use)
    suite.add("sexp_attach_source_locked", test_source_locked)
    suite.add("sexp_attach_source_not_a_root", test_source_not_a_root)
    suite.add("sexp_attach_source_already_attached", test_source_already_attached)
    suite.add("sexp_attach_must_specify_target", test_must_specify_target)

    # =================================================================
    # Entity mode (Case A')
    # =================================================================

    def test_entity_event_formula():
        # Create an event, detach its default formula (preserving it), then
        # build a new formula and attach it.
        r = client.call_tool("create_event", {"name": "attach_evt_test"})
        assert_success(r)
        old_formula = tool_data(r).get("formula")
        # Detach the default formula so it becomes an orphan.
        r = client.call_tool("detach_sexp_node", {"node": old_formula})
        assert_success(r)
        # Build a new formula.
        r = client.call_tool("text_to_sexp", {"text": "( when ( true ) ( do-nothing ) )"})
        assert_success(r)
        new_formula = tool_data(r)["node"]
        # Attach the new formula to the event.
        r = client.call_tool("attach_sexp_node", {
            "source_node": new_formula,
            "target_entity_type": "event",
            "target_entity_id": "attach_evt_test",
        })
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("source_node"), new_formula, "source_node in response")
        assert_equal(d.get("position"), "entity_formula", "position in response")
        # Verify the event now has the new formula.
        r = client.call_tool("get_event", {"name": "attach_evt_test"})
        assert_success(r)
        assert_equal(tool_data(r).get("formula"), new_formula, "event formula updated")
        # The displaced (do-nothing replacement from detach) should be returned.
        displaced = d.get("displaced_node")
        assert_true(displaced is not None, "displaced_node should be set")
        # Clean up.
        if displaced is not None and displaced != old_formula:
            client.call_tool("detach_sexp_node", {"node": displaced, "delete": True})
        client.call_tool("detach_sexp_node", {"node": old_formula, "delete": True})
        client.call_tool("delete_event", {"name": "attach_evt_test"})

    def test_entity_event_delete_displaced():
        r = client.call_tool("create_event", {"name": "attach_evt_del"})
        assert_success(r)
        old_formula = tool_data(r).get("formula")
        r = client.call_tool("text_to_sexp", {"text": "( when ( true ) ( do-nothing ) )"})
        assert_success(r)
        new_formula = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": new_formula,
            "target_entity_type": "event",
            "target_entity_id": "attach_evt_del",
            "delete_displaced": True,
        })
        assert_success(r)
        d = tool_data(r)
        assert_true(d.get("deleted_displaced") is True, "deleted_displaced should be true")
        assert_true(d.get("freed_count", 0) > 0, "freed_count should be > 0")
        # Old formula should be gone.
        r = client.call_tool("get_sexp_node", {"node": old_formula})
        assert_error(r)
        client.call_tool("delete_event", {"name": "attach_evt_del"})

    def test_entity_goal_formula():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "not",
            "operator_arguments": [{"argument_type": "boolean", "argument_value": "true"}],
        })
        assert_success(r)
        new_formula = tool_data(r)["node"]
        r = client.call_tool("create_goal", {"name": "attach_goal_test"})
        assert_success(r)
        r = client.call_tool("attach_sexp_node", {
            "source_node": new_formula,
            "target_entity_type": "goal",
            "target_entity_id": "attach_goal_test",
        })
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("position"), "entity_formula", "position")
        r = client.call_tool("get_goal", {"name": "attach_goal_test"})
        assert_success(r)
        assert_equal(tool_data(r).get("formula"), new_formula, "goal formula updated")
        client.call_tool("delete_goal", {"name": "attach_goal_test"})

    def test_entity_wrong_return_type():
        # do-nothing is OPR_NULL, goals expect OPR_BOOL.
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "do-nothing",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("create_goal", {"name": "attach_type_err"})
        assert_success(r)
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_entity_type": "goal",
            "target_entity_id": "attach_type_err",
        })
        assert_error(r)
        assert_in("return type", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"node": src, "delete": True})
        client.call_tool("delete_goal", {"name": "attach_type_err"})

    def test_entity_unknown_type():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "do-nothing",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_entity_type": "widget",
            "target_entity_id": "foo",
        })
        assert_error(r)
        client.call_tool("detach_sexp_node", {"node": src, "delete": True})

    def test_entity_nonexistent_id():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "do-nothing",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_entity_type": "event",
            "target_entity_id": "no_such_event_xyz",
        })
        assert_error(r)
        client.call_tool("detach_sexp_node", {"node": src, "delete": True})

    suite.add("sexp_attach_entity_event_formula", test_entity_event_formula)
    suite.add("sexp_attach_entity_event_delete_displaced", test_entity_event_delete_displaced)
    suite.add("sexp_attach_entity_goal_formula", test_entity_goal_formula)
    suite.add("sexp_attach_entity_wrong_return_type", test_entity_wrong_return_type)
    suite.add("sexp_attach_entity_unknown_type", test_entity_unknown_type)
    suite.add("sexp_attach_entity_nonexistent_id", test_entity_nonexistent_id)

    # =================================================================
    # Node-relative replace mode (Cases C'/D')
    # =================================================================

    def test_replace_placeholder_in_free_tree():
        # Build (+ 1 2 3), detach "2" (leaves placeholder), then attach a new
        # number "99" in the placeholder's slot.
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
        root = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": root})
        two_node = None
        for n in tool_data(r)["nodes"]:
            if n.get("value") == "2":
                two_node = n["node"]
                break
        assert_true(two_node is not None, "could not find '2' node")
        r = client.call_tool("detach_sexp_node", {"node": two_node})
        assert_success(r)
        placeholder = tool_data(r).get("replacement_node")
        assert_true(placeholder is not None, "expected a placeholder")
        # Create source node.
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "99",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        # Attach source at the placeholder's position.
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": placeholder,
        })
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("position"), "replace", "position")
        assert_true(d.get("displaced_node") is not None, "displaced_node should be set")
        # Walk the tree — should be + 1 99 3.
        r = client.call_tool("walk_sexp_tree", {"node": root})
        assert_success(r)
        vals = tree_values(tool_data(r)["nodes"])
        assert_equal(vals, ["+", "1", "99", "3"], "tree after replace")
        # Clean up.
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})
        client.call_tool("detach_sexp_node", {"node": two_node, "delete": True})

    def test_replace_with_delete_displaced():
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
        r = client.call_tool("walk_sexp_tree", {"node": root})
        two_node = None
        for n in tool_data(r)["nodes"]:
            if n.get("value") == "2":
                two_node = n["node"]
                break
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "99",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": two_node,
            "delete_displaced": True,
        })
        assert_success(r)
        d = tool_data(r)
        assert_true(d.get("deleted_displaced") is True, "deleted_displaced")
        assert_true(d.get("freed_count", 0) == 1, "freed_count == 1 for single atom")
        # The "2" node should be gone.
        r = client.call_tool("get_sexp_node", {"node": two_node})
        assert_error(r)
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_replace_free_standing_root_rejected():
        # Create two free-standing roots and try to replace one with the other.
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "1",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "2",
        })
        assert_success(r)
        target = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {"source_node": src, "target_node": target})
        assert_error(r)
        assert_in("free-standing root", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"node": src, "delete": True})
        client.call_tool("detach_sexp_node", {"node": target, "delete": True})

    def test_replace_rollback_on_syntax_error():
        # Create an event with the default formula ( when ( true ) ( do-nothing ) ).
        # Try to replace the ( do-nothing ) wrapper with a number "42". The when
        # operator expects OPF_NULL in the action slot; a number atom has
        # OPR_POSITIVE, so check_sexp_syntax rejects it and the handler must
        # roll back, leaving the tree byte-identical.
        r = client.call_tool("create_event", {"name": "attach_rollback_test"})
        assert_success(r)
        evt_formula = tool_data(r).get("formula")
        # Snapshot the tree and find the do-nothing operator.
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        sig_before = tree_signature(tool_data(r)["nodes"])
        donothing_node = None
        for n in tool_data(r)["nodes"]:
            if n.get("value") == "do-nothing" and n.get("role") == "operator":
                donothing_node = n["node"]
                break
        assert_true(donothing_node is not None, "could not find 'do-nothing' in default event formula")
        # Create a number atom — wrong type for an action slot.
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "42",
        })
        assert_success(r)
        bad_src = tool_data(r)["node"]
        # Try to replace do-nothing with "42". The retarget logic will redirect
        # to the wrapper around do-nothing, and the syntax check on the whole
        # formula should fail.
        r = client.call_tool("attach_sexp_node", {
            "source_node": bad_src,
            "target_node": donothing_node,
        })
        assert_error(r)
        assert_in("syntax", tool_text(r).lower())
        # Tree must be unchanged.
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        sig_after = tree_signature(tool_data(r)["nodes"])
        assert_equal(sig_before, sig_after, "tree unchanged after rollback")
        # Source must still be a free-standing root.
        r = client.call_tool("get_sexp_node", {"node": bad_src})
        assert_success(r)
        assert_equal(tool_data(r).get("node_parent"), -1, "source parent restored to -1")
        # Clean up.
        client.call_tool("detach_sexp_node", {"node": bad_src, "delete": True})
        client.call_tool("delete_event", {"name": "attach_rollback_test"})

    suite.add("sexp_attach_replace_placeholder_in_free_tree", test_replace_placeholder_in_free_tree)
    suite.add("sexp_attach_replace_with_delete_displaced", test_replace_with_delete_displaced)
    suite.add("sexp_attach_replace_free_standing_root_rejected", test_replace_free_standing_root_rejected)
    suite.add("sexp_attach_replace_rollback_on_syntax_error", test_replace_rollback_on_syntax_error)

    # =================================================================
    # Node-relative insert modes
    # =================================================================

    def test_insert_before_middle():
        # Build (+ 1 2 3), insert 99 before "2". Result: + 1 99 2 3.
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
        root = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": root})
        two_node = None
        for n in tool_data(r)["nodes"]:
            if n.get("value") == "2":
                two_node = n["node"]
                break
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "99",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": two_node,
            "position": "before",
        })
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("position"), "before", "position")
        assert_true(d.get("displaced_node") is None, "no displaced_node for insert")
        r = client.call_tool("walk_sexp_tree", {"node": root})
        vals = tree_values(tool_data(r)["nodes"])
        assert_equal(vals, ["+", "1", "99", "2", "3"], "tree after insert before")
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_insert_before_first_arg():
        # Build (+ 1 2), insert 99 before "1". Result: + 99 1 2.
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
        r = client.call_tool("walk_sexp_tree", {"node": root})
        one_node = None
        for n in tool_data(r)["nodes"]:
            if n.get("value") == "1":
                one_node = n["node"]
                break
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "99",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": one_node,
            "position": "before",
        })
        assert_success(r)
        r = client.call_tool("walk_sexp_tree", {"node": root})
        vals = tree_values(tool_data(r)["nodes"])
        assert_equal(vals, ["+", "99", "1", "2"], "tree after insert before first arg")
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_insert_after_last_arg():
        # Build (+ 1 2 3), insert 99 after "3". Result: + 1 2 3 99.
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
        root = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": root})
        three_node = None
        for n in tool_data(r)["nodes"]:
            if n.get("value") == "3":
                three_node = n["node"]
                break
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "99",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": three_node,
            "position": "after",
        })
        assert_success(r)
        r = client.call_tool("walk_sexp_tree", {"node": root})
        vals = tree_values(tool_data(r)["nodes"])
        assert_equal(vals, ["+", "1", "2", "3", "99"], "tree after insert after last")
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_insert_after_first_arg():
        # Build (+ 1 2 3), insert 99 after "1". Result: + 1 99 2 3.
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
        root = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": root})
        one_node = None
        for n in tool_data(r)["nodes"]:
            if n.get("value") == "1":
                one_node = n["node"]
                break
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "99",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": one_node,
            "position": "after",
        })
        assert_success(r)
        r = client.call_tool("walk_sexp_tree", {"node": root})
        vals = tree_values(tool_data(r)["nodes"])
        assert_equal(vals, ["+", "1", "99", "2", "3"], "tree after insert after first")
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_insert_before_root_rejected():
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "1",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        # Create a tree and try to insert before its root.
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "2"},
            ],
        })
        assert_success(r)
        root = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": root,
            "position": "before",
        })
        assert_error(r)
        client.call_tool("detach_sexp_node", {"node": src, "delete": True})
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_insert_after_root_rejected():
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "1",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "2"},
            ],
        })
        assert_success(r)
        root = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": root,
            "position": "after",
        })
        assert_error(r)
        client.call_tool("detach_sexp_node", {"node": src, "delete": True})
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    suite.add("sexp_attach_insert_before_middle", test_insert_before_middle)
    suite.add("sexp_attach_insert_before_first_arg", test_insert_before_first_arg)
    suite.add("sexp_attach_insert_after_last_arg", test_insert_after_last_arg)
    suite.add("sexp_attach_insert_after_first_arg", test_insert_after_first_arg)
    suite.add("sexp_attach_insert_before_root_rejected", test_insert_before_root_rejected)
    suite.add("sexp_attach_insert_after_root_rejected", test_insert_after_root_rejected)

    # =================================================================
    # Round-trip pairing with detach
    # =================================================================

    def test_detach_then_attach_restores_tree():
        # Build (+ 1 2 3), snapshot, detach "2" (default = placeholder),
        # then attach "2" back at the placeholder. Tree should match original.
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
        root = tool_data(r)["node"]
        r = client.call_tool("walk_sexp_tree", {"node": root})
        assert_success(r)
        original_vals = tree_values(tool_data(r)["nodes"])
        two_node = None
        for n in tool_data(r)["nodes"]:
            if n.get("value") == "2":
                two_node = n["node"]
                break
        # Detach "2" — leaves placeholder.
        r = client.call_tool("detach_sexp_node", {"node": two_node})
        assert_success(r)
        placeholder = tool_data(r).get("replacement_node")
        # Attach "2" back at the placeholder, deleting the placeholder.
        r = client.call_tool("attach_sexp_node", {
            "source_node": two_node,
            "target_node": placeholder,
            "delete_displaced": True,
        })
        assert_success(r)
        # Tree should be restored.
        r = client.call_tool("walk_sexp_tree", {"node": root})
        assert_success(r)
        restored_vals = tree_values(tool_data(r)["nodes"])
        assert_equal(restored_vals, original_vals, "tree restored after detach+attach round-trip")
        client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    suite.add("sexp_attach_detach_then_attach_restores_tree", test_detach_then_attach_restores_tree)


if __name__ == "__main__":
    run_module_standalone(register, "SEXP attach behavior tests")
