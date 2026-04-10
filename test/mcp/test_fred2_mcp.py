#!/usr/bin/env python3
"""
Comprehensive test suite for all FRED2 MCP tools.

Runs unattended against a running FRED2 instance with MCP enabled.
Uses only Python stdlib (no external dependencies).

Usage:
    python test_fred2_mcp.py [--url URL] [--timeout SECONDS] [--phase PHASE]

Exit codes:
    0 = all tests passed
    1 = one or more tests failed
    2 = connectivity failure (FRED2 not reachable)
"""

import json
import sys
import time
import urllib.request
import urllib.error
import argparse

# ---------------------------------------------------------------------------
# MCP Client
# ---------------------------------------------------------------------------

class MCPClient:
    def __init__(self, url="http://127.0.0.1:8080/mcp", timeout=30):
        self.url = url
        self.timeout = timeout
        self._next_id = 1

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
    def __init__(self):
        self.results = []
        self._tests = []  # (name, func, phase)

    def add(self, name, func, phase=0):
        self._tests.append((name, func, phase))

    def run(self, phase_filter=None):
        for name, func, phase in self._tests:
            if phase_filter is not None and phase != phase_filter:
                continue
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

    def run_phases(self, phases, abort_on_phase=None):
        """Run tests for the given phases in order.
        If abort_on_phase is set and any test in that phase fails, abort."""
        for phase in phases:
            phase_tests = [(n, f, p) for n, f, p in self._tests if p == phase]
            for name, func, _p in phase_tests:
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
                    if abort_on_phase is not None and phase in abort_on_phase:
                        print(f"\n*** ABORT: Critical test failed in phase {phase} ***\n")
                        return False
        return True

    def _print_result(self, status, name, elapsed, msg=""):
        tag = {"PASS": "\033[32mPASS\033[0m",
               "FAIL": "\033[31mFAIL\033[0m",
               "SKIP": "\033[33mSKIP\033[0m"}.get(status, status)
        line = f"  [{tag}] {name:<55s} ({elapsed:.2f}s)"
        if msg:
            # Truncate long messages
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
# Test context (populated dynamically during discovery)
# ---------------------------------------------------------------------------

ctx = {}


# ---------------------------------------------------------------------------
# Phase 0: Connectivity
# ---------------------------------------------------------------------------

def make_phase0_tests(suite, client):

    def test_initialize():
        resp = client.rpc("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "mcp-test", "version": "1.0"}
        })
        assert_true("result" in resp, "No result in initialize response")
        result = resp["result"]
        assert_has_key(result, "protocolVersion")
        # Send initialized notification
        client.notify("notifications/initialized")

    def test_tools_list():
        resp = client.rpc("tools/list", {})
        assert_true("result" in resp, "No result in tools/list response")
        tools = resp["result"].get("tools", [])
        assert_true(len(tools) >= 70, f"Expected at least 70 tools, got {len(tools)}")
        ctx["tool_count"] = len(tools)
        ctx["tool_names"] = {t["name"] for t in tools}

    suite.add("phase0_initialize", test_initialize, phase=0)
    suite.add("phase0_tools_list", test_tools_list, phase=0)


# ---------------------------------------------------------------------------
# Phase 1: Server/Utility
# ---------------------------------------------------------------------------

def make_phase1_tests(suite, client):

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
        # May or may not have mission data depending on whether one is loaded,
        # but the tool should always succeed
        text = tool_text(r)
        assert_true(len(text) > 0, "get_mission_info returned empty text")

    suite.add("phase1_get_server_info", test_get_server_info, phase=1)
    suite.add("phase1_get_ui_status", test_get_ui_status, phase=1)
    suite.add("phase1_get_timeout", test_get_timeout, phase=1)
    suite.add("phase1_set_timeout", test_set_timeout, phase=1)
    suite.add("phase1_get_mission_info", test_get_mission_info, phase=1)


# ---------------------------------------------------------------------------
# Phase 2: Reference/Discovery
# ---------------------------------------------------------------------------

