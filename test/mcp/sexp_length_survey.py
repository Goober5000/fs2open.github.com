"""Iterate every event in the loaded mission, call sexp_to_text for each
formula, and report the maximum SEXP text length found.

Usage:
    python sexp_length_survey.py [--url http://127.0.0.1:8080/mcp]
"""

import argparse
import json
import sys
import urllib.error
import urllib.request


# ---------------------------------------------------------------------------
# Minimal MCP client (no test-framework dependency)
# ---------------------------------------------------------------------------

class MCPClient:
    def __init__(self, url="http://127.0.0.1:8080/mcp", timeout=30):
        self.url = url
        self.timeout = timeout
        self._next_id = 1

    def call_tool(self, name, arguments=None):
        body = {
            "jsonrpc": "2.0",
            "id": self._next_id,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments or {}},
        }
        self._next_id += 1
        data = json.dumps(body).encode("utf-8")
        req = urllib.request.Request(
            self.url,
            data=data,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=self.timeout) as resp:
            result = json.loads(resp.read().decode("utf-8"))
        if "error" in result:
            raise RuntimeError(result["error"].get("message", "RPC error"))
        return result.get("result", {})


def tool_text(result):
    """Extract the text content from a tool result."""
    for item in result.get("content", []):
        if item.get("type") == "text":
            return item["text"]
    return ""


def tool_data(result):
    """Parse the JSON payload from a tool result."""
    text = tool_text(result)
    if not text:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {}


def is_error(result):
    return result.get("isError", False)


# ---------------------------------------------------------------------------
# Main survey
# ---------------------------------------------------------------------------

def run(client):
    # 1. List all events
    r = client.call_tool("list_events")
    if is_error(r):
        print(f"ERROR listing events: {tool_text(r)}", file=sys.stderr)
        sys.exit(1)

    events = tool_data(r)
    if not isinstance(events, list):
        events = events.get("events", [])
    if not events:
        print("No events in mission.")
        return

    print(f"Found {len(events)} event(s). Surveying SEXP lengths...\n")

    results = []
    errors = []

    for evt in events:
        name = evt.get("name", f"<event {evt.get('index', '?')}>")
        formula_node = evt.get("formula")

        if formula_node is None or formula_node < 0:
            errors.append(f"  {name}: no formula node")
            continue

        r2 = client.call_tool("sexp_to_text", {"node": formula_node})
        if is_error(r2):
            errors.append(f"  {name} (node {formula_node}): {tool_text(r2)}")
            continue

        sexp_text = tool_text(r2)
        length = len(sexp_text)
        results.append((length, name, formula_node, sexp_text))

    # Sort by length descending for display
    results.sort(reverse=True)

    # Print table
    col_w = max((len(r[1]) for r in results), default=4)
    header = f"{'Length':>8}  {'Node':>6}  {'Event'}"
    print(header)
    print("-" * (8 + 2 + 6 + 2 + col_w))
    for length, name, node, _ in results:
        print(f"{length:>8}  {node:>6}  {name}")

    if errors:
        print(f"\nSkipped {len(errors)} event(s) with errors:")
        for e in errors:
            print(e)

    if results:
        max_len, max_name, max_node, max_text = results[0]
        print(f"\nMaximum SEXP length: {max_len} characters")
        print(f"  Event : {max_name}")
        print(f"  Node  : {max_node}")
        preview = max_text[:200] + ("..." if len(max_text) > 200 else "")
        print(f"  Text  : {preview}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8080/mcp",
                        help="MCP server URL (default: http://127.0.0.1:8080/mcp)")
    args = parser.parse_args()

    client = MCPClient(url=args.url)
    try:
        run(client)
    except urllib.error.URLError as e:
        print(f"Could not connect to FRED2 MCP server at {args.url}: {e}",
              file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
