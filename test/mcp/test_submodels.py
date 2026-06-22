"""Tests for the ship submodel-instance pose MCP module.

Exercises list_ship_submodels / get_ship_submodel / update_ship_submodel:
the discovery listing (LOD duplicates filtered, axis metadata present), the
'basic' (angle_degrees/offset_meters) and 'advanced' (orientation/offset)
pose round-trips, blown_off toggling, and the negative/validation paths.

Submodel pose is transient editor state and is never written to the mission
file, so these tests only assert read-back consistency within the session.

The 'advanced' path works on any submodel, so it is always exercised.  The
'basic' path needs a submodel with a defined movement axis; the setup scans a
bounded number of ship classes to find one and skips the basic tests if the
test environment exposes none.

Assumes a freshly created mission (via the standard prelude).
"""

from mcp_test_lib import (
    SkipTest,
    assert_equal,
    assert_error,
    assert_has_key,
    assert_is_dict,
    assert_is_list,
    assert_non_empty_list,
    assert_success,
    assert_true,
    run_module_standalone,
    tool_data,
)


IDENTITY_ORIENT = {
    "rvec": {"x": 1.0, "y": 0.0, "z": 0.0},
    "uvec": {"x": 0.0, "y": 1.0, "z": 0.0},
    "fvec": {"x": 0.0, "y": 0.0, "z": 1.0},
}

# A non-identity orientation (90 degrees of heading) for the advanced round-trip.
HEADING_90_ORIENT = {
    "rvec": {"x": 0.0, "y": 0.0, "z": -1.0},
    "uvec": {"x": 0.0, "y": 1.0, "z": 0.0},
    "fvec": {"x": 1.0, "y": 0.0, "z": 0.0},
}

PROBE_SHIP = "MCP Submodel Probe"

# How many ship classes to instantiate while hunting for a submodel with a
# movement axis before giving up and skipping the 'basic' round-trip tests.
MAX_CLASS_SCAN = 25


def _matrix_close(a, b, tol=1e-3):
    for vk in ("rvec", "uvec", "fvec"):
        for ck in ("x", "y", "z"):
            if abs(a[vk][ck] - b[vk][ck]) > tol:
                return False
    return True


def _first(pred, submodels):
    for s in submodels:
        if pred(s):
            return s
    return None


def _delete_ship(client, name):
    try:
        client.call_tool("delete_ship", {"name": name, "force": True})
    except Exception:
        pass


def _create_probe(client, ship_class, name=PROBE_SHIP):
    r = client.call_tool("create_ship", {
        "name": name,
        "ship_class": ship_class,
        "position": {"x": 0.0, "y": 0.0, "z": 0.0},
        "orientation": IDENTITY_ORIENT,
    })
    assert_success(r)


