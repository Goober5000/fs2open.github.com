"""Team loadout tests (get_team_loadout / update_team_loadout).

A fresh mission seeds each team's pools with the default player ships
(count 5) and default player weapons (16 for lasers, 500 for missiles),
so shape checks can rely on non-empty pools.  Mutating tests capture the
team's current pools first and restore them in a finally block, so a
failure does not leak state into later areas.

Also covers the interactions between loadout entries and SEXP variables:
variable-based class/count entries, delete/rename/type-change guards on
referenced variables, and the player_entry_delay field on mission info.
"""

import os
import re

from mcp_test_lib import (
    assert_equal,
    assert_error,
    assert_in,
    assert_is_list,
    assert_not_in,
    assert_success,
    assert_true,
    run_module_standalone,
    SkipTest,
    tvt_mission,
    tool_data,
    tool_text,
)


SHIP_ENTRY_KEYS = ("ship_class", "class_variable", "count", "count_variable")
WEAPON_ENTRY_KEYS = SHIP_ENTRY_KEYS + ("weapon_class", "required")


def _get_loadout(client):
    r = client.call_tool("get_team_loadout")
    assert_success(r)
    data = tool_data(r)
    assert_is_list(data.get("teams"), "get_team_loadout returned no teams list")
    return data


def _get_team(client, team_name="Team 1"):
    data = _get_loadout(client)
    for team in data["teams"]:
        if team.get("team") == team_name:
            return team
    raise AssertionError(f"team '{team_name}' not present in get_team_loadout")


def _settable(entries, keys):
    """Project pool entries down to the fields update_team_loadout accepts."""
    return [{k: e[k] for k in keys if k in e} for e in entries]


def _snapshot_team(client, team_name="Team 1"):
    team = _get_team(client, team_name)
    return {
        "team": team_name,
        "do_not_validate": team.get("do_not_validate", False),
        "ships": _settable(team.get("ships", []), SHIP_ENTRY_KEYS),
        "weapons": _settable(team.get("weapons", []), WEAPON_ENTRY_KEYS),
    }


def _restore_team(client, snapshot):
    try:
        client.call_tool("update_team_loadout", snapshot)
    except Exception:
        pass


def _safe_delete_var(client, name):
    try:
        client.call_tool("delete_sexp_variable", {"name": name, "force": True})
    except Exception:
        pass


def _find_player_weapon_class(client, limit=80):
    """Find a weapon class that is allowed for the player.  A fresh mission
    only seeds the weapon pool from Default_player_weapon-flagged weapons,
    which some tables have none of, so the pool cannot be relied on."""
    r = client.call_tool("list_weapon_classes")
    assert_success(r)
    for entry in tool_data(r)[:limit]:
        name = entry.get("name")
        if entry.get("allowed_for_player") is True:
            return name
        if "allowed_for_player" not in entry:
            g = client.call_tool("get_weapon_class", {"name": name})
            if g and tool_data(g).get("allowed_for_player"):
                return name
    raise AssertionError("no player-allowed weapon class found")


def _find_player_weapon_classes(client, count, limit=120):
    """Return `count` distinct player-allowed weapon class names, or raise
    SkipTest if this mod's tables do not have that many in reach."""
    r = client.call_tool("list_weapon_classes")
    assert_success(r)
    found = []
    for entry in tool_data(r)[:limit]:
        name = entry.get("name")
        allowed = entry.get("allowed_for_player")
        if allowed is None:
            g = client.call_tool("get_weapon_class", {"name": name})
            allowed = bool(g and tool_data(g).get("allowed_for_player"))
        if allowed and name not in found:
            found.append(name)
            if len(found) == count:
                return found
    raise SkipTest("need %d player-allowed weapon classes, found %d" % (count, len(found)))


def _pick_pool_classes(client):
    """Return (ship_class, weapon_class) usable in loadout entries.  The ship
    comes from Team 1's default pool (seeded with default player ships);
    the weapon comes from the reference tables."""
    team = _get_team(client)
    ships = [e["ship_class"] for e in team.get("ships", []) if "ship_class" in e]
    assert_true(len(ships) > 0, "default ship pool is empty")
    return ships[0], _find_player_weapon_class(client)


