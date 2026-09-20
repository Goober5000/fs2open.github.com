"""Mission setup tests: new_mission, save_mission, load_mission.

The new_mission test is critical: every other CRUD/SEXP area assumes a
clean mission has been loaded.  Inside the comprehensive run, this file's
test_load_mission ends with another new_mission call to leave the world
empty for the rest of the suite — a contract the standalone area files
honor by registering their own mission prelude.
"""

import os

from mcp_test_lib import (
    assert_is_dict,
    assert_success,
    assert_true,
    run_module_standalone,
    SkipTest,
    tool_data,
)


def register(suite, client):
    ctx = suite.ctx

    def test_new_mission():
        r = client.call_tool("new_mission")
        assert_success(r)

    def test_save_mission():
        rp = client.call_tool("get_root_paths")
        assert_success(rp)
        paths = tool_data(rp)
        if not paths:
            raise SkipTest("No root paths available for save test")
        save_path = os.path.join(paths[0]["path"], "_mcp_test_save.fs2")
        ctx["test_save_path"] = save_path
        r = client.call_tool("save_mission", {"filepath": save_path})
        assert_success(r)

    def test_load_mission():
        save_path = ctx.get("test_save_path")
        if not save_path:
            raise SkipTest("No saved mission path from previous test")
        try:
            r = client.call_tool("load_mission", {"filepath": save_path})
            assert_success(r)
            # Verify mission info reflects the loaded file
            mi = client.call_tool("get_mission_info")
            assert_success(mi)
        finally:
            # Delete the scratch file written by test_save_mission so we don't
            # leave a stray _mcp_test_save.fs2 in the user's mission directory.
            # Wrapped in try/except so a missing/locked file never masks the
            # actual load_mission assertion.
            try:
                os.remove(save_path)
            except OSError:
                pass
            # Clean up: create a new mission so subsequent tests start fresh.
            # This is the contract that lets test_crud, test_sexp_*, and the
            # rest of the suite assume an empty mission state.  Wrapped in
            # try/except so a new_mission failure inside the finally cannot
            # mask the original load_mission assertion.
            try:
                client.call_tool("new_mission")
            except Exception:
                pass

    def test_save_load_bare_filename():
        # A bare filename should land in the primary mod's missions folder, and
        # loading it back should resolve the same way without a path.
        name = "_mcp_bare_save.fs2"
        r = client.call_tool("save_mission", {"filepath": name})
        assert_success(r)

        saved_path = None
        try:
            lm = client.call_tool("list_missions")
            assert_success(lm)
            for entry in tool_data(lm):
                if entry.get("packed"):
                    continue
                if any(m["filename"].lower() == name for m in entry["missions"]):
                    saved_path = os.path.join(entry["path"], name)
                    break
            assert_true(saved_path is not None,
                        "mission saved by bare filename should appear in list_missions")

            r = client.call_tool("load_mission", {"filepath": name})
            assert_success(r)
        finally:
            if saved_path:
                try:
                    os.remove(saved_path)
                except OSError:
                    pass
            try:
                client.call_tool("new_mission")
            except Exception:
                pass

    def test_get_mission_info_new():
        r = client.call_tool("get_mission_info")
        assert_success(r)
        d = tool_data(r)
        assert_is_dict(d)

    suite.add("mission_setup_new_mission", test_new_mission, critical=True)
    suite.add("mission_setup_save_mission", test_save_mission)
    suite.add("mission_setup_load_mission", test_load_mission)
    suite.add("mission_setup_save_load_bare_filename", test_save_load_bare_filename)
    suite.add("mission_setup_get_mission_info", test_get_mission_info_new)


if __name__ == "__main__":
    # This file *defines* mission setup, so don't register the standalone
    # mission prelude — it would be redundant with test_new_mission and would
    # leave a stray new_mission call before the area's own first test.
    run_module_standalone(register, "Mission setup tests", needs_mission=False)
