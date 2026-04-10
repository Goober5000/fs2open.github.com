"""Server utility tests: server info, UI status, timeouts, mission info.

These are read-only smoke tests of the always-available server tools.  They
don't depend on a particular mission state, so the standalone runner does
not need to register a mission prelude — but the comprehensive runner will
have already loaded a mission by the time this file's tests execute.
"""

from mcp_test_lib import (
    assert_equal,
    assert_has_key,
    assert_in,
    assert_success,
    assert_true,
    run_module_standalone,
    tool_data,
    tool_text,
)


def register(suite, client):
    ctx = suite.ctx

    def test_get_server_info():
        r = client.call_tool("get_server_info")
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "status")
        assert_equal(d["status"], "running", "server status")

    def test_get_ui_status():
        r = client.call_tool("get_ui_status")
        assert_success(r)
        text = tool_text(r)
        assert_true(len(text) > 0, "get_ui_status returned empty text")
        assert_in("modal_dialog_active", text, "Expected modal_dialog_active in ui status")

    def test_get_timeout():
        r = client.call_tool("get_timeout")
        assert_success(r)
        sc = r.get("structuredContent", {})
        assert_has_key(sc, "seconds")
        ctx["original_timeout"] = sc["seconds"]

    def test_set_timeout():
        r = client.call_tool("set_timeout", {"seconds": 15})
        assert_success(r)
        sc = r.get("structuredContent", {})
        assert_equal(sc.get("seconds"), 15, "set_timeout value")
        # Restore
        orig = ctx.get("original_timeout", 10)
        client.call_tool("set_timeout", {"seconds": orig})

    def test_get_mission_info():
        r = client.call_tool("get_mission_info")
        assert_success(r)
        text = tool_text(r)
        assert_true(len(text) > 0, "get_mission_info returned empty text")

    suite.add("server_utility_get_server_info", test_get_server_info)
    suite.add("server_utility_get_ui_status", test_get_ui_status)
    suite.add("server_utility_get_timeout", test_get_timeout)
    suite.add("server_utility_set_timeout", test_set_timeout)
    suite.add("server_utility_get_mission_info", test_get_mission_info)


if __name__ == "__main__":
    # Server-utility tests don't depend on a particular mission, but a
    # mission must exist for the FRED2 instance to be in a usable state,
    # so we leave needs_mission=True (the default) for safety.
    run_module_standalone(register, "Server utility tests")
