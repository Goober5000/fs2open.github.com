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
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"},
        {"argument_type": "number", "argument_value": "3"}
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
r = call("create_sexp_node", {
    "role": "operator",
    "operator_name": "not",
    "operator_arguments": [{"argument_type": "boolean", "argument_value": "true"}]
})
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
print("\n=== Test 5: Detach Locked_sexp_true embedded in event formula ===")
# The default event formula `( when ( true ) ( do-nothing ) )` is built by the
# parser, which reuses Locked_sexp_true for the inner "true" node.  Detaching
# any locked singleton must always be rejected, even when the singleton is
# embedded inside a larger tree -- this is checked before any retargeting or
# splice happens, so the formula must be left untouched.
r = call("create_event", {"name": "test_evt_embed"})
d = tool_data(r)
evt_formula = d.get("formula")
r = call("walk_sexp_tree", {"node": evt_formula})
nodes = tool_data(r)["nodes"]
true_node = None
for n in nodes:
    if n.get("value") == "true" and n["node"] != evt_formula:
        true_node = n["node"]
        break
check("Setup: found embedded 'true' node in default event formula",
      true_node is not None,
      f"true_node={true_node}")
r = call("detach_sexp_node", {"node": true_node})
err_text = tool_text(r)
check("Embedded Locked_sexp_true is rejected as a locked singleton",
      is_error(r) and "true or false" in err_text.lower(),
      err_text[:200])
# The formula must be unchanged after the rejection
r = call("get_event", {"name": "test_evt_embed"})
evt_d = tool_data(r)
check("Event formula unchanged after locked-singleton rejection",
      evt_d.get("formula") == evt_formula,
      f"formula={evt_d.get('formula')}, expected={evt_formula}")
call("delete_event", {"name": "test_evt_embed"})

