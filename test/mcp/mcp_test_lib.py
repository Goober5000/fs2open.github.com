"""Shared infrastructure for the FRED2 MCP test suite.

Provides the JSON-RPC client, assertion helpers, test runner, and a
standalone-mode entry point that every per-area test file uses.

Each per-area test file (test_connectivity.py, test_crud.py, ...) imports
from this module, defines a register(suite, client) function that adds
its tests to the supplied TestSuite, and ends with a __main__ block that
calls run_module_standalone(register, ...) so it can be invoked directly
for fast iteration.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request


# ---------------------------------------------------------------------------
# MCP Client
# ---------------------------------------------------------------------------

class MCPClient:
    def __init__(self, url="http://127.0.0.1:8080/mcp", timeout=30):
        self.url = url
        self.timeout = timeout
        self._next_id = 1
        self._tool_names = None  # lazy cache for tool_names property

    def rpc(self, method, params=None):
        """Send a JSON-RPC 2.0 request and return the parsed response."""
        body = {
            "jsonrpc": "2.0",
            "id": self._next_id,
            "method": method,
        }
        if params is not None:
            body["params"] = params
        self._next_id += 1

        data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(
            self.url,
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=self.timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def call_tool(self, name, arguments=None, timeout=None):
        """Call an MCP tool and return the result dict.

        Raises MCPError on JSON-RPC level errors.
        """
        params = {"name": name}
        if arguments is not None:
            params["arguments"] = arguments

        old_timeout = self.timeout
        if timeout is not None:
            self.timeout = timeout
        try:
            resp = self.rpc("tools/call", params)
        finally:
            self.timeout = old_timeout

        if "error" in resp:
            raise MCPError(resp["error"].get("message", "Unknown RPC error"),
                           resp["error"].get("code", -32000))
        return resp.get("result", {})

    def notify(self, method, params=None):
        """Send a JSON-RPC notification (no id, no response expected)."""
        body = {
            "jsonrpc": "2.0",
            "method": method,
        }
        if params is not None:
            body["params"] = params
        data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(
            self.url,
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                resp.read()
        except Exception:
            pass  # notifications don't require a response

    @property
    def tool_names(self):
        """Set of tool names exposed by the server, populated on first access.

        Some tests need to gate behavior on whether an optional tool exists
        (e.g. update_jump_node).  Caching this on the client lets per-area
        files run standalone without depending on a connectivity test having
        populated a shared dict first.
        """
        if self._tool_names is None:
            resp = self.rpc("tools/list", {})
            tools = resp.get("result", {}).get("tools", [])
            self._tool_names = {t["name"] for t in tools}
        return self._tool_names


class MCPError(Exception):
    def __init__(self, message, code=-32000):
        super().__init__(message)
        self.code = code


# ---------------------------------------------------------------------------
# Assertion helpers
# ---------------------------------------------------------------------------

class SkipTest(Exception):
    """Raised to skip a test (e.g., no data available)."""
    pass


def assert_success(result):
    """Assert the tool call succeeded (no isError flag)."""
    if result.get("isError"):
        text = _extract_text(result)
        raise AssertionError(f"Expected success, got error: {text}")


def assert_error(result):
    """Assert the tool call returned an error."""
    if not result.get("isError"):
        raise AssertionError("Expected error, got success")


def tool_data(result):
    """Parse the JSON embedded in content[0].text."""
    text = _extract_text(result)
    return json.loads(text)


def tool_text(result):
    """Return the raw text from content[0].text."""
    return _extract_text(result)


def _extract_text(result):
    content = result.get("content", [])
    if not content:
        raise AssertionError("No content in result")
    return content[0].get("text", "")


def assert_equal(actual, expected, msg=""):
    if actual != expected:
        raise AssertionError(f"{msg}: expected {expected!r}, got {actual!r}" if msg
                             else f"Expected {expected!r}, got {actual!r}")


def assert_true(value, msg=""):
    if not value:
        raise AssertionError(msg or f"Expected truthy, got {value!r}")


def assert_in(item, collection, msg=""):
    if item not in collection:
        raise AssertionError(msg or f"{item!r} not found in collection")


def assert_is_list(value, msg=""):
    if not isinstance(value, list):
        raise AssertionError(msg or f"Expected list, got {type(value).__name__}")


def assert_is_dict(value, msg=""):
    if not isinstance(value, dict):
        raise AssertionError(msg or f"Expected dict, got {type(value).__name__}")


def assert_has_key(d, key, msg=""):
    if not isinstance(d, dict) or key not in d:
        raise AssertionError(msg or f"Missing key {key!r}")


def assert_non_empty_list(value, msg=""):
    assert_is_list(value, msg)
    if len(value) == 0:
        raise AssertionError(msg or "Expected non-empty list")


# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------

class TestResult:
    def __init__(self, name, status, message="", duration=0.0):
        self.name = name
        self.status = status  # "pass", "fail", "skip"
        self.message = message
        self.duration = duration


class TestSuite:
    """Collects test functions and runs them in registration order.

    A test marked critical=True aborts the entire run as soon as it fails.
    Connectivity tests and the new_mission setup test are typically marked
    critical so a broken environment fails fast instead of producing a wall
    of cascade failures.

    self.ctx is a per-suite dict that test closures use to share state
    between tests in the same area (e.g. a discovery test stashes a name
    that a later test consumes).  Areas should not depend on ctx keys
    written by other areas — see test_crud.py's use of client.tool_names
    for the one place that used to leak across areas.
    """

    def __init__(self):
        self.results = []
        self._tests = []  # list of (name, func, critical)
        self.ctx = {}
        self._aborted = False

    def add(self, name, func, critical=False):
        self._tests.append((name, func, critical))

    def run(self):
        """Run all registered tests in order. Returns True iff no critical
        test failed.  Non-critical failures do not stop the run."""
        for name, func, critical in self._tests:
            t0 = time.time()
            try:
                func()
                elapsed = time.time() - t0
                self.results.append(TestResult(name, "pass", duration=elapsed))
                self._print_result("PASS", name, elapsed)
            except SkipTest as e:
                elapsed = time.time() - t0
                self.results.append(TestResult(name, "skip", str(e), elapsed))
                self._print_result("SKIP", name, elapsed, str(e))
            except Exception as e:
                elapsed = time.time() - t0
                msg = str(e)
                self.results.append(TestResult(name, "fail", msg, elapsed))
                self._print_result("FAIL", name, elapsed, msg)
                if critical:
                    print(f"\n*** ABORT: critical test {name!r} failed ***\n")
                    self._aborted = True
                    return False
        return True

    def _print_result(self, status, name, elapsed, msg=""):
        tag = {"PASS": "\033[32mPASS\033[0m",
               "FAIL": "\033[31mFAIL\033[0m",
               "SKIP": "\033[33mSKIP\033[0m"}.get(status, status)
        line = f"  [{tag}] {name:<55s} ({elapsed:.2f}s)"
        if msg:
            short = msg[:120].replace("\n", " ")
            line += f" -- {short}"
        print(line)

    def summary(self):
        passed = sum(1 for r in self.results if r.status == "pass")
        failed = sum(1 for r in self.results if r.status == "fail")
        skipped = sum(1 for r in self.results if r.status == "skip")
        total = len(self.results)
        total_time = sum(r.duration for r in self.results)

        print("\n" + "=" * 70)
        if failed:
            print(f"\033[31mResults: {passed} passed, {failed} failed, {skipped} skipped"
                  f" (total: {total}, {total_time:.1f}s)\033[0m")
            print("\nFailed tests:")
            for r in self.results:
                if r.status == "fail":
                    print(f"  - {r.name}: {r.message[:200]}")
        else:
            print(f"\033[32mResults: {passed} passed, {failed} failed, {skipped} skipped"
                  f" (total: {total}, {total_time:.1f}s)\033[0m")
        print("=" * 70)
        return 0 if failed == 0 else 1


# ---------------------------------------------------------------------------
# Server connection
# ---------------------------------------------------------------------------

def wait_for_server(client, max_attempts=10, delay=2.0):
    """Wait for FRED2 MCP server to become available.

    Uses a lightweight HTTP probe rather than the full initialize handshake,
    so the dedicated connectivity test can perform the single spec-required
    initialize call.  We deliberately treat *any* HTTP response -- including
    non-2xx like 405 Method Not Allowed -- as proof the server is alive.
    Without the explicit HTTPError catch, urlopen would raise it and the
    URLError clause below would treat the server as unreachable, since
    HTTPError is a subclass of URLError.
    """
    for attempt in range(max_attempts):
        try:
            probe = urllib.request.Request(client.url, method="GET")
            with urllib.request.urlopen(probe, timeout=client.timeout) as resp:
                resp.read()
            return True
        except urllib.error.HTTPError:
            # Server responded with an HTTP error code -- it's alive, just
            # doesn't like bare GETs.  That's fine; we only need reachability.
            return True
        except (urllib.error.URLError, ConnectionError, OSError) as e:
            if attempt < max_attempts - 1:
                print(f"  Waiting for FRED2 MCP server (attempt {attempt + 1}/{max_attempts})...")
                time.sleep(delay)
            else:
                print(f"\n  ERROR: Could not connect to FRED2 MCP server at {client.url}")
                print(f"  Last error: {e}")
                return False
    return False


# ---------------------------------------------------------------------------
# Standalone-mode preludes
# ---------------------------------------------------------------------------

def register_connectivity_prelude(suite, client):
    """Register the minimum-viable connectivity tests as critical preludes.

    Used by run_module_standalone() so any per-area file can run on its own
    without first dragging in test_connectivity.py.  The MCP server requires
    the initialize handshake before any other tool call, so even areas that
    don't otherwise care about connectivity need this prelude.
    """
    def test_initialize_prelude():
        resp = client.rpc("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "mcp-test", "version": "1.0"}
        })
        assert_true("result" in resp, "No result in initialize response")
        result = resp["result"]
        assert_has_key(result, "protocolVersion")
        client.notify("notifications/initialized")

    suite.add("prelude_initialize", test_initialize_prelude, critical=True)


def register_mission_prelude(suite, client):
    """Register a critical new_mission test so the area starts on a clean slate.

    The comprehensive runner relies on test_mission_setup running before any
    CRUD/SEXP area, then ending with a new_mission to leave the world clean.
    Per-area standalone runs need the same guarantee.
    """
    def test_new_mission_prelude():
        r = client.call_tool("new_mission")
        assert_success(r)

    suite.add("prelude_new_mission", test_new_mission_prelude, critical=True)


# ---------------------------------------------------------------------------
# Standalone runner
# ---------------------------------------------------------------------------

def run_module_standalone(register_fn, description,
                          needs_connectivity=True, needs_mission=True):
    """Entry point for running a single per-area test file directly.

    Each per-area file calls this from its __main__ block.  It parses CLI
    args, opens a client, waits for the server, registers the connectivity
    (and optionally mission-setup) preludes as critical tests, then calls
    register_fn(suite, client) to add the area's own tests, runs the suite,
    and exits with the appropriate code.

    Args:
        register_fn: callable taking (suite, client) that adds area tests.
        description: human-readable description of the area for the banner.
        needs_connectivity: if True (default), register an initialize prelude
            before the area's tests.  Set to False only in test_connectivity,
            which already includes the initialize handshake as one of its
            own tests and would otherwise call initialize twice in a row.
        needs_mission: if True (default), register a new_mission prelude
            before the area's tests.  Set to False for test_connectivity and
            test_mission_setup itself.
    """
    parser = argparse.ArgumentParser(description=f"FRED2 MCP test: {description}")
    parser.add_argument("--url", default="http://127.0.0.1:8080/mcp",
                        help="MCP server URL (default: http://127.0.0.1:8080/mcp)")
    parser.add_argument("--timeout", type=int, default=30,
                        help="Default HTTP timeout in seconds (default: 30)")
    args = parser.parse_args()

    client = MCPClient(url=args.url, timeout=args.timeout)

    print("=" * 70)
    print(f"FRED2 MCP Test: {description}")
    print(f"Server: {args.url}")
    print("=" * 70)

    print("\nConnecting to FRED2...")
    if not wait_for_server(client):
        sys.exit(2)
    print("  Connected!\n")

    suite = TestSuite()
    if needs_connectivity:
        register_connectivity_prelude(suite, client)
    if needs_mission:
        register_mission_prelude(suite, client)
    register_fn(suite, client)

    ok = suite.run()
    rc = suite.summary()
    if not ok:
        # Critical failure: exit 2 to distinguish from a non-critical failure
        # (exit 1).  See exit-code contract in test_fred2_mcp.py's docstring.
        sys.exit(2)
    sys.exit(rc)
