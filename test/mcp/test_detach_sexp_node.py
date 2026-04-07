"""Manual validation tests for detach_sexp_node."""
import json, urllib.request, sys

url = "http://127.0.0.1:8080/mcp"
_id = 1

def rpc(method, params=None):
    global _id
    body = {"jsonrpc": "2.0", "id": _id, "method": method}
    if params:
        body["params"] = params
    _id += 1
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read())

def call(name, args=None):
    params = {"name": name}
    if args:
        params["arguments"] = args
    return rpc("tools/call", params)

def tool_data(r):
    for c in r.get("result", {}).get("content", []):
        if c.get("type") == "text":
            try:
                return json.loads(c["text"])
            except Exception:
                pass
    return {}

def tool_text(r):
    for c in r.get("result", {}).get("content", []):
        if c.get("type") == "text":
            return c["text"]
    return ""

def is_error(r):
    return r.get("result", {}).get("isError", False)

passed = 0
failed = 0

def check(label, ok, detail=""):
    global passed, failed
    status = "PASS" if ok else "FAIL"
    msg = f"  [{status}] {label}"
    if detail:
        msg += f" -- {detail}"
    print(msg)
    if ok:
        passed += 1
    else:
        failed += 1

# Helpers for the new response format:
# - detached_node: always an int (node index)
# - detached_node_data: full node object (when not deleted)
# - replacement_node: int or null
# - replacement_node_data: full node object (when replacement exists)
def get_detached_data(d):
    """Get the detached node data object, or None."""
    return d.get("detached_node_data")

def get_replacement_data(d):
    """Get the replacement node data object, or None."""
    return d.get("replacement_node_data")

# Setup
call("new_mission", {"mission_name": "detach_sexp_test", "mission_title": "Test"})

# =====================================================================
print("\n=== Test 1: Detach+delete free-standing root (Case B) ===")
r = call("text_to_sexp", {"text": "( when ( true ) ( do-nothing ) )"})
node = tool_data(r)["node"]
r = call("detach_sexp_node", {"node": node, "delete": True})
d = tool_data(r)
check("Free-standing root freed, no replacement",
      not is_error(r) and d.get("detached_node") == node
      and d.get("deleted") is True and d.get("replacement_node") is None,
      json.dumps(d))

# =====================================================================
print("\n=== Test 2: Detach embedded node (default: preserved) ===")
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"},
        {"type": "number", "value": "3"}
    ]
})
root = tool_data(r)["node"]
r = call("walk_sexp_tree", {"node": root})
nodes = tool_data(r)["nodes"]
two_node = None
for n in nodes:
    if n.get("value") == "2":
        two_node = n["node"]
        break
r = call("detach_sexp_node", {"node": two_node})
d = tool_data(r)
repl_data = get_replacement_data(d)
repl_val = repl_data.get("value", "") if repl_data else ""
dn_data = get_detached_data(d)
check("Embedded node detached and preserved with placeholder",
      not is_error(r) and dn_data is not None and dn_data.get("node") == two_node
      and d.get("deleted") is False and repl_val == "<placeholder>",
      f"detached={dn_data}, replacement={repl_val}")
# Verify detached node is still accessible
r = call("get_sexp_node", {"node": two_node})
check("Detached node still accessible via get_sexp_node",
      not is_error(r),
      tool_text(r)[:80])
# Clean up
call("detach_sexp_node", {"node": root, "delete": True})
call("detach_sexp_node", {"node": two_node, "delete": True})

# =====================================================================
print("\n=== Test 3: Detach root of event formula (Case A, OPR_NULL) ===")
r = call("create_event", {"name": "test_evt_detach"})
d = tool_data(r)
evt_formula = d.get("formula")
r = call("detach_sexp_node", {"node": evt_formula})
d = tool_data(r)
repl_data = get_replacement_data(d)
repl_val = repl_data.get("value", "") if repl_data else ""
dn_data = get_detached_data(d)
check("Event formula replaced with do-nothing",
      not is_error(r) and dn_data is not None and dn_data.get("node") == evt_formula and repl_val == "do-nothing",
      f"replacement={repl_val}")
# Verify event updated
r = call("get_event", {"name": "test_evt_detach"})
evt_d = tool_data(r)
new_formula = evt_d.get("formula")
check("Event formula points to replacement node",
      new_formula == d.get("replacement_node"),
      f"formula={new_formula}, repl_node={d.get('replacement_node')}")
# Clean up detached node
call("detach_sexp_node", {"node": evt_formula, "delete": True})
call("delete_event", {"name": "test_evt_detach"})