# =====================================================================
print("\n=== Test 8: Delete last arg — placeholder preserved ===")
# Build (+ 1 2 3) then delete the last arg "3".
# After deletion: (+ 1 2 <placeholder>). Placeholder stays.
r = call("create_sexp_node", {
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"},
        {"argument_type": "number", "argument_value": "3"}
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
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"},
        {"argument_type": "number", "argument_value": "3"}
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
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"}
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
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"},
        {"argument_type": "number", "argument_value": "3"}
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
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"},
        {"argument_type": "number", "argument_value": "3"}
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
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"},
        {"argument_type": "number", "argument_value": "3"}
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
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"},
        {"argument_type": "string", "argument_value": "<placeholder>"}
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
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"},
        {"argument_type": "number", "argument_value": "3"}
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
print("\n=== Test 16: Detach already-free-standing root without delete is rejected ===")
# Detaching a node that is already a free-standing root, with delete=False and
# without an operator-atom retarget, would otherwise be a silent no-op (the
# node has nothing to be detached from).  The handler should report this
# rather than pretending success.
r = call("create_sexp_node", {
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("detach_sexp_node", {"node": plus_node})
err_text = tool_text(r)
check("Detach of already-free-standing root without delete is rejected",
      is_error(r) and "already the root" in err_text.lower(),
      err_text[:200])
# The node must still be accessible after the rejection
r = call("get_sexp_node", {"node": plus_node})
check("Rejected free-standing root is still accessible",
      not is_error(r),
      tool_text(r)[:80])
# Clean up
call("detach_sexp_node", {"node": plus_node, "delete": True})

# =====================================================================
print("\n=== Test 17: Detach+delete free-standing root (freed) ===")
# create_sexp_node returns the "+" operator atom directly with no list wrapper,
# and the number args are bare atoms in its .rest chain.  Freeing the tree
# should release exactly 3 nodes: the operator + the two number atoms.
r = call("create_sexp_node", {
    "role": "operator",
    "operator_name": "+",
    "operator_arguments": [
        {"argument_type": "number", "argument_value": "1"},
        {"argument_type": "number", "argument_value": "2"}
    ]
})
plus_node = tool_data(r)["node"]
r = call("detach_sexp_node", {"node": plus_node, "delete": True})
d = tool_data(r)
check("Free-standing root freed: response fields",
      not is_error(r) and d.get("detached_node") == plus_node
      and d.get("deleted") is True and get_detached_data(d) is None,
      f"detached_node={d.get('detached_node')}, deleted={d.get('deleted')}, "
      f"detached_node_data={get_detached_data(d)}")
check("Free-standing root freed: freed_count == 3 (operator + two number atoms)",
      d.get("freed_count") == 3,
      f"freed_count={d.get('freed_count')}")
# The deleted node must no longer be accessible via get_sexp_node
r = call("get_sexp_node", {"node": plus_node})
check("Deleted root reports not-in-use via get_sexp_node",
      is_error(r),
      tool_text(r)[:120])

# =====================================================================
print("\n=== Test 18: Case D retarget+unwrap (operator inside wrapper, no delete) ===")
# Build a free-standing tree with a nested operator subexpression:
#   ( when ( and ( true ) ( true ) ) ( do-nothing ) )
# Detaching the embedded "and" operator atom retargets to its enclosing list
# wrapper, splices the wrapper out of "when"'s argument chain (Case D), and
# then unwraps the detached subtree so the freed wrapper is gone and "and"
# becomes the new free-standing root with its arguments reparented to it.
r = call("text_to_sexp", {"text": "( when ( and ( true ) ( true ) ) ( do-nothing ) )"})
when_node = tool_data(r).get("node")
r = call("walk_sexp_tree", {"node": when_node})
nodes = tool_data(r)["nodes"]
and_node = None
and_wrapper_idx = None
for n in nodes:
    if n.get("value") == "and" and n.get("role") == "operator":
        and_node = n["node"]
        and_wrapper_idx = n.get("node_parent")
        break
check("Setup: found embedded 'and' operator and its wrapper",
      and_node is not None and and_wrapper_idx is not None and and_wrapper_idx >= 0,
      f"and_node={and_node}, wrapper={and_wrapper_idx}")
r = call("detach_sexp_node", {"node": and_node})
d = tool_data(r)
dn_data = get_detached_data(d)
repl_data = get_replacement_data(d)
check("Detach 'and': response reports the operator atom (not the wrapper)",
      not is_error(r) and d.get("detached_node") == and_node
      and dn_data is not None and dn_data.get("node") == and_node,
      f"detached_node={d.get('detached_node')}, dn_data={dn_data}")
check("Detached 'and' is now a free-standing root (parent=-1)",
      dn_data is not None and dn_data.get("node_parent") == -1,
      f"node_parent={dn_data.get('node_parent') if dn_data else None}")
check("Replacement is a placeholder string atom",
      repl_data is not None and repl_data.get("value") == "<placeholder>",
      f"replacement={repl_data}")
# The original wrapper around 'and' should have been freed by the unwrap step.
r2 = call("get_sexp_node", {"node": and_wrapper_idx})
check("Original 'and' wrapper was freed during unwrap",
      is_error(r2),
      tool_text(r2)[:120])
# Walking the detached subtree from 'and' should still see and+true+true.
# walk_sexp_tree also returns the empty-text list_wrapper entries; filter them
# out so we can match the meaningful payload exactly.
r2 = call("walk_sexp_tree", {"node": and_node})
detached_walk = [n["value"] for n in tool_data(r2)["nodes"] if n.get("value")]
check("Detached subtree contains and + true + true",
      detached_walk == ["and", "true", "true"],
      str(detached_walk))
# The original tree should now have a placeholder where the and-wrapper was.
r2 = call("walk_sexp_tree", {"node": when_node})
remaining = [n["value"] for n in tool_data(r2)["nodes"]]
check("Original tree has when + <placeholder> + do-nothing (no 'and')",
      "when" in remaining and "<placeholder>" in remaining and "do-nothing" in remaining
      and "and" not in remaining,
      str(remaining))
# Cleanup
call("detach_sexp_node", {"node": when_node, "delete": True})
call("detach_sexp_node", {"node": and_node, "delete": True})

# =====================================================================
print("\n=== Test 19: Case D retarget+unwrap with delete=true ===")
# Same setup as test 18, but the detached 'and' subtree is freed in one step.
# The 'and' subtree contains 4 freeable nodes: the wrapper around 'and', the
# 'and' operator atom itself, and the two ( true ) wrappers.  The two
# Locked_sexp_true singletons are not freeable and not counted.
r = call("text_to_sexp", {"text": "( when ( and ( true ) ( true ) ) ( do-nothing ) )"})
when_node = tool_data(r).get("node")
r = call("walk_sexp_tree", {"node": when_node})
nodes = tool_data(r)["nodes"]
and_node = None
for n in nodes:
    if n.get("value") == "and" and n.get("role") == "operator":
        and_node = n["node"]
        break
r = call("detach_sexp_node", {"node": and_node, "delete": True})
d = tool_data(r)
check("Detach+delete 'and': deleted=true, no detached_node_data",
      not is_error(r) and d.get("deleted") is True and get_detached_data(d) is None,
      f"deleted={d.get('deleted')}, dn_data={get_detached_data(d)}")
check("freed_count == 4 (and wrapper + and op + two true wrappers)",
      d.get("freed_count") == 4,
      f"freed_count={d.get('freed_count')}")
# 'and' index should now be unusable
r2 = call("get_sexp_node", {"node": and_node})
check("Deleted 'and' node is no longer in use",
      is_error(r2),
      tool_text(r2)[:120])
# The original tree should still be intact apart from the placeholder.
r2 = call("walk_sexp_tree", {"node": when_node})
remaining = [n["value"] for n in tool_data(r2)["nodes"]]
check("Original tree still has when + <placeholder> + do-nothing",
      "when" in remaining and "<placeholder>" in remaining and "do-nothing" in remaining
      and "and" not in remaining,
      str(remaining))
# Cleanup
call("detach_sexp_node", {"node": when_node, "delete": True})

# =====================================================================
print("\n=== Test 20: Case D retarget+unwrap with shrink mode ===")
# Build ( + 1 ( - 5 3 ) 2 ) and shrink-detach the embedded '-' operator.
# Retargeting takes us to the inner list wrapper, shrink mode removes it from
# '+'s argument chain without inserting a placeholder, and the unwrap step
# leaves '-' as the root of a new free-standing tree.
r = call("text_to_sexp", {"text": "( + 1 ( - 5 3 ) 2 )"})
plus_node = tool_data(r).get("node")
r = call("walk_sexp_tree", {"node": plus_node})
nodes = tool_data(r)["nodes"]
minus_node = None
minus_wrapper_idx = None
for n in nodes:
    if n.get("value") == "-" and n.get("role") == "operator":
        minus_node = n["node"]
        minus_wrapper_idx = n.get("node_parent")
        break
check("Setup: found embedded '-' operator and its wrapper",
      minus_node is not None and minus_wrapper_idx is not None and minus_wrapper_idx >= 0,
      f"minus_node={minus_node}, wrapper={minus_wrapper_idx}")
r = call("detach_sexp_node", {"node": minus_node, "shrink": True})
d = tool_data(r)
dn_data = get_detached_data(d)
check("Shrink+retarget: response reports the '-' operator atom",
      not is_error(r) and d.get("detached_node") == minus_node
      and dn_data is not None and dn_data.get("node") == minus_node,
      f"detached_node={d.get('detached_node')}, dn_data={dn_data}")
check("Shrink: no replacement node",
      d.get("replacement_node") is None,
      f"replacement_node={d.get('replacement_node')}")
check("Detached '-' is unwrapped to a free-standing root",
      dn_data is not None and dn_data.get("node_parent") == -1,
      f"node_parent={dn_data.get('node_parent') if dn_data else None}")
# Original wrapper around '-' should have been freed by the unwrap step.
r2 = call("get_sexp_node", {"node": minus_wrapper_idx})
check("Original '-' wrapper was freed during unwrap",
      is_error(r2),
      tool_text(r2)[:120])
# Original tree has + 1 2 (no '-', no placeholder, siblings shifted up).
r2 = call("walk_sexp_tree", {"node": plus_node})
remaining = [n["value"] for n in tool_data(r2)["nodes"]]
check("Original tree shrank to + 1 2",
      remaining == ["+", "1", "2"],
      str(remaining))
# Detached subtree has - 5 3.
r2 = call("walk_sexp_tree", {"node": minus_node})
detached_walk = [n["value"] for n in tool_data(r2)["nodes"]]
check("Detached subtree contains - 5 3",
      detached_walk == ["-", "5", "3"],
      str(detached_walk))
# Cleanup
call("detach_sexp_node", {"node": plus_node, "delete": True})
call("detach_sexp_node", {"node": minus_node, "delete": True})

# =====================================================================
print("\n=== Test 21: Case C syntax-error rollback in attached formula ===")
# Build an event whose formula is `( when ( and ( true ) ( true ) ) ( do-nothing ) )`
# and try to detach the embedded 'and' operator.  The default mode would
# replace the inner list wrapper with a <placeholder> string atom in the
# OPF_BOOL conditional position; check_sexp_syntax rejects this with
# SEXP_CHECK_TYPE_MISMATCH, so the handler must roll back and leave the
# event formula structurally identical to before the call.
r = call("text_to_sexp", {"text": "( when ( and ( true ) ( true ) ) ( do-nothing ) )"})
formula_node = tool_data(r).get("node")
r = call("create_event", {"name": "test_evt_rollback", "formula": formula_node})
event_formula = tool_data(r).get("formula")
r = call("walk_sexp_tree", {"node": event_formula})
nodes_before = tool_data(r)["nodes"]
and_node = None
for n in nodes_before:
    if n.get("value") == "and" and n.get("role") == "operator":
        and_node = n["node"]
        break
check("Setup: found 'and' operator inside attached event formula",
      and_node is not None,
      f"and_node={and_node}")
# Snapshot the tree as a (value, node, parent, rest) tuple per node so we can
# detect any structural drift caused by a buggy rollback.
def tree_signature(walk_nodes):
    return [(n["node"], n.get("value"), n.get("node_parent"), n.get("node_rest"),
             n.get("node_first")) for n in walk_nodes]
sig_before = tree_signature(nodes_before)
r = call("detach_sexp_node", {"node": and_node})
err_text = tool_text(r)
check("Detach of 'and' is rejected due to syntax check failure",
      is_error(r) and "syntax" in err_text.lower(),
      err_text[:200])
# Verify the event still references the same formula root
r = call("get_event", {"name": "test_evt_rollback"})
check("Event formula root unchanged after rollback",
      tool_data(r).get("formula") == event_formula,
      f"formula={tool_data(r).get('formula')}, expected={event_formula}")
# Verify the tree structure is byte-for-byte identical to before the call
r = call("walk_sexp_tree", {"node": event_formula})
sig_after = tree_signature(tool_data(r)["nodes"])
check("Tree structure unchanged after rollback",
      sig_before == sig_after,
      f"before={sig_before[:3]}..., after={sig_after[:3]}...")
call("delete_event", {"name": "test_evt_rollback"})

# =====================================================================
print(f"\n{'='*50}")
print(f"Results: {passed} passed, {failed} failed")
print(f"{'='*50}")
sys.exit(1 if failed else 0)
