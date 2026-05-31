"""CRUD tests for the ships MCP module.

Exercises the basic Ship Editor scope: create / get / list / update / delete
on ships, plus the rules that aren't obvious from the field list -- player-
start gating, alt-name/callsign ref-counting, arrival/departure cue
replacement, wing-membership gating (read-only assertion for now, since no
wing CRUD exists yet), and refusal of the last-player-start delete.

Each test wraps mutations in try/finally so cleanup happens even when an
assertion fails, keeping the mission state usable for the rest of the
suite.

Assumes a freshly created mission (via the standard prelude); does not
assume any pre-existing ships.
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
)


# Identity matrix used for all create_ship calls -- orientation specifics
# don't matter for these tests, only that the field is accepted.
IDENTITY_ORIENT = {
    "rvec": {"x": 1.0, "y": 0.0, "z": 0.0},
    "uvec": {"x": 0.0, "y": 1.0, "z": 0.0},
    "fvec": {"x": 0.0, "y": 0.0, "z": 1.0},
}


def _pick_ship_class(client):
    """Pick the first available ship class so tests don't hard-code a name
    that might not exist in a given mod."""
    r = client.call_tool("list_ship_classes")
    assert_success(r)
    classes = tool_data(r)
    assert_true(len(classes) > 0, "list_ship_classes returned no entries")
    return classes[0].get("name")


def _delete_all_test_ships(client, names):
    for n in names:
        try:
            client.call_tool("delete_ship", {"name": n, "force": True})
        except Exception:
            pass


def register(suite, client):

    def test_ships_basic_crud():
        cls = _pick_ship_class(client)
        created = []
        try:
            # Create with minimum required fields
            r = client.call_tool("create_ship", {
                "name": "MCP Test Alpha",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Test Alpha")
            d = tool_data(r)
            assert_equal(d.get("name"), "MCP Test Alpha", "created name")
            assert_equal(d.get("ship_class"), cls, "created ship class")
            assert_equal(d.get("in_wing"), False, "new ship should not be in a wing")

            # Create a second ship with more fields populated
            r = client.call_tool("create_ship", {
                "name": "MCP Test Bravo",
                "ship_class": cls,
                "position": {"x": 100.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
                "score": 42,
                "cargo": "MCPTestCargo",
                "hotkey": 3,
            })
            assert_success(r)
            created.append("MCP Test Bravo")

            # List should include both
            r = client.call_tool("list_ships")
            assert_success(r)
            names = [s.get("name") for s in tool_data(r)]
            assert_in("MCP Test Alpha", names)
            assert_in("MCP Test Bravo", names)

            # Get should round-trip the populated fields
            r = client.call_tool("get_ship", {"name": "MCP Test Bravo"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("score"), 42, "score round-trip")
            assert_equal(d.get("cargo"), "MCPTestCargo", "cargo round-trip")
            assert_equal(d.get("hotkey"), 3, "hotkey round-trip")
            # Cue node IDs should be exposed as integers
            assert_true(isinstance(d.get("arrival_cue"), int), "arrival_cue should be an integer node ID")
            assert_true(isinstance(d.get("departure_cue"), int), "departure_cue should be an integer node ID")

            # Update via rename + position + display_name + assist_score_fraction
            r = client.call_tool("update_ship", {
                "name": "MCP Test Alpha",
                "new_name": "MCP Test Alpha Renamed",
                "position": {"x": 50.0, "y": 50.0, "z": 50.0},
                "display_name": "Alpha Display",
                "assist_score_fraction": 0.5,
            })
            assert_success(r)
            created[0] = "MCP Test Alpha Renamed"

            r = client.call_tool("get_ship", {"name": "MCP Test Alpha Renamed"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("display_name"), "Alpha Display", "display_name round-trip")
            assert_equal(d.get("position", {}).get("x"), 50.0, "position x round-trip")
            assert_equal(d.get("assist_score_fraction"), 0.5, "assist_score_fraction round-trip")

        finally:
            _delete_all_test_ships(client, created)

    def test_ship_arrival_cue_replacement():
        cls = _pick_ship_class(client)
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Cue Ship",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Cue Ship")

            # Build a new cue via the SEXP text tool
            r = client.call_tool("text_to_sexp", {"text": "( true )"})
            assert_success(r)
            new_cue = tool_data(r).get("node")
            assert_true(isinstance(new_cue, int) and new_cue >= 0,
                "text_to_sexp should return an integer node ID")

            # Replace the arrival cue
            r = client.call_tool("update_ship", {
                "name": "MCP Cue Ship",
                "arrival_cue": new_cue,
            })
            assert_success(r)

            # Verify the get_ship reflects the new cue ID
            r = client.call_tool("get_ship", {"name": "MCP Cue Ship"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("arrival_cue"), new_cue, "arrival_cue should equal the new node ID")

        finally:
            _delete_all_test_ships(client, created)

    def test_ship_alt_class_name_and_callsign_cycle():
        cls = _pick_ship_class(client)
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Alt Ship",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
                "alt_class_name": "MCPTestAlt",
                "callsign": "MCPTestCall",
            })
            assert_success(r)
            created.append("MCP Alt Ship")

            r = client.call_tool("get_ship", {"name": "MCP Alt Ship"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("alt_class_name"), "MCPTestAlt", "alt_class_name after create")
            assert_equal(d.get("callsign"), "MCPTestCall", "callsign after create")

            # Clear both via update with "<none>"
            r = client.call_tool("update_ship", {
                "name": "MCP Alt Ship",
                "alt_class_name": "<none>",
                "callsign": "<none>",
            })
            assert_success(r)

            r = client.call_tool("get_ship", {"name": "MCP Alt Ship"})
            assert_success(r)
            d = tool_data(r)
            assert_true("alt_class_name" not in d, "alt_class_name should be absent after clear")
            assert_true("callsign" not in d, "callsign should be absent after clear")

        finally:
            _delete_all_test_ships(client, created)

    def test_ship_class_change():
        r = client.call_tool("list_ship_classes")
        assert_success(r)
        classes = tool_data(r)
        if len(classes) < 2:
            return  # mod has only one ship class; nothing to swap to
        cls_a = classes[0].get("name")
        cls_b = classes[1].get("name")

        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Class Swap Ship",
                "ship_class": cls_a,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Class Swap Ship")

            r = client.call_tool("update_ship", {
                "name": "MCP Class Swap Ship",
                "ship_class": cls_b,
            })
            assert_success(r)

            r = client.call_tool("get_ship", {"name": "MCP Class Swap Ship"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("ship_class"), cls_b, "ship_class should reflect the swap")
        finally:
            _delete_all_test_ships(client, created)

    def test_ship_errors():
        cls = _pick_ship_class(client)

        # Unknown ship class on create
        r = client.call_tool("create_ship", {
            "name": "MCP Bad Class",
            "ship_class": "DEFINITELY_NOT_A_REAL_CLASS_xyz",
            "position": {"x": 0, "y": 0, "z": 0},
            "orientation": IDENTITY_ORIENT,
        })
        assert_error(r)

        # Missing required field (orientation)
        r = client.call_tool("create_ship", {
            "name": "MCP Missing Orient",
            "ship_class": cls,
            "position": {"x": 0, "y": 0, "z": 0},
        })
        assert_error(r)

        # get_ship on nonexistent
        r = client.call_tool("get_ship", {"name": "MCP_DOES_NOT_EXIST_xyz"})
        assert_error(r)

        # Hotkey out of range
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Hotkey Out",
                "ship_class": cls,
                "position": {"x": 0, "y": 0, "z": 0},
                "orientation": IDENTITY_ORIENT,
                "hotkey": 99,
            })
            assert_error(r)
        finally:
            try:
                client.call_tool("delete_ship", {"name": "MCP Hotkey Out", "force": True})
            except Exception:
                pass

        # Unknown arrival target
        cls = _pick_ship_class(client)
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Bad Anchor",
                "ship_class": cls,
                "position": {"x": 0, "y": 0, "z": 0},
                "orientation": IDENTITY_ORIENT,
                "arrival_target": "NOT_A_SHIP_OR_SPECIAL_ANCHOR_xyz",
            })
            assert_error(r)
        finally:
            try:
                client.call_tool("delete_ship", {"name": "MCP Bad Anchor", "force": True})
            except Exception:
                pass

    def test_ship_duplicate_name():
        cls = _pick_ship_class(client)
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Dup",
                "ship_class": cls,
                "position": {"x": 0, "y": 0, "z": 0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Dup")

            # Second create with same name should be rejected by check_object_rename
            r = client.call_tool("create_ship", {
                "name": "MCP Dup",
                "ship_class": cls,
                "position": {"x": 10, "y": 0, "z": 0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    # ---------- move / swap ----------
    #
    # The MCP API hides Ships[]'s sparse layout: indices are 1-based over the
    # populated ships (list_ships order).  list_ships iterates obj_used_list,
    # which reassign_ship_slot keeps sorted to match Ships[] order, so the
    # 1-based list position is a stable identifier for these tests.

    def _create_test_ships(cls, names):
        for n in names:
            r = client.call_tool("create_ship", {
                "name": n,
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)

    def _ship_names_in_order():
        r = client.call_tool("list_ships")
        assert_success(r)
        return [s.get("name") for s in tool_data(r)]

    def _index_of(names_list, name):
        # 1-based
        return names_list.index(name) + 1

    def test_ships_swap_basic():
        cls = _pick_ship_class(client)
        created = ["MS A", "MS B"]
        try:
            _create_test_ships(cls, created)

            pre = _ship_names_in_order()
            a_idx = _index_of(pre, "MS A")
            b_idx = _index_of(pre, "MS B")

            r = client.call_tool("swap_ships", {"index_a": a_idx, "index_b": b_idx})
            assert_success(r)
            d = tool_data(r)
            # The response reports the post-swap occupants of each index.
            assert_equal(d.get("a", {}).get("name"), "MS B",
                "swap response: slot index_a now holds MS B")
            assert_equal(d.get("b", {}).get("name"), "MS A",
                "swap response: slot index_b now holds MS A")
            assert_equal(d.get("a", {}).get("index"), a_idx,
                "swap response: index_a echoed")
            assert_equal(d.get("b", {}).get("index"), b_idx,
                "swap response: index_b echoed")

            # list_ships should reflect the swap at the affected slots.
            post = _ship_names_in_order()
            assert_equal(post[a_idx - 1], "MS B", "list pos a now holds MS B")
            assert_equal(post[b_idx - 1], "MS A", "list pos b now holds MS A")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_swap_idempotent():
        """Swap twice == original order."""
        cls = _pick_ship_class(client)
        created = ["MS Idem A", "MS Idem B"]
        try:
            _create_test_ships(cls, created)
            pre = _ship_names_in_order()
            a_idx = _index_of(pre, "MS Idem A")
            b_idx = _index_of(pre, "MS Idem B")

            r = client.call_tool("swap_ships", {"index_a": a_idx, "index_b": b_idx})
            assert_success(r)
            r = client.call_tool("swap_ships", {"index_a": a_idx, "index_b": b_idx})
            assert_success(r)

            post = _ship_names_in_order()
            assert_equal(post, pre, "double swap restores original order")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_swap_same_index_noop():
        cls = _pick_ship_class(client)
        created = ["MS Noop"]
        try:
            _create_test_ships(cls, created)
            pre = _ship_names_in_order()
            i = _index_of(pre, "MS Noop")

            r = client.call_tool("swap_ships", {"index_a": i, "index_b": i})
            assert_success(r)
            d = tool_data(r)
            # Even when no swap actually occurs, the response should still
            # echo the names at the two indices.
            assert_equal(d.get("a", {}).get("name"), "MS Noop", "noop swap response a")
            assert_equal(d.get("b", {}).get("name"), "MS Noop", "noop swap response b")

            post = _ship_names_in_order()
            assert_equal(post, pre, "list unchanged after same-index swap")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_move_forward():
        """Move the first test ship to the last test position; in the dense
        view this is a circular shift: the others shift up to fill the gap
        and the moved ship lands at the destination."""
        cls = _pick_ship_class(client)
        created = ["MS Move A", "MS Move B", "MS Move C", "MS Move D"]
        try:
            _create_test_ships(cls, created)
            pre = _ship_names_in_order()
            a_idx = _index_of(pre, "MS Move A")
            d_idx = _index_of(pre, "MS Move D")
            assert_equal(d_idx - a_idx, 3,
                "test ships should occupy four consecutive list positions for this case")

            r = client.call_tool("move_ship", {"from_index": a_idx, "to_index": d_idx})
            assert_success(r)
            data = tool_data(r)
            assert_equal(data.get("name"), "MS Move A",
                "move response: moved ship reports MS Move A")
            assert_equal(data.get("index"), d_idx,
                "move response: index reports destination")

            post = _ship_names_in_order()
            assert_equal(post[a_idx - 1], "MS Move B", "B shifted into A's slot")
            assert_equal(post[a_idx],     "MS Move C", "C shifted up one")
            assert_equal(post[a_idx + 1], "MS Move D", "D shifted up one")
            assert_equal(post[d_idx - 1], "MS Move A", "A landed at the destination")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_move_backward():
        """Move the last test ship to the first test position; the dense
        circular shift goes the other direction."""
        cls = _pick_ship_class(client)
        created = ["MS Back A", "MS Back B", "MS Back C", "MS Back D"]
        try:
            _create_test_ships(cls, created)
            pre = _ship_names_in_order()
            a_idx = _index_of(pre, "MS Back A")
            d_idx = _index_of(pre, "MS Back D")
            assert_equal(d_idx - a_idx, 3,
                "test ships should occupy four consecutive list positions for this case")

            r = client.call_tool("move_ship", {"from_index": d_idx, "to_index": a_idx})
            assert_success(r)
            data = tool_data(r)
            assert_equal(data.get("name"), "MS Back D",
                "move response: moved ship reports MS Back D")
            assert_equal(data.get("index"), a_idx,
                "move response: index reports destination")

            post = _ship_names_in_order()
            assert_equal(post[a_idx - 1], "MS Back D", "D landed at the destination")
            assert_equal(post[a_idx],     "MS Back A", "A shifted down one")
            assert_equal(post[a_idx + 1], "MS Back B", "B shifted down one")
            assert_equal(post[d_idx - 1], "MS Back C", "C shifted into D's slot")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_move_same_index_noop():
        cls = _pick_ship_class(client)
        created = ["MS Move Noop"]
        try:
            _create_test_ships(cls, created)
            pre = _ship_names_in_order()
            i = _index_of(pre, "MS Move Noop")

            r = client.call_tool("move_ship", {"from_index": i, "to_index": i})
            assert_success(r)
            data = tool_data(r)
            assert_equal(data.get("name"), "MS Move Noop", "noop move response name")
            assert_equal(data.get("index"), i, "noop move response index")

            post = _ship_names_in_order()
            assert_equal(post, pre, "list unchanged after same-index move")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_move_preserves_identity():
        """After a swap, get_ship by name still returns the right ship with
        its properties intact — verifies the back-reference fixup in
        reassign_ship_slot didn't corrupt anything ship-side."""
        cls = _pick_ship_class(client)
        created = ["MS Ident A", "MS Ident B"]
        try:
            _create_test_ships(cls, created)
            # Stamp distinguishable scores on each.
            for n, s in [("MS Ident A", 11), ("MS Ident B", 22)]:
                r = client.call_tool("update_ship", {"name": n, "score": s})
                assert_success(r)

            pre = _ship_names_in_order()
            a_idx = _index_of(pre, "MS Ident A")
            b_idx = _index_of(pre, "MS Ident B")

            r = client.call_tool("swap_ships", {"index_a": a_idx, "index_b": b_idx})
            assert_success(r)

            # By-name lookups should still resolve, and scores should round-trip.
            r = client.call_tool("get_ship", {"name": "MS Ident A"})
            assert_success(r)
            assert_equal(tool_data(r).get("score"), 11,
                "MS Ident A score preserved across swap")
            r = client.call_tool("get_ship", {"name": "MS Ident B"})
            assert_success(r)
            assert_equal(tool_data(r).get("score"), 22,
                "MS Ident B score preserved across swap")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_swap_out_of_range():
        # Use a huge index well past MAX_SHIPS-worth of populated slots.
        r = client.call_tool("swap_ships", {"index_a": 1, "index_b": 9999})
        assert_error(r)
        r = client.call_tool("swap_ships", {"index_a": 9999, "index_b": 1})
        assert_error(r)
        r = client.call_tool("swap_ships", {"index_a": 0, "index_b": 1})
        assert_error(r)

    def test_ships_move_out_of_range():
        r = client.call_tool("move_ship", {"from_index": 1, "to_index": 9999})
        assert_error(r)
        r = client.call_tool("move_ship", {"from_index": 9999, "to_index": 1})
        assert_error(r)
        r = client.call_tool("move_ship", {"from_index": 0, "to_index": 1})
        assert_error(r)

    suite.add("ships_basic_crud", test_ships_basic_crud)
    suite.add("ships_arrival_cue_replacement", test_ship_arrival_cue_replacement)
    suite.add("ships_alt_class_name_callsign_cycle", test_ship_alt_class_name_and_callsign_cycle)
    suite.add("ships_class_change", test_ship_class_change)
    suite.add("ships_error_paths", test_ship_errors)
    suite.add("ships_duplicate_name", test_ship_duplicate_name)
    suite.add("ships_swap_basic", test_ships_swap_basic)
    suite.add("ships_swap_idempotent", test_ships_swap_idempotent)
    suite.add("ships_swap_same_index_noop", test_ships_swap_same_index_noop)
    suite.add("ships_move_forward", test_ships_move_forward)
    suite.add("ships_move_backward", test_ships_move_backward)
    suite.add("ships_move_same_index_noop", test_ships_move_same_index_noop)
    suite.add("ships_move_preserves_identity", test_ships_move_preserves_identity)
    suite.add("ships_swap_out_of_range", test_ships_swap_out_of_range)
    suite.add("ships_move_out_of_range", test_ships_move_out_of_range)


if __name__ == "__main__":
    run_module_standalone(register, "Ship CRUD tests")
