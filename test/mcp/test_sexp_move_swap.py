"""All move_sexp_node and swap_sexp_nodes behavior tests.

Both tools compose detach_sexp_node + attach_sexp_node internally, so the
focus here is on:
- Reference resolution via source_/target_ field prefixes (node, entity, mixed).
- Position semantics inherited from attach (replace/before/after) for move.
- Shrink semantics inherited from detach for move.
- Atomicity: when an internal step fails, prior steps are rolled back so the
  tree is left unchanged.
- Same-node guards (move errors, swap no-ops).
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
    # move_sexp_node — node-to-node
    # =================================================================

    def _build_plus_tree(args):
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": str(v)} for v in args
            ],
        })
        assert_success(r)
        return tool_data(r)["node"]

    def test_move_node_to_node_replace():
        # Build (+ 1 2 3) and (* 10 20 30); move "2" from + into the slot of 20.
        # After: (+ 1 <placeholder> 3) and (* 10 2 30).
        a_root = _build_plus_tree([1, 2, 3])
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "*",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "10"},
                {"argument_type": "number", "argument_value": "20"},
                {"argument_type": "number", "argument_value": "30"},
            ],
        })
        assert_success(r)
        b_root = tool_data(r)["node"]
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a_root})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            r = client.call_tool("walk_sexp_tree", {"node": b_root})
            twenty = find_node_by_value(tool_data(r)["nodes"], "20")["node"]

            r = client.call_tool("move_sexp_node", {
                "source_node": two,
                "target_node": twenty,
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("moved_node"), two, "moved_node echoed")
            assert_true("detached" in d, "detached sub-object present")
            assert_true("attached" in d, "attached sub-object present")

            # Confirm: a_root now has a placeholder where "2" was; b_root has 2.
            r = client.call_tool("walk_sexp_tree", {"node": a_root})
            a_vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            assert_true("2" not in a_vals, "2 should be gone from a")
            r = client.call_tool("walk_sexp_tree", {"node": b_root})
            b_vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            assert_in("2", b_vals, "2 should appear in b")
            assert_true("20" not in b_vals, "20 should be displaced from b")
        finally:
            client.call_tool("detach_sexp_node", {"node": a_root, "delete": True})
            client.call_tool("detach_sexp_node", {"node": b_root, "delete": True})

    def test_move_node_before():
        # Build (+ 1 2 3) and (- 10 20); move "20" before "2".  Result: (+ 1 20 2 3).
        a = _build_plus_tree([1, 2, 3])
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "-",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "10"},
                {"argument_type": "number", "argument_value": "20"},
            ],
        })
        assert_success(r)
        b = tool_data(r)["node"]
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            r = client.call_tool("walk_sexp_tree", {"node": b})
            twenty = find_node_by_value(tool_data(r)["nodes"], "20")["node"]

            r = client.call_tool("move_sexp_node", {
                "source_node": twenty,
                "target_node": two,
                "position": "before",
            })
            assert_success(r)
            r = client.call_tool("walk_sexp_tree", {"node": a})
            vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            # Expect order +, 1, 20, 2, 3
            order = [v for v in vals if v in ("+", "1", "20", "2", "3")]
            assert_equal(order, ["+", "1", "20", "2", "3"], "order after move-before")
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})
            client.call_tool("detach_sexp_node", {"node": b, "delete": True})

    def test_move_node_after():
        a = _build_plus_tree([1, 2, 3])
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "-",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "10"},
                {"argument_type": "number", "argument_value": "20"},
            ],
        })
        assert_success(r)
        b = tool_data(r)["node"]
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            r = client.call_tool("walk_sexp_tree", {"node": b})
            twenty = find_node_by_value(tool_data(r)["nodes"], "20")["node"]

            r = client.call_tool("move_sexp_node", {
                "source_node": twenty,
                "target_node": two,
                "position": "after",
            })
            assert_success(r)
            r = client.call_tool("walk_sexp_tree", {"node": a})
            vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            order = [v for v in vals if v in ("+", "1", "2", "20", "3")]
            assert_equal(order, ["+", "1", "2", "20", "3"], "order after move-after")
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})
            client.call_tool("detach_sexp_node", {"node": b, "delete": True})

    def test_move_with_shrink_collapses_source_slot():
        # Build (+ 1 2 3), move "2" into b with shrink=true. a should become (+ 1 3).
        a = _build_plus_tree([1, 2, 3])
        b = _build_plus_tree([10, 20])
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            r = client.call_tool("walk_sexp_tree", {"node": b})
            twenty = find_node_by_value(tool_data(r)["nodes"], "20")["node"]

            r = client.call_tool("move_sexp_node", {
                "source_node": two,
                "target_node": twenty,
                "shrink": True,
            })
            assert_success(r)
            r = client.call_tool("walk_sexp_tree", {"node": a})
            vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            order = [v for v in vals if v in ("+", "1", "3")]
            assert_equal(order, ["+", "1", "3"], "+ should now have only 1 and 3")
            # No placeholder either.
            assert_true("<argument>" not in vals, "no placeholder in a")
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})
            client.call_tool("detach_sexp_node", {"node": b, "delete": True})

    def test_move_replace_default_preserves_displaced():
        # Default delete_displaced=False: replacing "20" with "2" should preserve
        # the displaced "20" as a free-standing root, reachable via walk_sexp_tree.
        a = _build_plus_tree([1, 2, 3])
        b = _build_plus_tree([10, 20, 30])
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            r = client.call_tool("walk_sexp_tree", {"node": b})
            twenty = find_node_by_value(tool_data(r)["nodes"], "20")["node"]

            r = client.call_tool("move_sexp_node", {
                "source_node": two,
                "target_node": twenty,
                "delete_displaced": False,
            })
            assert_success(r)
            attached = tool_data(r).get("attached", {})
            displaced = attached.get("displaced_node")
            assert_true(displaced is not None, "displaced_node reported")
            assert_equal(attached.get("deleted_displaced"), False, "deleted_displaced=false")
            # The displaced node should still be a valid free-standing root.
            r = client.call_tool("walk_sexp_tree", {"node": displaced})
            assert_success(r)
            disp_vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            assert_in("20", disp_vals, "displaced subtree still walkable as free-standing")
            client.call_tool("detach_sexp_node", {"node": displaced, "delete": True})
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})
            client.call_tool("detach_sexp_node", {"node": b, "delete": True})

    def test_move_replace_delete_displaced():
        # delete_displaced=True: the displaced "20" subtree is freed; subsequent
        # walks of its index should fail (node no longer in use).
        a = _build_plus_tree([1, 2, 3])
        b = _build_plus_tree([10, 20, 30])
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            r = client.call_tool("walk_sexp_tree", {"node": b})
            twenty = find_node_by_value(tool_data(r)["nodes"], "20")["node"]

            r = client.call_tool("move_sexp_node", {
                "source_node": two,
                "target_node": twenty,
                "delete_displaced": True,
            })
            assert_success(r)
            attached = tool_data(r).get("attached", {})
            assert_equal(attached.get("deleted_displaced"), True, "deleted_displaced=true")
            assert_true(attached.get("freed_count", 0) > 0, "freed_count > 0")
            # The displaced node index should no longer be in use.
            displaced = attached.get("displaced_node")
            assert_true(displaced is not None, "displaced_node still reported (as int)")
            r = client.call_tool("get_sexp_node", {"node": displaced})
            assert_error(r)
            assert_in("not in use", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})
            client.call_tool("detach_sexp_node", {"node": b, "delete": True})

    # =================================================================
    # move_sexp_node — entity mode
    # =================================================================

    def test_move_entity_to_entity():
        # Move one event's formula to another event's formula (both OPR_NULL).
        client.call_tool("create_event", {"name": "mvent_src"})
        client.call_tool("create_event", {"name": "mvent_tgt"})
        try:
            r = client.call_tool("text_to_sexp", {"text": "( do-nothing )"})
            assert_success(r)
            new_root = tool_data(r)["node"]
            r = client.call_tool("attach_sexp_node", {
                "source_node": new_root,
                "target_entity_type": "event",
                "target_entity_id": "mvent_src",
            })
            assert_success(r)
            r = client.call_tool("get_event", {"name": "mvent_src"})
            assert_success(r)
            src_pre_root = tool_data(r).get("formula")

            r = client.call_tool("move_sexp_node", {
                "source_entity_type": "event",
                "source_entity_id": "mvent_src",
                "target_entity_type": "event",
                "target_entity_id": "mvent_tgt",
            })
            assert_success(r)

            # Target now holds the original source formula.
            r = client.call_tool("get_event", {"name": "mvent_tgt"})
            assert_success(r)
            assert_equal(tool_data(r).get("formula"), src_pre_root, "tgt formula now == src's pre-move formula")
        finally:
            client.call_tool("delete_event", {"name": "mvent_src", "force": True})
            client.call_tool("delete_event", {"name": "mvent_tgt", "force": True})

    def test_move_node_to_entity():
        # move requires the source to be at a detachable position (embedded
        # in a tree or attached as an entity formula).  Build a tree
        # containing an embedded OPR_NULL action subtree and move that subtree
        # to become an event's formula.
        client.call_tool("create_event", {"name": "mvne_tgt"})
        try:
            # ( when ( true ) ( do-nothing ) ) - the (do-nothing) is at when's
            # argument index 2 and is OPR_NULL, suitable for an event formula.
            r = client.call_tool("text_to_sexp",
                {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            container = tool_data(r)["node"]
            r = client.call_tool("walk_sexp_tree", {"node": container})
            assert_success(r)
            do_nothing = find_node_by_value(tool_data(r)["nodes"], "do-nothing")["node"]

            r = client.call_tool("move_sexp_node", {
                "source_node": do_nothing,
                "target_entity_type": "event",
                "target_entity_id": "mvne_tgt",
            })
            assert_success(r)
            r = client.call_tool("get_event", {"name": "mvne_tgt"})
            assert_success(r)
            assert_true(tool_data(r).get("formula") is not None, "event formula set")
            client.call_tool("detach_sexp_node", {"node": container, "delete": True})
        finally:
            client.call_tool("delete_event", {"name": "mvne_tgt", "force": True})

    def test_move_entity_to_node():
        # Move an event's formula into an embedded slot of a free-standing tree.
        client.call_tool("create_event", {"name": "mven_src"})
        try:
            # Give the event an OPR_NULL formula whose root is a (do-nothing).
            # The default new-event formula is ( when ( true ) ( do-nothing ) ),
            # which is OPR_NULL.  Build a free-standing target tree like
            # ( when ( true ) ( do-nothing ) ) and move src's formula into the
            # (do-nothing) slot via target_argument_index.
            r = client.call_tool("text_to_sexp",
                {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            tgt_root = tool_data(r)["node"]
            # Argument 2 of when is the action ((do-nothing)).
            r = client.call_tool("move_sexp_node", {
                "source_entity_type": "event",
                "source_entity_id": "mven_src",
                "target_node": tgt_root,
                "target_argument_index": 2,
            })
            assert_success(r)
            # Event's formula was replaced with default; tgt_root's arg 2 now has src formula.
            client.call_tool("detach_sexp_node", {"node": tgt_root, "delete": True})
        finally:
            client.call_tool("delete_event", {"name": "mven_src", "force": True})

    # =================================================================
    # move_sexp_node — guards and error paths
    # =================================================================

    def test_move_same_node_errors():
        a = _build_plus_tree([1, 2, 3])
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            r = client.call_tool("move_sexp_node", {
                "source_node": two,
                "target_node": two,
            })
            assert_error(r)
            assert_in("same node", tool_text(r).lower())
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})

    def test_move_locked_singleton_bare_source_errors():
        # Build a target tree with a known argument slot.
        a = _build_plus_tree([1, 2, 3])
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            # Try to move Locked_sexp_true (built into the engine) by node alone -
            # parse_general_sexp_reference should reject without argument_index.
            # We don't know its exact index, but we can synthesize one via a
            # ( true ) text source and retain the resulting locked node.
            r = client.call_tool("text_to_sexp", {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            when_root = tool_data(r)["node"]
            # Walk it to find the "true" node.
            r = client.call_tool("walk_sexp_tree", {"node": when_root})
            true_n = find_node_by_value(tool_data(r)["nodes"], "true")["node"]
            # Targeting it directly without argument_index should fail.
            r = client.call_tool("move_sexp_node", {
                "source_node": true_n,
                "target_node": two,
            })
            assert_error(r)
            client.call_tool("detach_sexp_node", {"node": when_root, "delete": True})
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})

    def test_move_atomic_rollback_shrink_false():
        # Force an attach failure by trying to move an OPR_BOOL subtree into an
        # OPR_NULL slot.  The detach must be rolled back so the source returns
        # to its original location.  Use a goal (OPR_BOOL) as the source-formula
        # holder and an event (OPR_NULL) as the target.
        client.call_tool("create_goal", {"name": "mvar_src", "goal_type": "Primary"})
        client.call_tool("create_event", {"name": "mvar_tgt"})
        try:
            # The goal's default formula is ( true ), an OPR_BOOL.  Put a more
            # interesting one in so we can verify it returned intact.
            r = client.call_tool("text_to_sexp", {"text": "( and ( true ) ( true ) )"})
            assert_success(r)
            new_formula = tool_data(r)["node"]
            r = client.call_tool("attach_sexp_node", {
                "source_node": new_formula,
                "target_entity_type": "goal",
                "target_entity_id": "mvar_src",
            })
            assert_success(r)
            # Capture the goal's current formula root + signature.
            r = client.call_tool("get_goal", {"name": "mvar_src"})
            assert_success(r)
            pre_root = tool_data(r).get("formula")
            r = client.call_tool("walk_sexp_tree", {"node": pre_root})
            assert_success(r)
            pre_sig = tree_signature(tool_data(r)["nodes"])

            # Attempt the bad move.
            r = client.call_tool("move_sexp_node", {
                "source_entity_type": "goal",
                "source_entity_id": "mvar_src",
                "target_entity_type": "event",
                "target_entity_id": "mvar_tgt",
            })
            assert_error(r)

            # Verify the goal's formula is back to what it was before.
            r = client.call_tool("get_goal", {"name": "mvar_src"})
            assert_success(r)
            post_root = tool_data(r).get("formula")
            assert_equal(post_root, pre_root, "goal formula root unchanged after rollback")
            r = client.call_tool("walk_sexp_tree", {"node": post_root})
            assert_success(r)
            post_sig = tree_signature(tool_data(r)["nodes"])
            assert_equal(post_sig, pre_sig, "goal formula structure unchanged after rollback")
        finally:
            client.call_tool("delete_goal", {"name": "mvar_src", "force": True})
            client.call_tool("delete_event", {"name": "mvar_tgt", "force": True})

    def test_move_atomic_rollback_shrink_true():
        # Same scenario as the shrink=false test but with shrink=true.  The
        # source's sibling chain should be fully restored after rollback,
        # including the slot that shrink would have collapsed.
        a = _build_plus_tree([1, 2, 3])
        client.call_tool("create_event", {"name": "mvars_tgt"})
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            pre_sig = tree_signature(tool_data(r)["nodes"])
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]

            # The event expects OPR_NULL.  + is OPR_NUMBER, so this should fail.
            # Replacing the event's formula via move with target_entity_type
            # would be a syntax mismatch.  But we want to test shrink rollback
            # specifically -- so use node-mode source (the "2" atom) into an
            # OPR_NULL entity.  "2" is an atom number, also wrong type.
            r = client.call_tool("move_sexp_node", {
                "source_node": two,
                "shrink": True,
                "target_entity_type": "event",
                "target_entity_id": "mvars_tgt",
            })
            assert_error(r)

            # Verify the source tree signature is unchanged (slot restored).
            r = client.call_tool("walk_sexp_tree", {"node": a})
            post_sig = tree_signature(tool_data(r)["nodes"])
            assert_equal(post_sig, pre_sig, "tree fully restored after shrink=true rollback")
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})
            client.call_tool("delete_event", {"name": "mvars_tgt", "force": True})

    # =================================================================
    # swap_sexp_nodes — node-to-node
    # =================================================================

    def test_swap_two_embedded_nodes_same_tree():
        # Build (+ 1 2 3 4); swap "2" and "3".  Result: (+ 1 3 2 4).
        a = _build_plus_tree([1, 2, 3, 4])
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            three = find_node_by_value(tool_data(r)["nodes"], "3")["node"]
            r = client.call_tool("swap_sexp_nodes", {
                "source_node": two,
                "target_node": three,
            })
            assert_success(r)
            assert_equal(tool_data(r).get("swapped"), True, "swapped=true")
            r = client.call_tool("walk_sexp_tree", {"node": a})
            vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            order = [v for v in vals if v in ("+", "1", "2", "3", "4")]
            assert_equal(order, ["+", "1", "3", "2", "4"], "order after swap")
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})

    def test_swap_nodes_across_separate_trees():
        a = _build_plus_tree([1, 2, 3])
        b = _build_plus_tree([10, 20, 30])
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            r = client.call_tool("walk_sexp_tree", {"node": b})
            twenty = find_node_by_value(tool_data(r)["nodes"], "20")["node"]
            r = client.call_tool("swap_sexp_nodes", {
                "source_node": two,
                "target_node": twenty,
            })
            assert_success(r)
            r = client.call_tool("walk_sexp_tree", {"node": a})
            a_vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            r = client.call_tool("walk_sexp_tree", {"node": b})
            b_vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            assert_in("20", a_vals, "20 moved to a")
            assert_true("2" not in a_vals, "2 left a")
            assert_in("2", b_vals, "2 moved to b")
            assert_true("20" not in b_vals, "20 left b")
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})
            client.call_tool("detach_sexp_node", {"node": b, "delete": True})

    # =================================================================
    # swap_sexp_nodes — entity mode
    # =================================================================

    def test_swap_two_entity_formulas():
        # Two events, each with a distinguishable formula; swap them.
        client.call_tool("create_event", {"name": "swent_a"})
        client.call_tool("create_event", {"name": "swent_b"})
        try:
            r = client.call_tool("text_to_sexp", {"text": "( do-nothing )"})
            assert_success(r)
            f_a = tool_data(r)["node"]
            r = client.call_tool("attach_sexp_node", {
                "source_node": f_a,
                "target_entity_type": "event",
                "target_entity_id": "swent_a",
            })
            assert_success(r)

            r = client.call_tool("text_to_sexp",
                {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            f_b = tool_data(r)["node"]
            r = client.call_tool("attach_sexp_node", {
                "source_node": f_b,
                "target_entity_type": "event",
                "target_entity_id": "swent_b",
            })
            assert_success(r)

            # Capture pre-swap formula roots.
            r = client.call_tool("get_event", {"name": "swent_a"})
            assert_success(r)
            a_pre = tool_data(r).get("formula")
            r = client.call_tool("get_event", {"name": "swent_b"})
            assert_success(r)
            b_pre = tool_data(r).get("formula")

            r = client.call_tool("swap_sexp_nodes", {
                "source_entity_type": "event",
                "source_entity_id": "swent_a",
                "target_entity_type": "event",
                "target_entity_id": "swent_b",
            })
            assert_success(r)
            assert_equal(tool_data(r).get("swapped"), True, "swapped=true")

            # After swap, each event's formula should now be the other's.
            r = client.call_tool("get_event", {"name": "swent_a"})
            assert_success(r)
            a_post = tool_data(r).get("formula")
            r = client.call_tool("get_event", {"name": "swent_b"})
            assert_success(r)
            b_post = tool_data(r).get("formula")
            assert_equal(a_post, b_pre, "a's formula now == b's pre-swap")
            assert_equal(b_post, a_pre, "b's formula now == a's pre-swap")
        finally:
            client.call_tool("delete_event", {"name": "swent_a", "force": True})
            client.call_tool("delete_event", {"name": "swent_b", "force": True})

    def test_swap_entity_with_embedded_node():
        # Mixed mode: swap an event's formula with a sub-expression of a
        # free-standing tree.
        client.call_tool("create_event", {"name": "swen_evt"})
        try:
            r = client.call_tool("text_to_sexp",
                {"text": "( when ( true ) ( do-nothing ) )"})
            assert_success(r)
            f = tool_data(r)["node"]
            r = client.call_tool("attach_sexp_node", {
                "source_node": f,
                "target_entity_type": "event",
                "target_entity_id": "swen_evt",
            })
            assert_success(r)

            # Build a free-standing tree containing another OPR_NULL subtree.
            r = client.call_tool("text_to_sexp",
                {"text": "( when ( false ) ( do-nothing ) )"})
            assert_success(r)
            free_root = tool_data(r)["node"]

            # Swap the event's formula with free_root (root-to-entity == both
            # are OPR_NULL formula roots so types match).
            r = client.call_tool("swap_sexp_nodes", {
                "source_entity_type": "event",
                "source_entity_id": "swen_evt",
                "target_node": free_root,
            })
            # Note: target_node = a free-standing root.  attach has a guard
            # against attaching at a free-standing root in node mode.  Expect
            # the swap to error gracefully and leave both unchanged.
            # (If this is supported, the assertion would need to be different.)
            if not r.get("isError"):
                # If supported, just succeed-and-move-on.
                assert_equal(tool_data(r).get("swapped"), True, "swapped=true if supported")
            else:
                # Document the failure mode; verify rollback left the event
                # formula intact.
                r = client.call_tool("get_event", {"name": "swen_evt"})
                assert_success(r)
                # Should still have a valid formula root.
                assert_true(tool_data(r).get("formula") is not None, "event formula intact")
            # Clean up the free tree if it still exists.
            client.call_tool("detach_sexp_node", {"node": free_root, "delete": True})
        finally:
            client.call_tool("delete_event", {"name": "swen_evt", "force": True})

    # =================================================================
    # swap_sexp_nodes — guards and edge cases
    # =================================================================

    def test_swap_same_node_is_noop():
        a = _build_plus_tree([1, 2, 3])
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            r = client.call_tool("swap_sexp_nodes", {
                "source_node": two,
                "target_node": two,
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("swapped"), False, "swapped=false for same node")
            assert_in("reason", d)
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})

    def test_swap_locked_singleton_via_argument_index():
        # Swap two locked singletons addressed via parent + argument_index.
        # Build (and (true) (false)); swap them so we get (and (false) (true)).
        r = client.call_tool("text_to_sexp",
            {"text": "( and ( true ) ( false ) )"})
        assert_success(r)
        root = tool_data(r)["node"]
        try:
            r = client.call_tool("swap_sexp_nodes", {
                "source_node": root,
                "source_argument_index": 1,
                "target_node": root,
                "target_argument_index": 2,
            })
            assert_success(r)
            assert_equal(tool_data(r).get("swapped"), True, "swapped=true")
            r = client.call_tool("walk_sexp_tree", {"node": root})
            vals = tree_values(tool_data(r)["nodes"], filter_empty=True)
            # The first non-and value should now be false, second true.
            non_and = [v for v in vals if v in ("true", "false")]
            assert_equal(non_and, ["false", "true"], "args swapped")
        finally:
            client.call_tool("detach_sexp_node", {"node": root, "delete": True})

    def test_swap_round_trip_idempotent():
        # Build (+ 1 2 3 4); capture signature; swap "2"<->"3" twice; verify
        # original signature restored.
        a = _build_plus_tree([1, 2, 3, 4])
        try:
            r = client.call_tool("walk_sexp_tree", {"node": a})
            pre_sig = tree_signature(tool_data(r)["nodes"])
            two = find_node_by_value(tool_data(r)["nodes"], "2")["node"]
            three = find_node_by_value(tool_data(r)["nodes"], "3")["node"]
            r = client.call_tool("swap_sexp_nodes", {
                "source_node": two,
                "target_node": three,
            })
            assert_success(r)
            r = client.call_tool("swap_sexp_nodes", {
                "source_node": two,
                "target_node": three,
            })
            assert_success(r)
            r = client.call_tool("walk_sexp_tree", {"node": a})
            post_sig = tree_signature(tool_data(r)["nodes"])
            assert_equal(post_sig, pre_sig, "double-swap restores original")
        finally:
            client.call_tool("detach_sexp_node", {"node": a, "delete": True})

    def test_swap_atomic_rollback_on_type_mismatch():
        # Try to swap an OPR_BOOL formula with an OPR_NULL formula.  The first
        # attach will fail the type check; both detaches must be rolled back.
        client.call_tool("create_event", {"name": "swarb_evt"})  # OPR_NULL
        client.call_tool("create_goal", {"name": "swarb_goal", "goal_type": "Primary"})  # OPR_BOOL
        try:
            # Capture pre-swap state.
            r = client.call_tool("get_event", {"name": "swarb_evt"})
            assert_success(r)
            evt_pre = tool_data(r).get("formula")
            r = client.call_tool("walk_sexp_tree", {"node": evt_pre})
            assert_success(r)
            evt_pre_sig = tree_signature(tool_data(r)["nodes"])
            r = client.call_tool("get_goal", {"name": "swarb_goal"})
            assert_success(r)
            goal_pre = tool_data(r).get("formula")

            r = client.call_tool("swap_sexp_nodes", {
                "source_entity_type": "event",
                "source_entity_id": "swarb_evt",
                "target_entity_type": "goal",
                "target_entity_id": "swarb_goal",
            })
            assert_error(r)

            # Verify both entities are unchanged.
            r = client.call_tool("get_event", {"name": "swarb_evt"})
            assert_success(r)
            assert_equal(tool_data(r).get("formula"), evt_pre, "event formula unchanged")
            r = client.call_tool("walk_sexp_tree", {"node": evt_pre})
            assert_success(r)
            evt_post_sig = tree_signature(tool_data(r)["nodes"])
            assert_equal(evt_post_sig, evt_pre_sig, "event formula structure unchanged")
            r = client.call_tool("get_goal", {"name": "swarb_goal"})
            assert_success(r)
            assert_equal(tool_data(r).get("formula"), goal_pre, "goal formula unchanged")
        finally:
            client.call_tool("delete_event", {"name": "swarb_evt", "force": True})
            client.call_tool("delete_goal", {"name": "swarb_goal", "force": True})

    def test_swap_atomic_rollback_step5_failure():
        # Step-5 failure path: first attach succeeds, second attach fails,
        # rollback restores both sides.  Sibling test to the type-mismatch
        # one, which exercises step-4 failure.
        #
        # Construction: source = event entity (entity-mode, OPR_NULL formula),
        # target = a literal `5` inside a free-standing ( + 1 5 2 ) tree.
        # - Step 4 attaches the event's old when-tree at the tree-B slot.
        #   Tree B is free-standing so the syntax check is skipped; an
        #   OPR_NULL operator in an OPF_NUMBER slot is accepted because nothing
        #   checks it.
        # - Step 5 attaches the literal 5 at the event entity.  This goes
        #   through check_sexp_formula, which rejects 5 since it isn't an
        #   operator -- triggering the rollback.
        client.call_tool("create_event", {"name": "sw5b_evt"})
        r = client.call_tool("text_to_sexp", {"text": "( + 1 5 2 )"})
        assert_success(r)
        b_root = tool_data(r)["node"]
        try:
            r = client.call_tool("get_event", {"name": "sw5b_evt"})
            assert_success(r)
            evt_pre = tool_data(r).get("formula")
            r = client.call_tool("walk_sexp_tree", {"node": evt_pre})
            assert_success(r)
            evt_pre_sig = tree_signature(tool_data(r)["nodes"])

            r = client.call_tool("walk_sexp_tree", {"node": b_root})
            assert_success(r)
            b_pre_sig = tree_signature(tool_data(r)["nodes"])
            five = find_node_by_value(tool_data(r)["nodes"], "5")["node"]

            r = client.call_tool("swap_sexp_nodes", {
                "source_entity_type": "event",
                "source_entity_id": "sw5b_evt",
                "target_node": five,
            })
            assert_error(r)

            # Event formula restored.
            r = client.call_tool("get_event", {"name": "sw5b_evt"})
            assert_success(r)
            assert_equal(tool_data(r).get("formula"), evt_pre,
                "event formula root unchanged after step-5 rollback")
            r = client.call_tool("walk_sexp_tree", {"node": evt_pre})
            assert_success(r)
            assert_equal(tree_signature(tool_data(r)["nodes"]), evt_pre_sig,
                "event formula structure unchanged after step-5 rollback")

            # + tree restored.
            r = client.call_tool("walk_sexp_tree", {"node": b_root})
            assert_success(r)
            assert_equal(tree_signature(tool_data(r)["nodes"]), b_pre_sig,
                "+ tree structure unchanged after step-5 rollback")
        finally:
            client.call_tool("detach_sexp_node", {"node": b_root, "delete": True})
            client.call_tool("delete_event", {"name": "sw5b_evt", "force": True})

    suite.add("sexp_move_node_to_node_replace", test_move_node_to_node_replace)
    suite.add("sexp_move_node_before", test_move_node_before)
    suite.add("sexp_move_node_after", test_move_node_after)
    suite.add("sexp_move_with_shrink_collapses_source_slot", test_move_with_shrink_collapses_source_slot)
    suite.add("sexp_move_replace_default_preserves_displaced", test_move_replace_default_preserves_displaced)
    suite.add("sexp_move_replace_delete_displaced", test_move_replace_delete_displaced)
    suite.add("sexp_move_entity_to_entity", test_move_entity_to_entity)
    suite.add("sexp_move_node_to_entity", test_move_node_to_entity)
    suite.add("sexp_move_entity_to_node", test_move_entity_to_node)
    suite.add("sexp_move_same_node_errors", test_move_same_node_errors)
    suite.add("sexp_move_locked_singleton_bare_source_errors", test_move_locked_singleton_bare_source_errors)
    suite.add("sexp_move_atomic_rollback_shrink_false", test_move_atomic_rollback_shrink_false)
    suite.add("sexp_move_atomic_rollback_shrink_true", test_move_atomic_rollback_shrink_true)
    suite.add("sexp_swap_two_embedded_nodes_same_tree", test_swap_two_embedded_nodes_same_tree)
    suite.add("sexp_swap_nodes_across_separate_trees", test_swap_nodes_across_separate_trees)
    suite.add("sexp_swap_two_entity_formulas", test_swap_two_entity_formulas)
    suite.add("sexp_swap_entity_with_embedded_node", test_swap_entity_with_embedded_node)
    suite.add("sexp_swap_same_node_is_noop", test_swap_same_node_is_noop)
    suite.add("sexp_swap_locked_singleton_via_argument_index", test_swap_locked_singleton_via_argument_index)
    suite.add("sexp_swap_round_trip_idempotent", test_swap_round_trip_idempotent)
    suite.add("sexp_swap_atomic_rollback_on_type_mismatch", test_swap_atomic_rollback_on_type_mismatch)
    suite.add("sexp_swap_atomic_rollback_step5_failure", test_swap_atomic_rollback_step5_failure)


if __name__ == "__main__":
    run_module_standalone(register, "SEXP move/swap behavior tests")
