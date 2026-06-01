"""CRUD + move/swap tests for the wings MCP module.

CRUD tests (basic_crud, cue_replacement, rename_cascade, delete/disband
semantics, error paths) self-fabricate their own ships and wings, then
clean up.  Move/swap tests use `_require_at_least_two_wings`, which
auto-fabricates a pair of wings if the mission doesn't already have two
and tears them down afterwards.

Wing membership is enumerated via list_ships: each ship reports its wing
name (when in a wing).
"""

from mcp_test_lib import (
    assert_equal,
    assert_error,
    assert_in,
    assert_is_list,
    assert_success,
    assert_true,
    run_module_standalone,
    SkipTest,
    tool_data,
)


# Identity matrix used by ship-creation helpers.
IDENTITY_ORIENT = {
    "rvec": {"x": 1.0, "y": 0.0, "z": 0.0},
    "uvec": {"x": 0.0, "y": 1.0, "z": 0.0},
    "fvec": {"x": 0.0, "y": 0.0, "z": 1.0},
}


def _pick_ship_class(client):
    r = client.call_tool("list_ship_classes")
    assert_success(r)
    classes = tool_data(r)
    assert_true(len(classes) > 0, "list_ship_classes returned no entries")
    return classes[0].get("name")


def _create_ship(client, name, cls, x=0.0):
    r = client.call_tool("create_ship", {
        "name": name,
        "ship_class": cls,
        "position": {"x": x, "y": 0.0, "z": 0.0},
        "orientation": IDENTITY_ORIENT,
    })
    assert_success(r)


def _safe_delete_wing(client, name):
    try:
        client.call_tool("delete_wing", {"name": name, "force": True})
    except Exception:
        pass