def _find_non_player_ship_class(client, limit=60):
    """Find a ship class that is not allowed for the player (for the
    eligibility negative test).  Returns None if the mod has none."""
    r = client.call_tool("list_ship_classes")
    assert_success(r)
    for entry in tool_data(r)[:limit]:
        name = entry.get("name")
        if entry.get("allowed_for_player") is False:
            return name
        if "allowed_for_player" not in entry:
            g = client.call_tool("get_ship_class", {"name": name})
            if g and not tool_data(g).get("allowed_for_player", True):
                return name
    return None


def register(suite, client):

    # ---------- read / shape ----------

    def test_loadout_get_shape():
        data = _get_loadout(client)
        assert_in("num_teams", data, "get_team_loadout includes num_teams")
        assert_equal(len(data["teams"]), data["num_teams"],
            "get_team_loadout reports exactly the teams the mission has")
        team = data["teams"][0]
        for key in ("team", "do_not_validate", "ships", "weapons"):
            assert_in(key, team, f"team object includes '{key}'")
        for e in team["ships"]:
            assert_true("ship_class" in e or "class_variable" in e,
                "every ship entry has a class or a class variable")
            assert_true("count" in e or "count_variable" in e,
                "every ship entry has a count or a count variable")
        for e in team["weapons"]:
            if "weapon_class" in e:
                assert_in("required", e, "static weapon entries report 'required'")
                assert_in("used_in_wings", e, "static weapon entries report usage")

    # ---------- static pool replacement ----------

    def test_loadout_static_update_roundtrip():
        snap = _snapshot_team(client)
        try:
            ship_cls, weapon_cls = _pick_pool_classes(client)

            r = client.call_tool("update_team_loadout", {
                "ships": [{"ship_class": ship_cls, "count": 7}],
                "weapons": [{"weapon_class": weapon_cls, "count": 3, "required": True}],
                "do_not_validate": True,
            })
            assert_success(r)
            team = tool_data(r)
            assert_equal(team.get("do_not_validate"), True, "do_not_validate applied")
            assert_equal(_settable(team["ships"], SHIP_ENTRY_KEYS),
                [{"ship_class": ship_cls, "count": 7}], "ship pool replaced")
            assert_equal(len(team["weapons"]), 1, "weapon pool replaced")
            wep = team["weapons"][0]
            assert_equal(wep.get("weapon_class"), weapon_cls, "weapon class echoed")
            assert_equal(wep.get("count"), 3, "weapon count echoed")
            assert_equal(wep.get("required"), True, "required flag applied")

            # A later update that omits both arrays leaves the pools alone.
            r = client.call_tool("update_team_loadout", {"do_not_validate": False})
            assert_success(r)
            team = tool_data(r)
            assert_equal(team.get("do_not_validate"), False, "do_not_validate cleared")
            assert_equal(len(team["ships"]), 1, "omitted ships array left pool unchanged")

            # Replacing the pool without the required flag clears it.
            r = client.call_tool("update_team_loadout", {
                "weapons": [{"weapon_class": weapon_cls, "count": 3}],
            })
            assert_success(r)
            assert_equal(tool_data(r)["weapons"][0].get("required"), False,
                "required flag cleared by replacement without it")
        finally:
            _restore_team(client, snap)

    def test_loadout_full_roundtrip_preserves_pools():
        """Writing back exactly what get returned must be lossless."""
        snap = _snapshot_team(client)
        try:
            r = client.call_tool("update_team_loadout", {
                "ships": snap["ships"],
                "weapons": snap["weapons"],
            })
            assert_success(r)
            after = _snapshot_team(client)
            assert_equal(after["ships"], snap["ships"], "ship pool round-trips")
            assert_equal(after["weapons"], snap["weapons"], "weapon pool round-trips")
        finally:
            _restore_team(client, snap)

    def test_loadout_clear_pools():
        snap = _snapshot_team(client)
        try:
            r = client.call_tool("update_team_loadout", {"ships": [], "weapons": []})
            assert_success(r)
            team = tool_data(r)
            assert_equal(team["ships"], [], "empty array clears the ship pool")
            assert_equal(team["weapons"], [], "empty array clears the weapon pool")
        finally:
            _restore_team(client, snap)

    def test_loadout_team2_independent():
        with tvt_mission(client):
            snap1 = _snapshot_team(client, "Team 1")
            snap2 = _snapshot_team(client, "Team 2")
            try:
                ship_cls, _ = _pick_pool_classes(client)
                r = client.call_tool("update_team_loadout", {
                    "team": "Team 2",
                    "ships": [{"ship_class": ship_cls, "count": 2}],
                })
                assert_success(r)
                assert_equal(tool_data(r).get("team"), "Team 2", "update echoes Team 2")

                team1 = _get_team(client, "Team 1")
                assert_equal(_settable(team1["ships"], SHIP_ENTRY_KEYS), snap1["ships"],
                    "editing Team 2 leaves Team 1's pool untouched")
            finally:
                _restore_team(client, snap2)
                _restore_team(client, snap1)

    def test_loadout_team2_requires_tvt():
        """Outside team-versus-team the mission format has nowhere to store
        Team 2 -- the parser and saver both stop at Num_teams -- so FRED greys
        it out of the loadout dialog and the tools must refuse it rather than
        accept edits that would be silently discarded."""
        data = _get_loadout(client)
        assert_equal(data["num_teams"], 1, "a non-TVT mission has one team")
        assert_equal(len(data["teams"]), 1, "only Team 1 is reported")
        assert_equal(data["teams"][0].get("team"), "Team 1", "the one team is Team 1")

        ship_cls, weapon_cls = _pick_pool_classes(client)
        assert_error(client.call_tool("update_team_loadout",
            {"team": "Team 2", "ships": []}))
        assert_error(client.call_tool("set_team_loadout_ship",
            {"team": "Team 2", "ship_class": ship_cls, "count": 1}))
        assert_error(client.call_tool("set_team_loadout_weapon",
            {"team": "Team 2", "weapon_class": weapon_cls, "count": 1}))

    def test_loadout_pool_order_normalized():
        """Pools are stored in the Team Loadout dialog's canonical order --
        variable-class entries first, then literal classes by class index --
        so an MCP-authored pool does not reshuffle the first time a designer
        opens the dialog and presses OK."""
        snap = _snapshot_team(client)
        vars_ = []
        try:
            pool_ships = [e["ship_class"] for e in snap["ships"] if "ship_class" in e]
            if len(pool_ships) < 2:
                return      # too few default player ships in this mod to order-test
            first, second = pool_ships[0], pool_ships[1]

            r = client.call_tool("create_sexp_variable", {
                "name": "tl_ord_cls", "default_value": first,
                "variable_type": "string"})
            assert_success(r)
            vars_.append("tl_ord_cls")

            # deliberately submitted in the wrong order
            r = client.call_tool("update_team_loadout", {
                "ships": [
                    {"ship_class": second, "count": 1},
                    {"ship_class": first, "count": 2},
                    {"class_variable": "tl_ord_cls", "count": 3},
                ],
            })
            assert_success(r)
            assert_equal(_settable(tool_data(r)["ships"], SHIP_ENTRY_KEYS), [
                {"class_variable": "tl_ord_cls", "count": 3},
                {"ship_class": first, "count": 2},
                {"ship_class": second, "count": 1},
            ], "pool normalized to variable-first, then ascending class index")
        finally:
            _restore_team(client, snap)
            for v in vars_:
                _safe_delete_var(client, v)

    def test_loadout_required_weapon_not_in_pool():
        """required_weapons is stored independently of the weapon pool, so a
        mission can require a weapon that no pool entry offers.  The API
        cannot create that state (by design), so build it by saving a mission
        and editing the "+Required for mission:" line before loading it back."""
        pool_wep, other_wep, orphan_wep = _find_player_weapon_classes(client, 3)

        rp = client.call_tool("get_root_paths")
        assert_success(rp)
        paths = tool_data(rp)
        if not paths:
            raise SkipTest("no root paths available to save a fixture mission")
        save_path = os.path.join(paths[0]["path"], "_mcp_test_required.fs2")

        try:
            r = client.call_tool("update_team_loadout", {
                "weapons": [
                    {"weapon_class": pool_wep, "count": 5, "required": True},
                    {"weapon_class": other_wep, "count": 5},
                ],
            })
            assert_success(r)
            assert_success(client.call_tool("save_mission", {"filepath": save_path}))

            with open(save_path, encoding="latin-1") as fh:
                text = fh.read()
            if "+Required for mission:" not in text:
                raise SkipTest("mission saved in a format without required-weapon support")

            # add a weapon the pool does not offer
            patched, n = re.subn(
                r'\+Required for mission:\s*\(([^)]*)\)',
                lambda m: '+Required for mission: (%s "%s" )' % (m.group(1), orphan_wep),
                text, count=1)
            assert_equal(n, 1, "patched the required-weapon line")
            with open(save_path, "w", encoding="latin-1", newline="") as fh:
                fh.write(patched)

            assert_success(client.call_tool("load_mission", {"filepath": save_path}))

            team = _get_team(client)
            assert_equal(team.get("required_weapons_not_in_pool"), [orphan_wep],
                "a required weapon absent from the pool is reported separately")
            for e in team["weapons"]:
                if e.get("weapon_class") == pool_wep:
                    assert_equal(e.get("required"), True,
                        "a required weapon that IS in the pool still reports on its entry")

            # replacing the pool must not discard the orphan, even though no
            # entry in the request could have expressed it
            r = client.call_tool("update_team_loadout", {
                "weapons": [{"weapon_class": other_wep, "count": 7}],
            })
            assert_success(r)
            team = tool_data(r)
            assert_equal(team.get("required_weapons_not_in_pool"), [orphan_wep],
                "orphaned required weapon survives a whole-pool replacement")
            assert_equal(_settable(team["weapons"], WEAPON_ENTRY_KEYS),
                [{"weapon_class": other_wep, "count": 7, "required": False}],
                "replacing the pool cleared the previously-required pooled weapon")

            # once the class IS in the pool, the entry's flag is authoritative
            r = client.call_tool("update_team_loadout", {
                "weapons": [{"weapon_class": orphan_wep, "count": 3}],
            })
            assert_success(r)
            team = tool_data(r)
            assert_not_in("required_weapons_not_in_pool", team,
                "a formerly-orphaned class in the pool is no longer orphaned")
            assert_equal(team["weapons"][0].get("required"), False,
                "the entry's omitted 'required' flag wins once the class is pooled")
        finally:
            try:
                os.remove(save_path)
            except OSError:
                pass
            try:
                client.call_tool("new_mission")
            except Exception:
                pass

    # ---------- variable-based entries ----------

    def test_loadout_variable_entries():
        snap = _snapshot_team(client)
        vars_ = []
        try:
            ship_cls, weapon_cls = _pick_pool_classes(client)

            r = client.call_tool("create_sexp_variable", {
                "name": "tl_ship_cls", "default_value": ship_cls,
                "variable_type": "string"})
            assert_success(r)
            vars_.append("tl_ship_cls")
            r = client.call_tool("create_sexp_variable", {
                "name": "tl_count", "default_value": "4",
                "variable_type": "number"})
            assert_success(r)
            vars_.append("tl_count")

            r = client.call_tool("update_team_loadout", {
                "ships": [
                    {"ship_class": ship_cls, "count_variable": "tl_count"},
                    {"class_variable": "tl_ship_cls", "count": 2},
                ],
                "weapons": [{"weapon_class": weapon_cls, "count": 5}],
            })
            assert_success(r)
            ships = _settable(tool_data(r)["ships"], SHIP_ENTRY_KEYS)
            assert_equal(ships, [
                {"class_variable": "tl_ship_cls", "count": 2},
                {"ship_class": ship_cls, "count_variable": "tl_count"},
            ], "variable-based entries round-trip through get, in canonical order")
        finally:
            _restore_team(client, snap)
            for v in vars_:
                _safe_delete_var(client, v)

    def test_loadout_variable_delete_guard():
        snap = _snapshot_team(client)
        vars_ = []
        try:
            ship_cls, _ = _pick_pool_classes(client)
            r = client.call_tool("create_sexp_variable", {
                "name": "tl_guard_cls", "default_value": ship_cls,
                "variable_type": "string"})
            assert_success(r)
            vars_.append("tl_guard_cls")

            r = client.call_tool("update_team_loadout", {
                "ships": [{"class_variable": "tl_guard_cls", "count": 2}],
            })
            assert_success(r)

            # Unforced delete is blocked and mentions the loadout.
            r = client.call_tool("delete_sexp_variable", {"name": "tl_guard_cls"})
            assert_error(r)
            assert_in("loadout", tool_text(r).lower())

            # Forced delete succeeds and removes the loadout entry.
            r = client.call_tool("delete_sexp_variable", {
                "name": "tl_guard_cls", "force": True})
            assert_success(r)
            team = _get_team(client)
            assert_true(all(e.get("class_variable") != "tl_guard_cls"
                            for e in team["ships"]),
                "forced variable delete removed the loadout entry")
        finally:
            _restore_team(client, snap)
            for v in vars_:
                _safe_delete_var(client, v)

    def test_loadout_variable_rename_propagates():
        snap = _snapshot_team(client)
        vars_ = []
        try:
            ship_cls, _ = _pick_pool_classes(client)
            r = client.call_tool("create_sexp_variable", {
                "name": "tl_old_name", "default_value": ship_cls,
                "variable_type": "string"})
            assert_success(r)
            vars_.append("tl_old_name")

            r = client.call_tool("update_team_loadout", {
                "ships": [{"class_variable": "tl_old_name", "count": 1}],
            })
            assert_success(r)

            r = client.call_tool("update_sexp_variable", {
                "name": "tl_old_name", "new_name": "tl_new_name"})
            assert_success(r)
            vars_.append("tl_new_name")

            team = _get_team(client)
            assert_true(any(e.get("class_variable") == "tl_new_name"
                            for e in team["ships"]),
                "loadout entry follows the variable rename")

            # A number<->string type change is blocked while referenced.
            r = client.call_tool("update_sexp_variable", {
                "name": "tl_new_name", "variable_type": "number",
                "default_value": "1"})
            assert_error(r)
            assert_in("loadout", tool_text(r).lower())
        finally:
            _restore_team(client, snap)
            for v in vars_:
                _safe_delete_var(client, v)

    # ---------- negative paths ----------

    def test_loadout_rejects_bad_entries():
        snap = _snapshot_team(client)
        try:
            ship_cls, weapon_cls = _pick_pool_classes(client)

            cases = [
                # unknown class
                {"ships": [{"ship_class": "DOES_NOT_EXIST_xyz", "count": 1}]},
                # class XOR violations
                {"ships": [{"count": 1}]},
                {"ships": [{"ship_class": ship_cls, "class_variable": "x", "count": 1}]},
                # count XOR violations
                {"ships": [{"ship_class": ship_cls}]},
                {"ships": [{"ship_class": ship_cls, "count": 1, "count_variable": "x"}]},
                # count range
                {"ships": [{"ship_class": ship_cls, "count": -1}]},
                {"ships": [{"ship_class": ship_cls, "count": 10000}]},
                # duplicates
                {"ships": [{"ship_class": ship_cls, "count": 1},
                           {"ship_class": ship_cls, "count": 2}]},
                # unknown count variable
                {"ships": [{"ship_class": ship_cls, "count_variable": "tl_missing"}]},
                # required on a zero-count weapon
                {"weapons": [{"weapon_class": weapon_cls, "count": 0, "required": True}]},
                # bad team
                {"team": "none", "ships": []},
            ]
            for args in cases:
                r = client.call_tool("update_team_loadout", args)
                assert_error(r)

            # Failed updates must not have modified anything.
            assert_equal(_snapshot_team(client), snap,
                "rejected updates left the loadout unchanged")
        finally:
            _restore_team(client, snap)

    def test_loadout_variable_type_mismatch_rejected():
        snap = _snapshot_team(client)
        vars_ = []
        try:
            ship_cls, _ = _pick_pool_classes(client)
            r = client.call_tool("create_sexp_variable", {
                "name": "tl_num", "default_value": "3",
                "variable_type": "number"})
            assert_success(r)
            vars_.append("tl_num")
            r = client.call_tool("create_sexp_variable", {
                "name": "tl_str", "default_value": "not a ship class",
                "variable_type": "string"})
            assert_success(r)
            vars_.append("tl_str")

            # class_variable must be a string variable...
            r = client.call_tool("update_team_loadout", {
                "ships": [{"class_variable": "tl_num", "count": 1}]})
            assert_error(r)
            # ...whose value is a valid class...
            r = client.call_tool("update_team_loadout", {
                "ships": [{"class_variable": "tl_str", "count": 1}]})
            assert_error(r)
            # ...and count_variable must be a number variable.
            r = client.call_tool("update_team_loadout", {
                "ships": [{"ship_class": ship_cls, "count_variable": "tl_str"}]})
            assert_error(r)
            # required is only for static classes
            valid_str = "tl_valid_cls"
            r = client.call_tool("create_sexp_variable", {
                "name": valid_str, "default_value": ship_cls,
                "variable_type": "string"})
            assert_success(r)
            vars_.append(valid_str)
            r = client.call_tool("update_team_loadout", {
                "weapons": [{"class_variable": valid_str, "count": 1, "required": True}]})
            assert_error(r)
        finally:
            _restore_team(client, snap)
            for v in vars_:
                _safe_delete_var(client, v)

    def test_loadout_non_player_ship_rejected():
        cls = _find_non_player_ship_class(client)
        if cls is None:
            return  # mod has no non-player ship classes; nothing to test
        r = client.call_tool("update_team_loadout", {
            "ships": [{"ship_class": cls, "count": 1}]})
        assert_error(r)
        assert_in("player", tool_text(r).lower())

    # ---------- single-entry upsert tools ----------

    def test_loadout_upsert_ship():
        snap = _snapshot_team(client)
        try:
            ship_cls, _ = _pick_pool_classes(client)
            r = client.call_tool("update_team_loadout", {"ships": []})
            assert_success(r)

            # create
            r = client.call_tool("set_team_loadout_ship", {
                "ship_class": ship_cls, "count": 3})
            assert_success(r)
            assert_equal(_settable(tool_data(r)["ships"], SHIP_ENTRY_KEYS),
                [{"ship_class": ship_cls, "count": 3}], "upsert created the entry")

            # update in place
            r = client.call_tool("set_team_loadout_ship", {
                "ship_class": ship_cls, "count": 8})
            assert_success(r)
            assert_equal(_settable(tool_data(r)["ships"], SHIP_ENTRY_KEYS),
                [{"ship_class": ship_cls, "count": 8}],
                "upsert updated the entry instead of duplicating it")

            # remove
            r = client.call_tool("set_team_loadout_ship", {
                "ship_class": ship_cls, "enable": False})
            assert_success(r)
            assert_equal(tool_data(r)["ships"], [], "enable=false removed the entry")

            # remove of a nonexistent entry errors
            r = client.call_tool("set_team_loadout_ship", {
                "ship_class": ship_cls, "enable": False})
            assert_error(r)
        finally:
            _restore_team(client, snap)

    def test_loadout_upsert_weapon():
        snap = _snapshot_team(client)
        vars_ = []
        try:
            _, weapon_cls = _pick_pool_classes(client)
            r = client.call_tool("update_team_loadout", {"weapons": []})
            assert_success(r)

            # create with required
            r = client.call_tool("set_team_loadout_weapon", {
                "weapon_class": weapon_cls, "count": 2, "required": True})
            assert_success(r)
            wep = tool_data(r)["weapons"][0]
            assert_equal(wep.get("count"), 2, "upsert created the weapon entry")
            assert_equal(wep.get("required"), True, "required applied on create")

            # partial update: flip required only, count untouched
            r = client.call_tool("set_team_loadout_weapon", {
                "weapon_class": weapon_cls, "required": False})
            assert_success(r)
            wep = tool_data(r)["weapons"][0]
            assert_equal(wep.get("required"), False, "required flipped by partial update")
            assert_equal(wep.get("count"), 2, "count preserved by partial update")

            # switch the count from a literal to a variable
            r = client.call_tool("create_sexp_variable", {
                "name": "tl_up_count", "default_value": "6",
                "variable_type": "number"})
            assert_success(r)
            vars_.append("tl_up_count")
            r = client.call_tool("set_team_loadout_weapon", {
                "weapon_class": weapon_cls, "count_variable": "tl_up_count"})
            assert_success(r)
            wep = tool_data(r)["weapons"][0]
            assert_equal(wep.get("count_variable"), "tl_up_count",
                "count switched from literal to variable")
            assert_true("count" not in wep, "literal count replaced by the variable")

            # removal clears the required flag: re-require, remove, re-add
            r = client.call_tool("set_team_loadout_weapon", {
                "weapon_class": weapon_cls, "required": True})
            assert_success(r)
            r = client.call_tool("set_team_loadout_weapon", {
                "weapon_class": weapon_cls, "enable": False})
            assert_success(r)
            assert_equal(tool_data(r)["weapons"], [], "entry removed")
            r = client.call_tool("set_team_loadout_weapon", {
                "weapon_class": weapon_cls, "count": 1})
            assert_success(r)
            assert_equal(tool_data(r)["weapons"][0].get("required"), False,
                "required flag was cleared by the removal")
        finally:
            _restore_team(client, snap)
            for v in vars_:
                _safe_delete_var(client, v)

    def test_loadout_upsert_variable_entry():
        snap = _snapshot_team(client)
        vars_ = []
        try:
            ship_cls, _ = _pick_pool_classes(client)
            r = client.call_tool("create_sexp_variable", {
                "name": "tl_up_cls", "default_value": ship_cls,
                "variable_type": "string"})
            assert_success(r)
            vars_.append("tl_up_cls")

            r = client.call_tool("update_team_loadout", {"ships": []})
            assert_success(r)

            r = client.call_tool("set_team_loadout_ship", {
                "class_variable": "tl_up_cls", "count": 2})
            assert_success(r)
            assert_equal(_settable(tool_data(r)["ships"], SHIP_ENTRY_KEYS),
                [{"class_variable": "tl_up_cls", "count": 2}],
                "variable-keyed entry created")

            # a static entry of the same class is a distinct key
            r = client.call_tool("set_team_loadout_ship", {
                "ship_class": ship_cls, "count": 1})
            assert_success(r)
            assert_equal(len(tool_data(r)["ships"]), 2,
                "static entry coexists with the variable entry")

            r = client.call_tool("set_team_loadout_ship", {
                "class_variable": "tl_up_cls", "enable": False})
            assert_success(r)
            ships = tool_data(r)["ships"]
            assert_equal(_settable(ships, SHIP_ENTRY_KEYS),
                [{"ship_class": ship_cls, "count": 1}],
                "variable entry removed; static entry untouched")
        finally:
            _restore_team(client, snap)
            for v in vars_:
                _safe_delete_var(client, v)

    def test_loadout_upsert_negative():
        snap = _snapshot_team(client)
        try:
            ship_cls, weapon_cls = _pick_pool_classes(client)
            r = client.call_tool("update_team_loadout", {"ships": [], "weapons": []})
            assert_success(r)

            cases = [
                ("set_team_loadout_ship", {"ship_class": "DOES_NOT_EXIST_xyz", "count": 1}),
                # create without a count
                ("set_team_loadout_ship", {"ship_class": ship_cls}),
                # both count forms
                ("set_team_loadout_ship", {"ship_class": ship_cls, "count": 1,
                                           "count_variable": "x"}),
                # key XOR violations
                ("set_team_loadout_ship", {"count": 1}),
                ("set_team_loadout_ship", {"ship_class": ship_cls,
                                           "class_variable": "x", "count": 1}),
                # count range
                ("set_team_loadout_ship", {"ship_class": ship_cls, "count": 10000}),
                # required with a zero count
                ("set_team_loadout_weapon", {"weapon_class": weapon_cls, "count": 0,
                                             "required": True}),
                ("set_team_loadout_ship", {"team": "none", "ship_class": ship_cls,
                                           "count": 1}),
            ]
            for tool, args in cases:
                r = client.call_tool(tool, args)
                assert_error(r)

            team = _get_team(client)
            assert_equal(team["ships"], [], "rejected upserts created no ship entries")
            assert_equal(team["weapons"], [], "rejected upserts created no weapon entries")
        finally:
            _restore_team(client, snap)

    # ---------- player entry delay (mission info) ----------

    def test_loadout_player_entry_delay():
        r = client.call_tool("get_mission_info")
        assert_success(r)
        original = tool_data(r).get("player_entry_delay")
        assert_true(isinstance(original, int),
            "get_mission_info reports player_entry_delay")
        try:
            r = client.call_tool("update_mission_info", {"player_entry_delay": 5})
            assert_success(r)
            assert_equal(tool_data(r).get("player_entry_delay"), 5,
                "player_entry_delay applied")

            r = client.call_tool("update_mission_info", {"player_entry_delay": -1})
            assert_error(r)
            r = client.call_tool("update_mission_info", {"player_entry_delay": 31})
            assert_error(r)
        finally:
            try:
                client.call_tool("update_mission_info",
                    {"player_entry_delay": original if original is not None else 0})
            except Exception:
                pass

    suite.add("loadout_get_shape", test_loadout_get_shape)
    suite.add("loadout_static_update_roundtrip", test_loadout_static_update_roundtrip)
    suite.add("loadout_full_roundtrip_preserves_pools", test_loadout_full_roundtrip_preserves_pools)
    suite.add("loadout_clear_pools", test_loadout_clear_pools)
    suite.add("loadout_team2_independent", test_loadout_team2_independent)
    suite.add("loadout_team2_requires_tvt", test_loadout_team2_requires_tvt)
    suite.add("loadout_pool_order_normalized", test_loadout_pool_order_normalized)
    suite.add("loadout_required_weapon_not_in_pool", test_loadout_required_weapon_not_in_pool)
    suite.add("loadout_variable_entries", test_loadout_variable_entries)
    suite.add("loadout_variable_delete_guard", test_loadout_variable_delete_guard)
    suite.add("loadout_variable_rename_propagates", test_loadout_variable_rename_propagates)
    suite.add("loadout_rejects_bad_entries", test_loadout_rejects_bad_entries)
    suite.add("loadout_variable_type_mismatch_rejected", test_loadout_variable_type_mismatch_rejected)
    suite.add("loadout_non_player_ship_rejected", test_loadout_non_player_ship_rejected)
    suite.add("loadout_upsert_ship", test_loadout_upsert_ship)
    suite.add("loadout_upsert_weapon", test_loadout_upsert_weapon)
    suite.add("loadout_upsert_variable_entry", test_loadout_upsert_variable_entry)
    suite.add("loadout_upsert_negative", test_loadout_upsert_negative)
    suite.add("loadout_player_entry_delay", test_loadout_player_entry_delay)


if __name__ == "__main__":
    run_module_standalone(register, "Team loadout tests")
