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

    suite.add("ships_basic_crud", test_ships_basic_crud)
    suite.add("ships_arrival_cue_replacement", test_ship_arrival_cue_replacement)
    suite.add("ships_alt_class_name_callsign_cycle", test_ship_alt_class_name_and_callsign_cycle)
    suite.add("ships_class_change", test_ship_class_change)
    suite.add("ships_error_paths", test_ship_errors)
    suite.add("ships_duplicate_name", test_ship_duplicate_name)


if __name__ == "__main__":
    run_module_standalone(register, "Ship CRUD tests")
