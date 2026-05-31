"""Move/swap tests for the wings MCP module.

The wing MCP module currently exposes only move_wing and swap_wings — no
CRUD yet — so this file is scoped accordingly.  Positive tests need at
least two wings to exist; without a create_wing tool we can't fabricate
them ourselves, so those tests SkipTest gracefully when the mission
contains fewer than two wings.  Negative-path / range-check tests run
unconditionally because they exercise the dispatcher, not the wing data.

Wing membership is enumerated via list_ships: each ship reports its wing
name (when in a wing).  When create_wing lands, the positive tests will
"wake up" without code changes.
"""

from mcp_test_lib import (
    assert_equal,
    assert_error,
    assert_success,
    run_module_standalone,
    SkipTest,
    tool_data,
)


def register(suite, client):

    def _list_wing_names_in_ship_order():
        """Return the wing names in ship-iteration order (each wing's first
        appearance among the populated ships).  list_ships iterates
        obj_used_list, which reassign_ship_slot keeps sorted to Ships[] order,
        and Wings[] order tracks the order wings are encountered in that walk
        for our purposes."""
        r = client.call_tool("list_ships")
        assert_success(r)
        seen = []
        for s in tool_data(r):
            if s.get("in_wing") and s.get("wing") and s.get("wing") not in seen:
                seen.append(s.get("wing"))
        return seen

    def _require_at_least_two_wings():
        wings = _list_wing_names_in_ship_order()
        if len(wings) < 2:
            raise SkipTest(
                "wing positive tests need >= 2 wings in the mission; "
                "create_wing MCP tool does not exist yet"
            )
        return wings

    # ---------- swap ----------

    def test_wings_swap_basic():
        wings = _require_at_least_two_wings()
        # Pick the first two wings.  Indices are 1-based over the populated
        # wing list.
        a_idx, b_idx = 1, 2
        a_name_pre, b_name_pre = wings[0], wings[1]
        try:
            r = client.call_tool("swap_wings", {"index_a": a_idx, "index_b": b_idx})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("a", {}).get("name"), b_name_pre,
                "swap response: slot index_a now holds wing originally at index_b")
            assert_equal(d.get("b", {}).get("name"), a_name_pre,
                "swap response: slot index_b now holds wing originally at index_a")
            assert_equal(d.get("a", {}).get("index"), a_idx,
                "swap response: index_a echoed")
            assert_equal(d.get("b", {}).get("index"), b_idx,
                "swap response: index_b echoed")
        finally:
            # Restore the original order for the rest of the suite.
            try:
                client.call_tool("swap_wings", {"index_a": a_idx, "index_b": b_idx})
            except Exception:
                pass

    def test_wings_swap_idempotent():
        wings = _require_at_least_two_wings()
        r = client.call_tool("swap_wings", {"index_a": 1, "index_b": 2})
        assert_success(r)
        r = client.call_tool("swap_wings", {"index_a": 1, "index_b": 2})
        assert_success(r)
        # After a round trip, the same wing names should appear at the same
        # positions as before.
        post = _list_wing_names_in_ship_order()
        assert_equal(post[:2], wings[:2], "double swap restores original wing order")

    def test_wings_swap_same_index_noop():
        wings = _list_wing_names_in_ship_order()
        if len(wings) < 1:
            raise SkipTest("wing same-index noop test needs >= 1 wing")
        r = client.call_tool("swap_wings", {"index_a": 1, "index_b": 1})
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("a", {}).get("name"), wings[0],
            "noop swap response a")
        assert_equal(d.get("b", {}).get("name"), wings[0],
            "noop swap response b")

    def test_wings_swap_preserves_ship_membership():
        """After swapping wing slots the ships should still report the same
        wing names — Ships[i].wingnum gets re-pointed by reassign_wing_slot,
        so the by-name view should be unchanged."""
        wings = _require_at_least_two_wings()

        # Capture {ship_name: wing_name} before.
        r = client.call_tool("list_ships")
        assert_success(r)
        membership_pre = {s.get("name"): s.get("wing")
                          for s in tool_data(r) if s.get("in_wing")}

        try:
            r = client.call_tool("swap_wings", {"index_a": 1, "index_b": 2})
            assert_success(r)

            r = client.call_tool("list_ships")
            assert_success(r)
            membership_post = {s.get("name"): s.get("wing")
                               for s in tool_data(r) if s.get("in_wing")}

            assert_equal(membership_post, membership_pre,
                "ship-to-wing-name membership unchanged after wing slot swap")
        finally:
            # Restore the original wing order.
            client.call_tool("swap_wings", {"index_a": 1, "index_b": 2})

    # ---------- move ----------

    def test_wings_move_basic():
        wings = _require_at_least_two_wings()
        # Move the first wing one slot down.
        first_pre = wings[0]
        second_pre = wings[1]
        moved = False
        try:
            r = client.call_tool("move_wing", {"from_index": 1, "to_index": 2})
            assert_success(r)
            moved = True
            d = tool_data(r)
            assert_equal(d.get("name"), first_pre,
                "move response: moved wing is the original first wing")
            assert_equal(d.get("index"), 2,
                "move response: destination echoed")
        finally:
            if moved:
                try:
                    client.call_tool("move_wing", {"from_index": 2, "to_index": 1})
                except Exception:
                    pass

        post = _list_wing_names_in_ship_order()
        assert_equal(post[0], first_pre, "move + reverse-move restores original first")
        assert_equal(post[1], second_pre, "move + reverse-move restores original second")

    def test_wings_move_same_index_noop():
        wings = _list_wing_names_in_ship_order()
        if len(wings) < 1:
            raise SkipTest("wing same-index noop test needs >= 1 wing")
        r = client.call_tool("move_wing", {"from_index": 1, "to_index": 1})
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("name"), wings[0], "noop move response name")
        assert_equal(d.get("index"), 1, "noop move response index")

    # ---------- range-check failures ----------

    def test_wings_swap_out_of_range():
        # Use a huge index well past MAX_WINGS.  Works regardless of wing count.
        r = client.call_tool("swap_wings", {"index_a": 1, "index_b": 9999})
        assert_error(r)
        r = client.call_tool("swap_wings", {"index_a": 9999, "index_b": 1})
        assert_error(r)
        r = client.call_tool("swap_wings", {"index_a": 0, "index_b": 1})
        assert_error(r)

    def test_wings_move_out_of_range():
        r = client.call_tool("move_wing", {"from_index": 1, "to_index": 9999})
        assert_error(r)
        r = client.call_tool("move_wing", {"from_index": 9999, "to_index": 1})
        assert_error(r)
        r = client.call_tool("move_wing", {"from_index": 0, "to_index": 1})
        assert_error(r)

    suite.add("wings_swap_basic", test_wings_swap_basic)
    suite.add("wings_swap_idempotent", test_wings_swap_idempotent)
    suite.add("wings_swap_same_index_noop", test_wings_swap_same_index_noop)
    suite.add("wings_swap_preserves_ship_membership", test_wings_swap_preserves_ship_membership)
    suite.add("wings_move_basic", test_wings_move_basic)
    suite.add("wings_move_same_index_noop", test_wings_move_same_index_noop)
    suite.add("wings_swap_out_of_range", test_wings_swap_out_of_range)
    suite.add("wings_move_out_of_range", test_wings_move_out_of_range)


if __name__ == "__main__":
    run_module_standalone(register, "Wing move/swap tests")
