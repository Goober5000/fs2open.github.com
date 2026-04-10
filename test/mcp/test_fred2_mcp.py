#!/usr/bin/env python3
"""Comprehensive FRED2 MCP test runner.

This file is the single entry point that runs every per-area test file in
order.  Each per-area file (test_connectivity.py, test_server_utility.py,
...) is also runnable on its own for fast iteration; this orchestrator
just imports each one's register(suite, client) function and adds them
all to a single shared TestSuite.

Usage:
    python test_fred2_mcp.py [--url URL] [--timeout SECONDS]

Exit codes:
    0 = all tests passed
    1 = one or more non-critical tests failed
    2 = critical test failed (connectivity or mission setup), or
        could not connect to FRED2 at startup
"""

import argparse
import sys

from mcp_test_lib import (
    MCPClient,
    TestSuite,
    wait_for_server,
)

import test_connectivity
import test_server_utility
import test_reference
import test_mission_setup
import test_crud
import test_sexp_roundtrip
import test_sexp_detach
import test_negative


# Order matters: connectivity must run first, mission setup must run before
# anything that needs an empty mission, and the SEXP files come after CRUD
# so any cross-area mutation hazards happen in a predictable order.
AREAS = [
    test_connectivity,
    test_server_utility,
    test_reference,
    test_mission_setup,
    test_crud,
    test_sexp_roundtrip,
    test_sexp_detach,
    test_negative,
]


def main():
    parser = argparse.ArgumentParser(description="FRED2 MCP comprehensive test suite")
    parser.add_argument("--url", default="http://127.0.0.1:8080/mcp",
                        help="MCP server URL (default: http://127.0.0.1:8080/mcp)")
    parser.add_argument("--timeout", type=int, default=30,
                        help="Default HTTP timeout in seconds (default: 30)")
    args = parser.parse_args()

    client = MCPClient(url=args.url, timeout=args.timeout)

    print("=" * 70)
    print("FRED2 MCP Comprehensive Test Suite")
    print(f"Server: {args.url}")
    print("=" * 70)

    print("\nConnecting to FRED2...")
    if not wait_for_server(client):
        sys.exit(2)
    print("  Connected!\n")

    suite = TestSuite()
    for area in AREAS:
        area.register(suite, client)

    try:
        ok = suite.run()
    finally:
        # Best-effort cleanup: leave the FRED2 instance in a clean state.
        # The current set of tests creates and destroys their own state, but
        # an aborted run might leave a half-built tree or stale entity around.
        # Wrap in try/except so this never masks a real failure.
        try:
            client.call_tool("new_mission")
        except Exception:
            pass

    rc = suite.summary()
    if not ok:
        # Critical failure: exit code 2 distinguishes "couldn't even bring the
        # environment up" from "one test broke" (exit 1).  Documented at the
        # top of this file.
        sys.exit(2)
    sys.exit(rc)


if __name__ == "__main__":
    main()
