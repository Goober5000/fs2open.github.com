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
        r = client.call_tool("load_mission", {"filepath": save_path})
        assert_success(r)
        # Verify mission info reflects the loaded file
        mi = client.call_tool("get_mission_info")
        assert_success(mi)
        # Clean up: create a new mission so subsequent tests start fresh.
        # This is the contract that lets test_crud, test_sexp_*, and the
        # rest of the suite assume an empty mission state.
        client.call_tool("new_mission")

    def test_get_mission_info_new():
        r = client.call_tool("get_mission_info")
        assert_success(r)
        d = tool_data(r)
        assert_is_dict(d)

    suite.add("mission_setup_new_mission", test_new_mission, critical=True)
    suite.add("mission_setup_save_mission", test_save_mission)
    suite.add("mission_setup_load_mission", test_load_mission)
    suite.add("mission_setup_get_mission_info", test_get_mission_info_new)


if __name__ == "__main__":
    # This file *defines* mission setup, so don't register the standalone
    # mission prelude — it would be redundant with test_new_mission and would
    # leave a stray new_mission call before the area's own first test.
    run_module_standalone(register, "Mission setup tests", needs_mission=False)
