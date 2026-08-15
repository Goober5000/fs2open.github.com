"""Context-sensitive list_sexp_argument_values tests.

Covers the node/arg_index context path of list_sexp_argument_values: the
tool passes a raw Sexp_nodes[] index straight into the MCP sexp forest
(1:1 tree_nodes/Sexp_nodes mapping), and SexpTreeOPF derives the legal
values from the surrounding tree — e.g. which special subsystem entries
apply depends on the parent operator.  Also pins the context-free
behavior of the seven context-dependent argument types, which must
return an empty listing rather than asserting.

The context-free happy path with a discovered type name is covered in
test_reference.py, which deliberately runs before mission setup.  This
file needs a mission (and its "Alpha 1" player start), so it runs after
mission setup in the comprehensive suite.

Like test_sexp_roundtrip.py, this file uses detach_sexp_node only as a
cleanup mechanism for the trees its tests create.
"""

from mcp_test_lib import (
    assert_has_key,
    assert_in,
    assert_success,
    assert_true,
    assert_equal,
    find_node_by_value,
    run_module_standalone,
    SkipTest,
    tool_data,
)


# Argument types whose listings require a parent-node context.  Without a
# node these must return an empty listing (they used to hard-assert on
# parent_node < 0 in SexpTreeOPF).
CONTEXT_DEPENDENT_TYPES = [
    "subsystem",
    "ai_goal",
    "goal_name",
    "event_name",
    "docker_point",
    "dockee_point",
    "animation_name",
]


def register(suite, client):
    ctx = suite.ctx

    def walk_nodes(node):
        r = client.call_tool("walk_sexp_tree", {"node": node})
        assert_success(r)
        d = tool_data(r)
        if isinstance(d, dict):
            return d.get("nodes", [])
        return d

    def list_values(args):
        r = client.call_tool("list_sexp_argument_values", args)
        assert_success(r)
        d = tool_data(r)
        assert_has_key(d, "values")
        return d["values"]

    # ----- Context-free: graceful empty listings, not assertions -----

    def test_context_free_graceful():
        for name in CONTEXT_DEPENDENT_TYPES:
            values = list_values({"name": name})
            assert_equal(values, [], "context-free '%s' listing should be empty" % name)

    # ----- Subsystem context: OPS_STRENGTH parent appends Hull entries -----

    def test_subsystem_strength_context():
        r = client.call_tool("text_to_sexp", {
            "text": '( when ( has-time-elapsed 5 ) ( sabotage-subsystem "Alpha 1" "engine" 10 ) )'
        })
        assert_success(r)
        ctx["sexp_listing_strength_root"] = tool_data(r)["node"]

        nodes = walk_nodes(ctx["sexp_listing_strength_root"])
        op = find_node_by_value(nodes, "sabotage-subsystem", role="operator")

        # arg_index is 1-based: arg 1 is the ship, arg 2 is the subsystem
        values = list_values({"name": "subsystem", "node": op["node"], "arg_index": 2})
        assert_in("Hull", values, "strength-class operator should offer Hull")
        assert_in("Simulated Hull", values, "strength-class operator should offer Simulated Hull")

    # ----- Subsystem context: plain parent gets no special entries -----
    # Built after the previous listing, so this also exercises a partial
    # forest rebuild of a freshly-marked dirty root.

    def test_subsystem_plain_context():
        r = client.call_tool("text_to_sexp", {
            "text": '( when ( is-subsystem-destroyed-delay "Alpha 1" "engine" 1 ) ( do-nothing ) )'
        })
        assert_success(r)
        ctx["sexp_listing_plain_root"] = tool_data(r)["node"]

        nodes = walk_nodes(ctx["sexp_listing_plain_root"])
        op = find_node_by_value(nodes, "is-subsystem-destroyed-delay", role="operator")

        values = list_values({"name": "subsystem", "node": op["node"], "arg_index": 2})
        # Model subsystems are not loaded in a bare FRED session, so we only
        # assert on the operator-dependent special entries staying absent.
        assert_true("Hull" not in values, "plain subsystem operator should not offer Hull")
        assert_true("Simulated Hull" not in values,
                    "plain subsystem operator should not offer Simulated Hull")

    # ----- Event-name context: created events appear in the listing -----

    def test_event_name_context():
        for name in ("Ctx Event A", "Ctx Event B"):
            r = client.call_tool("create_event", {"name": name})
            assert_success(r)
            ctx.setdefault("sexp_listing_events", []).append(name)

        r = client.call_tool("text_to_sexp", {
            "text": '( when ( is-event-true-delay "Ctx Event A" 1 ) ( do-nothing ) )'
        })
        assert_success(r)
        ctx["sexp_listing_event_root"] = tool_data(r)["node"]

        nodes = walk_nodes(ctx["sexp_listing_event_root"])
        op = find_node_by_value(nodes, "is-event-true-delay", role="operator")

        values = list_values({"name": "event_name", "node": op["node"]})
        assert_in("Ctx Event A", values)
        assert_in("Ctx Event B", values)

    # ----- Cleanup -----

    def test_cleanup():
        for key in ("sexp_listing_strength_root", "sexp_listing_plain_root",
                    "sexp_listing_event_root"):
            node = ctx.pop(key, None)
            if node is not None:
                client.call_tool("detach_sexp_node", {"node": node, "delete": True})
        for name in ctx.pop("sexp_listing_events", []):
            client.call_tool("delete_event", {"name": name})

    tests = [
        ("sexp_listing_context_free_graceful", test_context_free_graceful),
        ("sexp_listing_subsystem_strength_context", test_subsystem_strength_context),
        ("sexp_listing_subsystem_plain_context", test_subsystem_plain_context),
        ("sexp_listing_event_name_context", test_event_name_context),
        ("sexp_listing_cleanup", test_cleanup),
    ]
    for name, func in tests:
        suite.add(name, func)


if __name__ == "__main__":
    run_module_standalone(register, "Context-sensitive sexp argument listing tests")