def make_phase2_tests(suite, client):

    # -- Ship/Weapon --

    def test_list_ship_types():
        r = client.call_tool("list_ship_types")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            ctx["ship_type_name"] = d[0].get("name") or d[0].get("ship_type")

    def test_get_ship_type():
        name = ctx.get("ship_type_name")
        if not name:
            raise SkipTest("No ship types available")
        r = client.call_tool("get_ship_type", {"name": name})
        assert_success(r)

    def test_list_ship_classes():
        r = client.call_tool("list_ship_classes")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            ctx["ship_class_name"] = d[0].get("name")
            # Also grab species and ship_type for filtered tests
            if d[0].get("species"):
                ctx["ship_species"] = d[0]["species"]
            if d[0].get("ship_type"):
                ctx["ship_class_type"] = d[0]["ship_type"]

    def test_list_ship_classes_filtered_species():
        species = ctx.get("ship_species")
        if not species:
            raise SkipTest("No species to filter by")
        r = client.call_tool("list_ship_classes", {"species": species})
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        assert_true(len(d) > 0, "Filtered list should not be empty")

    def test_list_ship_classes_filtered_type():
        ship_type = ctx.get("ship_class_type")
        if not ship_type:
            raise SkipTest("No ship_type to filter by")
        r = client.call_tool("list_ship_classes", {"ship_type": ship_type})
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)

    def test_get_ship_class():
        name = ctx.get("ship_class_name")
        if not name:
            raise SkipTest("No ship classes available")
        r = client.call_tool("get_ship_class", {"name": name})
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "name")

    def test_get_ship_class_model_details():
        name = ctx.get("ship_class_name")
        if not name:
            raise SkipTest("No ship classes available")
        r = client.call_tool("get_ship_class_model_details", {"name": name}, timeout=120)
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "name")

    def test_list_weapon_classes():
        r = client.call_tool("list_weapon_classes")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            ctx["weapon_class_name"] = d[0].get("name")

    def test_list_weapon_classes_filtered():
        r = client.call_tool("list_weapon_classes", {"subtype": "primary"})
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)

    def test_get_weapon_class():
        name = ctx.get("weapon_class_name")
        if not name:
            raise SkipTest("No weapon classes available")
        r = client.call_tool("get_weapon_class", {"name": name})
        assert_success(r)

    # -- Species/IFF/Intel --

    def test_list_species():
        r = client.call_tool("list_species")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            ctx["species_name"] = d[0] if isinstance(d[0], str) else d[0].get("name")

    def test_list_iffs():
        r = client.call_tool("list_iffs")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            ctx["iff_name"] = d[0] if isinstance(d[0], str) else d[0].get("name")

    def test_get_iff():
        name = ctx.get("iff_name")
        if not name:
            raise SkipTest("No IFFs available")
        r = client.call_tool("get_iff", {"name": name})
        assert_success(r)

    def test_list_intel_entries():
        r = client.call_tool("list_intel_entries")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            ctx["intel_name"] = d[0] if isinstance(d[0], str) else d[0].get("name")

    def test_get_intel_entry():
        name = ctx.get("intel_name")
        if not name:
            raise SkipTest("No intel entries available")
        r = client.call_tool("get_intel_entry", {"name": name})
        assert_success(r)

    # -- Personas/UI --

    def test_list_persona_types():
        r = client.call_tool("list_persona_types")
        assert_success(r)
        d = tool_data(r)
        assert_non_empty_list(d, "persona types should not be empty")

    def test_list_personas():
        r = client.call_tool("list_personas")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            ctx["persona_name"] = d[0] if isinstance(d[0], str) else d[0].get("name")

    def test_list_talking_heads():
        r = client.call_tool("list_talking_heads")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d, "talking heads should be a list")

    def test_list_fonts():
        r = client.call_tool("list_fonts")
        assert_success(r)
        d = tool_data(r)
        assert_non_empty_list(d, "fonts should not be empty")

    # -- SEXP Metadata --

    def test_list_sexp_categories():
        r = client.call_tool("list_sexp_categories")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            ctx["sexp_category"] = d[0] if isinstance(d[0], str) else d[0].get("name")

    def test_list_sexp_operators():
        r = client.call_tool("list_sexp_operators")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        assert_true(len(d) > 0, "Expected at least one SEXP operator")

    def test_list_sexp_operators_filtered_category():
        cat = ctx.get("sexp_category")
        if not cat:
            raise SkipTest("No sexp category available")
        r = client.call_tool("list_sexp_operators", {"category": cat})
        assert_success(r)

    def test_list_sexp_operators_search():
        r = client.call_tool("list_sexp_operators", {"search": "when"})
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)

    def test_get_sexp_operator():
        r = client.call_tool("get_sexp_operator", {"name": "when"})
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "name")

    def test_get_sexp_argument_type_list():
        r = client.call_tool("get_sexp_argument_type")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            name = d[0] if isinstance(d[0], str) else d[0].get("name")
            ctx["sexp_arg_type_name"] = name

    def test_get_sexp_argument_type():
        name = ctx.get("sexp_arg_type_name")
        if not name:
            raise SkipTest("No sexp argument types available")
        r = client.call_tool("get_sexp_argument_type", {"name": name})
        assert_success(r)

    def test_list_sexp_argument_values():
        # Use the first argument type name discovered from get_sexp_argument_type_list
        name = ctx.get("sexp_arg_type_name")
        if not name:
            raise SkipTest("No sexp argument types discovered yet")
        r = client.call_tool("list_sexp_argument_values", {"name": name})
        assert_success(r)

    def test_get_sexp_return_type_list():
        r = client.call_tool("get_sexp_return_type")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            name = d[0] if isinstance(d[0], str) else d[0].get("name")
            ctx["sexp_ret_type_name"] = name

    def test_get_sexp_return_type():
        name = ctx.get("sexp_ret_type_name")
        if not name:
            raise SkipTest("No sexp return types available")
        r = client.call_tool("get_sexp_return_type", {"name": name})
        assert_success(r)

    # -- Reference Notes --

    def test_list_reference_notes():
        r = client.call_tool("list_reference_notes")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            ctx["ref_note_topic"] = d[0] if isinstance(d[0], str) else d[0].get("topic", d[0].get("name"))
            ctx["ref_note_category"] = (d[0].get("category") if isinstance(d[0], dict) else None)

    def test_list_reference_notes_search():
        r = client.call_tool("list_reference_notes", {"search": "ship"})
        assert_success(r)

    def test_get_reference_note():
        topic = ctx.get("ref_note_topic")
        if not topic:
            raise SkipTest("No reference notes available")
        r = client.call_tool("get_reference_note", {"topic": topic})
        assert_success(r)

    # -- Scripting --

    def test_list_scripting_elements():
        # First call to any scripting tool triggers a one-time cache build
        # of the full scripting API documentation, which can take 30-60s.
        r = client.call_tool("list_scripting_elements", timeout=120)
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d)
        if d:
            ctx["scripting_element_name"] = d[0] if isinstance(d[0], str) else d[0].get("name")

    def test_list_scripting_elements_filtered():
        r = client.call_tool("list_scripting_elements", {"element_type": "library"})
        assert_success(r)

    def test_get_scripting_element():
        name = ctx.get("scripting_element_name")
        if not name:
            raise SkipTest("No scripting elements available")
        r = client.call_tool("get_scripting_element", {"name": name})
        assert_success(r)

    def test_search_scripting_children():
        r = client.call_tool("search_scripting_children", {"search": "get"})
        assert_success(r)

    def test_search_scripting_children_typed():
        r = client.call_tool("search_scripting_children", {"child_type": "function"})
        assert_success(r)

    def test_list_scripting_hooks():
        r = client.call_tool("list_scripting_hooks")
        assert_success(r)

    def test_list_scripting_hooks_filtered():
        r = client.call_tool("list_scripting_hooks", {"overridable": True})
        assert_success(r)

    def test_list_scripting_enums():
        r = client.call_tool("list_scripting_enums")
        assert_success(r)

    def test_get_scripting_misc_conditions():
        r = client.call_tool("get_scripting_misc", {"section": "conditions"})
        assert_success(r)

    def test_get_scripting_misc_globalvars():
        r = client.call_tool("get_scripting_misc", {"section": "globalVars"})
        assert_success(r)

    # -- Misc --

    def test_get_mod_info():
        r = client.call_tool("get_mod_info")
        assert_success(r)
        t = tool_text(r)
        assert_true(len(t) > 0, "mod info should return non-empty text")

    def test_list_missions():
        r = client.call_tool("list_missions")
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "directory")

    def test_get_root_paths():
        r = client.call_tool("get_root_paths")
        assert_success(r)
        d = tool_data(r)
        assert_is_list(d, "root paths should be a list")
        assert_true(len(d) > 0, "root paths should not be empty")
        assert_has_key(d[0], "path", "each root path entry should have 'path' key")

    def test_subsystem_names_compare():
        r = client.call_tool("subsystem_names_compare",
                             {"name1": "turret01", "name2": "turret02"})
        assert_success(r)
        sc = r.get("structuredContent", {})
        assert_has_key(sc, "result", "compare should return 'result' key")
        assert_true(isinstance(sc["result"], (int, float)), "compare result should be numeric")

    def test_subsystem_names_equal():
        r = client.call_tool("subsystem_names_equal",
                             {"name1": "Turret01", "name2": "turret01"})
        assert_success(r)
        sc = r.get("structuredContent", {})
        assert_has_key(sc, "equal", "equal should return 'equal' key")
        assert_true(sc["equal"] is True, "Turret01 should equal turret01 (case-insensitive)")

    def test_coordinate_transform():
        identity = {
            "rvec": {"x": 1, "y": 0, "z": 0},
            "uvec": {"x": 0, "y": 1, "z": 0},
            "fvec": {"x": 0, "y": 0, "z": 1}
        }
        r = client.call_tool("coordinate_transform", {
            "mode": "local_to_world",
            "reference_frame_orientation": identity,
            "position": {"x": 100, "y": 200, "z": 300},
        })
        assert_success(r)
        d = tool_data(r)
        # With identity orientation and zero reference position, output should match input
        if "position" in d:
            pos = d["position"]
            assert_true(abs(pos["x"] - 100) < 0.01, f"x: {pos['x']}")
            assert_true(abs(pos["y"] - 200) < 0.01, f"y: {pos['y']}")
            assert_true(abs(pos["z"] - 300) < 0.01, f"z: {pos['z']}")

    # Register all phase 2 tests
    tests = [
        ("list_ship_types", test_list_ship_types),
        ("get_ship_type", test_get_ship_type),
        ("list_ship_classes", test_list_ship_classes),
        ("list_ship_classes_filtered_species", test_list_ship_classes_filtered_species),
        ("list_ship_classes_filtered_type", test_list_ship_classes_filtered_type),
        ("get_ship_class", test_get_ship_class),
        ("get_ship_class_model_details", test_get_ship_class_model_details),
        ("list_weapon_classes", test_list_weapon_classes),
        ("list_weapon_classes_filtered", test_list_weapon_classes_filtered),
        ("get_weapon_class", test_get_weapon_class),
        ("list_species", test_list_species),
        ("list_iffs", test_list_iffs),
        ("get_iff", test_get_iff),
        ("list_intel_entries", test_list_intel_entries),
        ("get_intel_entry", test_get_intel_entry),
        ("list_persona_types", test_list_persona_types),
        ("list_personas", test_list_personas),
        ("list_talking_heads", test_list_talking_heads),
        ("list_fonts", test_list_fonts),
        ("list_sexp_categories", test_list_sexp_categories),
        ("list_sexp_operators", test_list_sexp_operators),
        ("list_sexp_operators_filtered_category", test_list_sexp_operators_filtered_category),
        ("list_sexp_operators_search", test_list_sexp_operators_search),
        ("get_sexp_operator", test_get_sexp_operator),
        ("get_sexp_argument_type_list", test_get_sexp_argument_type_list),
        ("get_sexp_argument_type", test_get_sexp_argument_type),
        ("list_sexp_argument_values", test_list_sexp_argument_values),
        ("get_sexp_return_type_list", test_get_sexp_return_type_list),
        ("get_sexp_return_type", test_get_sexp_return_type),
        ("list_reference_notes", test_list_reference_notes),
        ("list_reference_notes_search", test_list_reference_notes_search),
        ("get_reference_note", test_get_reference_note),
        ("list_scripting_elements", test_list_scripting_elements),
        ("list_scripting_elements_filtered", test_list_scripting_elements_filtered),
        ("get_scripting_element", test_get_scripting_element),
        ("search_scripting_children", test_search_scripting_children),
        ("search_scripting_children_typed", test_search_scripting_children_typed),
        ("list_scripting_hooks", test_list_scripting_hooks),
        ("list_scripting_hooks_filtered", test_list_scripting_hooks_filtered),
        ("list_scripting_enums", test_list_scripting_enums),
        ("get_scripting_misc_conditions", test_get_scripting_misc_conditions),
        ("get_scripting_misc_globalvars", test_get_scripting_misc_globalvars),
        ("get_mod_info", test_get_mod_info),
        ("list_missions", test_list_missions),
        ("get_root_paths", test_get_root_paths),
        ("subsystem_names_compare", test_subsystem_names_compare),
        ("subsystem_names_equal", test_subsystem_names_equal),
        ("coordinate_transform", test_coordinate_transform),
    ]
    for name, func in tests:
        suite.add(f"phase2_{name}", func, phase=2)


