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
    find_node_by_value,
    run_module_standalone,
    tool_data,
    tool_text,
    tree_signature,
    tree_values,
)


def register(suite, client):

    # =================================================================
    # Source validation
    # =================================================================

    def test_source_out_of_range():
        r = client.call_tool("attach_sexp_node", {"source_node": 99999, "target_node": 0})
        assert_error(r)
        assert_in("out of range", tool_text(r).lower())

    def test_source_not_in_use():
        # Create the target first so it doesn't reuse the freed node's slot.
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 )"})
        assert_success(r)
        target = tool_data(r)["node"]
        try:
            # Allocate and immediately free a node to get a NOT_USED index.
            r = client.call_tool("create_sexp_node", {
                "role": "argument",
                "argument_type": "number",
                "argument_value": "42",
            })
            assert_success(r)
            freed_node = tool_data(r)["node"]
            client.call_tool("detach_sexp_node", {"target_node": freed_node, "delete": True})

            r = client.call_tool("attach_sexp_node", {"source_node": freed_node, "target_node": target})
            assert_error(r)
            assert_in("not in use", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node", {"target_node": target, "delete": True})

    def test_attach_locked_false_to_entity():
        # Locked singletons (true/false) are valid attach sources.
        # Entity mode just stores the node index without modifying fields.
        r = client.call_tool("create_debriefing_stage", {"text": "locked test"})
        assert_success(r)
        # Get the Locked_sexp_false singleton.
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "false",
        })
        assert_success(r)
        false_node = tool_data(r)["node"]
        # Attach false to the debriefing stage (OPR_BOOL).
        r = client.call_tool("attach_sexp_node", {
            "source_node": false_node,
            "target_entity_type": "debriefing_stage",
            "target_entity_id": "1",
        })
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("position"), "entity_formula", "position")
        assert_equal(d.get("source_node"), false_node, "source_node echoed")
        # Clean up.
        client.call_tool("delete_debriefing_stage", {"index": 1})

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
        one_node = find_node_by_value(tool_data(r)["nodes"], "1")["node"]
        r = client.call_tool("attach_sexp_node", {"source_node": one_node, "target_node": root})
        assert_error(r)
        assert_in("free-standing root", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

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
        client.call_tool("detach_sexp_node", {"target_node": target, "delete": True})
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
        client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})

    def test_both_targets_specified():
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "1",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": 0,
            "target_entity_type": "event",
            "target_entity_id": "foo",
        })
        assert_error(r)
        assert_in("exactly one", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})

    suite.add("sexp_attach_source_out_of_range", test_source_out_of_range)
    suite.add("sexp_attach_source_not_in_use", test_source_not_in_use)
    suite.add("sexp_attach_locked_false_to_entity", test_attach_locked_false_to_entity)
    suite.add("sexp_attach_source_not_a_root", test_source_not_a_root)
    suite.add("sexp_attach_source_already_attached", test_source_already_attached)
    suite.add("sexp_attach_must_specify_target", test_must_specify_target)
    suite.add("sexp_attach_both_targets_specified", test_both_targets_specified)

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
        r = client.call_tool("detach_sexp_node", {"target_node": old_formula})
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
            client.call_tool("detach_sexp_node", {"target_node": displaced, "delete": True})
        client.call_tool("detach_sexp_node", {"target_node": old_formula, "delete": True})
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
        # Default event formula "( when ( true ) ( do-nothing ) )" frees 4 nodes:
        # the when operator, two list wrappers, and do-nothing.  The locked
        # singleton "true" is shared and not freed.
        assert_equal(d.get("freed_count"), 4, "freed_count for default event formula")
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
        client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})
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
        assert_in("invalid value", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})

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
        assert_in("not found", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})

    def test_entity_debriefing_stage_default_tag():
        # Create a debriefing stage on the default team (team 1).
        r = client.call_tool("create_debriefing_stage", {"text": "attach debrief test"})
        assert_success(r)
        # Build a (not (true)) formula — avoids the Locked_sexp_true singleton.
        r = client.call_tool("text_to_sexp", {"text": "( not ( true ) )"})
        assert_success(r)
        src = tool_data(r)["node"]
        # Attach to the debriefing stage (team_1 is the default).
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_entity_type": "debriefing_stage",
            "target_entity_id": "1",
        })
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("position"), "entity_formula", "position")
        assert_equal(d.get("source_node"), src, "source_node echoed")
        # Clean up.
        client.call_tool("delete_debriefing_stage", {"index": 1})

    def test_entity_debriefing_stage_team_2():
        # Create a debriefing stage on team 2.
        r = client.call_tool("create_debriefing_stage", {"text": "attach debrief t2", "team": "Team 2"})
        assert_success(r)
        # Build a (not (true)) formula — avoids the Locked_sexp_true singleton.
        r = client.call_tool("text_to_sexp", {"text": "( not ( true ) )"})
        assert_success(r)
        src = tool_data(r)["node"]
        # Attach via entity_tag="team_2".
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_entity_type": "debriefing_stage",
            "target_entity_id": "1",
            "entity_tag": "team_2",
        })
        assert_success(r)
        assert_equal(tool_data(r).get("position"), "entity_formula", "position")
        # Clean up.
        client.call_tool("delete_debriefing_stage", {"index": 1, "team": "Team 2"})

    def test_entity_fiction_viewer_stage():
        # Create a fiction viewer stage.
        r = client.call_tool("create_fiction_viewer_stage", {"story_filename": "test_attach.txt"})
        assert_success(r)
        # Build a (not (true)) formula — avoids the Locked_sexp_true singleton.
        r = client.call_tool("text_to_sexp", {"text": "( not ( true ) )"})
        assert_success(r)
        src = tool_data(r)["node"]
        # Attach to the fiction viewer stage.
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_entity_type": "fiction_viewer_stage",
            "target_entity_id": "1",
        })
        assert_success(r)
        assert_equal(tool_data(r).get("position"), "entity_formula", "position")
        # Clean up.
        client.call_tool("delete_fiction_viewer_stage", {"index": 1})

    def test_entity_rollback_preserves_formula():
        # Create a goal — default formula is Locked_sexp_true.
        r = client.call_tool("create_goal", {"name": "attach_rb_goal"})
        assert_success(r)
        original_formula = tool_data(r).get("formula")
        # Build a do-nothing operator (OPR_NULL — wrong for OPR_BOOL goal).
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "do-nothing",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        # Attempt attach — should fail on return type mismatch.
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_entity_type": "goal",
            "target_entity_id": "attach_rb_goal",
        })
        assert_error(r)
        # Verify the goal's formula is unchanged.
        r = client.call_tool("get_goal", {"name": "attach_rb_goal"})
        assert_success(r)
        assert_equal(tool_data(r).get("formula"), original_formula, "goal formula unchanged after failed attach")
        # Clean up.
        client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})
        client.call_tool("delete_goal", {"name": "attach_rb_goal"})

    suite.add("sexp_attach_entity_event_formula", test_entity_event_formula)
    suite.add("sexp_attach_entity_event_delete_displaced", test_entity_event_delete_displaced)
    suite.add("sexp_attach_entity_goal_formula", test_entity_goal_formula)
    suite.add("sexp_attach_entity_wrong_return_type", test_entity_wrong_return_type)
    suite.add("sexp_attach_entity_unknown_type", test_entity_unknown_type)
    suite.add("sexp_attach_entity_nonexistent_id", test_entity_nonexistent_id)
    suite.add("sexp_attach_entity_debriefing_default_tag", test_entity_debriefing_stage_default_tag)
    suite.add("sexp_attach_entity_debriefing_team_2", test_entity_debriefing_stage_team_2)
    suite.add("sexp_attach_entity_fiction_viewer_stage", test_entity_fiction_viewer_stage)
    suite.add("sexp_attach_entity_rollback_preserves_formula", test_entity_rollback_preserves_formula)

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
        two_node = None
        try:
            r = client.call_tool("walk_sexp_tree", {"node": root})
            two_node = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            r = client.call_tool("detach_sexp_node", {"target_node": two_node})
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
        finally:
            client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})
            if two_node is not None:
                client.call_tool("detach_sexp_node", {"target_node": two_node, "delete": True})

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
        two_node = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
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
        client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

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
        client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})
        client.call_tool("detach_sexp_node", {"target_node": target, "delete": True})

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
        donothing_node = find_node_by_value(tool_data(r)["nodes"],
                                            "do-nothing", role="operator")["node"]
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
        client.call_tool("detach_sexp_node", {"target_node": bad_src, "delete": True})
        client.call_tool("delete_event", {"name": "attach_rollback_test"})

    def test_attach_subtree_source():
        # Build (+ 3 4) as source subtree.  create_sexp_node returns the
        # operator atom; the handler auto-wraps it in a list node when
        # splicing into a rest chain, preserving the argument chain.
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "3"},
                {"argument_type": "number", "argument_value": "4"},
            ],
        })
        assert_success(r)
        src = tool_data(r)["node"]
        # Build (* 1 2) as the target tree.
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "*",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
            ],
        })
        assert_success(r)
        root = tool_data(r)["node"]
        # Find the "2" node.
        r = client.call_tool("walk_sexp_tree", {"node": root})
        assert_success(r)
        two_node = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
        # Replace "2" with the (+ 3 4) subtree.
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": two_node,
        })
        assert_success(r)
        # Walk and verify: * 1 (+ 3 4) — the auto-created list wrapper has an
        # empty value, so filter it out.
        r = client.call_tool("walk_sexp_tree", {"node": root})
        assert_success(r)
        vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
        assert_equal(vals, ["*", "1", "+", "3", "4"], "tree after subtree replace")
        # Clean up.
        client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})
        client.call_tool("detach_sexp_node", {"target_node": two_node, "delete": True})

    def test_replace_live_node_preserve_displaced():
        # Build (+ 1 2), replace live "2" with "99" without delete_displaced.
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
        two_node = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "99",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        # Replace without deleting displaced.
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": two_node,
        })
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("displaced_node"), two_node, "displaced_node is the replaced node")
        assert_true(d.get("displaced_node_data") is not None, "displaced_node_data present")
        assert_true(d.get("deleted_displaced") is False, "deleted_displaced is false")
        # Displaced node should still be accessible as a free-standing root.
        r = client.call_tool("get_sexp_node", {"node": two_node})
        assert_success(r)
        assert_equal(tool_data(r).get("node_parent"), -1, "displaced node parent is -1")
        # Tree should be + 1 99.
        r = client.call_tool("walk_sexp_tree", {"node": root})
        assert_success(r)
        vals = tree_values(tool_data(r)["nodes"])
        assert_equal(vals, ["+", "1", "99"], "tree after replace")
        # Clean up.
        client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})
        client.call_tool("detach_sexp_node", {"target_node": two_node, "delete": True})

    suite.add("sexp_attach_replace_placeholder_in_free_tree", test_replace_placeholder_in_free_tree)
    suite.add("sexp_attach_replace_with_delete_displaced", test_replace_with_delete_displaced)
    suite.add("sexp_attach_replace_free_standing_root_rejected", test_replace_free_standing_root_rejected)
    suite.add("sexp_attach_replace_rollback_on_syntax_error", test_replace_rollback_on_syntax_error)
    def test_attach_locked_true_replace_embedded():
        # Replace the (true) condition inside an event's when formula with (false).
        # Since true is a locked singleton, we must use target_argument_index
        # to address it via its parent operator.
        r = client.call_tool("create_event", {"name": "attach_locked_repl"})
        assert_success(r)
        evt_formula = tool_data(r).get("formula")
        # The event formula is ( when ( true ) ( do-nothing ) ).
        # The root 'when' operator is evt_formula; true is argument 0.
        # Get Locked_sexp_false.
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "false",
        })
        assert_success(r)
        false_node = tool_data(r)["node"]
        # Replace argument 1 of the when operator (the true condition) with false.
        r = client.call_tool("attach_sexp_node", {
            "source_node": false_node,
            "target_node": evt_formula,
            "target_argument_index": 1,
        })
        assert_success(r)
        assert_equal(tool_data(r).get("position"), "replace", "position")
        # Walk and verify false appears where true was.
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
        assert_in("false", vals, "false should appear in tree")
        assert_true("true" not in vals, "true should not appear in tree")
        # Clean up.
        client.call_tool("delete_event", {"name": "attach_locked_repl"})

    suite.add("sexp_attach_subtree_source", test_attach_subtree_source)
    suite.add("sexp_attach_replace_live_node_preserve_displaced", test_replace_live_node_preserve_displaced)
    suite.add("sexp_attach_locked_true_replace_embedded", test_attach_locked_true_replace_embedded)

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
        try:
            r = client.call_tool("walk_sexp_tree", {"node": root})
            two_node = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
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
        finally:
            client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

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
        one_node = find_node_by_value(tool_data(r)["nodes"], "1")["node"]
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
        client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

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
        three_node = find_node_by_value(tool_data(r)["nodes"], "3")["node"]
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
        client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

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
        one_node = find_node_by_value(tool_data(r)["nodes"], "1")["node"]
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
        client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

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
        assert_in("free-standing root", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})
        client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

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
        assert_in("free-standing root", tool_text(r).lower())
        client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})
        client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

    def test_insert_before_in_attached_tree():
        # Create an event with default formula ( when ( true ) ( do-nothing ) ).
        r = client.call_tool("create_event", {"name": "attach_ins_evt"})
        assert_success(r)
        evt_formula = tool_data(r).get("formula")
        # Walk to find the do-nothing operator in the event formula.
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        donothing_op = find_node_by_value(tool_data(r)["nodes"],
                                          "do-nothing", role="operator")["node"]
        # Build a second do-nothing operator.  The handler auto-wraps operator
        # atoms in list wrappers when inserting into rest chains.
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "do-nothing",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        # Insert before the existing do-nothing.
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": donothing_op,
            "position": "before",
        })
        assert_success(r)
        assert_equal(tool_data(r).get("position"), "before", "position")
        # Walk and verify that "do-nothing" appears twice.
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
        dn_count = vals.count("do-nothing")
        assert_equal(dn_count, 2, "two do-nothing operators after insert")
        # Clean up.
        client.call_tool("delete_event", {"name": "attach_ins_evt"})

    def test_insert_after_rollback_in_attached_tree():
        # Create an event with default formula.
        r = client.call_tool("create_event", {"name": "attach_ins_rb"})
        assert_success(r)
        evt_formula = tool_data(r).get("formula")
        # Snapshot the tree.
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        sig_before = tree_signature(tool_data(r)["nodes"])
        # Find the do-nothing operator node.
        donothing_node = find_node_by_value(tool_data(r)["nodes"],
                                            "do-nothing", role="operator")["node"]
        # Create a number atom — invalid as a sibling in the when's action list.
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "42",
        })
        assert_success(r)
        bad_src = tool_data(r)["node"]
        # Try to insert after the do-nothing wrapper — syntax check should fail.
        r = client.call_tool("attach_sexp_node", {
            "source_node": bad_src,
            "target_node": donothing_node,
            "position": "after",
        })
        assert_error(r)
        assert_in("syntax", tool_text(r).lower())
        # Tree must be unchanged.
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        sig_after = tree_signature(tool_data(r)["nodes"])
        assert_equal(sig_before, sig_after, "tree unchanged after rollback")
        # Source must still be free-standing.
        r = client.call_tool("get_sexp_node", {"node": bad_src})
        assert_success(r)
        assert_equal(tool_data(r).get("node_parent"), -1, "source parent restored to -1")
        # Clean up.
        client.call_tool("detach_sexp_node", {"target_node": bad_src, "delete": True})
        client.call_tool("delete_event", {"name": "attach_ins_rb"})

    suite.add("sexp_attach_insert_before_middle", test_insert_before_middle)
    suite.add("sexp_attach_insert_before_first_arg", test_insert_before_first_arg)
    suite.add("sexp_attach_insert_after_last_arg", test_insert_after_last_arg)
    suite.add("sexp_attach_insert_after_first_arg", test_insert_after_first_arg)
    suite.add("sexp_attach_insert_before_root_rejected", test_insert_before_root_rejected)
    suite.add("sexp_attach_insert_after_root_rejected", test_insert_after_root_rejected)
    def test_attach_locked_false_insert_in_attached():
        # Insert Locked_sexp_false after the do-nothing in a when's action list.
        # This exercises insert-after with a locked singleton source on an
        # attached tree.  We target the do-nothing (not a locked singleton) to
        # avoid parent-chain issues with locked singletons having parent == -1.
        r = client.call_tool("create_event", {"name": "attach_locked_ins"})
        assert_success(r)
        evt_formula = tool_data(r).get("formula")
        # Walk to find the do-nothing operator.
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        donothing_node = find_node_by_value(tool_data(r)["nodes"],
                                            "do-nothing", role="operator")["node"]
        # Get Locked_sexp_false.  It has OPR_BOOL return type, not OPR_NULL,
        # so inserting it after do-nothing in a when's action list should fail
        # syntax check and be rolled back.  This verifies rollback works for
        # locked singleton sources.
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "false",
        })
        assert_success(r)
        false_node = tool_data(r)["node"]
        # Insert false after do-nothing — should fail syntax check.
        r = client.call_tool("attach_sexp_node", {
            "source_node": false_node,
            "target_node": donothing_node,
            "position": "after",
        })
        assert_error(r)
        assert_in("syntax", tool_text(r).lower())
        # Verify tree is unchanged after rollback.
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
        assert_true("false" not in vals, "false should not appear after rollback")
        # Clean up.
        client.call_tool("delete_event", {"name": "attach_locked_ins"})

    suite.add("sexp_attach_insert_before_in_attached_tree", test_insert_before_in_attached_tree)
    suite.add("sexp_attach_insert_after_rollback_in_attached_tree", test_insert_after_rollback_in_attached_tree)
    suite.add("sexp_attach_locked_false_insert_in_attached", test_attach_locked_false_insert_in_attached)

    def _wrap_rollback_helper(position):
        """Exercise the wrapped-source rollback path for a given position.

        Builds a (+ 1 2) operator source (a bare operator atom, so need_wrap
        fires) and tries to splice it into an event's action list where the
        syntax check rejects it (+ returns OPR_POSITIVE, action list expects
        OPR_NULL).  Verifies rollback restores the tree and source parent.
        """
        r = client.call_tool("create_event", {"name": "wrap_rb_test"})
        assert_success(r)
        evt_formula = tool_data(r).get("formula")
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        sig_before = tree_signature(tool_data(r)["nodes"])
        donothing_node = find_node_by_value(tool_data(r)["nodes"],
                                            "do-nothing", role="operator")["node"]
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"},
            ],
        })
        assert_success(r)
        src = tool_data(r)["node"]
        r = client.call_tool("attach_sexp_node", {
            "source_node": src,
            "target_node": donothing_node,
            "position": position,
        })
        assert_error(r)
        assert_in("syntax", tool_text(r).lower())
        r = client.call_tool("walk_sexp_tree", {"node": evt_formula})
        assert_success(r)
        sig_after = tree_signature(tool_data(r)["nodes"])
        assert_equal(sig_before, sig_after, "tree unchanged after rollback")
        r = client.call_tool("get_sexp_node", {"node": src})
        assert_success(r)
        assert_equal(tool_data(r).get("node_parent"), -1,
                     "source parent restored to -1 after wrap rollback")
        client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})
        client.call_tool("delete_event", {"name": "wrap_rb_test"})

    def test_attach_replace_wrap_rollback():
        """Replace with a wrapped operator source should roll back cleanly."""
        _wrap_rollback_helper("replace")

    def test_attach_before_wrap_rollback():
        """Insert-before with a wrapped operator source should roll back cleanly."""
        _wrap_rollback_helper("before")

    def test_attach_after_wrap_rollback():
        """Insert-after with a wrapped operator source should roll back cleanly."""
        _wrap_rollback_helper("after")

    suite.add("sexp_attach_replace_wrap_rollback", test_attach_replace_wrap_rollback)
    suite.add("sexp_attach_before_wrap_rollback", test_attach_before_wrap_rollback)
    suite.add("sexp_attach_after_wrap_rollback", test_attach_after_wrap_rollback)

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
        try:
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            original_vals = tree_values(tool_data(r)["nodes"])
            two_node = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            # Detach "2" — leaves placeholder.
            r = client.call_tool("detach_sexp_node", {"target_node": two_node})
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
        finally:
            client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

    suite.add("sexp_attach_detach_then_attach_restores_tree", test_detach_then_attach_restores_tree)

    # =================================================================
    # Target resolution: singleton rejection, argument index
    # =================================================================

    def test_attach_target_singleton_without_index_is_rejected():
        """Targeting a locked singleton directly should be rejected."""
        r = client.call_tool("text_to_sexp", {"text": "( when ( true ) ( do-nothing ) )"})
        assert_success(r)
        root = tool_data(r)["node"]
        src = None
        try:
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            true_node = find_node_by_value(tool_data(r)["nodes"],
                                           "true", role="operator")["node"]

            r = client.call_tool("create_sexp_node", {
                "role": "argument", "argument_type": "number", "argument_value": "1",
            })
            assert_success(r)
            src = tool_data(r)["node"]

            r = client.call_tool("attach_sexp_node", {
                "source_node": src, "target_node": true_node,
            })
            assert_error(r)
            assert_in("singleton", tool_text(r).lower())
            assert_in("target_argument_index", tool_text(r))
        finally:
            if src is not None:
                client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})
            client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

    def test_attach_replace_singleton_via_argument_index():
        """Replace a singleton boolean via argument index addressing."""
        r = client.call_tool("text_to_sexp", {"text": "( when ( true ) ( do-nothing ) )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            # Create a replacement boolean subtree: ( and ( true ) ( true ) )
            r = client.call_tool("text_to_sexp", {"text": "( and ( true ) ( true ) )"})
            assert_success(r)
            src = tool_data(r)["node"]

            # Replace argument 1 of the root when operator (the condition = true)
            r = client.call_tool("attach_sexp_node", {
                "source_node": src,
                "target_node": root,
                "target_argument_index": 1,
            })
            assert_success(r)

            # Walk the tree and verify the condition is now "and" not just "true"
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            values = tree_values(tool_data(r)["nodes"], filter_empty=True)
            assert_in("and", values)
        finally:
            client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

    def test_attach_argument_index_out_of_range():
        """Out-of-range argument index should be rejected."""
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        src = None
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "argument", "argument_type": "number", "argument_value": "9",
            })
            assert_success(r)
            src = tool_data(r)["node"]

            r = client.call_tool("attach_sexp_node", {
                "source_node": src, "target_node": root, "target_argument_index": 99,
            })
            assert_error(r)
            assert_in("out of range", tool_text(r).lower())
        finally:
            if src is not None:
                client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})
            client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

    def test_attach_argument_index_on_non_operator():
        """target_argument_index with a non-operator target should error."""
        r = client.call_tool("create_sexp_node", {
            "role": "argument", "argument_type": "number", "argument_value": "42",
        })
        assert_success(r)
        target = tool_data(r)["node"]
        src = None
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "argument", "argument_type": "number", "argument_value": "1",
            })
            assert_success(r)
            src = tool_data(r)["node"]

            r = client.call_tool("attach_sexp_node", {
                "source_node": src, "target_node": target, "target_argument_index": 1,
            })
            assert_error(r)
            assert_in("operator", tool_text(r).lower())
        finally:
            if src is not None:
                client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})
            client.call_tool("detach_sexp_node", {"target_node": target, "delete": True})

    def test_attach_before_via_argument_index():
        """Insert before a specific argument using target_argument_index."""
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "argument", "argument_type": "number", "argument_value": "99",
            })
            assert_success(r)
            src = tool_data(r)["node"]

            # Insert before argument 2 (the "2")
            r = client.call_tool("attach_sexp_node", {
                "source_node": src,
                "target_node": root,
                "target_argument_index": 2,
                "position": "before",
            })
            assert_success(r)

            # Walk and check order: should be +, 1, 99, 2
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            values = tree_values(tool_data(r)["nodes"], filter_empty=True)
            assert_equal(values, ["+", "1", "99", "2"])
        finally:
            client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

    def test_attach_after_via_argument_index():
        """Insert after a specific argument using target_argument_index."""
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "argument", "argument_type": "number", "argument_value": "99",
            })
            assert_success(r)
            src = tool_data(r)["node"]

            # Insert after argument 1 (the "1")
            r = client.call_tool("attach_sexp_node", {
                "source_node": src,
                "target_node": root,
                "target_argument_index": 1,
                "position": "after",
            })
            assert_success(r)

            # Walk and check order: should be +, 1, 99, 2
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            values = tree_values(tool_data(r)["nodes"], filter_empty=True)
            assert_equal(values, ["+", "1", "99", "2"])
        finally:
            client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

    def test_attach_argument_index_with_entity_mode_rejected():
        """Passing target_argument_index with entity mode should error."""
        r = client.call_tool("create_sexp_node", {
            "role": "argument", "argument_type": "number", "argument_value": "1",
        })
        assert_success(r)
        src = tool_data(r)["node"]
        try:
            r = client.call_tool("attach_sexp_node", {
                "source_node": src,
                "target_entity_type": "event",
                "target_entity_id": "foo",
                "target_argument_index": 1,
            })
            assert_error(r)
            assert_in("target_node", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})

    def test_attach_argument_index_below_minimum():
        """Non-positive argument index should be rejected (1-based)."""
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        src = None
        try:
            r = client.call_tool("create_sexp_node", {
                "role": "argument", "argument_type": "number", "argument_value": "9",
            })
            assert_success(r)
            src = tool_data(r)["node"]

            r = client.call_tool("attach_sexp_node", {
                "source_node": src, "target_node": root, "target_argument_index": 0,
            })
            assert_error(r)
            assert_in("1-based", tool_text(r).lower())
        finally:
            if src is not None:
                client.call_tool("detach_sexp_node", {"target_node": src, "delete": True})
            client.call_tool("detach_sexp_node", {"target_node": root, "delete": True})

    suite.add("sexp_attach_target_singleton_without_index_rejected",
              test_attach_target_singleton_without_index_is_rejected)
    suite.add("sexp_attach_replace_singleton_via_argument_index",
              test_attach_replace_singleton_via_argument_index)
    suite.add("sexp_attach_argument_index_out_of_range",
              test_attach_argument_index_out_of_range)
    suite.add("sexp_attach_argument_index_below_minimum",
              test_attach_argument_index_below_minimum)
    suite.add("sexp_attach_argument_index_on_non_operator",
              test_attach_argument_index_on_non_operator)
    suite.add("sexp_attach_before_via_argument_index",
              test_attach_before_via_argument_index)
    suite.add("sexp_attach_after_via_argument_index",
              test_attach_after_via_argument_index)
    suite.add("sexp_attach_argument_index_with_entity_mode_rejected",
              test_attach_argument_index_with_entity_mode_rejected)

    # =================================================================
    # Cycle detection
    # =================================================================

    def test_attach_cycle_rejected():
        """Attaching a tree root into one of its own children should be
        rejected to prevent cycles."""
        r = client.call_tool("text_to_sexp", {"text": "( + 1 2 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            # Find the "1" argument node inside root's tree.
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            one_node = find_node_by_value(tool_data(r)["nodes"], "1")["node"]

            # Try to attach root (= the whole tree) into its own child's
            # position.  This would create a cycle.
            r = client.call_tool("attach_sexp_node", {
                "source_node": root,
                "target_node": one_node,
                "position": "replace",
            })
            assert_error(r)
            assert_in("cycle", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node",
                             {"target_node": root, "delete": True})

    suite.add("sexp_attach_cycle_rejected",
              test_attach_cycle_rejected)

    def test_attach_cycle_grandchild_rejected():
        """Attaching a root into a grandchild position should be rejected.
        Exercises the multi-level walk-up inside the cycle check."""
        r = client.call_tool("text_to_sexp", {"text": "( + ( * 2 3 ) 4 )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            three_node = find_node_by_value(tool_data(r)["nodes"], "3")["node"]
            r = client.call_tool("attach_sexp_node", {
                "source_node": root,
                "target_node": three_node,
                "position": "replace",
            })
            assert_error(r)
            assert_in("cycle", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node",
                             {"target_node": root, "delete": True})

    suite.add("sexp_attach_cycle_grandchild_rejected",
              test_attach_cycle_grandchild_rejected)

    # =================================================================
    # Round-trip: attach result survives serialize/parse
    # =================================================================

    def test_attach_then_serialize_roundtrip():
        """After attach, serializing the host formula and re-parsing should
        reproduce the same token sequence."""
        r = client.call_tool("create_event", {"name": "attach_rt_evt"})
        assert_success(r)
        try:
            r = client.call_tool("detach_sexp_node",
                                 {"target_node": tool_data(r).get("formula"),
                                  "delete": True})
            assert_success(r)
            r = client.call_tool("text_to_sexp",
                                 {"text": "( when ( and ( true ) ( false ) ) "
                                          "( do-nothing ) )"})
            assert_success(r)
            new_formula = tool_data(r)["node"]
            r = client.call_tool("attach_sexp_node", {
                "source_node": new_formula,
                "target_entity_type": "event",
                "target_entity_id": "attach_rt_evt",
            })
            assert_success(r)
            # Snapshot the attached formula's token sequence.
            r = client.call_tool("walk_sexp_tree", {"node": new_formula})
            assert_success(r)
            original_vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            # Round-trip through text.
            r = client.call_tool("sexp_to_text", {"node": new_formula})
            assert_success(r)
            serialized = tool_text(r)
            r = client.call_tool("text_to_sexp", {"text": serialized})
            assert_success(r)
            parsed_root = tool_data(r)["node"]
            parsed_vals = None
            try:
                r = client.call_tool("walk_sexp_tree", {"node": parsed_root})
                assert_success(r)
                parsed_vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
                assert_equal(parsed_vals, original_vals,
                             "round-trip tokens should match the attached formula")
            finally:
                client.call_tool("detach_sexp_node",
                                 {"target_node": parsed_root, "delete": True})
        finally:
            client.call_tool("delete_event", {"name": "attach_rt_evt"})

    suite.add("sexp_attach_then_serialize_roundtrip",
              test_attach_then_serialize_roundtrip)

    # =================================================================
    # Cross-entity: detach a subtree from one entity, attach into another
    # =================================================================

    def test_cross_entity_attach():
        """Attach a boolean subtree as one goal's formula, detach it (which
        makes it free-standing again), and re-attach it as a different goal's
        formula.  Exercises the entity-to-entity transfer path."""
        r = client.call_tool("create_goal", {"name": "attach_cross_a"})
        assert_success(r)
        try:
            r = client.call_tool("create_goal", {"name": "attach_cross_b"})
            assert_success(r)
            try:
                # Plant an 'and' subtree as goal A's formula.
                r = client.call_tool("text_to_sexp",
                                     {"text": "( and ( true ) ( true ) )"})
                assert_success(r)
                and_node = tool_data(r)["node"]
                r = client.call_tool("attach_sexp_node", {
                    "source_node": and_node,
                    "target_entity_type": "goal",
                    "target_entity_id": "attach_cross_a",
                    "delete_displaced": True,
                })
                assert_success(r)
                # Detach goal A's formula — returns the 'and' subtree as a
                # free-standing root and installs a default replacement in A.
                r = client.call_tool("detach_sexp_node",
                                     {"target_node": and_node})
                assert_success(r)
                # Re-attach as goal B's formula.
                r = client.call_tool("attach_sexp_node", {
                    "source_node": and_node,
                    "target_entity_type": "goal",
                    "target_entity_id": "attach_cross_b",
                    "delete_displaced": True,
                })
                assert_success(r)
                # Verify: goal B's formula is the transferred 'and' subtree.
                r = client.call_tool("get_goal", {"name": "attach_cross_b"})
                assert_success(r)
                assert_equal(tool_data(r).get("formula"), and_node,
                             "goal B formula should be the cross-attached subtree")
                r = client.call_tool("walk_sexp_tree", {"node": and_node})
                assert_success(r)
                b_vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
                assert_equal(b_vals, ["and", "true", "true"],
                             "goal B tokens after cross-entity attach")
            finally:
                client.call_tool("delete_goal", {"name": "attach_cross_b"})
        finally:
            client.call_tool("delete_goal", {"name": "attach_cross_a"})

    suite.add("sexp_attach_cross_entity", test_cross_entity_attach)


if __name__ == "__main__":
    run_module_standalone(register, "SEXP attach behavior tests")