def register(suite, client):

    def test_submodels_setup():
        """Create a probe ship, preferring a class that has a submodel with a
        movement axis so the 'basic' tests can run.  Stash state in ctx."""
        r = client.call_tool("list_ship_classes")
        assert_success(r)
        classes = [c.get("name") for c in tool_data(r) if c.get("name")]
        assert_non_empty_list(classes, "list_ship_classes returned no entries")

        chosen_name = None
        chosen_submodels = None
        fallback_name = None
        fallback_submodels = None

        for cls in classes[:MAX_CLASS_SCAN]:
            _delete_ship(client, PROBE_SHIP)
            _create_probe(client, cls)
            r = client.call_tool("list_ship_submodels", {"name": PROBE_SHIP})
            assert_success(r)
            submodels = tool_data(r).get("submodels", [])
            if fallback_name is None and submodels:
                fallback_name, fallback_submodels = PROBE_SHIP, submodels
            if _first(lambda s: s.get("rotates") or s.get("translates"), submodels):
                chosen_name, chosen_submodels = PROBE_SHIP, submodels
                break
            # Not movable: drop it (unless we want it as the fallback and have none yet).
            if fallback_name != PROBE_SHIP:
                _delete_ship(client, PROBE_SHIP)

        if chosen_name is None:
            # No movable submodel found in the scanned window; keep a fallback
            # ship for the advanced/blown_off/negative tests.
            if fallback_name is None:
                # Re-create from the first class so non-basic tests still run.
                _create_probe(client, classes[0])
                r = client.call_tool("list_ship_submodels", {"name": PROBE_SHIP})
                assert_success(r)
                fallback_submodels = tool_data(r).get("submodels", [])
            chosen_name, chosen_submodels = PROBE_SHIP, fallback_submodels

        suite.ctx["submodels_ship"] = chosen_name
        suite.ctx["submodels_list"] = chosen_submodels

    def _ship():
        name = suite.ctx.get("submodels_ship")
        if not name:
            raise SkipTest("probe ship not available (setup failed)")
        return name

    def _submodels():
        return suite.ctx.get("submodels_list") or []

    def test_list_ship_submodels_shape():
        ship = _ship()
        subs = _submodels()
        assert_non_empty_list(subs, "expected at least one submodel")
        seen_names = set()
        for s in subs:
            assert_is_dict(s)
            # These keys are always present.
            for key in ("submodel", "rotates", "translates", "blown_off", "advanced"):
                assert_has_key(s, key, f"submodel entry missing {key!r}")
            # advanced is always present with both fields
            assert_is_dict(s["advanced"], "advanced should be an object")
            assert_has_key(s["advanced"], "orientation")
            assert_has_key(s["advanced"], "offset")
            # Axis metadata and basic are present only when there's a movement axis;
            # otherwise they are omitted (not emitted as null).
            if s["rotates"]:
                assert_has_key(s, "rotation_axis", "rotation_axis should be present when rotates")
            else:
                assert_true("rotation_axis" not in s, "rotation_axis should be omitted when not rotating")
            if s["translates"]:
                assert_has_key(s, "translation_axis", "translation_axis should be present when translates")
            else:
                assert_true("translation_axis" not in s, "translation_axis should be omitted when not translating")
            if s["rotates"] or s["translates"]:
                assert_is_dict(s["basic"], "basic should be an object when movable")
            else:
                assert_true("basic" not in s, "basic should be omitted when axis-less")
            # names should be unique (LOD duplicates are filtered out)
            nm = s["submodel"]
            assert_true(nm not in seen_names, f"duplicate submodel name {nm!r} (LOD not filtered?)")
            seen_names.add(nm)

    def test_get_ship_submodel_roundtrip_shape():
        ship = _ship()
        subs = _submodels()
        name = subs[0]["submodel"]
        r = client.call_tool("get_ship_submodel", {"name": ship, "submodel": name})
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("submodel"), name, "get returns requested submodel")
        assert_equal(d.get("ship"), ship, "get returns ship name")
        assert_has_key(d, "advanced")
        # basic is present only when the submodel has a movement axis.
        if d.get("rotates") or d.get("translates"):
            assert_has_key(d, "basic")
        else:
            assert_true("basic" not in d, "basic should be omitted when axis-less")

    def test_advanced_pose_roundtrip():
        """Advanced orientation+offset works on any submodel."""
        ship = _ship()
        subs = _submodels()
        name = subs[0]["submodel"]
        offset = {"x": 1.5, "y": -2.0, "z": 3.25}
        try:
            r = client.call_tool("update_ship_submodel", {
                "name": ship, "submodel": name,
                "advanced": {"orientation": HEADING_90_ORIENT, "offset": offset},
            })
            assert_success(r)
            r = client.call_tool("get_ship_submodel", {"name": ship, "submodel": name})
            assert_success(r)
            adv = tool_data(r)["advanced"]
            assert_true(_matrix_close(adv["orientation"], HEADING_90_ORIENT),
                        f"orientation round-trip mismatch: {adv['orientation']}")
            for ck in ("x", "y", "z"):
                assert_true(abs(adv["offset"][ck] - offset[ck]) < 1e-3,
                            f"offset round-trip mismatch on {ck}: {adv['offset']}")
        finally:
            # Reset the pose back to identity/zero.
            client.call_tool("update_ship_submodel", {
                "name": ship, "submodel": name,
                "advanced": {"orientation": IDENTITY_ORIENT,
                             "offset": {"x": 0.0, "y": 0.0, "z": 0.0}},
            })

    def test_basic_angle_roundtrip():
        ship = _ship()
        rot = _first(lambda s: s.get("rotates"), _submodels())
        if rot is None:
            raise SkipTest("no rotatable submodel in test environment")
        name = rot["submodel"]
        try:
            r = client.call_tool("update_ship_submodel", {
                "name": ship, "submodel": name, "basic": {"angle_degrees": 45.0},
            })
            assert_success(r)
            r = client.call_tool("get_ship_submodel", {"name": ship, "submodel": name})
            assert_success(r)
            basic = tool_data(r)["basic"]
            assert_true(abs(basic["angle_degrees"] - 45.0) < 0.5,
                        f"angle round-trip mismatch: {basic['angle_degrees']}")
        finally:
            client.call_tool("update_ship_submodel", {
                "name": ship, "submodel": name, "basic": {"angle_degrees": 0.0},
            })

    def test_basic_offset_roundtrip():
        ship = _ship()
        tr = _first(lambda s: s.get("translates"), _submodels())
        if tr is None:
            raise SkipTest("no translatable submodel in test environment")
        name = tr["submodel"]
        try:
            r = client.call_tool("update_ship_submodel", {
                "name": ship, "submodel": name, "basic": {"offset_meters": 2.5},
            })
            assert_success(r)
            r = client.call_tool("get_ship_submodel", {"name": ship, "submodel": name})
            assert_success(r)
            basic = tool_data(r)["basic"]
            assert_true(abs(basic["offset_meters"] - 2.5) < 1e-3,
                        f"offset round-trip mismatch: {basic['offset_meters']}")
        finally:
            client.call_tool("update_ship_submodel", {
                "name": ship, "submodel": name, "basic": {"offset_meters": 0.0},
            })

    def test_blown_off_roundtrip():
        ship = _ship()
        name = _submodels()[0]["submodel"]
        try:
            r = client.call_tool("update_ship_submodel", {
                "name": ship, "submodel": name, "blown_off": True,
            })
            assert_success(r)
            assert_equal(tool_data(r).get("blown_off"), True, "blown_off set to True")
            r = client.call_tool("get_ship_submodel", {"name": ship, "submodel": name})
            assert_equal(tool_data(r).get("blown_off"), True, "blown_off persisted")
        finally:
            client.call_tool("update_ship_submodel", {
                "name": ship, "submodel": name, "blown_off": False,
            })

    def test_negative_unknown_ship():
        r = client.call_tool("update_ship_submodel", {
            "name": "No Such Ship 9999", "submodel": "whatever",
            "basic": {"angle_degrees": 10.0},
        })
        assert_error(r)

    def test_negative_unknown_submodel():
        ship = _ship()
        r = client.call_tool("get_ship_submodel", {
            "name": ship, "submodel": "no_such_submodel_xyz",
        })
        assert_error(r)

    def test_negative_basic_and_advanced():
        ship = _ship()
        name = _submodels()[0]["submodel"]
        r = client.call_tool("update_ship_submodel", {
            "name": ship, "submodel": name,
            "basic": {"angle_degrees": 10.0},
            "advanced": {"offset": {"x": 1.0, "y": 0.0, "z": 0.0}},
        })
        assert_error(r)

    def test_negative_basic_angle_on_axisless():
        ship = _ship()
        static = _first(lambda s: not s.get("rotates") and not s.get("translates"), _submodels())
        if static is None:
            raise SkipTest("no axis-less submodel to test the guidance error")
        r = client.call_tool("update_ship_submodel", {
            "name": ship, "submodel": static["submodel"],
            "basic": {"angle_degrees": 30.0},
        })
        assert_error(r)

    def test_submodels_teardown():
        _delete_ship(client, suite.ctx.get("submodels_ship") or PROBE_SHIP)

    suite.add("submodels_setup", test_submodels_setup)
    suite.add("list_ship_submodels_shape", test_list_ship_submodels_shape)
    suite.add("get_ship_submodel_roundtrip_shape", test_get_ship_submodel_roundtrip_shape)
    suite.add("advanced_pose_roundtrip", test_advanced_pose_roundtrip)
    suite.add("basic_angle_roundtrip", test_basic_angle_roundtrip)
    suite.add("basic_offset_roundtrip", test_basic_offset_roundtrip)
    suite.add("blown_off_roundtrip", test_blown_off_roundtrip)
    suite.add("negative_unknown_ship", test_negative_unknown_ship)
    suite.add("negative_unknown_submodel", test_negative_unknown_submodel)
    suite.add("negative_basic_and_advanced", test_negative_basic_and_advanced)
    suite.add("negative_basic_angle_on_axisless", test_negative_basic_angle_on_axisless)
    suite.add("submodels_teardown", test_submodels_teardown)


if __name__ == "__main__":
    run_module_standalone(register, "ship submodel-instance pose")