def _safe_delete_ship(client, name):
    try:
        client.call_tool("delete_ship", {"name": name, "force": True})
    except Exception:
        pass


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

    def _require_at_least_two_wings(seeded):
        """If the mission has fewer than two wings, fabricate "MCP Seed Alpha"
        and "MCP Seed Bravo" wings (one ship each) and register their cleanup
        tags on `seeded` so the caller's finally-block tears them down."""
        wings = _list_wing_names_in_ship_order()
        if len(wings) >= 2:
            return wings

        cls = _pick_ship_class(client)
        _create_ship(client, "MCP Seed Alpha Ship", cls, x=0.0)
        seeded["ships"].append("MCP Seed Alpha Ship")
        r = client.call_tool("form_wing", {
            "name": "MCP Seed Alpha",
            "members": ["MCP Seed Alpha Ship"],
        })
        assert_success(r)
        seeded["wings"].append("MCP Seed Alpha")

        _create_ship(client, "MCP Seed Bravo Ship", cls, x=100.0)
        seeded["ships"].append("MCP Seed Bravo Ship")
        r = client.call_tool("form_wing", {
            "name": "MCP Seed Bravo",
            "members": ["MCP Seed Bravo Ship"],
        })
        assert_success(r)
        seeded["wings"].append("MCP Seed Bravo")

        return _list_wing_names_in_ship_order()

    def _cleanup_seeded(seeded):
        for w in seeded.get("wings", []):
            _safe_delete_wing(client, w)
        # delete_wing already removed the member ships; any standalone test
        # ships need explicit cleanup.
        for s in seeded.get("ships", []):
            _safe_delete_ship(client, s)

    # ---------- swap ----------

    def test_wings_swap_basic():
        seeded = {"wings": [], "ships": []}
        try:
            wings = _require_at_least_two_wings(seeded)
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
        finally:
            _cleanup_seeded(seeded)

    def test_wings_swap_idempotent():
        seeded = {"wings": [], "ships": []}
        try:
            wings = _require_at_least_two_wings(seeded)
            r = client.call_tool("swap_wings", {"index_a": 1, "index_b": 2})
            assert_success(r)
            r = client.call_tool("swap_wings", {"index_a": 1, "index_b": 2})
            assert_success(r)
            post = _list_wing_names_in_ship_order()
            assert_equal(post[:2], wings[:2], "double swap restores original wing order")
        finally:
            _cleanup_seeded(seeded)

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
        wing names -- Ships[i].wingnum gets re-pointed by reassign_wing_slot,
        so the by-name view should be unchanged."""
        seeded = {"wings": [], "ships": []}
        try:
            _require_at_least_two_wings(seeded)

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
        finally:
            _cleanup_seeded(seeded)

    # ---------- move ----------

    def test_wings_move_basic():
        seeded = {"wings": [], "ships": []}
        try:
            wings = _require_at_least_two_wings(seeded)
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
        finally:
            _cleanup_seeded(seeded)

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

    # ---------- CRUD ----------

    def test_wings_basic_crud():
        cls = _pick_ship_class(client)
        ships = ["MCP CRUD Ship 1", "MCP CRUD Ship 2"]
        wing_name = "MCP CRUD Wing"
        created_wing = False
        try:
            _create_ship(client, ships[0], cls, x=0.0)
            _create_ship(client, ships[1], cls, x=100.0)

            r = client.call_tool("form_wing", {"name": wing_name, "members": ships})
            assert_success(r)
            created_wing = True
            d = tool_data(r)
            assert_equal(d.get("name"), wing_name, "form_wing returns the new wing name")
            assert_equal(d.get("wave_count"), 2, "wave_count reflects member count")
            assert_equal(d.get("members"), [f"{wing_name} 1", f"{wing_name} 2"],
                "member ships were wing-bashed to '<wing> 1', '<wing> 2'")

            # Member ships should no longer exist under their original names
            r = client.call_tool("list_ships")
            assert_success(r)
            ship_names = [s.get("name") for s in tool_data(r)]
            assert_true(ships[0] not in ship_names,
                "original ship name 1 should be gone after wing formation")
            assert_in(f"{wing_name} 1", ship_names)

            # list_wings should include the new wing
            r = client.call_tool("list_wings")
            assert_success(r)
            assert_in(wing_name, [w.get("name") for w in tool_data(r)])

            # update_wing round-trip
            r = client.call_tool("update_wing", {
                "name": wing_name,
                "num_waves": 3,
                "new_wave_threshold": 1,
                "hotkey": 5,
            })
            assert_success(r)
            r = client.call_tool("get_wing", {"name": wing_name})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("num_waves"), 3, "num_waves round-trip")
            assert_equal(d.get("new_wave_threshold"), 1, "new_wave_threshold round-trip")
            assert_equal(d.get("hotkey"), 5, "hotkey round-trip")
        finally:
            if created_wing:
                _safe_delete_wing(client, wing_name)
            for s in ships:
                _safe_delete_ship(client, s)
            # bashed names may also be lingering if cleanup ran out of order
            _safe_delete_ship(client, f"{wing_name} 1")
            _safe_delete_ship(client, f"{wing_name} 2")

    def test_wings_member_rename_cascade():
        cls = _pick_ship_class(client)
        wing_name = "MCP Rename Wing"
        new_name = "MCP Renamed Wing"
        try:
            _create_ship(client, "MCP Rename Seed 1", cls, x=0.0)
            _create_ship(client, "MCP Rename Seed 2", cls, x=100.0)
            r = client.call_tool("form_wing", {
                "name": wing_name,
                "members": ["MCP Rename Seed 1", "MCP Rename Seed 2"],
            })
            assert_success(r)

            r = client.call_tool("update_wing", {"name": wing_name, "new_name": new_name})
            assert_success(r)

            r = client.call_tool("get_wing", {"name": new_name})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("members"), [f"{new_name} 1", f"{new_name} 2"],
                "member ships were re-bashed to the new wing name")

            r = client.call_tool("list_ships")
            assert_success(r)
            names = [s.get("name") for s in tool_data(r)]
            assert_in(f"{new_name} 1", names)
            assert_in(f"{new_name} 2", names)
            assert_true(f"{wing_name} 1" not in names,
                "old bashed name 1 should be gone")
        finally:
            _safe_delete_wing(client, new_name)
            _safe_delete_wing(client, wing_name)
            for n in (f"{wing_name} 1", f"{wing_name} 2",
                      f"{new_name} 1", f"{new_name} 2",
                      "MCP Rename Seed 1", "MCP Rename Seed 2"):
                _safe_delete_ship(client, n)

    def test_wings_arrival_cue_replacement():
        cls = _pick_ship_class(client)
        wing_name = "MCP Cue Wing"
        try:
            _create_ship(client, "MCP Cue Ship", cls, x=0.0)
            r = client.call_tool("form_wing", {"name": wing_name, "members": ["MCP Cue Ship"]})
            assert_success(r)

            # Build a new cue and assign it to the wing
            r = client.call_tool("text_to_sexp", {"text": "( false )"})
            assert_success(r)
            new_cue = tool_data(r).get("node")
            assert_true(isinstance(new_cue, int) and new_cue >= 0,
                "text_to_sexp returns an integer node ID")

            r = client.call_tool("update_wing", {"name": wing_name, "arrival_cue": new_cue})
            assert_success(r)

            r = client.call_tool("get_wing", {"name": wing_name})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("arrival_cue"), new_cue,
                "arrival_cue round-trips the new node ID")
        finally:
            _safe_delete_wing(client, wing_name)
            for n in (f"{wing_name} 1", "MCP Cue Ship"):
                _safe_delete_ship(client, n)

    def test_wings_delete_cascades_to_members():
        cls = _pick_ship_class(client)
        wing_name = "MCP Cascade Delete"
        try:
            _create_ship(client, "MCP Cascade Seed", cls, x=0.0)
            r = client.call_tool("form_wing", {"name": wing_name, "members": ["MCP Cascade Seed"]})
            assert_success(r)

            r = client.call_tool("delete_wing", {"name": wing_name})
            assert_success(r)

            r = client.call_tool("list_ships")
            assert_success(r)
            names = [s.get("name") for s in tool_data(r)]
            assert_true(f"{wing_name} 1" not in names,
                "delete_wing removes the member ship too")
            assert_true("MCP Cascade Seed" not in names,
                "old ship name should not reappear")

            r = client.call_tool("list_wings")
            assert_success(r)
            assert_true(wing_name not in [w.get("name") for w in tool_data(r)],
                "wing is gone from list_wings")
        finally:
            _safe_delete_wing(client, wing_name)
            for n in (f"{wing_name} 1", "MCP Cascade Seed"):
                _safe_delete_ship(client, n)

    def test_wings_disband_keeps_members():
        cls = _pick_ship_class(client)
        wing_name = "MCP Disband Wing"
        try:
            _create_ship(client, "MCP Disband Seed", cls, x=0.0)
            r = client.call_tool("form_wing", {"name": wing_name, "members": ["MCP Disband Seed"]})
            assert_success(r)

            r = client.call_tool("disband_wing", {"name": wing_name})
            assert_success(r)

            # The wing should be gone but the ship should remain (engine
            # renames it back to a default class name on remove_wing).
            r = client.call_tool("list_wings")
            assert_success(r)
            assert_true(wing_name not in [w.get("name") for w in tool_data(r)],
                "wing is gone from list_wings after disband")

            r = client.call_tool("list_ships")
            assert_success(r)
            unwinged = [s for s in tool_data(r) if not s.get("in_wing")]
            assert_true(len(unwinged) >= 1,
                "at least one standalone ship should remain after disband")
        finally:
            _safe_delete_wing(client, wing_name)
            # Cleanup: any ship that came out of the wing now has an engine-default name.
            # We can't predict it exactly, so do a best-effort cleanup of the seed
            # and bashed candidates.
            for n in (f"{wing_name} 1", "MCP Disband Seed"):
                _safe_delete_ship(client, n)
            # Best-effort cleanup of any ships of this class with default names.
            try:
                r = client.call_tool("list_ships")
                if not r.get("isError"):
                    for s in tool_data(r):
                        if not s.get("in_wing") and s.get("ship_class") == cls:
                            _safe_delete_ship(client, s.get("name"))
            except Exception:
                pass

    def test_wings_form_requires_members():
        r = client.call_tool("form_wing", {"name": "MCP Bad Form"})
        assert_error(r)
        r = client.call_tool("form_wing", {"name": "MCP Bad Form", "members": []})
        assert_error(r)
        # No wing should have been created.
        r = client.call_tool("list_wings")
        assert_success(r)
        assert_true("MCP Bad Form" not in [w.get("name") for w in tool_data(r)])

    def test_wings_form_member_not_found():
        r = client.call_tool("form_wing", {
            "name": "MCP Bad Member",
            "members": ["DOES_NOT_EXIST_xyz"],
        })
        assert_error(r)
        r = client.call_tool("list_wings")
        assert_success(r)
        assert_true("MCP Bad Member" not in [w.get("name") for w in tool_data(r)])

    def test_wings_duplicate_name():
        cls = _pick_ship_class(client)
        ship_name = "MCP Dup Ship"
        wing_name = "MCP Dup Wing"
        try:
            _create_ship(client, ship_name, cls, x=0.0)
            r = client.call_tool("form_wing", {"name": wing_name, "members": [ship_name]})
            assert_success(r)

            # Forming a second wing with the same name should fail
            _create_ship(client, "MCP Dup Ship 2", cls, x=200.0)
            r = client.call_tool("form_wing", {"name": wing_name, "members": ["MCP Dup Ship 2"]})
            assert_error(r)
        finally:
            _safe_delete_wing(client, wing_name)
            for n in (f"{wing_name} 1", ship_name, "MCP Dup Ship 2"):
                _safe_delete_ship(client, n)

    suite.add("wings_basic_crud", test_wings_basic_crud)
    suite.add("wings_member_rename_cascade", test_wings_member_rename_cascade)
    suite.add("wings_arrival_cue_replacement", test_wings_arrival_cue_replacement)
    suite.add("wings_delete_cascades_to_members", test_wings_delete_cascades_to_members)
    suite.add("wings_disband_keeps_members", test_wings_disband_keeps_members)
    suite.add("wings_form_requires_members", test_wings_form_requires_members)
    suite.add("wings_form_member_not_found", test_wings_form_member_not_found)
    suite.add("wings_duplicate_name", test_wings_duplicate_name)

    suite.add("wings_swap_basic", test_wings_swap_basic)
    suite.add("wings_swap_idempotent", test_wings_swap_idempotent)
    suite.add("wings_swap_same_index_noop", test_wings_swap_same_index_noop)
    suite.add("wings_swap_preserves_ship_membership", test_wings_swap_preserves_ship_membership)
    suite.add("wings_move_basic", test_wings_move_basic)
    suite.add("wings_move_same_index_noop", test_wings_move_same_index_noop)
    suite.add("wings_swap_out_of_range", test_wings_swap_out_of_range)
    suite.add("wings_move_out_of_range", test_wings_move_out_of_range)


if __name__ == "__main__":
    run_module_standalone(register, "Wing CRUD + move/swap tests")