# =====================================================================
print("\n=== Test 4: Detach root of goal formula (Case A, OPR_BOOL) ===")
r = call("create_sexp_node", {"operator": "not", "operator_arguments": [{"type": "boolean", "value": "true"}]})
custom_formula = tool_data(r)["node"]
r = call("create_goal", {"name": "test_goal_detach", "formula": custom_formula})
d = tool_data(r)
goal_formula = d.get("formula")
r = call("detach_sexp_node", {"node": goal_formula})
d = tool_data(r)
repl_data = get_replacement_data(d)
repl_val = repl_data.get("value", "") if repl_data else ""
dn_data = get_detached_data(d)
check("Goal formula replaced with true",
      not is_error(r) and dn_data is not None and dn_data.get("node") == goal_formula and repl_val == "true",
      f"replacement={repl_val}")
call("detach_sexp_node", {"node": goal_formula, "delete": True})
call("delete_goal", {"name": "test_goal_detach"})

# =====================================================================
print("\n=== Test 5: Detach embedded node in event formula (Case C) ===")
r = call("create_event", {"name": "test_evt_embed"})
d = tool_data(r)
evt_formula = d.get("formula")
# Walk to find an embedded node (e.g. the "true" condition of "when")
r = call("walk_sexp_tree", {"node": evt_formula})
nodes = tool_data(r)["nodes"]
true_node = None
for n in nodes:
    if n.get("value") == "true" and n["node"] != evt_formula:
        true_node = n["node"]
        break
if true_node is not None:
    r = call("detach_sexp_node", {"node": true_node})
    if is_error(r):
        err_text = tool_text(r)
        # The parser uses Locked_sexp_true (shared singleton), so detaching it
        # is rejected as a locked singleton.
        check("Embedded detach rejected (locked singleton or syntax error)",
              "locked singleton" in err_text.lower() or "true or false" in err_text.lower()
              or "syntax error" in err_text.lower(),
              err_text[:150])
        # Verify formula is unchanged
        r = call("get_event", {"name": "test_evt_embed"})
        evt_d = tool_data(r)
        check("Event formula unchanged after rollback",
              evt_d.get("formula") == evt_formula,
              f"formula={evt_d.get('formula')}, expected={evt_formula}")
    else:
        d = tool_data(r)
        repl_data = get_replacement_data(d)
        repl_val = repl_data.get("value", "") if repl_data else ""
        check("Embedded node replaced with placeholder (syntax check passed)",
              repl_val == "<placeholder>",
              f"replacement={repl_val}")
else:
    print("  [SKIP] Could not find embedded true node")
call("delete_event", {"name": "test_evt_embed"})