# ---------------------------------------------------------------------------
# Phase 3: Mission Setup
# ---------------------------------------------------------------------------

def make_phase3_tests(suite, client):

    def test_new_mission():
        r = client.call_tool("new_mission")
        assert_success(r)

    def test_save_mission():
        # Get a writable path from root_paths
        rp = client.call_tool("get_root_paths")
        assert_success(rp)
        paths = tool_data(rp)
        if not paths:
            raise SkipTest("No root paths available for save test")
        import os
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
        # Clean up: create a new mission so subsequent tests start fresh
        client.call_tool("new_mission")

    def test_get_mission_info_new():
        r = client.call_tool("get_mission_info")
        assert_success(r)
        d = tool_data(r)
        assert_is_dict(d)

    suite.add("phase3_new_mission", test_new_mission, phase=3)
    suite.add("phase3_save_mission", test_save_mission, phase=3)
    suite.add("phase3_load_mission", test_load_mission, phase=3)
    suite.add("phase3_get_mission_info", test_get_mission_info_new, phase=3)


# ---------------------------------------------------------------------------
# Phase 4: CRUD Lifecycle
# ---------------------------------------------------------------------------

def make_phase4_tests(suite, client):

    # ---- Messages ----

    def test_messages_crud():
        created = []
        try:
            # Create A
            r = client.call_tool("create_message", {
                "name": "Test Message A", "message": "Hello world"
            })
            assert_success(r)
            created.append("Test Message A")

            # Create B
            r = client.call_tool("create_message", {
                "name": "Test Message B", "message": "Goodbye world"
            })
            assert_success(r)
            created.append("Test Message B")

            # List
            r = client.call_tool("list_messages", {"source": "mission"})
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            names = [m.get("name") for m in d]
            assert_in("Test Message A", names)
            assert_in("Test Message B", names)

            # Get
            r = client.call_tool("get_message", {"name": "Test Message A"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("message"), "Hello world", "message text")

            # Update
            r = client.call_tool("update_message", {
                "name": "Test Message A",
                "new_name": "Test Msg A",
                "message": "Updated hello"
            })
            assert_success(r)
            created[0] = "Test Msg A"

            # Get after update
            r = client.call_tool("get_message", {"name": "Test Msg A"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("message"), "Updated hello", "updated message text")

            # Swap
            r = client.call_tool("swap_messages", {"index_a": 1, "index_b": 2})
            assert_success(r)

            # List after swap - verify order changed
            r = client.call_tool("list_messages", {"source": "mission"})
            assert_success(r)
            d = tool_data(r)
            names = [m.get("name") for m in d]
            assert_equal(names[0], "Test Message B", "first after swap")

            # Move
            r = client.call_tool("move_message", {"from_index": 2, "to_index": 1})
            assert_success(r)

        finally:
            # Cleanup
            for name in created:
                try:
                    client.call_tool("delete_message", {"name": name, "force": True})
                except Exception:
                    pass

            # Verify empty
            r = client.call_tool("list_messages", {"source": "mission"})
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            assert_equal(len(d), 0, "messages should be empty after cleanup")

    # ---- Events ----

    def test_events_crud():
        created = []
        try:
            r = client.call_tool("create_event", {"name": "Test Event A"})
            assert_success(r)
            created.append("Test Event A")

            r = client.call_tool("create_event", {
                "name": "Test Event B",
                "repeat_count": 3,
                "score": 10,
                "objective_text": "Test objective"
            })
            assert_success(r)
            created.append("Test Event B")

            # List
            r = client.call_tool("list_events")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            names = [e.get("name") for e in d]
            assert_in("Test Event A", names)
            assert_in("Test Event B", names)

            # Get
            r = client.call_tool("get_event", {"name": "Test Event B"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("repeat_count"), 3, "repeat_count")
            assert_equal(d.get("score"), 10, "score")

            # Update
            r = client.call_tool("update_event", {
                "name": "Test Event A",
                "new_name": "Test Evt A",
                "score": 50
            })
            assert_success(r)
            created[0] = "Test Evt A"

            # Get after update
            r = client.call_tool("get_event", {"name": "Test Evt A"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("score"), 50, "updated score")

            # Swap
            r = client.call_tool("swap_events", {"index_a": 1, "index_b": 2})
            assert_success(r)

            # Move
            r = client.call_tool("move_event", {"from_index": 2, "to_index": 1})
            assert_success(r)

        finally:
            for name in created:
                try:
                    client.call_tool("delete_event", {"name": name, "force": True})
                except Exception:
                    pass

            r = client.call_tool("list_events")
            assert_success(r)
            d = tool_data(r)
            assert_equal(len(d), 0, "events should be empty")

    # ---- Goals ----

    def test_goals_crud():
        created = []
        try:
            r = client.call_tool("create_goal", {
                "name": "Test Goal A", "goal_type": "Primary"
            })
            assert_success(r)
            created.append("Test Goal A")

            r = client.call_tool("create_goal", {
                "name": "Test Goal B", "goal_type": "Secondary", "score": 25
            })
            assert_success(r)
            created.append("Test Goal B")

            # List
            r = client.call_tool("list_goals")
            assert_success(r)
            d = tool_data(r)
            names = [g.get("name") for g in d]
            assert_in("Test Goal A", names)

            # Get
            r = client.call_tool("get_goal", {"name": "Test Goal B"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("score"), 25, "goal score")

            # Update
            r = client.call_tool("update_goal", {
                "name": "Test Goal A",
                "new_name": "Test Gl A",
                "goal_type": "Bonus",
                "score": 100
            })
            assert_success(r)
            created[0] = "Test Gl A"

            # Get verify
            r = client.call_tool("get_goal", {"name": "Test Gl A"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("score"), 100, "updated goal score")

            # Swap
            r = client.call_tool("swap_goals", {"index_a": 1, "index_b": 2})
            assert_success(r)

            # Move
            r = client.call_tool("move_goal", {"from_index": 2, "to_index": 1})
            assert_success(r)

        finally:
            for name in created:
                try:
                    client.call_tool("delete_goal", {"name": name, "force": True})
                except Exception:
                    pass

            r = client.call_tool("list_goals")
            assert_success(r)
            d = tool_data(r)
            assert_equal(len(d), 0, "goals should be empty")

    # ---- Command Briefing Stages ----

    def test_cmd_brief_stages_crud():
        try:
            r = client.call_tool("create_cmd_brief_stage", {
                "text": "Stage 1: Approach the target"
            })
            assert_success(r)

            r = client.call_tool("create_cmd_brief_stage", {
                "text": "Stage 2: Engage hostiles"
            })
            assert_success(r)

            # List
            r = client.call_tool("list_cmd_brief_stages")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            assert_true(len(d) >= 2, f"Expected at least 2 stages, got {len(d)}")

            # Get
            r = client.call_tool("get_cmd_brief_stage", {"index": 1})
            assert_success(r)
            d = tool_data(r)
            assert_in("Approach", d.get("text", ""), "stage text")

            # Update
            r = client.call_tool("update_cmd_brief_stage", {
                "index": 1, "text": "Stage 1: Updated approach"
            })
            assert_success(r)

            # Verify update
            r = client.call_tool("get_cmd_brief_stage", {"index": 1})
            assert_success(r)
            d = tool_data(r)
            assert_in("Updated", d.get("text", ""), "updated stage text")

            # Swap
            r = client.call_tool("swap_cmd_brief_stages", {"index_a": 1, "index_b": 2})
            assert_success(r)

            # Move
            r = client.call_tool("move_cmd_brief_stage", {"from_index": 2, "to_index": 1})
            assert_success(r)

        finally:
            # Delete stages in reverse order
            for _ in range(5):  # safety bound
                try:
                    r = client.call_tool("list_cmd_brief_stages")
                    d = tool_data(r)
                    if not d:
                        break
                    client.call_tool("delete_cmd_brief_stage", {"index": len(d)})
                except Exception:
                    break

    # ---- Fiction Viewer Stages ----

    def test_fiction_viewer_stages_crud():
        try:
            r = client.call_tool("create_fiction_viewer_stage", {
                "story_filename": "test_story_a.txt"
            })
            assert_success(r)

            r = client.call_tool("create_fiction_viewer_stage", {
                "story_filename": "test_story_b.txt"
            })
            assert_success(r)

            # List
            r = client.call_tool("list_fiction_viewer_stages")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            assert_true(len(d) >= 2, f"Expected at least 2 stages, got {len(d)}")

            # Get
            r = client.call_tool("get_fiction_viewer_stage", {"index": 1})
            assert_success(r)

            # Update
            r = client.call_tool("update_fiction_viewer_stage", {
                "index": 1, "story_filename": "test_story_updated.txt"
            })
            assert_success(r)

            # Swap
            r = client.call_tool("swap_fiction_viewer_stages", {"index_a": 1, "index_b": 2})
            assert_success(r)

            # Move
            r = client.call_tool("move_fiction_viewer_stage", {"from_index": 2, "to_index": 1})
            assert_success(r)

        finally:
            for _ in range(5):
                try:
                    r = client.call_tool("list_fiction_viewer_stages")
                    d = tool_data(r)
                    if not d:
                        break
                    client.call_tool("delete_fiction_viewer_stage", {"index": len(d)})
                except Exception:
                    break

    # ---- Debriefing Stages ----

    def test_debriefing_stages_crud():
        try:
            r = client.call_tool("create_debriefing_stage", {
                "text": "Debrief stage 1: Mission success"
            })
            assert_success(r)

            r = client.call_tool("create_debriefing_stage", {
                "text": "Debrief stage 2: Mission failure",
                "recommendation_text": "Try harder next time"
            })
            assert_success(r)

            # List
            r = client.call_tool("list_debriefing_stages")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            assert_true(len(d) >= 2, f"Expected at least 2 stages, got {len(d)}")

            # Get
            r = client.call_tool("get_debriefing_stage", {"index": 1})
            assert_success(r)
            d = tool_data(r)
            assert_in("Mission success", d.get("text", ""), "debrief text")

            # Update
            r = client.call_tool("update_debriefing_stage", {
                "index": 2,
                "recommendation_text": "Updated recommendation"
            })
            assert_success(r)

            # Verify
            r = client.call_tool("get_debriefing_stage", {"index": 2})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("recommendation_text"), "Updated recommendation")

            # Swap
            r = client.call_tool("swap_debriefing_stages", {"index_a": 1, "index_b": 2})
            assert_success(r)

            # Move
            r = client.call_tool("move_debriefing_stage", {"from_index": 2, "to_index": 1})
            assert_success(r)

        finally:
            for _ in range(5):
                try:
                    r = client.call_tool("list_debriefing_stages")
                    d = tool_data(r)
                    if not d:
                        break
                    client.call_tool("delete_debriefing_stage", {"index": len(d)})
                except Exception:
                    break

    # ---- Jump Nodes ----

    def test_jump_nodes_crud():
        created = []
        try:
            r = client.call_tool("create_jump_node", {
                "name": "Test Node A",
                "position": {"x": 0, "y": 0, "z": 0}
            })
            assert_success(r)
            created.append("Test Node A")

            r = client.call_tool("create_jump_node", {
                "name": "Test Node B",
                "position": {"x": 100, "y": 200, "z": 300},
                "hidden": True
            })
            assert_success(r)
            created.append("Test Node B")

            # List
            r = client.call_tool("list_jump_nodes")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            names = [n.get("name") for n in d]
            assert_in("Test Node A", names)

            # Get
            r = client.call_tool("get_jump_node", {"name": "Test Node B"})
            assert_success(r)
            d = tool_data(r)

            # Update (if available)
            if "update_jump_node" in ctx.get("tool_names", set()):
                r = client.call_tool("update_jump_node", {
                    "name": "Test Node A",
                    "new_name": "Test Nd A",
                    "position": {"x": 50, "y": 50, "z": 50},
                    "display_name": "Updated Node"
                })
                assert_success(r)
                created[0] = "Test Nd A"

                # Verify update
                r = client.call_tool("get_jump_node", {"name": "Test Nd A"})
                assert_success(r)

            # Swap
            r = client.call_tool("swap_jump_nodes", {"index_a": 1, "index_b": 2})
            assert_success(r)

            # Move
            r = client.call_tool("move_jump_node", {"from_index": 2, "to_index": 1})
            assert_success(r)

        finally:
            for name in created:
                try:
                    client.call_tool("delete_jump_node", {"name": name, "force": True})
                except Exception:
                    pass

            r = client.call_tool("list_jump_nodes")
            assert_success(r)
            d = tool_data(r)
            assert_equal(len(d), 0, "jump nodes should be empty")

    # ---- Waypoint Lists + Waypoints ----

    def test_waypoints_crud():
        created_lists = []
        try:
            # Create list A with 2 points
            r = client.call_tool("create_waypoint_list", {
                "name": "Test WPL A",
                "points": [{"x": 0, "y": 0, "z": 0}, {"x": 100, "y": 0, "z": 0}]
            })
            assert_success(r)
            created_lists.append("Test WPL A")

            # Create list B with 1 point
            r = client.call_tool("create_waypoint_list", {
                "name": "Test WPL B",
                "points": [{"x": 0, "y": 100, "z": 0}]
            })
            assert_success(r)
            created_lists.append("Test WPL B")

            # List waypoint lists
            r = client.call_tool("list_waypoint_lists")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            names = [w.get("name") for w in d]
            assert_in("Test WPL A", names)

            # Get waypoint list
            r = client.call_tool("get_waypoint_list", {"name": "Test WPL A"})
            assert_success(r)
            d = tool_data(r)
            # Should have waypoints
            wps = d.get("waypoints", d.get("points", []))
            assert_true(len(wps) >= 2, "Expected at least 2 waypoints")

            # Update (rename)
            r = client.call_tool("update_waypoint_list", {
                "name": "Test WPL A",
                "new_name": "Test WPL Alpha"
            })
            assert_success(r)
            created_lists[0] = "Test WPL Alpha"

            # Create waypoint in list
            r = client.call_tool("create_waypoint", {
                "list": "Test WPL Alpha",
                "position": {"x": 200, "y": 0, "z": 0}
            })
            assert_success(r)

            # Update waypoint
            r = client.call_tool("update_waypoint", {
                "list": "Test WPL Alpha",
                "index": 3,
                "position": {"x": 300, "y": 0, "z": 0}
            })
            assert_success(r)

            # Swap waypoints within list
            r = client.call_tool("swap_waypoints", {
                "list": "Test WPL Alpha",
                "index_a": 1,
                "index_b": 2
            })
            assert_success(r)

            # Move waypoint within list
            r = client.call_tool("move_waypoint", {
                "list": "Test WPL Alpha",
                "from_index": 2,
                "to_index": 1
            })
            assert_success(r)

            # Delete a waypoint
            r = client.call_tool("delete_waypoint", {
                "list": "Test WPL Alpha",
                "index": 3
            })
            assert_success(r)

            # Swap lists
            r = client.call_tool("swap_waypoint_lists", {"index_a": 1, "index_b": 2})
            assert_success(r)

            # Move list
            r = client.call_tool("move_waypoint_list", {"from_index": 2, "to_index": 1})
            assert_success(r)

        finally:
            for name in created_lists:
                try:
                    client.call_tool("delete_waypoint_list", {"name": name, "force": True})
                except Exception:
                    pass

            r = client.call_tool("list_waypoint_lists")
            assert_success(r)
            d = tool_data(r)
            assert_equal(len(d), 0, "waypoint lists should be empty")

    # ---- SEXP Variables ----

    def test_sexp_variables_crud():
        created = []
        try:
            r = client.call_tool("create_sexp_variable", {
                "name": "test_var_a",
                "default_value": "0",
                "variable_type": "number"
            })
            assert_success(r)
            created.append("test_var_a")

            r = client.call_tool("create_sexp_variable", {
                "name": "test_var_b",
                "default_value": "hello",
                "variable_type": "string"
            })
            assert_success(r)
            created.append("test_var_b")

            # List
            r = client.call_tool("list_sexp_variables")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            names = [v.get("name") for v in d]
            assert_in("test_var_a", names)
            assert_in("test_var_b", names)

            # Get
            r = client.call_tool("get_sexp_variable", {"name": "test_var_a"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("default_value"), "0", "default value")

            # Update
            r = client.call_tool("update_sexp_variable", {
                "name": "test_var_a",
                "new_name": "test_var_alpha",
                "default_value": "42"
            })
            assert_success(r)
            created[0] = "test_var_alpha"

            # Verify
            r = client.call_tool("get_sexp_variable", {"name": "test_var_alpha"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("default_value"), "42", "updated value")

        finally:
            for name in created:
                try:
                    client.call_tool("delete_sexp_variable", {"name": name, "force": True})
                except Exception:
                    pass

            r = client.call_tool("list_sexp_variables")
            assert_success(r)
            d = tool_data(r)
            assert_equal(len(d), 0, "sexp variables should be empty")

    # Register CRUD tests
    suite.add("phase4_messages_crud", test_messages_crud, phase=4)
    suite.add("phase4_events_crud", test_events_crud, phase=4)
    suite.add("phase4_goals_crud", test_goals_crud, phase=4)
    suite.add("phase4_cmd_brief_stages_crud", test_cmd_brief_stages_crud, phase=4)
    suite.add("phase4_fiction_viewer_stages_crud", test_fiction_viewer_stages_crud, phase=4)
    suite.add("phase4_debriefing_stages_crud", test_debriefing_stages_crud, phase=4)
    suite.add("phase4_jump_nodes_crud", test_jump_nodes_crud, phase=4)
    suite.add("phase4_waypoints_crud", test_waypoints_crud, phase=4)
    suite.add("phase4_sexp_variables_crud", test_sexp_variables_crud, phase=4)


# ---------------------------------------------------------------------------
# Phase 5: SEXP Tree Operations
# ---------------------------------------------------------------------------

def make_phase5_tests(suite, client):

    def test_text_to_sexp_simple():
        # Use do-nothing instead of true, since true is a locked singleton
        r = client.call_tool("text_to_sexp", {"text": "( do-nothing )"})
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "node")
        ctx["sexp_simple_node"] = d["node"]

    def test_sexp_to_text_simple():
        node = ctx.get("sexp_simple_node")
        if node is None:
            raise SkipTest("No sexp node from text_to_sexp")
        r = client.call_tool("sexp_to_text", {"node": node})
        assert_success(r)
        text = tool_text(r).lower()
        assert_in("do-nothing", text, "round-trip text should contain 'do-nothing'")

    def test_get_sexp_node_simple():
        node = ctx.get("sexp_simple_node")
        if node is None:
            raise SkipTest("No sexp node")
        r = client.call_tool("get_sexp_node", {"node": node})
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "value")

    def test_walk_sexp_tree_simple():
        node = ctx.get("sexp_simple_node")
        if node is None:
            raise SkipTest("No sexp node")
        r = client.call_tool("walk_sexp_tree", {"node": node})
        assert_success(r)
        d = tool_data(r)
        # Should be a list or have nodes
        if isinstance(d, list):
            assert_true(len(d) > 0, "walk should return nodes")
        elif isinstance(d, dict) and "nodes" in d:
            assert_true(len(d["nodes"]) > 0, "walk should return nodes")

    def test_detach_sexp_node_simple():
        node = ctx.get("sexp_simple_node")
        if node is None:
            raise SkipTest("No sexp node")
        r = client.call_tool("detach_sexp_node", {"node": node})
        assert_success(r)
        ctx.pop("sexp_simple_node", None)

    def test_text_to_sexp_complex():
        r = client.call_tool("text_to_sexp", {
            "text": "( when ( true ) ( do-nothing ) )"
        })
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "node")
        ctx["sexp_complex_node"] = d["node"]

    def test_walk_sexp_tree_depth():
        node = ctx.get("sexp_complex_node")
        if node is None:
            raise SkipTest("No complex sexp node")
        r = client.call_tool("walk_sexp_tree", {"node": node, "depth": 1})
        assert_success(r)

    def test_sexp_to_text_complex():
        node = ctx.get("sexp_complex_node")
        if node is None:
            raise SkipTest("No complex sexp node")
        r = client.call_tool("sexp_to_text", {"node": node})
        assert_success(r)
        text = tool_text(r).lower()
        assert_in("when", text)

    def test_detach_sexp_node_complex():
        node = ctx.get("sexp_complex_node")
        if node is None:
            raise SkipTest("No complex sexp node")
        r = client.call_tool("detach_sexp_node", {"node": node})
        assert_success(r)
        ctx.pop("sexp_complex_node", None)

    def test_create_sexp_node_simple():
        # Use do-nothing (not true/false which are locked singletons)
        r = client.call_tool("create_sexp_node", {"role": "operator", "operator_name": "do-nothing"})
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "node")
        ctx["sexp_created_node"] = d["node"]

    def test_get_created_sexp_node():
        node = ctx.get("sexp_created_node")
        if node is None:
            raise SkipTest("No created sexp node")
        r = client.call_tool("get_sexp_node", {"node": node})
        assert_success(r)

    def test_sexp_to_text_created():
        node = ctx.get("sexp_created_node")
        if node is None:
            raise SkipTest("No created sexp node")
        r = client.call_tool("sexp_to_text", {"node": node})
        assert_success(r)
        text = tool_text(r).lower()
        assert_in("do-nothing", text)

    def test_detach_sexp_node_created():
        node = ctx.get("sexp_created_node")
        if node is None:
            raise SkipTest("No created sexp node")
        r = client.call_tool("detach_sexp_node", {"node": node})
        assert_success(r)
        ctx.pop("sexp_created_node", None)

    def test_create_sexp_node_with_args():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"}
            ]
        })
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "node")
        ctx["sexp_args_node"] = d["node"]

    def test_sexp_to_text_args():
        node = ctx.get("sexp_args_node")
        if node is None:
            raise SkipTest("No sexp args node")
        r = client.call_tool("sexp_to_text", {"node": node})
        assert_success(r)

    def test_detach_sexp_node_args():
        node = ctx.get("sexp_args_node")
        if node is None:
            raise SkipTest("No sexp args node")
        r = client.call_tool("detach_sexp_node", {"node": node})
        assert_success(r)
        ctx.pop("sexp_args_node", None)

    def test_create_sexp_node_with_node_arg():
        """Create a tree that composes sub-expressions via 'node' arguments."""
        # Create a sub-expression: (+ 1 2)
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"}
            ]
        })
        assert_success(r)
        sub_node = tool_data(r)["node"]
        ctx["sexp_sub_node"] = sub_node

        # Compose into: (> (+ 1 2) 0)
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": ">",
            "operator_arguments": [
                {"argument_type": "node", "argument_value": str(sub_node)},
                {"argument_type": "number", "argument_value": "0"}
            ]
        })
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "node")
        ctx["sexp_composed_node"] = d["node"]

    def test_sexp_to_text_composed():
        node = ctx.get("sexp_composed_node")
        if node is None:
            raise SkipTest("No composed sexp node")
        r = client.call_tool("sexp_to_text", {"node": node})
        assert_success(r)

    def test_detach_sexp_node_composed():
        node = ctx.get("sexp_composed_node")
        if node is None:
            raise SkipTest("No composed sexp node")
        r = client.call_tool("detach_sexp_node", {"node": node, "delete": True})
        assert_success(r)
        ctx.pop("sexp_composed_node", None)
        ctx.pop("sexp_sub_node", None)

    def test_create_sexp_node_non_root_rejected():
        """Node args that reference non-root nodes must be rejected."""
        # Create a tree: (+ 1 2) -- the "1" argument is not a root
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"}
            ]
        })
        assert_success(r)
        op_node = tool_data(r)["node"]
        ctx["sexp_nonroot_tree"] = op_node

        # Walk the tree to find a non-root argument node
        r = client.call_tool("walk_sexp_tree", {"node": op_node})
        assert_success(r)
        d = tool_data(r)
        nodes = d["nodes"] if isinstance(d, dict) and "nodes" in d else d
        non_root = None
        for n in nodes:
            if n.get("role") == "argument":
                non_root = n["node"]
                break
        assert_true(non_root is not None, "Should have found an argument node")

        # Try to use the non-root node as a node argument -- should fail
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": ">",
            "operator_arguments": [
                {"argument_type": "node", "argument_value": str(non_root)},
                {"argument_type": "number", "argument_value": "0"}
            ]
        })
        assert_error(r)
        assert_in("not the root of its own tree", tool_text(r))

    def test_cleanup_nonroot_tree():
        node = ctx.get("sexp_nonroot_tree")
        if node is None:
            raise SkipTest("No nonroot tree")
        r = client.call_tool("detach_sexp_node", {"node": node, "delete": True})
        assert_success(r)
        ctx.pop("sexp_nonroot_tree", None)

    def test_create_sexp_node_type_error_preserves_referenced_tree():
        """When a type error rolls back create_sexp_node, referenced node trees survive."""
        # Create a sub-expression: (+ 1 2) -- returns number
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "1"},
                {"argument_type": "number", "argument_value": "2"}
            ]
        })
        assert_success(r)
        sub_node = tool_data(r)["node"]
        ctx["sexp_preserved_node"] = sub_node

        # Try to use it in an incompatible position: is-destroyed expects a ship
        # name (string), not a number-returning sub-expression
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "is-destroyed",
            "operator_arguments": [
                {"argument_type": "node", "argument_value": str(sub_node)}
            ]
        })
        assert_error(r)

        # The referenced sub-expression must still be intact
        r = client.call_tool("get_sexp_node", {"node": sub_node})
        assert_success(r)
        d = tool_data(r)
        assert_equal(d.get("role"), "operator", "referenced node role")
        assert_equal(d.get("value"), "+", "referenced node value")

    def test_cleanup_preserved_tree():
        node = ctx.get("sexp_preserved_node")
        if node is None:
            raise SkipTest("No preserved tree")
        r = client.call_tool("detach_sexp_node", {"node": node, "delete": True})
        assert_success(r)
        ctx.pop("sexp_preserved_node", None)

    def test_detach_shrink_rollback_restores_last_sibling():
        """Bug regression: shrink-mode rollback fails when target_rest == -1.

        When detach_sexp_node is called with shrink=true on the last sibling of
        an attached formula's operator, removing that sibling violates the
        operator's min argument count, the syntax check fails, and the function
        is supposed to roll back. In the buggy version the rollback uses
        splice_replace_node(target_rest, n) where target_rest is -1 -- a no-op
        -- so the predecessor's rest pointer is left at -1 and the original
        last sibling is permanently orphaned even though the API returns an
        error claiming a clean rollback.
        """
        # Default event formula is `(when (true) (do-nothing))`. `when` requires
        # at least 2 args (cond + 1 action), so removing `do-nothing` violates min.
        r = client.call_tool("create_event", {"name": "test_shrink_rollback"})
        assert_success(r)
        evt_d = tool_data(r)
        formula_root = evt_d.get("formula")
        assert_true(formula_root is not None and formula_root >= 0,
                    f"event should have a formula root, got {formula_root}")

        try:
            # Locate the do-nothing operator atom (the last argument of `when`).
            r = client.call_tool("walk_sexp_tree", {"node": formula_root})
            assert_success(r)
            nodes = tool_data(r).get("nodes", [])
            do_nothing = None
            for n in nodes:
                if n.get("value") == "do-nothing" and n.get("role") == "operator":
                    do_nothing = n["node"]
                    break
            assert_true(do_nothing is not None,
                        f"could not find do-nothing operator in default event formula; "
                        f"walked {[(n.get('value'), n.get('role')) for n in nodes]}")

            # Shrink-detach the last action. The retarget logic moves to its
            # enclosing list wrapper, whose .rest is -1, exercising the bug path.
            # The syntax check on `when` should fail and the operation should
            # roll back, leaving the formula tree exactly as it was.
            r = client.call_tool("detach_sexp_node",
                                 {"node": do_nothing, "shrink": True})
            assert_error(r)

            # The rollback must have restored do-nothing in the formula tree.
            # In the buggy version, do-nothing is orphaned and walk_sexp_tree
            # cannot reach it from the formula root.
            r = client.call_tool("walk_sexp_tree", {"node": formula_root})
            assert_success(r)
            nodes = tool_data(r).get("nodes", [])
            values = [n.get("value") for n in nodes]
            assert_in("do-nothing", values,
                      f"shrink-mode rollback failed to restore do-nothing in the "
                      f"event formula; reachable nodes after failed detach = {values}")
        finally:
            client.call_tool("delete_event",
                             {"name": "test_shrink_rollback", "force": True})

    def test_detach_unwrap_preserves_child_parent_pointers():
        """Bug regression: unwrapping after detach leaves children with stale parents.

        When the user passes an operator atom to detach_sexp_node, the handler
        retargets to the enclosing list wrapper, processes the detach, and then
        (on the !do_delete path) frees the wrapper and resets ONLY the operator
        atom's parent pointer back to -1. But for any wrapper allocated by the
        SEXP parser, alloc_sexp set the entire rest chain's parent fields to
        the wrapper, so the operator atom's siblings (its arguments) all still
        point at the now-freed wrapper slot. The detached free-standing subtree
        is then internally inconsistent.

        This only manifests when the operator actually has an enclosing wrapper,
        which in parser-produced trees happens for *nested* sub-expressions,
        not top-level operators. So the test nests `when` inside `and`.
        """
        r = client.call_tool("text_to_sexp", {
            "text": "( and ( when ( true ) ( do-nothing ) ) ( true ) )"
        })
        assert_success(r)
        formula_root = tool_data(r)["node"]

        when_op = None
        detached_root = None
        try:
            # Find the nested `when` operator atom. It sits inside a list
            # wrapper that was allocated via alloc_sexp with first=when_op,
            # so true_arg and do-nothing_arg have parent = that wrapper.
            r = client.call_tool("walk_sexp_tree", {"node": formula_root})
            assert_success(r)
            nodes = tool_data(r).get("nodes", [])
            for n in nodes:
                if n.get("value") == "when" and n.get("role") == "operator":
                    when_op = n["node"]
                    break
            assert_true(when_op is not None,
                        "could not find nested `when` operator")

            # Detach the `when` atom (preserve, not delete). The handler
            # retargets to the enclosing wrapper, splices it out of `and`'s
            # rest chain, then unwraps back to the operator atom.
            r = client.call_tool("detach_sexp_node", {"node": when_op})
            assert_success(r)
            d = tool_data(r)
            detached_root = d.get("detached_node")
            assert_equal(detached_root, when_op,
                         "preserved detach should report the operator atom index")

            # Walk the new free-standing subtree and check a sibling's parent.
            # The proper parent is detached_root (the new root after unwrap).
            # In the buggy version, parent still points at the freed wrapper.
            r = client.call_tool("walk_sexp_tree", {"node": detached_root})
            assert_success(r)
            nodes = tool_data(r).get("nodes", [])
            sibling = None
            for n in nodes:
                if n["node"] != detached_root and n.get("role") == "list_wrapper":
                    sibling = n
                    break
            assert_true(sibling is not None,
                        f"could not find a sibling list_wrapper under {detached_root}; "
                        f"walked {[(n['node'], n.get('role')) for n in nodes]}")

            parent = sibling.get("node_parent")
            assert_equal(parent, detached_root,
                         f"sibling node {sibling['node']} should have parent="
                         f"{detached_root} (the new root after unwrap); got "
                         f"parent={parent}, which points to a freed wrapper slot")
        finally:
            # Clean up both the residual `and` tree and the detached `when`.
            if detached_root is not None:
                client.call_tool("detach_sexp_node",
                                 {"node": detached_root, "delete": True})
            client.call_tool("detach_sexp_node",
                             {"node": formula_root, "delete": True})

    def test_detach_reports_consistent_node_when_retargeted():
        """Bug regression: detached_node response differs between delete=true and delete=false.

        When the user passes an operator atom that gets retargeted to its
        enclosing wrapper, the !do_delete path reverts `n` back to the
        original operator atom before building the response, so the user
        sees the index they passed in. The do_delete path skips that revert,
        so the response reports the (now-freed) wrapper index instead -- a
        different number than the user's input.

        As with the parent-pointer test, the retargeting only fires when the
        operator actually has an enclosing wrapper, which means the target
        `when` must be a nested sub-expression.
        """
        def build_and_find_nested_when():
            r = client.call_tool("text_to_sexp", {
                "text": "( and ( when ( true ) ( do-nothing ) ) ( true ) )"
            })
            assert_success(r)
            root = tool_data(r)["node"]
            r = client.call_tool("walk_sexp_tree", {"node": root})
            assert_success(r)
            for n in tool_data(r).get("nodes", []):
                if n.get("value") == "when" and n.get("role") == "operator":
                    return root, n["node"]
            raise AssertionError("could not find nested `when` operator")

        # delete=false case: report the operator atom index via the unwrap path.
        root_preserved, when_op_preserved = build_and_find_nested_when()
        detached_preserved = None
        try:
            r = client.call_tool("detach_sexp_node",
                                 {"node": when_op_preserved, "delete": False})
            assert_success(r)
            preserved_reported = tool_data(r).get("detached_node")
            detached_preserved = preserved_reported
        finally:
            if detached_preserved is not None:
                client.call_tool("detach_sexp_node",
                                 {"node": detached_preserved, "delete": True})
            client.call_tool("detach_sexp_node",
                             {"node": root_preserved, "delete": True})

        # delete=true case: should report the same operator atom index.
        root_deleted, when_op_deleted = build_and_find_nested_when()
        try:
            r = client.call_tool("detach_sexp_node",
                                 {"node": when_op_deleted, "delete": True})
            assert_success(r)
            deleted_reported = tool_data(r).get("detached_node")
        finally:
            client.call_tool("detach_sexp_node",
                             {"node": root_deleted, "delete": True})

        # Both calls passed an operator atom; both should report it back.
        assert_equal(preserved_reported, when_op_preserved,
                     "delete=false should report the operator atom index")
        assert_equal(deleted_reported, when_op_deleted,
                     "delete=true should report the operator atom index "
                     "(buggy version reports the wrapper index instead)")

    tests = [
        ("text_to_sexp_simple", test_text_to_sexp_simple),
        ("sexp_to_text_simple", test_sexp_to_text_simple),
        ("get_sexp_node_simple", test_get_sexp_node_simple),
        ("walk_sexp_tree_simple", test_walk_sexp_tree_simple),
        ("detach_sexp_node_simple", test_detach_sexp_node_simple),
        ("text_to_sexp_complex", test_text_to_sexp_complex),
        ("walk_sexp_tree_depth", test_walk_sexp_tree_depth),
        ("sexp_to_text_complex", test_sexp_to_text_complex),
        ("detach_sexp_node_complex", test_detach_sexp_node_complex),
        ("create_sexp_node_simple", test_create_sexp_node_simple),
        ("get_created_sexp_node", test_get_created_sexp_node),
        ("sexp_to_text_created", test_sexp_to_text_created),
        ("detach_sexp_node_created", test_detach_sexp_node_created),
        ("create_sexp_node_with_args", test_create_sexp_node_with_args),
        ("sexp_to_text_args", test_sexp_to_text_args),
        ("detach_sexp_node_args", test_detach_sexp_node_args),
        ("create_sexp_node_with_node_arg", test_create_sexp_node_with_node_arg),
        ("sexp_to_text_composed", test_sexp_to_text_composed),
        ("detach_sexp_node_composed", test_detach_sexp_node_composed),
        ("create_sexp_node_non_root_rejected", test_create_sexp_node_non_root_rejected),
        ("cleanup_nonroot_tree", test_cleanup_nonroot_tree),
        ("create_sexp_node_type_error_preserves_referenced_tree", test_create_sexp_node_type_error_preserves_referenced_tree),
        ("cleanup_preserved_tree", test_cleanup_preserved_tree),
        ("detach_shrink_rollback_restores_last_sibling", test_detach_shrink_rollback_restores_last_sibling),
        ("detach_unwrap_preserves_child_parent_pointers", test_detach_unwrap_preserves_child_parent_pointers),
        ("detach_reports_consistent_node_when_retargeted", test_detach_reports_consistent_node_when_retargeted),
    ]
    for name, func in tests:
        suite.add(f"phase5_{name}", func, phase=5)


