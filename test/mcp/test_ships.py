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
    assert_has_key,
    assert_in,
    assert_is_dict,
    assert_is_list,
    assert_non_empty_list,
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

    # ---------- ship_flags ----------
    #
    # ship_flags spans three engine flag enums (Ship_Flags, Object_Flags,
    # AI_Flags) dispatched via sexp_check_flag_arrays / sexp_alter_ship_flag_helper.
    # The names come from Parse_object_flags (same source as list_ship_flags).
    # IFF defaults can pre-seed some flags (e.g. Friendly seeds "cargo-known"),
    # so tests check inclusion/exclusion rather than exact array equality.

    def test_ship_flags_basic_round_trip():
        cls = _pick_ship_class(client)
        created = []
        try:
            # Create with one flag from each enum:
            #   cargo-known   -> Ship::Ship_Flags::Cargo_revealed
            #   protect-ship  -> Object::Object_Flags::Protected
            #   kamikaze      -> AI::AI_Flags::Kamikaze
            r = client.call_tool("create_ship", {
                "name": "MCP Flags Ship",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
                "ship_flags": {
                    "cargo-known": True,
                    "protect-ship": True,
                    "kamikaze": True,
                },
            })
            assert_success(r)
            created.append("MCP Flags Ship")
            d = tool_data(r)
            flags = d.get("ship_flags", [])
            assert_is_list(flags, "ship_flags")
            for n in ("cargo-known", "protect-ship", "kamikaze"):
                assert_in(n, flags)

            # Partial update: clear one, add one; others untouched.
            r = client.call_tool("update_ship", {
                "name": "MCP Flags Ship",
                "ship_flags": {"cargo-known": False, "ignore-count": True},
            })
            assert_success(r)
            flags = tool_data(r).get("ship_flags", [])
            assert_true("cargo-known" not in flags, "cargo-known cleared")
            assert_in("ignore-count", flags)
            assert_in("protect-ship", flags)
            assert_in("kamikaze", flags)
        finally:
            _delete_all_test_ships(client, created)

    def test_ship_flags_no_collide_inversion():
        # The Parse_object_flags entry {"no_collide", Object::Object_Flags::Collides, ...}
        # is inverted: setting "no_collide" true must CLEAR the Collides bit, and GET
        # must emit "no_collide" when Collides is clear.  sexp_alter_ship_flag_helper
        # handles the inversion internally; this verifies the GET pairs with it.
        cls = _pick_ship_class(client)
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP NoCollide Ship",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP NoCollide Ship")
            # Baseline: no_collide should be absent (Collides is set by default).
            flags = tool_data(r).get("ship_flags", [])
            assert_true("no_collide" not in flags, "no_collide absent at create")

            r = client.call_tool("update_ship", {
                "name": "MCP NoCollide Ship",
                "ship_flags": {"no_collide": True},
            })
            assert_success(r)
            flags = tool_data(r).get("ship_flags", [])
            assert_in("no_collide", flags)

            r = client.call_tool("update_ship", {
                "name": "MCP NoCollide Ship",
                "ship_flags": {"no_collide": False},
            })
            assert_success(r)
            flags = tool_data(r).get("ship_flags", [])
            assert_true("no_collide" not in flags, "no_collide cleared")
        finally:
            _delete_all_test_ships(client, created)

    def test_ship_flags_errors_and_atomicity():
        cls = _pick_ship_class(client)
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Flag Errs",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Flag Errs")

            # Make sure scannable starts cleared (some IFFs may not preseed it,
            # but be explicit so the atomicity check is unambiguous).
            client.call_tool("update_ship", {
                "name": "MCP Flag Errs",
                "ship_flags": {"scannable": False},
            })

            # Unknown flag name -> error mentioning list_ship_flags
            r = client.call_tool("update_ship", {
                "name": "MCP Flag Errs",
                "ship_flags": {"bogus-flag-xyz": True},
            })
            assert_error(r)

            # Non-bool value -> error
            r = client.call_tool("update_ship", {
                "name": "MCP Flag Errs",
                "ship_flags": {"cargo-known": "yes"},
            })
            assert_error(r)

            # Validate-before-mutate: mix valid + invalid -> error, valid NOT applied
            r = client.call_tool("update_ship", {
                "name": "MCP Flag Errs",
                "ship_flags": {"scannable": True, "bogus-flag-xyz": True},
            })
            assert_error(r)
            r = client.call_tool("get_ship", {"name": "MCP Flag Errs"})
            assert_success(r)
            flags = tool_data(r).get("ship_flags", [])
            assert_true("scannable" not in flags,
                "scannable must NOT be set when mixed with an invalid flag")
        finally:
            _delete_all_test_ships(client, created)

    def test_ship_flags_excluded_names_rejected():
        # Three parse flags are deliberately scrubbed from the MCP surface
        # (see flag_excluded in mcp_ships.cpp):
        #   player-start: edits flow through is_player_start (manages Player_starts and OBJ_START)
        #   immobile:     deprecated; callers should set don't-change-position + don't-change-orientation
        #   locked:       deprecated; callers should set ship-locked + weapons-locked
        # They must error on SET, must never appear in GET, and must be omitted
        # from list_ship_flags.  "immobile" in particular has a backdoor via
        # Object_flag_names -- the early flag_excluded check is what blocks it.
        cls = _pick_ship_class(client)
        created = []
        try:
            # list_ship_flags omits all three
            r = client.call_tool("list_ship_flags")
            assert_success(r)
            names = {e.get("name") for e in tool_data(r)}
            for excluded in ("player-start", "immobile", "locked"):
                assert_true(excluded not in names,
                    f"list_ship_flags must omit {excluded!r}")

            r = client.call_tool("create_ship", {
                "name": "MCP Excluded Flags",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Excluded Flags")

            # SET rejected for each excluded name
            for excluded in ("player-start", "immobile", "locked"):
                r = client.call_tool("update_ship", {
                    "name": "MCP Excluded Flags",
                    "ship_flags": {excluded: True},
                })
                assert_error(r)

            # GET must not surface "immobile" when its two half-flags are set
            r = client.call_tool("update_ship", {
                "name": "MCP Excluded Flags",
                "ship_flags": {
                    "don't-change-position": True,
                    "don't-change-orientation": True,
                },
            })
            assert_success(r)
            flags = tool_data(r).get("ship_flags", [])
            assert_in("don't-change-position", flags)
            assert_in("don't-change-orientation", flags)
            assert_true("immobile" not in flags,
                "immobile must not surface via the two half-flags")

            # GET must not surface "locked" when both lock half-flags are set
            r = client.call_tool("update_ship", {
                "name": "MCP Excluded Flags",
                "ship_flags": {"ship-locked": True, "weapons-locked": True},
            })
            assert_success(r)
            flags = tool_data(r).get("ship_flags", [])
            assert_in("ship-locked", flags)
            assert_in("weapons-locked", flags)
            assert_true("locked" not in flags,
                "locked must not surface as a composite of the two lock flags")
        finally:
            _delete_all_test_ships(client, created)

    def test_ship_flag_paired_scalars():
        # Three flag-paired scalars surface as typed top-level fields:
        #   escort + escort_priority
        #   kamikaze + kamikaze_damage
        #   (Kill_before_mission has no flag entry; destroy_before_mission_seconds
        #    is the single source of truth -- non-null enables it, null clears.)
        # Scalars are emitted only when the matching flag is set.
        cls = _pick_ship_class(client)
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Paired Scalars",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
                "ship_flags": {"escort": True, "kamikaze": True},
                "escort_priority": 7,
                "kamikaze_damage": 250,
                "destroy_before_mission_seconds": 30,
            })
            assert_success(r)
            created.append("MCP Paired Scalars")
            d = tool_data(r)
            flags = d.get("ship_flags", [])
            assert_in("escort", flags)
            assert_in("kamikaze", flags)
            assert_equal(d.get("escort_priority"), 7, "escort_priority echoed")
            assert_equal(d.get("kamikaze_damage"), 250, "kamikaze_damage echoed")
            assert_equal(d.get("destroy_before_mission_seconds"), 30,
                "destroy_before_mission_seconds echoed")

            # Adjust scalars without touching flags.
            r = client.call_tool("update_ship", {
                "name": "MCP Paired Scalars",
                "escort_priority": 1,
                "kamikaze_damage": 500,
                "destroy_before_mission_seconds": 60,
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("escort_priority"), 1, "escort_priority updated")
            assert_equal(d.get("kamikaze_damage"), 500, "kamikaze_damage updated")
            assert_equal(d.get("destroy_before_mission_seconds"), 60,
                "destroy_before_mission_seconds updated")

            # Clear flags via ship_flags: scalars are still stored but no longer
            # surfaced in the response (qtFRED semantics: scalar storage is
            # independent of flag state).
            r = client.call_tool("update_ship", {
                "name": "MCP Paired Scalars",
                "ship_flags": {"escort": False, "kamikaze": False},
                "destroy_before_mission_seconds": -1,
            })
            assert_success(r)
            d = tool_data(r)
            flags = d.get("ship_flags", [])
            assert_true("escort" not in flags, "escort cleared")
            assert_true("kamikaze" not in flags, "kamikaze cleared")
            assert_true("escort_priority" not in d,
                "escort_priority must not surface when escort is off")
            assert_true("kamikaze_damage" not in d,
                "kamikaze_damage must not surface when kamikaze is off")
            assert_true("destroy_before_mission_seconds" not in d,
                "destroy_before_mission_seconds must not surface after -1 disable")
        finally:
            _delete_all_test_ships(client, created)

    def test_ship_flag_paired_scalars_errors():
        cls = _pick_ship_class(client)
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Paired Errs",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Paired Errs")

            # Non-negative integer required.
            r = client.call_tool("update_ship", {
                "name": "MCP Paired Errs", "escort_priority": -1,
            })
            assert_error(r)
            r = client.call_tool("update_ship", {
                "name": "MCP Paired Errs", "kamikaze_damage": -5,
            })
            assert_error(r)
            # -1 is the disable sentinel, so the first out-of-range value is -2.
            r = client.call_tool("update_ship", {
                "name": "MCP Paired Errs", "destroy_before_mission_seconds": -2,
            })
            assert_error(r)

            # Wrong type.
            r = client.call_tool("update_ship", {
                "name": "MCP Paired Errs", "escort_priority": "five",
            })
            assert_error(r)
            r = client.call_tool("update_ship", {
                "name": "MCP Paired Errs", "destroy_before_mission_seconds": "thirty",
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    # ---------- initial state percents ----------
    #
    # initial_hull_percent / initial_shield_percent / initial_speed_percent
    # are stored directly in the runtime object fields when FRED is running
    # (hull_strength, shield_quadrant[0], phys_info.speed) -- the engine
    # normalizes them to 0-100 percents in edit mode so missionsave can emit
    # them as "+Initial Hull/Shields/Velocity:" without conversion.

    def test_ship_initial_percents_round_trip():
        cls = _pick_ship_class(client)
        created = []
        try:
            # Create with explicit values for all three.
            r = client.call_tool("create_ship", {
                "name": "MCP Initial Pcts",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
                "initial_hull_percent": 80,
                "initial_shield_percent": 50,
                "initial_speed_percent": 25,
            })
            assert_success(r)
            created.append("MCP Initial Pcts")
            d = tool_data(r)
            assert_equal(d.get("initial_hull_percent"), 80, "initial_hull_percent echoed")
            assert_equal(d.get("initial_shield_percent"), 50, "initial_shield_percent echoed")
            assert_equal(d.get("initial_speed_percent"), 25, "initial_speed_percent echoed")

            # Update with new values.
            r = client.call_tool("update_ship", {
                "name": "MCP Initial Pcts",
                "initial_hull_percent": 100,
                "initial_shield_percent": 0,
                "initial_speed_percent": 100,
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("initial_hull_percent"), 100, "initial_hull_percent updated")
            assert_equal(d.get("initial_shield_percent"), 0, "initial_shield_percent updated")
            assert_equal(d.get("initial_speed_percent"), 100, "initial_speed_percent updated")

            # Defaults are visible on a ship created without these fields.
            r = client.call_tool("create_ship", {
                "name": "MCP Initial Defaults",
                "ship_class": cls,
                "position": {"x": 100.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Initial Defaults")
            d = tool_data(r)
            # FRED defaults: hull=100, shield=100, speed=33.
            assert_equal(d.get("initial_hull_percent"), 100, "default hull")
            assert_equal(d.get("initial_shield_percent"), 100, "default shield")
            assert_equal(d.get("initial_speed_percent"), 33, "default speed")
        finally:
            _delete_all_test_ships(client, created)

    def test_ship_initial_shield_percent_no_shields_gating():
        # initial_shield_percent is meaningful only when the no-shields flag is
        # off.  When the flag is on, GET responses omit the field (the stored
        # value is inert), but updates remain permitted so callers can pre-stage
        # a value for when the flag is later turned off.
        cls = _pick_ship_class(client)
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP NoShield Init",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
                "initial_shield_percent": 60,
            })
            assert_success(r)
            created.append("MCP NoShield Init")
            d = tool_data(r)
            assert_equal(d.get("initial_shield_percent"), 60, "visible at create")

            # Turn on no-shields: field disappears.
            r = client.call_tool("update_ship", {
                "name": "MCP NoShield Init",
                "ship_flags": {"no-shields": True},
            })
            assert_success(r)
            d = tool_data(r)
            assert_true("initial_shield_percent" not in d,
                "initial_shield_percent must be omitted when no-shields is set")

            # Update the value while no-shields is on: succeeds, still omitted.
            r = client.call_tool("update_ship", {
                "name": "MCP NoShield Init",
                "initial_shield_percent": 25,
            })
            assert_success(r)
            d = tool_data(r)
            assert_true("initial_shield_percent" not in d,
                "still omitted after a write while flag is set")

            # Clear the flag: stored value re-emerges.
            r = client.call_tool("update_ship", {
                "name": "MCP NoShield Init",
                "ship_flags": {"no-shields": False},
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("initial_shield_percent"), 25,
                "stored value visible again after no-shields cleared")
        finally:
            _delete_all_test_ships(client, created)

    def test_ship_initial_percents_errors():
        cls = _pick_ship_class(client)
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Initial Errs",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Initial Errs")

            # Out of range (0-100).
            for field in ("initial_hull_percent", "initial_shield_percent", "initial_speed_percent"):
                r = client.call_tool("update_ship", {"name": "MCP Initial Errs", field: -1})
                assert_error(r)
                r = client.call_tool("update_ship", {"name": "MCP Initial Errs", field: 101})
                assert_error(r)
                r = client.call_tool("update_ship", {"name": "MCP Initial Errs", field: "half"})
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

    # ---------------------------------------------------------------------
    # Docking
    # ---------------------------------------------------------------------
    #
    # Need a ship class with at least two dockpoints whose type-flag sets
    # overlap so the same class can dock to itself.  Iterate list_ship_classes
    # until we find one (most cap ships have multiple generic bays).  Tests
    # that need this skip cleanly when no suitable class exists.

    def _big_or_huge_classes():
        """Return ship-class entries whose size_classification is 'big' or
        'huge'.  Used by dockability and cargo-scanning helpers to skip
        fighters/bombers, which can't satisfy those tests' preconditions and
        would otherwise force a model-load per skipped class."""
        r = client.call_tool("list_ship_classes")
        assert_success(r)
        return [c for c in tool_data(r)
                if c.get("size_classification") in ("big", "huge")]

    def _pick_dockable_class():
        """Return (class_name, [point1_name, point2_name]) where point1 and
        point2 can be docked together (their dock_types intersect).  Returns
        None if no class has two such dockpoints."""
        for c in _big_or_huge_classes():
            name = c.get("name")
            if not name:
                continue
            rr = client.call_tool("list_ship_class_dockpoints", {"ship_class": name})
            if rr.get("isError"):
                continue
            bays = tool_data(rr)
            if len(bays) < 2:
                continue
            # Pick the first two bays whose dock_types overlap.
            for i in range(len(bays)):
                ti = set(bays[i].get("dock_types", []))
                if not ti:
                    continue
                for j in range(len(bays)):
                    if i == j:
                        continue
                    tj = set(bays[j].get("dock_types", []))
                    if ti & tj:
                        return (name, [bays[i]["name"], bays[j]["name"]])
        return None

    def _create_dockable_pair(cls, name_a, name_b):
        client.call_tool("create_ship", {
            "name": name_a, "ship_class": cls,
            "position": {"x": 0.0, "y": 0.0, "z": 0.0},
            "orientation": IDENTITY_ORIENT,
        })
        client.call_tool("create_ship", {
            "name": name_b, "ship_class": cls,
            "position": {"x": 200.0, "y": 0.0, "z": 0.0},
            "orientation": IDENTITY_ORIENT,
        })

    def test_ships_dock_basic_round_trip():
        pick = _pick_dockable_class()
        if pick is None:
            return  # skip cleanly if no class has compatible dockpoints
        cls, (pt1, pt2) = pick
        created = ["MCP Dock A", "MCP Dock B"]
        try:
            _create_dockable_pair(cls, *created)
            r = client.call_tool("dock_ships", {
                "docker": "MCP Dock A", "dockee": "MCP Dock B",
                "docker_point": pt1, "dockee_point": pt2,
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("docker"), "MCP Dock A", "echoed docker")
            assert_equal(d.get("dockee"), "MCP Dock B", "echoed dockee")
            assert_equal(d.get("docker_point"), pt1, "echoed docker_point")
            assert_equal(d.get("dockee_point"), pt2, "echoed dockee_point")
            # Dock leader is one of the two ships
            assert_in(d.get("dock_leader"), ["MCP Dock A", "MCP Dock B"])

            # get_ship reports docked_to + is_dock_leader on both sides.
            ra = tool_data(client.call_tool("get_ship", {"name": "MCP Dock A"}))
            rb = tool_data(client.call_tool("get_ship", {"name": "MCP Dock B"}))
            assert_in("docked_to", ra)
            assert_in("docked_to", rb)
            assert_equal(len(ra["docked_to"]), 1, "A docked_to length")
            assert_equal(ra["docked_to"][0]["ship"], "MCP Dock B", "A->B partner")
            assert_equal(ra["docked_to"][0]["my_dockpoint"], pt1, "A my_dockpoint")
            assert_equal(ra["docked_to"][0]["other_dockpoint"], pt2, "A other_dockpoint")
            assert_equal(rb["docked_to"][0]["ship"], "MCP Dock A", "B->A partner")
            # Mirror: my_dockpoint is always THIS ship's point, regardless of
            # which side was the docker in the original dock_ships call.
            assert_equal(rb["docked_to"][0]["my_dockpoint"], pt2, "B my_dockpoint")
            assert_equal(rb["docked_to"][0]["other_dockpoint"], pt1, "B other_dockpoint")
            assert_in("is_dock_leader", ra)
            assert_in("is_dock_leader", rb)
            # Exactly one of them is the leader.
            leaders = [s for s in (ra, rb) if s.get("is_dock_leader")]
            assert_equal(len(leaders), 1, "exactly one dock leader")

            # Undock the pair.
            r = client.call_tool("undock_ships", {
                "ship": "MCP Dock A", "other_ship": "MCP Dock B",
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("ship"), "MCP Dock A")
            assert_equal(d.get("other_ship"), "MCP Dock B")
            # No further partners after this undock of a one-on-one pair.
            assert_equal(d.get("still_docked_to"), [], "still_docked_to empty after final undock")

            # docked_to / is_dock_leader disappear when undocked.
            ra = tool_data(client.call_tool("get_ship", {"name": "MCP Dock A"}))
            rb = tool_data(client.call_tool("get_ship", {"name": "MCP Dock B"}))
            assert_true("docked_to" not in ra, "A docked_to omitted when undocked")
            assert_true("docked_to" not in rb, "B docked_to omitted when undocked")
            assert_true("is_dock_leader" not in ra, "A is_dock_leader omitted")
            assert_true("is_dock_leader" not in rb, "B is_dock_leader omitted")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_dock_validation():
        pick = _pick_dockable_class()
        if pick is None:
            return
        cls, (pt1, pt2) = pick
        created = ["MCP DV A", "MCP DV B"]
        try:
            _create_dockable_pair(cls, *created)

            # Self-dock
            r = client.call_tool("dock_ships", {
                "docker": "MCP DV A", "dockee": "MCP DV A",
                "docker_point": pt1, "dockee_point": pt2,
            })
            assert_error(r)

            # Unknown ship name
            r = client.call_tool("dock_ships", {
                "docker": "MCP DV A", "dockee": "Does Not Exist",
                "docker_point": pt1, "dockee_point": pt2,
            })
            assert_error(r)

            # Unknown dockpoint name
            r = client.call_tool("dock_ships", {
                "docker": "MCP DV A", "dockee": "MCP DV B",
                "docker_point": "no-such-point", "dockee_point": pt2,
            })
            assert_error(r)

            # Now do a real dock.
            r = client.call_tool("dock_ships", {
                "docker": "MCP DV A", "dockee": "MCP DV B",
                "docker_point": pt1, "dockee_point": pt2,
            })
            assert_success(r)

            # Already docked.
            r = client.call_tool("dock_ships", {
                "docker": "MCP DV A", "dockee": "MCP DV B",
                "docker_point": pt1, "dockee_point": pt2,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_undock_derive_partner():
        pick = _pick_dockable_class()
        if pick is None:
            return
        cls, (pt1, pt2) = pick
        created = ["MCP UD A", "MCP UD B"]
        try:
            _create_dockable_pair(cls, *created)
            r = client.call_tool("dock_ships", {
                "docker": "MCP UD A", "dockee": "MCP UD B",
                "docker_point": pt1, "dockee_point": pt2,
            })
            assert_success(r)

            # Omit other_ship; should derive "MCP UD B".
            r = client.call_tool("undock_ships", {"ship": "MCP UD A"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("other_ship"), "MCP UD B", "derived partner")

            ra = tool_data(client.call_tool("get_ship", {"name": "MCP UD A"}))
            assert_true("docked_to" not in ra, "A no longer docked")
        finally:
            _delete_all_test_ships(client, created)

    def _pick_dockable_class_three_points():
        """Like _pick_dockable_class but requires the class to have three
        dockpoints that can all be paired with two others.  We just look for
        a class with >= 3 dockpoints whose types overlap pairwise."""
        for c in _big_or_huge_classes():
            name = c.get("name")
            if not name:
                continue
            rr = client.call_tool("list_ship_class_dockpoints", {"ship_class": name})
            if rr.get("isError"):
                continue
            bays = tool_data(rr)
            if len(bays) < 3:
                continue
            # Pick three points where bay0 overlaps bay1 and bay0 overlaps bay2.
            for i in range(len(bays)):
                ti = set(bays[i].get("dock_types", []))
                if not ti:
                    continue
                compat = []
                for j in range(len(bays)):
                    if i == j:
                        continue
                    if set(bays[j].get("dock_types", [])) & ti:
                        compat.append(bays[j]["name"])
                        if len(compat) == 2:
                            return (name, bays[i]["name"], compat[0], compat[1])
        return None

    def test_ships_undock_ambiguous():
        pick = _pick_dockable_class_three_points()
        if pick is None:
            return
        cls, hub_pt, leaf_pt_b, leaf_pt_c = pick
        # Hub will be A; B docks at leaf_pt_b on hub_pt's side... actually
        # the hub's pt is what B and C connect TO.  So both B and C use leaf_pt
        # as their docker_point and hub_pt is unused -- wait, that's wrong.
        # Re-think: A has dockpoints {hub_pt, leaf_pt_b, leaf_pt_c}.
        # We dock B to A on hub_pt <-> leaf_pt_b (B uses some dockpoint compatible),
        # and C to A on hub_pt <-> leaf_pt_c.  But A can only use hub_pt once.
        # The simpler arrangement: A has multiple dockpoints; B and C each dock
        # to a *different* dockpoint on A.  So we just need 2 dockpoints on A
        # that B/C can connect to.  Re-pick using leaf_pt_b and leaf_pt_c as
        # the two dockpoints on A.
        a_pt_for_b = leaf_pt_b
        a_pt_for_c = leaf_pt_c
        b_pt = hub_pt
        c_pt = hub_pt
        created = ["MCP UA Hub", "MCP UA B", "MCP UA C"]
        try:
            for nm, x in zip(created, [0.0, 200.0, 400.0]):
                r = client.call_tool("create_ship", {
                    "name": nm, "ship_class": cls,
                    "position": {"x": x, "y": 0.0, "z": 0.0},
                    "orientation": IDENTITY_ORIENT,
                })
                assert_success(r)
            r = client.call_tool("dock_ships", {
                "docker": "MCP UA Hub", "dockee": "MCP UA B",
                "docker_point": a_pt_for_b, "dockee_point": b_pt,
            })
            assert_success(r)
            r = client.call_tool("dock_ships", {
                "docker": "MCP UA Hub", "dockee": "MCP UA C",
                "docker_point": a_pt_for_c, "dockee_point": c_pt,
            })
            assert_success(r)

            # Hub is docked to 2 ships; undock without other_ship should error.
            r = client.call_tool("undock_ships", {"ship": "MCP UA Hub"})
            assert_error(r)

            # Disambiguated undock of one partner: still_docked_to should list
            # the remaining partner (the other dock attachment is untouched).
            r = client.call_tool("undock_ships", {
                "ship": "MCP UA Hub", "other_ship": "MCP UA B",
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("ship"), "MCP UA Hub")
            assert_equal(d.get("other_ship"), "MCP UA B")
            assert_equal(d.get("still_docked_to"), ["MCP UA C"],
                "still_docked_to should list the remaining partner after partial undock")

            # Now undock the last partner: still_docked_to becomes empty.
            r = client.call_tool("undock_ships", {
                "ship": "MCP UA Hub", "other_ship": "MCP UA C",
            })
            assert_success(r)
            assert_equal(tool_data(r).get("still_docked_to"), [],
                "still_docked_to empty once hub has no partners")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_undock_all():
        pick = _pick_dockable_class_three_points()
        if pick is None:
            return
        cls, hub_pt, a_pt_for_b, a_pt_for_c = pick
        b_pt = hub_pt
        c_pt = hub_pt
        created = ["MCP UA2 Hub", "MCP UA2 B", "MCP UA2 C"]
        try:
            for nm, x in zip(created, [0.0, 200.0, 400.0]):
                r = client.call_tool("create_ship", {
                    "name": nm, "ship_class": cls,
                    "position": {"x": x, "y": 0.0, "z": 0.0},
                    "orientation": IDENTITY_ORIENT,
                })
                assert_success(r)
            r = client.call_tool("dock_ships", {
                "docker": "MCP UA2 Hub", "dockee": "MCP UA2 B",
                "docker_point": a_pt_for_b, "dockee_point": b_pt,
            })
            assert_success(r)
            r = client.call_tool("dock_ships", {
                "docker": "MCP UA2 Hub", "dockee": "MCP UA2 C",
                "docker_point": a_pt_for_c, "dockee_point": c_pt,
            })
            assert_success(r)

            r = client.call_tool("undock_all_ships", {"ship": "MCP UA2 Hub"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("ship"), "MCP UA2 Hub")
            partners = set(d.get("formerly_docked_to", []))
            assert_equal(partners, {"MCP UA2 B", "MCP UA2 C"}, "formerly_docked_to set")

            for name in created:
                rr = tool_data(client.call_tool("get_ship", {"name": name}))
                assert_true("docked_to" not in rr, f"{name} no longer docked")
        finally:
            _delete_all_test_ships(client, created)

    # ---------- list_docked_group / set_dock_leader ----------

    def test_ships_list_docked_group_undocked():
        cls = _pick_ship_class(client)
        created = ["MCP LG Solo"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP LG Solo", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("list_docked_group", {"ship": "MCP LG Solo"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("ship"), "MCP LG Solo")
            assert_equal(d.get("dock_leader"), None,
                "dock_leader should be null for a solitary ship")
            assert_equal(d.get("ships"), ["MCP LG Solo"], "single-element group")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_list_docked_group_pair():
        pick = _pick_dockable_class()
        if pick is None:
            return
        cls, (pt1, pt2) = pick
        created = ["MCP LG A", "MCP LG B"]
        try:
            _create_dockable_pair(cls, *created)
            r = client.call_tool("dock_ships", {
                "docker": "MCP LG A", "dockee": "MCP LG B",
                "docker_point": pt1, "dockee_point": pt2,
            })
            assert_success(r)

            # Calling from either side returns the same group.
            for query in ("MCP LG A", "MCP LG B"):
                r = client.call_tool("list_docked_group", {"ship": query})
                assert_success(r)
                d = tool_data(r)
                assert_equal(set(d.get("ships", [])), {"MCP LG A", "MCP LG B"},
                    f"group from {query}")
                assert_in(d.get("dock_leader"), ["MCP LG A", "MCP LG B"],
                    "dock_leader must be one of the pair")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_list_docked_group_chain():
        pick = _pick_dockable_class_three_points()
        if pick is None:
            return
        cls, hub_pt, a_pt_for_b, a_pt_for_c = pick
        created = ["MCP LG3 Hub", "MCP LG3 B", "MCP LG3 C"]
        try:
            for nm, x in zip(created, [0.0, 200.0, 400.0]):
                r = client.call_tool("create_ship", {
                    "name": nm, "ship_class": cls,
                    "position": {"x": x, "y": 0.0, "z": 0.0},
                    "orientation": IDENTITY_ORIENT,
                })
                assert_success(r)
            r = client.call_tool("dock_ships", {
                "docker": "MCP LG3 Hub", "dockee": "MCP LG3 B",
                "docker_point": a_pt_for_b, "dockee_point": hub_pt,
            })
            assert_success(r)
            r = client.call_tool("dock_ships", {
                "docker": "MCP LG3 Hub", "dockee": "MCP LG3 C",
                "docker_point": a_pt_for_c, "dockee_point": hub_pt,
            })
            assert_success(r)

            # Calling from any of the three should yield the same group.
            for query in created:
                r = client.call_tool("list_docked_group", {"ship": query})
                assert_success(r)
                d = tool_data(r)
                assert_equal(set(d.get("ships", [])),
                    {"MCP LG3 Hub", "MCP LG3 B", "MCP LG3 C"},
                    f"group from {query}")
                assert_in(d.get("dock_leader"), created,
                    "dock_leader must be one of the three")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_set_dock_leader_swap():
        pick = _pick_dockable_class()
        if pick is None:
            return
        cls, (pt1, pt2) = pick
        created = ["MCP SL A", "MCP SL B"]
        try:
            _create_dockable_pair(cls, *created)
            r = client.call_tool("dock_ships", {
                "docker": "MCP SL A", "dockee": "MCP SL B",
                "docker_point": pt1, "dockee_point": pt2,
            })
            assert_success(r)
            auto_leader = tool_data(r).get("dock_leader")
            other = "MCP SL B" if auto_leader == "MCP SL A" else "MCP SL A"

            # Capture the leader's arrival cue before the swap.
            r = client.call_tool("get_ship", {"name": auto_leader})
            assert_success(r)
            leader_cue_before = tool_data(r).get("arrival_cue")

            # Promote the other ship.
            r = client.call_tool("set_dock_leader", {"ship": other})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("ship"), other)
            assert_equal(d.get("previous_leader"), auto_leader,
                "previous_leader should be the auto-picked ship")

            # New leader has is_dock_leader: true, old one does not.
            ra = tool_data(client.call_tool("get_ship", {"name": other}))
            rb = tool_data(client.call_tool("get_ship", {"name": auto_leader}))
            assert_equal(ra.get("is_dock_leader"), True, "new leader flag set")
            assert_equal(rb.get("is_dock_leader"), False, "old leader flag cleared")

            # The leader's cue moved with the title.
            assert_equal(ra.get("arrival_cue"), leader_cue_before,
                "new leader carries the previous leader's arrival cue")
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_set_dock_leader_already_leader_noop():
        pick = _pick_dockable_class()
        if pick is None:
            return
        cls, (pt1, pt2) = pick
        created = ["MCP SL2 A", "MCP SL2 B"]
        try:
            _create_dockable_pair(cls, *created)
            r = client.call_tool("dock_ships", {
                "docker": "MCP SL2 A", "dockee": "MCP SL2 B",
                "docker_point": pt1, "dockee_point": pt2,
            })
            assert_success(r)
            auto_leader = tool_data(r).get("dock_leader")

            r = client.call_tool("set_dock_leader", {"ship": auto_leader})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("ship"), auto_leader)
            assert_equal(d.get("previous_leader"), auto_leader,
                "previous_leader echoes the same ship for no-op")

            # Still the leader; flag still set.
            rr = tool_data(client.call_tool("get_ship", {"name": auto_leader}))
            assert_equal(rr.get("is_dock_leader"), True)
        finally:
            _delete_all_test_ships(client, created)

    def test_ships_set_dock_leader_validation():
        cls = _pick_ship_class(client)
        created = ["MCP SLV Solo"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP SLV Solo", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)

            # Unknown ship.
            r = client.call_tool("set_dock_leader", {"ship": "Does Not Exist"})
            assert_error(r)

            # Solitary ship is not docked → error.
            r = client.call_tool("set_dock_leader", {"ship": "MCP SLV Solo"})
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    # ---------------------------------------------------------------------
    # Weapon-bank tools
    # ---------------------------------------------------------------------
    #
    # Most of these depend on mod content (a ship class with banks, a weapon
    # of a given type, a ballistic weapon for the ammo tests).  Each test
    # uses a _pick_* helper and returns early if the required content is
    # absent so the suite stays green on minimal data setups.

    def _pick_ship_class_with_banks(kind):
        """First ship class with at least one bank of the given kind
        ("primary" or "secondary"), or None if none found."""
        r = client.call_tool("list_ship_classes")
        assert_success(r)
        for c in tool_data(r):
            name = c.get("name")
            if not name:
                continue
            rr = client.call_tool("get_ship_class", {"name": name})
            if rr.get("isError"):
                continue
            d = tool_data(rr)
            key = "primary_banks" if kind == "primary" else "secondary_banks"
            banks = d.get(key) or []
            if len(banks) > 0:
                return name
        return None

    def _pick_selectable_weapon(subtype):
        """First weapon of given subtype that is selectable_in_editor."""
        r = client.call_tool("list_weapon_classes", {"subtype": subtype})
        assert_success(r)
        for w in tool_data(r):
            if w.get("selectable_in_editor", True):
                return w.get("name")
        return None

    def test_list_ship_weapons_pilot_shape():
        cls = _pick_ship_class_with_banks("primary")
        if cls is None:
            return
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Weapons Shape",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Weapons Shape")

            r = client.call_tool("list_ship_weapons", {"name": "MCP Weapons Shape"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("ship"), "MCP Weapons Shape", "ship name echoed")

            pilot = d.get("pilot")
            assert_is_dict(pilot, "pilot block")

            primaries = pilot.get("primary_banks")
            assert_non_empty_list(primaries, "pilot.primary_banks")
            for i, bank in enumerate(primaries):
                assert_equal(bank.get("bank"), i + 1, f"primary bank index {i}")
                assert_has_key(bank, "weapon_class", f"primary bank {i+1} weapon_class")
                assert_has_key(bank, "ammo_count", f"primary bank {i+1} ammo_count")

            secondaries = pilot.get("secondary_banks")
            assert_is_list(secondaries, "pilot.secondary_banks")

            turrets = d.get("turrets")
            assert_is_list(turrets, "turrets")
        finally:
            _delete_all_test_ships(client, created)

    def test_get_ship_weapon_bank_pilot_default_matches_explicit():
        cls = _pick_ship_class_with_banks("primary")
        if cls is None:
            return
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Weapons Default",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Weapons Default")

            base = {"name": "MCP Weapons Default", "bank_type": "primary", "bank": 1}
            r_default = client.call_tool("get_ship_weapon_bank", base)
            r_pilot = client.call_tool("get_ship_weapon_bank", dict(base, subsystem="Pilot"))
            assert_success(r_default)
            assert_success(r_pilot)
            assert_equal(tool_data(r_default), tool_data(r_pilot),
                "Default subsystem should equal explicit 'Pilot'")
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_weapon_bank_change_primary():
        cls = _pick_ship_class_with_banks("primary")
        if cls is None:
            return
        weapon = _pick_selectable_weapon("primary")
        if weapon is None:
            return
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Wp Change",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Wp Change")

            r = client.call_tool("update_ship_weapon_bank", {
                "name": "MCP Wp Change", "bank_type": "primary", "bank": 1,
                "weapon_class": weapon, "force": True,
            })
            assert_success(r)

            r = client.call_tool("get_ship_weapon_bank", {
                "name": "MCP Wp Change", "bank_type": "primary", "bank": 1,
            })
            assert_success(r)
            assert_equal(tool_data(r).get("weapon_class"), weapon,
                "primary bank 1 weapon round-trip")
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_weapon_bank_clear():
        cls = _pick_ship_class_with_banks("primary")
        if cls is None:
            return
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Wp Clear",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Wp Clear")

            r = client.call_tool("update_ship_weapon_bank", {
                "name": "MCP Wp Clear", "bank_type": "primary", "bank": 1,
                "weapon_class": "<none>",
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("weapon_class"), "<none>", "cleared weapon_class")
            assert_equal(d.get("ammo_count"), 0, "cleared bank ammo_count is 0")
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_weapon_bank_wrong_type_reject():
        cls = _pick_ship_class_with_banks("primary")
        if cls is None:
            return
        secondary = _pick_selectable_weapon("secondary")
        if secondary is None:
            return
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Wp Wrong",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Wp Wrong")

            # Secondary weapon in a primary slot is a structural rule; force
            # should not bypass it.
            r = client.call_tool("update_ship_weapon_bank", {
                "name": "MCP Wp Wrong", "bank_type": "primary", "bank": 1,
                "weapon_class": secondary, "force": True,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_weapon_bank_ammo_bounds():
        cls = _pick_ship_class_with_banks("secondary")
        if cls is None:
            return
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Wp Ammo",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Wp Ammo")

            # Search for a secondary weapon with non-zero max for this ship's
            # secondary bank 1 (i.e. a ballistic secondary the bank can hold).
            r = client.call_tool("list_weapon_classes", {"subtype": "secondary"})
            assert_success(r)
            weapon = None
            max_ammo = 0
            for w in tool_data(r):
                name = w.get("name")
                if not name:
                    continue
                rr = client.call_tool("get_max_ammo_for_bank", {
                    "name": "MCP Wp Ammo", "bank_type": "secondary", "bank": 1,
                    "weapon_class": name,
                })
                if rr.get("isError"):
                    continue
                m = tool_data(rr).get("max_ammo_count", 0)
                if m > 0:
                    weapon = name
                    max_ammo = m
                    break
            if weapon is None:
                return

            # Over-max rejected even with force.
            r = client.call_tool("update_ship_weapon_bank", {
                "name": "MCP Wp Ammo", "bank_type": "secondary", "bank": 1,
                "weapon_class": weapon, "ammo_count": max_ammo + 1, "force": True,
            })
            assert_error(r)

            # Exactly at max accepted; round-trip.
            r = client.call_tool("update_ship_weapon_bank", {
                "name": "MCP Wp Ammo", "bank_type": "secondary", "bank": 1,
                "weapon_class": weapon, "ammo_count": max_ammo, "force": True,
            })
            assert_success(r)
            assert_equal(tool_data(r).get("ammo_count"), max_ammo,
                "ammo_count at exact max should round-trip")
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_weapon_bank_clear_with_nonzero_ammo_rejected():
        cls = _pick_ship_class_with_banks("primary")
        if cls is None:
            return
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Wp ClearAmmo",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Wp ClearAmmo")

            r = client.call_tool("update_ship_weapon_bank", {
                "name": "MCP Wp ClearAmmo", "bank_type": "primary", "bank": 1,
                "weapon_class": "<none>", "ammo_count": 5,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_weapon_bank_out_of_range_bank():
        cls = _pick_ship_class_with_banks("primary")
        if cls is None:
            return
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Wp Range",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Wp Range")

            r = client.call_tool("get_ship_weapon_bank", {
                "name": "MCP Wp Range", "bank_type": "primary", "bank": 0,
            })
            assert_error(r)
            r = client.call_tool("get_ship_weapon_bank", {
                "name": "MCP Wp Range", "bank_type": "primary", "bank": 99,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    def test_get_max_ammo_for_bank_returns_int():
        cls = _pick_ship_class_with_banks("secondary")
        if cls is None:
            return
        weapon = _pick_selectable_weapon("secondary")
        if weapon is None:
            return
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Wp Max",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Wp Max")

            r = client.call_tool("get_max_ammo_for_bank", {
                "name": "MCP Wp Max", "bank_type": "secondary", "bank": 1,
                "weapon_class": weapon,
            })
            assert_success(r)
            m = tool_data(r).get("max_ammo_count")
            assert_true(isinstance(m, int) and m >= 0,
                f"max_ammo_count should be a non-negative int, got {m!r}")
        finally:
            _delete_all_test_ships(client, created)

    def test_get_ship_weapon_bank_nonexistent_subsystem_rejected():
        cls = _pick_ship_class_with_banks("primary")
        if cls is None:
            return
        created = []
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Wp NoSubsys",
                "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            created.append("MCP Wp NoSubsys")

            r = client.call_tool("get_ship_weapon_bank", {
                "name": "MCP Wp NoSubsys",
                "subsystem": "does_not_exist_xyz",
                "bank_type": "primary", "bank": 1,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    # ---------------------------------------------------------------------
    # Subsystem tools
    # ---------------------------------------------------------------------
    #
    # Per-subsystem initial state -- health, cargo, cargo_title, and
    # (turret-only) ai_class.  Mod content drives what's testable, so each
    # test skips cleanly when it can't find a suitable ship/subsystem.

    def _pick_class_with_subsystem_type(type_name):
        """First ship class that, once instantiated, has at least one subsystem
        whose type equals type_name (e.g. "Engines", "Turrets").  Returns
        (class_name, subsystem_name) or None.

        This creates and deletes probe ships, so callers should not assume the
        mission state is unchanged on return."""
        r = client.call_tool("list_ship_classes")
        assert_success(r)
        probe = "MCP Subsys Probe"
        for c in tool_data(r):
            name = c.get("name")
            if not name:
                continue
            rr = client.call_tool("create_ship", {
                "name": probe, "ship_class": name,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            if rr.get("isError"):
                continue
            try:
                rs = client.call_tool("list_ship_subsystems", {"name": probe})
                if rs.get("isError"):
                    continue
                for s in tool_data(rs).get("subsystems", []):
                    if s.get("subsystem_type") == type_name:
                        return (name, s.get("subsystem"))
            finally:
                client.call_tool("delete_ship", {"name": probe, "force": True})
        return None

    def _probe_has_scannable_subsystems(class_name):
        """Try a no-force cargo write on the first subsystem of a probe ship of
        the given class.  Returns True iff the write succeeds (i.e. the class
        has scannable subsystems by FRED's rule).  Note this is about
        *subsystem* scannability specifically -- a class can be ship-scannable
        (cargo on the ship itself) without having any scannable subsystems."""
        probe = "MCP Scan Probe"
        rr = client.call_tool("create_ship", {
            "name": probe, "ship_class": class_name,
            "position": {"x": 0.0, "y": 0.0, "z": 0.0},
            "orientation": IDENTITY_ORIENT,
        })
        if rr.get("isError"):
            return False
        try:
            rs = client.call_tool("list_ship_subsystems", {"name": probe})
            assert_success(rs)
            subs = tool_data(rs).get("subsystems", [])
            if not subs:
                return False
            r = client.call_tool("update_ship_subsystem", {
                "name": probe, "subsystem": subs[0]["subsystem"],
                "cargo": "MCPProbeCargo",
            })
            return not r.get("isError")
        finally:
            client.call_tool("delete_ship", {"name": probe, "force": True})

    def _pick_class_with_scannable_subsystems():
        for c in _big_or_huge_classes():
            name = c.get("name")
            if name and _probe_has_scannable_subsystems(name):
                return name
        return None

    def _pick_class_without_scannable_subsystems():
        for c in _big_or_huge_classes():
            name = c.get("name")
            if name and not _probe_has_scannable_subsystems(name):
                return name
        return None

    def test_list_ship_subsystems_shape():
        cls = _pick_ship_class(client)
        created = ["MCP Subsys Shape"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys Shape", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("list_ship_subsystems", {"name": "MCP Subsys Shape"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("ship"), "MCP Subsys Shape", "echoed ship name")
            subs = d.get("subsystems")
            assert_is_list(subs, "subsystems")
            if not subs:
                return  # ship class with zero subsystems; skip detailed checks
            for s in subs:
                assert_is_dict(s)
                assert_has_key(s, "subsystem")
                assert_has_key(s, "subsystem_type")
                assert_has_key(s, "initial_health_percent")
        finally:
            _delete_all_test_ships(client, created)

    def test_get_ship_subsystem_matches_bulk():
        # Routing regression guard for list_ship_subsystems vs get_ship_subsystem.
        cls = _pick_ship_class(client)
        created = ["MCP Subsys Match"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys Match", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("list_ship_subsystems", {"name": "MCP Subsys Match"})
            assert_success(r)
            subs = tool_data(r).get("subsystems", [])
            if not subs:
                return
            target = subs[0]
            r = client.call_tool("get_ship_subsystem", {
                "name": "MCP Subsys Match",
                "subsystem": target["subsystem"],
            })
            assert_success(r)
            assert_equal(tool_data(r), target,
                "single get must equal the matching bulk entry")
        finally:
            _delete_all_test_ships(client, created)

    def test_get_ship_subsystem_unknown_reject():
        cls = _pick_ship_class(client)
        created = ["MCP Subsys Unk"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys Unk", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("get_ship_subsystem", {
                "name": "MCP Subsys Unk",
                "subsystem": "does_not_exist_xyz",
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_subsystem_health_round_trip():
        cls = _pick_ship_class(client)
        created = ["MCP Subsys Health"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys Health", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("list_ship_subsystems", {"name": "MCP Subsys Health"})
            assert_success(r)
            subs = tool_data(r).get("subsystems", [])
            if not subs:
                return
            target = subs[0]["subsystem"]
            r = client.call_tool("update_ship_subsystem", {
                "name": "MCP Subsys Health", "subsystem": target,
                "initial_health_percent": 50,
            })
            assert_success(r)
            assert_equal(tool_data(r).get("initial_health_percent"), 50,
                "echoed health 50")
            r = client.call_tool("get_ship_subsystem", {
                "name": "MCP Subsys Health", "subsystem": target,
            })
            assert_success(r)
            assert_equal(tool_data(r).get("initial_health_percent"), 50,
                "round-trip health 50")
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_subsystem_health_out_of_range_reject():
        cls = _pick_ship_class(client)
        created = ["MCP Subsys HRange"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys HRange", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("list_ship_subsystems", {"name": "MCP Subsys HRange"})
            assert_success(r)
            subs = tool_data(r).get("subsystems", [])
            if not subs:
                return
            target = subs[0]["subsystem"]
            for bad in (-1, 101):
                r = client.call_tool("update_ship_subsystem", {
                    "name": "MCP Subsys HRange", "subsystem": target,
                    "initial_health_percent": bad,
                })
                assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_subsystem_unknown_subsystem_reject():
        cls = _pick_ship_class(client)
        created = ["MCP Subsys UnkUpd"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys UnkUpd", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("update_ship_subsystem", {
                "name": "MCP Subsys UnkUpd", "subsystem": "no_such_subsys",
                "initial_health_percent": 75,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_subsystem_cargo_scannable():
        cls = _pick_class_with_scannable_subsystems()
        if cls is None:
            return
        created = ["MCP Subsys Cargo"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys Cargo", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("list_ship_subsystems", {"name": "MCP Subsys Cargo"})
            assert_success(r)
            subs = tool_data(r).get("subsystems", [])
            if not subs:
                return
            target = subs[0]["subsystem"]
            r = client.call_tool("update_ship_subsystem", {
                "name": "MCP Subsys Cargo", "subsystem": target,
                "cargo": "MCPTestSubsysCargo",
            })
            assert_success(r)
            assert_equal(tool_data(r).get("cargo"), "MCPTestSubsysCargo",
                "cargo round-trip")
            # Clear via "<none>".
            r = client.call_tool("update_ship_subsystem", {
                "name": "MCP Subsys Cargo", "subsystem": target,
                "cargo": "<none>",
            })
            assert_success(r)
            assert_true("cargo" not in tool_data(r),
                "cargo cleared by \"<none>\" should be absent in response")
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_subsystem_cargo_non_scannable_reject_and_force():
        cls = _pick_class_without_scannable_subsystems()
        if cls is None:
            return
        created = ["MCP Subsys NonScan"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys NonScan", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("list_ship_subsystems", {"name": "MCP Subsys NonScan"})
            assert_success(r)
            subs = tool_data(r).get("subsystems", [])
            if not subs:
                return
            target = subs[0]["subsystem"]
            r = client.call_tool("update_ship_subsystem", {
                "name": "MCP Subsys NonScan", "subsystem": target,
                "cargo": "MCPNonScanProbe",
            })
            assert_error(r)
            r = client.call_tool("update_ship_subsystem", {
                "name": "MCP Subsys NonScan", "subsystem": target,
                "cargo": "MCPNonScanProbe", "force": True,
            })
            assert_success(r)
            assert_equal(tool_data(r).get("cargo"), "MCPNonScanProbe",
                "cargo set under force")
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_subsystem_cargo_title_round_trip():
        cls = _pick_class_with_scannable_subsystems()
        if cls is None:
            return
        created = ["MCP Subsys Title"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys Title", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("list_ship_subsystems", {"name": "MCP Subsys Title"})
            assert_success(r)
            subs = tool_data(r).get("subsystems", [])
            if not subs:
                return
            target = subs[0]["subsystem"]
            r = client.call_tool("update_ship_subsystem", {
                "name": "MCP Subsys Title", "subsystem": target,
                "cargo": "MCPTitleCargo", "cargo_title": "Passengers",
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("cargo"), "MCPTitleCargo")
            assert_equal(d.get("cargo_title"), "Passengers")
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_subsystem_ai_class_turret():
        pick = _pick_class_with_subsystem_type("Turrets")
        if pick is None:
            return
        cls, turret = pick
        # Pick a valid ai_class.
        r = client.call_tool("list_ai_classes")
        assert_success(r)
        classes = tool_data(r)
        if not classes:
            return
        # list_ai_classes entries are either dicts with "name" or bare strings.
        first = classes[0]
        ai_name = first.get("name") if isinstance(first, dict) else first
        if not ai_name:
            return

        created = ["MCP Subsys AI"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys AI", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("update_ship_subsystem", {
                "name": "MCP Subsys AI", "subsystem": turret,
                "ai_class": ai_name,
            })
            assert_success(r)
            assert_equal(tool_data(r).get("ai_class"), ai_name,
                "ai_class round-trip on turret")
        finally:
            _delete_all_test_ships(client, created)

    def test_update_ship_subsystem_ai_class_non_turret_reject():
        pick = _pick_class_with_subsystem_type("Engines")
        if pick is None:
            return
        cls, engine = pick
        r = client.call_tool("list_ai_classes")
        assert_success(r)
        classes = tool_data(r)
        if not classes:
            return
        first = classes[0]
        ai_name = first.get("name") if isinstance(first, dict) else first
        if not ai_name:
            return

        created = ["MCP Subsys AINon"]
        try:
            r = client.call_tool("create_ship", {
                "name": "MCP Subsys AINon", "ship_class": cls,
                "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                "orientation": IDENTITY_ORIENT,
            })
            assert_success(r)
            r = client.call_tool("update_ship_subsystem", {
                "name": "MCP Subsys AINon", "subsystem": engine,
                "ai_class": ai_name,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, created)

    # ---------------------------------------------------------------------
    # Special explosion & special hitpoints
    # ---------------------------------------------------------------------

    def _create_basic_ship(name):
        cls = _pick_ship_class(client)
        r = client.call_tool("create_ship", {
            "name": name, "ship_class": cls,
            "position": {"x": 0.0, "y": 0.0, "z": 0.0},
            "orientation": IDENTITY_ORIENT,
        })
        assert_success(r)

    def test_ship_special_explosion_defaults():
        name = "MCP SpecExp Default"
        try:
            _create_basic_ship(name)
            r = client.call_tool("get_ship_special_explosion", {"name": name})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("enabled"), False, "fresh ship: enabled is False")
            assert_true("damage" not in d, "no damage field when disabled")
            assert_true("shockwave_enabled" not in d, "no shockwave_enabled when disabled")
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_explosion_enable_from_defaults():
        name = "MCP SpecExp Seed"
        try:
            _create_basic_ship(name)
            r = client.call_tool("update_ship_special_explosion",
                {"name": name, "enable": True})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("enabled"), True, "enabled becomes True")
            for f in ("damage", "blast_force", "inner_radius", "outer_radius",
                      "shockwave_enabled"):
                assert_has_key(d, f, f"{f} populated after seed-from-defaults")
            assert_true(d["inner_radius"] >= 1, "inner_radius >= 1 after seed")
            assert_true(d["outer_radius"] >= 2, "outer_radius >= 2 after seed")
            assert_true(d["inner_radius"] <= d["outer_radius"],
                "inner_radius <= outer_radius after seed")
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_explosion_enable_with_overrides():
        name = "MCP SpecExp Set"
        try:
            _create_basic_ship(name)
            r = client.call_tool("update_ship_special_explosion", {
                "name": name, "enable": True,
                "damage": 1234, "blast_force": 567,
                "inner_radius": 10, "outer_radius": 50,
                "shockwave_enabled": True, "shockwave_speed": 7,
                "deathroll_time_ms": 4000,
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d["damage"], 1234, "damage round-trip")
            assert_equal(d["blast_force"], 567, "blast_force round-trip")
            assert_equal(d["inner_radius"], 10, "inner_radius round-trip")
            assert_equal(d["outer_radius"], 50, "outer_radius round-trip")
            assert_equal(d["shockwave_enabled"], True, "shockwave_enabled round-trip")
            assert_equal(d["shockwave_speed"], 7, "shockwave_speed round-trip")
            assert_equal(d["deathroll_time_ms"], 4000, "deathroll_time round-trip")
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_explosion_inner_gt_outer_rejected():
        name = "MCP SpecExp InnerGtOuter"
        try:
            _create_basic_ship(name)
            r = client.call_tool("update_ship_special_explosion", {
                "name": name, "enable": True,
                "inner_radius": 100, "outer_radius": 50,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_explosion_disable_wipes():
        name = "MCP SpecExp Wipe"
        try:
            _create_basic_ship(name)
            r = client.call_tool("update_ship_special_explosion", {
                "name": name, "enable": True,
                "inner_radius": 10, "outer_radius": 50,
            })
            assert_success(r)
            r = client.call_tool("update_ship_special_explosion",
                {"name": name, "enable": False})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("enabled"), False, "disabled after wipe")
            assert_true("damage" not in d, "damage absent after wipe")
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_explosion_disable_with_other_fields_rejected():
        name = "MCP SpecExp DisableConflict"
        try:
            _create_basic_ship(name)
            r = client.call_tool("update_ship_special_explosion", {
                "name": name, "enable": False, "damage": 100,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_explosion_shockwave_subtoggle():
        name = "MCP SpecExp Subtoggle"
        try:
            _create_basic_ship(name)
            # Enable with shockwave off; speed must not be required and
            # the response should omit shockwave_speed.
            r = client.call_tool("update_ship_special_explosion", {
                "name": name, "enable": True,
                "inner_radius": 10, "outer_radius": 50,
                "shockwave_enabled": False,
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d["shockwave_enabled"], False, "shockwave off")
            assert_true("shockwave_speed" not in d, "no shockwave_speed when shockwave off")
            # Re-enable shockwave with explicit speed.
            r = client.call_tool("update_ship_special_explosion", {
                "name": name, "enable": True,
                "shockwave_enabled": True, "shockwave_speed": 5,
            })
            assert_success(r)
            d = tool_data(r)
            assert_equal(d["shockwave_enabled"], True)
            assert_equal(d["shockwave_speed"], 5)
            # Enabling shockwave with speed=0 must be rejected.
            r = client.call_tool("update_ship_special_explosion", {
                "name": name, "enable": True,
                "shockwave_enabled": True, "shockwave_speed": 0,
            })
            assert_error(r)
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_explosion_deathroll_validation():
        name = "MCP SpecExp Deathroll"
        try:
            _create_basic_ship(name)
            # deathroll == 0 (inherit class default) is accepted.
            r = client.call_tool("update_ship_special_explosion", {
                "name": name, "enable": True, "deathroll_time_ms": 0,
            })
            assert_success(r)
            d = tool_data(r)
            assert_true("deathroll_time_ms" not in d,
                "deathroll absent when 0 (inherit)")
            # 1 ms is invalid.
            r = client.call_tool("update_ship_special_explosion", {
                "name": name, "enable": True, "deathroll_time_ms": 1,
            })
            assert_error(r)
            # 2 ms is the minimum override.
            r = client.call_tool("update_ship_special_explosion", {
                "name": name, "enable": True, "deathroll_time_ms": 2,
            })
            assert_success(r)
            assert_equal(tool_data(r).get("deathroll_time_ms"), 2)
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_hitpoints_defaults():
        name = "MCP SpecHP Default"
        try:
            _create_basic_ship(name)
            r = client.call_tool("get_ship_special_hitpoints", {"name": name})
            assert_success(r)
            d = tool_data(r)
            assert_true("hull" not in d, "no hull field when not overridden")
            assert_true("shield" not in d, "no shield field when not overridden")
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_hitpoints_hull_only_and_kamikaze_recalc():
        name = "MCP SpecHP Hull"
        try:
            _create_basic_ship(name)
            # Set the kamikaze flag so get_ship will surface kamikaze_damage
            # (paired-scalar: only emitted when the flag is on).
            r = client.call_tool("update_ship",
                {"name": name, "ship_flags": {"kamikaze": True}})
            assert_success(r)
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "hull": 8000})
            assert_success(r)
            assert_equal(tool_data(r).get("hull"), 8000, "hull round-trip")
            # Kamikaze recalc: min(1000, 200 + 8000/4) = min(1000, 2200) = 1000.
            r = client.call_tool("get_ship", {"name": name})
            assert_success(r)
            assert_equal(tool_data(r).get("kamikaze_damage"), 1000,
                "kamikaze recalc capped at 1000")
            # Smaller hull -> smaller kamikaze value.
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "hull": 100})
            assert_success(r)
            r = client.call_tool("get_ship", {"name": name})
            assert_equal(tool_data(r).get("kamikaze_damage"), 225,
                "kamikaze recalc = 200 + 100/4 = 225")
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_hitpoints_shield_zero_valid():
        name = "MCP SpecHP Shield0"
        try:
            _create_basic_ship(name)
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "shield": 0})
            assert_success(r)
            assert_equal(tool_data(r).get("shield"), 0,
                "shield: 0 round-trips as a valid override")
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_hitpoints_disable_via_sentinel():
        name = "MCP SpecHP Sentinel"
        try:
            _create_basic_ship(name)
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "hull": 5000, "shield": 200})
            assert_success(r)
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "hull": 0})
            assert_success(r)
            d = tool_data(r)
            assert_true("hull" not in d, "hull absent after sentinel-disable")
            assert_equal(d.get("shield"), 200, "shield unchanged by hull-disable update")
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_hitpoints_omit_leaves_unchanged():
        name = "MCP SpecHP Omit"
        try:
            _create_basic_ship(name)
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "hull": 5000, "shield": 200})
            assert_success(r)
            # Update only shield; hull must persist.
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "shield": 300})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("hull"), 5000, "hull unchanged after shield-only update")
            assert_equal(d.get("shield"), 300, "shield updated")
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_hitpoints_range_validation():
        name = "MCP SpecHP Range"
        try:
            _create_basic_ship(name)
            # 0 (hull) and -1 (shield) are the disable sentinels, so the first
            # out-of-range values are -1 and -2 respectively.
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "hull": -1})
            assert_error(r)
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "shield": -2})
            assert_error(r)
        finally:
            _delete_all_test_ships(client, [name])

    def test_ship_special_hitpoints_disable_recalcs_to_class():
        name = "MCP SpecHP RecalcClass"
        try:
            _create_basic_ship(name)
            # Kamikaze flag must be set for kamikaze_damage to appear in get_ship.
            r = client.call_tool("update_ship",
                {"name": name, "ship_flags": {"kamikaze": True}})
            assert_success(r)
            # Enable special hull, then disable and confirm kamikaze recomputes
            # against the ship class's max_hull_strength.
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "hull": 8000})
            assert_success(r)
            r = client.call_tool("update_ship_special_hitpoints",
                {"name": name, "hull": 0})
            assert_success(r)
            r = client.call_tool("get_ship", {"name": name})
            assert_success(r)
            ship_class = tool_data(r).get("ship_class")
            r = client.call_tool("get_ship_class", {"name": ship_class})
            assert_success(r)
            class_hull = tool_data(r).get("max_hull_strength")
            expected = min(1000, 200 + int(class_hull / 4.0))
            r = client.call_tool("get_ship", {"name": name})
            assert_equal(tool_data(r).get("kamikaze_damage"), expected,
                "kamikaze recalc against class max_hull_strength")
        finally:
            _delete_all_test_ships(client, [name])

    suite.add("ships_basic_crud", test_ships_basic_crud)
    suite.add("ships_arrival_cue_replacement", test_ship_arrival_cue_replacement)
    suite.add("ships_alt_class_name_callsign_cycle", test_ship_alt_class_name_and_callsign_cycle)
    suite.add("ships_class_change", test_ship_class_change)
    suite.add("ships_error_paths", test_ship_errors)
    suite.add("ships_duplicate_name", test_ship_duplicate_name)
    suite.add("ships_flags_basic_round_trip", test_ship_flags_basic_round_trip)
    suite.add("ships_flags_no_collide_inversion", test_ship_flags_no_collide_inversion)
    suite.add("ships_flags_errors_and_atomicity", test_ship_flags_errors_and_atomicity)
    suite.add("ships_flags_excluded_names_rejected", test_ship_flags_excluded_names_rejected)
    suite.add("ships_flag_paired_scalars", test_ship_flag_paired_scalars)
    suite.add("ships_flag_paired_scalars_errors", test_ship_flag_paired_scalars_errors)
    suite.add("ships_initial_percents_round_trip", test_ship_initial_percents_round_trip)
    suite.add("ships_initial_shield_percent_no_shields_gating", test_ship_initial_shield_percent_no_shields_gating)
    suite.add("ships_initial_percents_errors", test_ship_initial_percents_errors)
    suite.add("ships_swap_basic", test_ships_swap_basic)
    suite.add("ships_swap_idempotent", test_ships_swap_idempotent)
    suite.add("ships_swap_same_index_noop", test_ships_swap_same_index_noop)
    suite.add("ships_move_forward", test_ships_move_forward)
    suite.add("ships_move_backward", test_ships_move_backward)
    suite.add("ships_move_same_index_noop", test_ships_move_same_index_noop)
    suite.add("ships_move_preserves_identity", test_ships_move_preserves_identity)
    suite.add("ships_swap_out_of_range", test_ships_swap_out_of_range)
    suite.add("ships_move_out_of_range", test_ships_move_out_of_range)
    suite.add("ships_dock_basic_round_trip", test_ships_dock_basic_round_trip)
    suite.add("ships_dock_validation", test_ships_dock_validation)
    suite.add("ships_undock_derive_partner", test_ships_undock_derive_partner)
    suite.add("ships_undock_ambiguous", test_ships_undock_ambiguous)
    suite.add("ships_undock_all", test_ships_undock_all)
    suite.add("ships_list_docked_group_undocked", test_ships_list_docked_group_undocked)
    suite.add("ships_list_docked_group_pair", test_ships_list_docked_group_pair)
    suite.add("ships_list_docked_group_chain", test_ships_list_docked_group_chain)
    suite.add("ships_set_dock_leader_swap", test_ships_set_dock_leader_swap)
    suite.add("ships_set_dock_leader_already_leader_noop", test_ships_set_dock_leader_already_leader_noop)
    suite.add("ships_set_dock_leader_validation", test_ships_set_dock_leader_validation)
    suite.add("ship_weapons_pilot_shape", test_list_ship_weapons_pilot_shape)
    suite.add("ship_weapon_bank_pilot_default_matches_explicit",
        test_get_ship_weapon_bank_pilot_default_matches_explicit)
    suite.add("ship_weapon_bank_change_primary", test_update_ship_weapon_bank_change_primary)
    suite.add("ship_weapon_bank_clear", test_update_ship_weapon_bank_clear)
    suite.add("ship_weapon_bank_wrong_type_reject",
        test_update_ship_weapon_bank_wrong_type_reject)
    suite.add("ship_weapon_bank_ammo_bounds", test_update_ship_weapon_bank_ammo_bounds)
    suite.add("ship_weapon_bank_clear_with_nonzero_ammo_rejected",
        test_update_ship_weapon_bank_clear_with_nonzero_ammo_rejected)
    suite.add("ship_weapon_bank_out_of_range", test_update_ship_weapon_bank_out_of_range_bank)
    suite.add("ship_weapon_bank_max_ammo_returns_int", test_get_max_ammo_for_bank_returns_int)
    suite.add("ship_weapon_bank_nonexistent_subsystem_rejected",
        test_get_ship_weapon_bank_nonexistent_subsystem_rejected)
    suite.add("ship_subsystems_shape", test_list_ship_subsystems_shape)
    suite.add("ship_subsystem_matches_bulk", test_get_ship_subsystem_matches_bulk)
    suite.add("ship_subsystem_unknown_reject", test_get_ship_subsystem_unknown_reject)
    suite.add("ship_subsystem_health_round_trip",
        test_update_ship_subsystem_health_round_trip)
    suite.add("ship_subsystem_health_out_of_range_reject",
        test_update_ship_subsystem_health_out_of_range_reject)
    suite.add("ship_subsystem_unknown_subsystem_reject",
        test_update_ship_subsystem_unknown_subsystem_reject)
    suite.add("ship_subsystem_cargo_scannable",
        test_update_ship_subsystem_cargo_scannable)
    suite.add("ship_subsystem_cargo_non_scannable_reject_and_force",
        test_update_ship_subsystem_cargo_non_scannable_reject_and_force)
    suite.add("ship_subsystem_cargo_title_round_trip",
        test_update_ship_subsystem_cargo_title_round_trip)
    suite.add("ship_subsystem_ai_class_turret",
        test_update_ship_subsystem_ai_class_turret)
    suite.add("ship_subsystem_ai_class_non_turret_reject",
        test_update_ship_subsystem_ai_class_non_turret_reject)
    suite.add("ship_special_explosion_defaults", test_ship_special_explosion_defaults)
    suite.add("ship_special_explosion_enable_from_defaults",
        test_ship_special_explosion_enable_from_defaults)
    suite.add("ship_special_explosion_enable_with_overrides",
        test_ship_special_explosion_enable_with_overrides)
    suite.add("ship_special_explosion_inner_gt_outer_rejected",
        test_ship_special_explosion_inner_gt_outer_rejected)
    suite.add("ship_special_explosion_disable_wipes",
        test_ship_special_explosion_disable_wipes)
    suite.add("ship_special_explosion_disable_with_other_fields_rejected",
        test_ship_special_explosion_disable_with_other_fields_rejected)
    suite.add("ship_special_explosion_shockwave_subtoggle",
        test_ship_special_explosion_shockwave_subtoggle)
    suite.add("ship_special_explosion_deathroll_validation",
        test_ship_special_explosion_deathroll_validation)
    suite.add("ship_special_hitpoints_defaults", test_ship_special_hitpoints_defaults)
    suite.add("ship_special_hitpoints_hull_only_and_kamikaze_recalc",
        test_ship_special_hitpoints_hull_only_and_kamikaze_recalc)
    suite.add("ship_special_hitpoints_shield_zero_valid",
        test_ship_special_hitpoints_shield_zero_valid)
    suite.add("ship_special_hitpoints_disable_via_sentinel",
        test_ship_special_hitpoints_disable_via_sentinel)
    suite.add("ship_special_hitpoints_omit_leaves_unchanged",
        test_ship_special_hitpoints_omit_leaves_unchanged)
    suite.add("ship_special_hitpoints_range_validation",
        test_ship_special_hitpoints_range_validation)
    suite.add("ship_special_hitpoints_disable_recalcs_to_class",
        test_ship_special_hitpoints_disable_recalcs_to_class)


if __name__ == "__main__":
    run_module_standalone(register, "Ship CRUD tests")