# =====================================================================
print("\n=== Test 8: Delete last arg — placeholder preserved ===")
# Build (+ 1 2 3) then delete the last arg "3".
# After deletion: (+ 1 2 <placeholder>). Placeholder stays.
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"},
        {"type": "number", "value": "3"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("walk_sexp_tree", {"node": plus_node})
nodes = tool_data(r)["nodes"]
three_node = None
for nd in nodes:
    if nd.get("value") == "3":
        three_node = nd["node"]
        break
r = call("detach_sexp_node", {"node": three_node})
d = tool_data(r)
repl_data = get_replacement_data(d)
check("Last arg deleted, placeholder inserted",
      not is_error(r) and repl_data is not None and repl_data.get("value") == "<placeholder>",
      f"replacement={repl_data}")
r = call("walk_sexp_tree", {"node": plus_node})
remaining = [n["value"] for n in tool_data(r)["nodes"]]
check("Tree has + 1 2 <placeholder>",
      remaining == ["+", "1", "2", "<placeholder>"],
      str(remaining))
call("detach_sexp_node", {"node": plus_node, "delete": True})

# =====================================================================
print("\n=== Test 9: Trailing placeholder cleanup — middle arg with trailing placeholders ===")
# Build (+ 1 2 3), delete "2". Chain becomes (+ 1 <placeholder> 3).
# "3" is not a placeholder, so <placeholder> stays.
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"},
        {"type": "number", "value": "3"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("walk_sexp_tree", {"node": plus_node})
nodes = tool_data(r)["nodes"]
two_node = None
for nd in nodes:
    if nd.get("value") == "2":
        two_node = nd["node"]
        break
r = call("detach_sexp_node", {"node": two_node})
d = tool_data(r)
repl_data = get_replacement_data(d)
check("Middle arg deleted, placeholder kept (non-placeholder follows)",
      not is_error(r) and repl_data is not None and repl_data.get("value") == "<placeholder>",
      f"replacement={repl_data}")
# Verify tree has placeholder in the middle
r = call("walk_sexp_tree", {"node": plus_node})
remaining = [n["value"] for n in tool_data(r)["nodes"]]
check("Tree has + 1 <placeholder> 3",
      "+" in remaining and "1" in remaining and "<placeholder>" in remaining and "3" in remaining,
      str(remaining))
call("detach_sexp_node", {"node": plus_node, "delete": True})

# =====================================================================
print("\n=== Test 10: Delete all args — placeholders preserved ===")
# Build (+ 1 2), delete "1" then "2". Both become placeholders.
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("walk_sexp_tree", {"node": plus_node})
nodes = tool_data(r)["nodes"]
one_node = None
two_node = None
for nd in nodes:
    if nd.get("value") == "1":
        one_node = nd["node"]
    elif nd.get("value") == "2":
        two_node = nd["node"]
# Delete "2" first
r = call("detach_sexp_node", {"node": two_node})
d = tool_data(r)
repl_data = get_replacement_data(d)
check("Second arg replaced with placeholder",
      not is_error(r) and repl_data is not None and repl_data.get("value") == "<placeholder>",
      f"replacement={repl_data}")
# Now delete "1"
r = call("detach_sexp_node", {"node": one_node})
d = tool_data(r)
repl_data = get_replacement_data(d)
check("First arg replaced with placeholder",
      not is_error(r) and repl_data is not None and repl_data.get("value") == "<placeholder>",
      f"replacement={repl_data}")
# Verify both are placeholders
r = call("walk_sexp_tree", {"node": plus_node})
remaining = [n["value"] for n in tool_data(r)["nodes"]]
check("Tree has + <placeholder> <placeholder>",
      remaining == ["+", "<placeholder>", "<placeholder>"],
      str(remaining))
call("detach_sexp_node", {"node": plus_node, "delete": True})

# =====================================================================
print("\n=== Test 11: Shrink — remove middle arg, siblings shift up ===")
# Build (+ 1 2 3), shrink-delete "2". Chain becomes (+ 1 3).
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"},
        {"type": "number", "value": "3"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("walk_sexp_tree", {"node": plus_node})
nodes = tool_data(r)["nodes"]
two_node = None
for nd in nodes:
    if nd.get("value") == "2":
        two_node = nd["node"]
        break
r = call("detach_sexp_node", {"node": two_node, "shrink": True})
d = tool_data(r)
check("Shrink middle arg, no placeholder",
      not is_error(r) and d.get("replacement_node") is None,
      f"replacement_node={d.get('replacement_node')}")
r = call("walk_sexp_tree", {"node": plus_node})
remaining = [n["value"] for n in tool_data(r)["nodes"]]
check("Tree has + 1 3 (no placeholder)",
      remaining == ["+", "1", "3"],
      str(remaining))
call("detach_sexp_node", {"node": plus_node, "delete": True})

# =====================================================================
print("\n=== Test 12: Shrink — remove last arg ===")
# Build (+ 1 2 3), shrink-delete "3". Chain becomes (+ 1 2).
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"},
        {"type": "number", "value": "3"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("walk_sexp_tree", {"node": plus_node})
nodes = tool_data(r)["nodes"]
three_node = None
for nd in nodes:
    if nd.get("value") == "3":
        three_node = nd["node"]
        break
r = call("detach_sexp_node", {"node": three_node, "shrink": True})
d = tool_data(r)
check("Shrink last arg",
      not is_error(r),
      f"replacement_node={d.get('replacement_node')}")
r = call("walk_sexp_tree", {"node": plus_node})
remaining = [n["value"] for n in tool_data(r)["nodes"]]
check("Tree has + 1 2",
      remaining == ["+", "1", "2"],
      str(remaining))
call("detach_sexp_node", {"node": plus_node, "delete": True})

# =====================================================================
print("\n=== Test 13: Shrink — remove first arg ===")
# Build (+ 1 2 3), shrink-delete "1". Chain becomes (+ 2 3).
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"},
        {"type": "number", "value": "3"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("walk_sexp_tree", {"node": plus_node})
nodes = tool_data(r)["nodes"]
one_node = None
for nd in nodes:
    if nd.get("value") == "1":
        one_node = nd["node"]
        break
r = call("detach_sexp_node", {"node": one_node, "shrink": True})
d = tool_data(r)
check("Shrink first arg",
      not is_error(r),
      f"replacement_node={d.get('replacement_node')}")
r = call("walk_sexp_tree", {"node": plus_node})
remaining = [n["value"] for n in tool_data(r)["nodes"]]
check("Tree has + 2 3",
      remaining == ["+", "2", "3"],
      str(remaining))
call("detach_sexp_node", {"node": plus_node, "delete": True})

# =====================================================================
print("\n=== Test 14: Shrink — placeholder sibling preserved ===")
# Build (+ 1 2 <placeholder>), shrink-delete "2".
# After shrink: (+ 1 <placeholder>). Placeholder is not removed.
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"},
        {"type": "string", "value": "<placeholder>"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("walk_sexp_tree", {"node": plus_node})
nodes = tool_data(r)["nodes"]
two_node = None
for nd in nodes:
    if nd.get("value") == "2":
        two_node = nd["node"]
        break
r = call("detach_sexp_node", {"node": two_node, "shrink": True})
d = tool_data(r)
check("Shrink removes node, no replacement",
      not is_error(r) and d.get("replacement_node") is None,
      f"replacement_node={d.get('replacement_node')}")
r = call("walk_sexp_tree", {"node": plus_node})
remaining = [n["value"] for n in tool_data(r)["nodes"]]
check("Tree has + 1 <placeholder>",
      remaining == ["+", "1", "<placeholder>"],
      str(remaining))
call("detach_sexp_node", {"node": plus_node, "delete": True})

# =====================================================================
print("\n=== Test 15: Shrink=false is same as default (placeholder) ===")
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"},
        {"type": "number", "value": "3"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("walk_sexp_tree", {"node": plus_node})
nodes = tool_data(r)["nodes"]
two_node = None
for nd in nodes:
    if nd.get("value") == "2":
        two_node = nd["node"]
        break
r = call("detach_sexp_node", {"node": two_node, "shrink": False})
d = tool_data(r)
repl_data = get_replacement_data(d)
check("shrink=false inserts placeholder",
      not is_error(r) and repl_data is not None and repl_data.get("value") == "<placeholder>",
      f"replacement={repl_data}")
call("detach_sexp_node", {"node": plus_node, "delete": True})

# =====================================================================
print("\n=== Test 6: Detach Locked_sexp_true (rejection) ===")
r = call("create_goal", {"name": "test_locked"})
d = tool_data(r)
locked_formula = d.get("formula")
r = call("detach_sexp_node", {"node": locked_formula})
check("Locked_sexp_true rejected",
      is_error(r) and ("true or false" in tool_text(r).lower()
      or "locked singleton" in tool_text(r).lower()),
      tool_text(r)[:120])
call("delete_goal", {"name": "test_locked"})

# =====================================================================
print("\n=== Test 7: Detach SEXP_NOT_USED node (rejection) ===")
r = call("detach_sexp_node", {"node": 99999})
check("Out-of-range node rejected",
      is_error(r),
      tool_text(r)[:120])

# =====================================================================
print("\n=== Test 16: Detach free-standing root without delete (preserved) ===")
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("detach_sexp_node", {"node": plus_node})
d = tool_data(r)
dn_data = get_detached_data(d)
check("Free-standing root preserved (not freed)",
      not is_error(r) and dn_data is not None and dn_data.get("node") == plus_node
      and d.get("deleted") is False and d.get("freed_count") == 0,
      f"detached_node_data={dn_data}, deleted={d.get('deleted')}")
# Verify node is still accessible
r = call("get_sexp_node", {"node": plus_node})
check("Preserved root still accessible",
      not is_error(r),
      tool_text(r)[:80])
# Clean up
call("detach_sexp_node", {"node": plus_node, "delete": True})

# =====================================================================
print("\n=== Test 17: Detach+delete free-standing root (freed) ===")
r = call("create_sexp_node", {
    "operator": "+",
    "operator_arguments": [
        {"type": "number", "value": "1"},
        {"type": "number", "value": "2"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("detach_sexp_node", {"node": plus_node, "delete": True})
d = tool_data(r)
check("Free-standing root freed",
      not is_error(r) and d.get("detached_node") == plus_node
      and d.get("deleted") is True and d.get("freed_count") > 0,
      f"detached_node={d.get('detached_node')}, deleted={d.get('deleted')}, freed={d.get('freed_count')}")

# =====================================================================
print(f"\n{'='*50}")
print(f"Results: {passed} passed, {failed} failed")
print(f"{'='*50}")
sys.exit(1 if failed else 0)
