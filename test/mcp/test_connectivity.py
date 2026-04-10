"""Connectivity tests for the FRED2 MCP server.

These are the abort-gate tests: if either fails, the rest of the suite
cannot meaningfully run, so they are marked critical=True.
"""

from mcp_test_lib import (
    assert_has_key,
    assert_true,
    run_module_standalone,
)


def register(suite, client):
    def test_initialize():
        resp = client.rpc("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "mcp-test", "version": "1.0"}
        })
        assert_true("result" in resp, "No result in initialize response")
        result = resp["result"]
        assert_has_key(result, "protocolVersion")
        client.notify("notifications/initialized")

    def test_tools_list():
        resp = client.rpc("tools/list", {})
        assert_true("result" in resp, "No result in tools/list response")
        tools = resp["result"].get("tools", [])
        assert_true(len(tools) >= 70, f"Expected at least 70 tools, got {len(tools)}")

    suite.add("connectivity_initialize", test_initialize, critical=True)
    suite.add("connectivity_tools_list", test_tools_list, critical=True)


if __name__ == "__main__":
    # Connectivity is its own prelude — registering the prelude would call
    # initialize twice.  And nothing here needs a fresh mission.
    run_module_standalone(register, "Connectivity tests",
                          needs_connectivity=False, needs_mission=False)
