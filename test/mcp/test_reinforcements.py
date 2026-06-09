"""CRUD tests for the reinforcement MCP module.

Tests fabricate their own ships and wings, exercise the three-tool surface
(list_reinforcements / get_reinforcement / set_reinforcement), then clean up.
The fresh-mission prelude leaves Reinforcements empty, so empty-state checks
run before any seeding.
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
    tool_text,
)


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


def _safe_delete_ship(client, name):
    try:
        client.call_tool("delete_ship", {"name": name, "force": True})
    except Exception:
        pass


def _safe_delete_wing(client, name):
    try:
        client.call_tool("delete_wing", {"name": name, "force": True})
    except Exception:
        pass


def _safe_clear_reinforcement(client, name):
    try:
        client.call_tool("set_reinforcement", {"name": name, "enable": False})
    except Exception:
        pass


def _list_reinforcement_names(client):
    r = client.call_tool("list_reinforcements")
    assert_success(r)
    data = tool_data(r)
    assert_is_list(data, "list_reinforcements returned non-list")
    return [entry.get("name") for entry in data]


def register(suite, client):

    # ---------- list / get on empty mission ----------

    def test_reinforcements_list_empty():
        names = _list_reinforcement_names(client)
        assert_equal(names, [], "fresh mission has no reinforcements")

    def test_reinforcements_get_not_found():
        r = client.call_tool("get_reinforcement", {"name": "DOES_NOT_EXIST_xyz"})
        assert_error(r)

    # ---------- create + read on a ship ----------

    def test_reinforcements_ship_basic_roundtrip():
        cls = _pick_ship_class(client)
        ship = "MCP Reinf Ship"
        try:
            _create_ship(client, ship, cls)

            r = client.call_tool("set_reinforcement", {"name": ship})
            assert_success(r)
            entry = tool_data(r)
            assert_equal(entry.get("name"), ship, "set echoed ship name")
            # Engine defaults: uses=1, arrival_delay=0
            assert_equal(entry.get("uses"), 1, "default uses for a new ship reinforcement")
            assert_equal(entry.get("arrival_delay"), 0, "default arrival_delay")

            assert_in(ship, _list_reinforcement_names(client),
                "new ship reinforcement appears in list")

            r = client.call_tool("get_reinforcement", {"name": ship})
            assert_success(r)
            got = tool_data(r)
            assert_equal(got, entry, "get matches set return value")
        finally:
            _safe_clear_reinforcement(client, ship)
            _safe_delete_ship(client, ship)

    # ---------- create + read on a wing ----------

    def test_reinforcements_wing_basic_roundtrip():
        cls = _pick_ship_class(client)
        member = "MCP Reinf Wing Seed"
        wing = "MCP Reinf Wing"
        try:
            _create_ship(client, member, cls)
            r = client.call_tool("form_wing", {"name": wing, "members": [member]})
            assert_success(r)

            r = client.call_tool("set_reinforcement", {
                "name": wing,
                "uses": 3,
                "arrival_delay": 45,
            })
            assert_success(r)
            entry = tool_data(r)
            assert_equal(entry.get("name"), wing, "wing reinforcement echoes its name")
            assert_equal(entry.get("uses"), 3, "wing reinforcement uses applied on create")
            assert_equal(entry.get("arrival_delay"), 45, "wing arrival_delay applied on create")

            assert_in(wing, _list_reinforcement_names(client),
                "wing reinforcement appears in list")
        finally:
            _safe_clear_reinforcement(client, wing)
            _safe_delete_wing(client, wing)
            _safe_delete_ship(client, f"{wing} 1")
            _safe_delete_ship(client, member)

    # ---------- update existing entry ----------

    def test_reinforcements_update_existing():
        cls = _pick_ship_class(client)
        member = "MCP Reinf Upd Seed"
        wing = "MCP Reinf Upd"
        try:
            _create_ship(client, member, cls)
            r = client.call_tool("form_wing", {"name": wing, "members": [member]})
            assert_success(r)

            r = client.call_tool("set_reinforcement", {"name": wing})
            assert_success(r)

            # No enable flag — update only.
            r = client.call_tool("set_reinforcement", {
                "name": wing,
                "uses": 7,
                "arrival_delay": 12,
            })
            assert_success(r)
            entry = tool_data(r)
            assert_equal(entry.get("uses"), 7, "uses updated on existing entry")
            assert_equal(entry.get("arrival_delay"), 12, "arrival_delay updated")

            # Partial update — only one field.
            r = client.call_tool("set_reinforcement", {"name": wing, "uses": 9})
            assert_success(r)
            entry = tool_data(r)
            assert_equal(entry.get("uses"), 9, "partial update only changes specified field")
            assert_equal(entry.get("arrival_delay"), 12, "untouched field preserved")
        finally:
            _safe_clear_reinforcement(client, wing)
            _safe_delete_wing(client, wing)
            _safe_delete_ship(client, f"{wing} 1")
            _safe_delete_ship(client, member)

    def test_reinforcements_set_explicit_enable_true():
        """`enable: true` on an existing entry is a no-op for presence but
        still applies optional uses/arrival_delay updates."""
        cls = _pick_ship_class(client)
        ship = "MCP Reinf EnableTrue"
        try:
            _create_ship(client, ship, cls)

            r = client.call_tool("set_reinforcement", {"name": ship})
            assert_success(r)

            r = client.call_tool("set_reinforcement", {
                "name": ship,
                "enable": True,
                "arrival_delay": 22,
            })
            assert_success(r)
            entry = tool_data(r)
            assert_equal(entry.get("arrival_delay"), 22,
                "enable=true on existing applies arrival_delay")

            assert_in(ship, _list_reinforcement_names(client),
                "entry still present after enable=true on an existing entry")
        finally:
            _safe_clear_reinforcement(client, ship)
            _safe_delete_ship(client, ship)

    # ---------- remove ----------

    def test_reinforcements_remove():
        cls = _pick_ship_class(client)
        ship = "MCP Reinf Removable"
        try:
            _create_ship(client, ship, cls)

            r = client.call_tool("set_reinforcement", {"name": ship})
            assert_success(r)
            assert_in(ship, _list_reinforcement_names(client),
                "precondition: entry present before remove")

            r = client.call_tool("set_reinforcement", {"name": ship, "enable": False})
            assert_success(r)
            assert_true("removed" in tool_text(r).lower(),
                "remove response mentions removal")

            assert_true(ship not in _list_reinforcement_names(client),
                "entry gone after enable=false")

            # get_reinforcement on the same name now errors.
            r = client.call_tool("get_reinforcement", {"name": ship})
            assert_error(r)
        finally:
            _safe_clear_reinforcement(client, ship)
            _safe_delete_ship(client, ship)

    def test_reinforcements_remove_nonexistent_errors():
        cls = _pick_ship_class(client)
        ship = "MCP Reinf NeverSet"
        try:
            _create_ship(client, ship, cls)
            r = client.call_tool("set_reinforcement", {"name": ship, "enable": False})
            assert_error(r)
        finally:
            _safe_delete_ship(client, ship)

    # ---------- negative paths ----------

    def test_reinforcements_ship_in_wing_rejected():
        cls = _pick_ship_class(client)
        member = "MCP Reinf MemberSeed"
        wing = "MCP Reinf MemberWing"
        try:
            _create_ship(client, member, cls)
            r = client.call_tool("form_wing", {"name": wing, "members": [member]})
            assert_success(r)

            # The seed ship is now renamed to "<wing> 1" by form_wing's bashing.
            bashed = f"{wing} 1"

            r = client.call_tool("set_reinforcement", {"name": bashed})
            assert_error(r)
            assert_true("wing" in tool_text(r).lower(),
                "error message mentions the wing")

            # Confirm no entry was created.
            assert_true(bashed not in _list_reinforcement_names(client),
                "ship-in-wing rejection did not create an entry")
        finally:
            _safe_clear_reinforcement(client, wing)
            _safe_delete_wing(client, wing)
            _safe_delete_ship(client, f"{wing} 1")
            _safe_delete_ship(client, member)

    def test_reinforcements_unknown_target_rejected():
        r = client.call_tool("set_reinforcement", {"name": "DOES_NOT_EXIST_xyz"})
        assert_error(r)

    def test_reinforcements_uses_range():
        cls = _pick_ship_class(client)
        ship = "MCP Reinf UsesRange"
        try:
            _create_ship(client, ship, cls)

            r = client.call_tool("set_reinforcement", {"name": ship, "uses": 0})
            assert_error(r)
            r = client.call_tool("set_reinforcement", {"name": ship, "uses": 100})
            assert_error(r)

            # Validation runs before any side effects, so no entry should exist.
            assert_true(ship not in _list_reinforcement_names(client),
                "range-rejected set_reinforcement did not create an entry")
        finally:
            _safe_clear_reinforcement(client, ship)
            _safe_delete_ship(client, ship)

    def test_reinforcements_arrival_delay_range():
        cls = _pick_ship_class(client)
        ship = "MCP Reinf DelayRange"
        try:
            _create_ship(client, ship, cls)

            r = client.call_tool("set_reinforcement", {"name": ship, "arrival_delay": -1})
            assert_error(r)
            r = client.call_tool("set_reinforcement", {"name": ship, "arrival_delay": 1001})
            assert_error(r)

            assert_true(ship not in _list_reinforcement_names(client),
                "range-rejected set_reinforcement did not create an entry")
        finally:
            _safe_clear_reinforcement(client, ship)
            _safe_delete_ship(client, ship)

    # ---------- ship flag isolation ----------

    def test_reinforcements_ship_flag_not_exposed():
        """The `reinforcement` parse flag is filtered out of MCP's ship_flags
        surface (see mcp_ship_flag_excluded).  Confirm get_ship doesn't leak
        it even when the underlying flag is set."""
        cls = _pick_ship_class(client)
        ship = "MCP Reinf FlagIsolation"
        try:
            _create_ship(client, ship, cls)
            r = client.call_tool("set_reinforcement", {"name": ship})
            assert_success(r)

            r = client.call_tool("get_ship", {"name": ship})
            assert_success(r)
            data = tool_data(r)
            assert_true("reinforcement" not in (data.get("ship_flags") or []),
                "reinforcement flag must not leak into ship_flags")

            # Setting it via update_ship's wing_flags / ship_flags should be rejected
            # as an unknown flag name.
            r = client.call_tool("update_ship", {
                "name": ship,
                "ship_flags": {"reinforcement": True},
            })
            assert_error(r)
        finally:
            _safe_clear_reinforcement(client, ship)
            _safe_delete_ship(client, ship)

    suite.add("reinforcements_list_empty", test_reinforcements_list_empty)
    suite.add("reinforcements_get_not_found", test_reinforcements_get_not_found)
    suite.add("reinforcements_ship_basic_roundtrip", test_reinforcements_ship_basic_roundtrip)
    suite.add("reinforcements_wing_basic_roundtrip", test_reinforcements_wing_basic_roundtrip)
    suite.add("reinforcements_update_existing", test_reinforcements_update_existing)
    suite.add("reinforcements_set_explicit_enable_true", test_reinforcements_set_explicit_enable_true)
    suite.add("reinforcements_remove", test_reinforcements_remove)
    suite.add("reinforcements_remove_nonexistent_errors", test_reinforcements_remove_nonexistent_errors)
    suite.add("reinforcements_ship_in_wing_rejected", test_reinforcements_ship_in_wing_rejected)
    suite.add("reinforcements_unknown_target_rejected", test_reinforcements_unknown_target_rejected)
    suite.add("reinforcements_uses_range", test_reinforcements_uses_range)
    suite.add("reinforcements_arrival_delay_range", test_reinforcements_arrival_delay_range)
    suite.add("reinforcements_ship_flag_not_exposed", test_reinforcements_ship_flag_not_exposed)


if __name__ == "__main__":
    run_module_standalone(register, "Reinforcement CRUD tests")