# ---------------------------------------------------------------------------
# Phase 6: Negative/Error Tests
# ---------------------------------------------------------------------------

def make_phase6_tests(suite, client):

    def test_unknown_tool():
        try:
            r = client.call_tool("nonexistent_tool_xyz")
            assert_error(r)
        except MCPError:
            pass  # JSON-RPC level error is also acceptable

    def test_get_message_missing_param():
        r = client.call_tool("get_message", {})
        assert_error(r)

    def test_get_message_nonexistent():
        r = client.call_tool("get_message", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    def test_set_timeout_too_low():
        r = client.call_tool("set_timeout", {"seconds": 0})
        assert_error(r)

    def test_set_timeout_too_high():
        r = client.call_tool("set_timeout", {"seconds": 999})
        assert_error(r)

    def test_create_message_duplicate():
        # Create, then try to create duplicate
        try:
            r = client.call_tool("create_message", {
                "name": "DupTest", "message": "first"
            })
            assert_success(r)
            r = client.call_tool("create_message", {
                "name": "DupTest", "message": "second"
            })
            assert_error(r)
        finally:
            try:
                client.call_tool("delete_message", {"name": "DupTest", "force": True})
            except Exception:
                pass

    def test_delete_message_nonexistent():
        r = client.call_tool("delete_message", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    def test_create_jump_node_missing_position():
        r = client.call_tool("create_jump_node", {"name": "bad node"})
        assert_error(r)

    def test_text_to_sexp_invalid():
        r = client.call_tool("text_to_sexp", {"text": "((( invalid ))) garbage"})
        # May return error or success with parse errors
        # Just verify it doesn't crash and returns a response
        assert_true("content" in r, "Should return content")

    def test_create_sexp_node_invalid_operator():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "not_a_real_operator_xyz"
        })
        assert_error(r)

    def test_create_sexp_node_wrong_arg_type():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "string", "argument_value": "not_a_number"}
            ]
        })
        assert_error(r)

    def test_create_sexp_node_invalid_number_value():
        r = client.call_tool("create_sexp_node", {
            "role": "operator",
            "operator_name": "+",
            "operator_arguments": [
                {"argument_type": "number", "argument_value": "abc"}
            ]
        })
        assert_error(r)
        assert_in("not a valid number", tool_text(r))

    def test_create_sexp_node_argument_invalid_number():
        r = client.call_tool("create_sexp_node", {
            "role": "argument",
            "argument_type": "number",
            "argument_value": "not_numeric"
        })
        assert_error(r)
        assert_in("not a valid number", tool_text(r))

    def test_detach_sexp_node_invalid():
        r = client.call_tool("detach_sexp_node", {"node": -999})
        assert_error(r)

    def test_swap_messages_out_of_range():
        r = client.call_tool("swap_messages", {"index_a": 999, "index_b": 998})
        assert_error(r)

    def test_move_event_out_of_range():
        r = client.call_tool("move_event", {"from_index": 999, "to_index": 998})
        assert_error(r)

    def test_get_sexp_operator_nonexistent():
        r = client.call_tool("get_sexp_operator", {
            "name": "not_a_real_operator_xyz"
        })
        assert_error(r)

    def test_coordinate_transform_missing_params():
        r = client.call_tool("coordinate_transform", {"mode": "local_to_world"})
        assert_error(r)

    def test_get_scripting_misc_invalid_section():
        r = client.call_tool("get_scripting_misc", {"section": "invalid_section"})
        assert_error(r)

    # -- Additional get/delete error tests --

    def test_get_event_nonexistent():
        r = client.call_tool("get_event", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    def test_get_goal_nonexistent():
        r = client.call_tool("get_goal", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    def test_get_jump_node_nonexistent():
        r = client.call_tool("get_jump_node", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    def test_get_waypoint_list_nonexistent():
        r = client.call_tool("get_waypoint_list", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    def test_get_sexp_variable_nonexistent():
        r = client.call_tool("get_sexp_variable", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    def test_delete_event_nonexistent():
        r = client.call_tool("delete_event", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    def test_delete_goal_nonexistent():
        r = client.call_tool("delete_goal", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    def test_update_event_nonexistent():
        r = client.call_tool("update_event", {"name": "DOES_NOT_EXIST_XYZ", "score": 0})
        assert_error(r)

    def test_get_ship_class_nonexistent():
        r = client.call_tool("get_ship_class", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    def test_get_weapon_class_nonexistent():
        r = client.call_tool("get_weapon_class", {"name": "DOES_NOT_EXIST_XYZ"})
        assert_error(r)

    # -- Range validation tests (issue #12 fix) --

    def test_create_event_repeat_count_zero():
        r = client.call_tool("create_event", {"name": "RangeTest", "repeat_count": 0})
        assert_error(r)

    def test_create_event_interval_negative():
        r = client.call_tool("create_event", {"name": "RangeTest", "interval": -1})
        assert_error(r)

    def test_create_event_chain_delay_invalid():
        r = client.call_tool("create_event", {"name": "RangeTest", "chain_delay": -2})
        assert_error(r)

    tests = [
        ("unknown_tool", test_unknown_tool),
        ("get_message_missing_param", test_get_message_missing_param),
        ("get_message_nonexistent", test_get_message_nonexistent),
        ("set_timeout_too_low", test_set_timeout_too_low),
        ("set_timeout_too_high", test_set_timeout_too_high),
        ("create_message_duplicate", test_create_message_duplicate),
        ("delete_message_nonexistent", test_delete_message_nonexistent),
        ("create_jump_node_missing_position", test_create_jump_node_missing_position),
        ("text_to_sexp_invalid", test_text_to_sexp_invalid),
        ("create_sexp_node_invalid_operator", test_create_sexp_node_invalid_operator),
        ("create_sexp_node_wrong_arg_type", test_create_sexp_node_wrong_arg_type),
        ("create_sexp_node_invalid_number_value", test_create_sexp_node_invalid_number_value),
        ("create_sexp_node_argument_invalid_number", test_create_sexp_node_argument_invalid_number),
        ("detach_sexp_node_invalid", test_detach_sexp_node_invalid),
        ("swap_messages_out_of_range", test_swap_messages_out_of_range),
        ("move_event_out_of_range", test_move_event_out_of_range),
        ("get_sexp_operator_nonexistent", test_get_sexp_operator_nonexistent),
        ("coordinate_transform_missing_params", test_coordinate_transform_missing_params),
        ("get_scripting_misc_invalid_section", test_get_scripting_misc_invalid_section),
        ("get_event_nonexistent", test_get_event_nonexistent),
        ("get_goal_nonexistent", test_get_goal_nonexistent),
        ("get_jump_node_nonexistent", test_get_jump_node_nonexistent),
        ("get_waypoint_list_nonexistent", test_get_waypoint_list_nonexistent),
        ("get_sexp_variable_nonexistent", test_get_sexp_variable_nonexistent),
        ("delete_event_nonexistent", test_delete_event_nonexistent),
        ("delete_goal_nonexistent", test_delete_goal_nonexistent),
        ("update_event_nonexistent", test_update_event_nonexistent),
        ("get_ship_class_nonexistent", test_get_ship_class_nonexistent),
        ("get_weapon_class_nonexistent", test_get_weapon_class_nonexistent),
        ("create_event_repeat_count_zero", test_create_event_repeat_count_zero),
        ("create_event_interval_negative", test_create_event_interval_negative),
        ("create_event_chain_delay_invalid", test_create_event_chain_delay_invalid),
    ]
    for name, func in tests:
        suite.add(f"phase6_{name}", func, phase=6)


# ---------------------------------------------------------------------------
# Phase 7: Cleanup
# ---------------------------------------------------------------------------

def make_phase7_tests(suite, client):

    def test_cleanup_new_mission():
        r = client.call_tool("new_mission")
        assert_success(r)

    suite.add("phase7_cleanup", test_cleanup_new_mission, phase=7)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def wait_for_server(client, max_attempts=10, delay=2.0):
    """Wait for FRED2 MCP server to become available.

    Uses a lightweight HTTP probe rather than the full initialize handshake,
    so that phase 0's test_initialize performs the single spec-required
    initialize call.
    """
    for attempt in range(max_attempts):
        try:
            # Use a simple HTTP GET to the server root to check liveness.
            # The MCP server returns an HTML status page for non-POST requests.
            probe = urllib.request.Request(client.url, method="GET")
            with urllib.request.urlopen(probe, timeout=client.timeout) as resp:
                resp.read()
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


def main():
    parser = argparse.ArgumentParser(description="FRED2 MCP comprehensive test suite")
    parser.add_argument("--url", default="http://127.0.0.1:8080/mcp",
                        help="MCP server URL (default: http://127.0.0.1:8080/mcp)")
    parser.add_argument("--timeout", type=int, default=30,
                        help="Default HTTP timeout in seconds (default: 30)")
    parser.add_argument("--phase", type=int, default=None,
                        help="Run only a specific phase (0-7)")
    args = parser.parse_args()

    client = MCPClient(url=args.url, timeout=args.timeout)

    print("=" * 70)
    print("FRED2 MCP Comprehensive Test Suite")
    print(f"Server: {args.url}")
    print("=" * 70)

    # Wait for server
    print("\nConnecting to FRED2...")
    if not wait_for_server(client):
        sys.exit(2)
    print("  Connected!\n")

    suite = TestSuite()

    # Register all test phases
    make_phase0_tests(suite, client)
    make_phase1_tests(suite, client)
    make_phase2_tests(suite, client)
    make_phase3_tests(suite, client)
    make_phase4_tests(suite, client)
    make_phase5_tests(suite, client)
    make_phase6_tests(suite, client)
    make_phase7_tests(suite, client)

    if args.phase is not None:
        print(f"Running phase {args.phase} only\n")
        suite.run(phase_filter=args.phase)
    else:
        # Run all phases, with abort gates on phases 0 and 3
        phases = list(range(8))
        ok = suite.run_phases(phases, abort_on_phase={0, 3})
        if not ok:
            suite.summary()
            sys.exit(2)

    rc = suite.summary()
    sys.exit(rc)


if __name__ == "__main__":
    main()
