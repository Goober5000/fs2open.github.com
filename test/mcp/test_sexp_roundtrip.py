"""SEXP tree round-trip and node-creation tests.

Covers text_to_sexp, sexp_to_text, get_sexp_node, walk_sexp_tree,
create_sexp_node, and the create_sexp_node regression tests for
referencing non-root nodes and rolling back type errors.

Detach-specific behavior is tested in test_sexp_detach.py.  This file
*uses* detach_sexp_node only as a cleanup mechanism for the trees its
tests create — those calls are explicitly named cleanup_* so they don't
look like detach tests.
"""

from mcp_test_lib import (
    assert_error,
    assert_equal,
    assert_has_key,
    assert_in,
    assert_success,
    assert_true,
    run_module_standalone,
    SkipTest,
    tool_data,
    tool_text,
)


def register(suite, client):
    ctx = suite.ctx

    # ----- Simple round-trip: ( do-nothing ) -----

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
        if isinstance(d, list):
            assert_true(len(d) > 0, "walk should return nodes")
        elif isinstance(d, dict) and "nodes" in d:
            assert_true(len(d["nodes"]) > 0, "walk should return nodes")

    def test_cleanup_simple_tree():
        node = ctx.get("sexp_simple_node")
        if node is None:
            raise SkipTest("No sexp node")
        r = client.call_tool("detach_sexp_node", {"node": node, "delete": True})
        assert_success(r)
        ctx.pop("sexp_simple_node", None)

    # ----- Complex round-trip: ( when ( true ) ( do-nothing ) ) -----

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

    def test_cleanup_complex_tree():
        node = ctx.get("sexp_complex_node")
        if node is None:
            raise SkipTest("No complex sexp node")
        r = client.call_tool("detach_sexp_node", {"node": node, "delete": True})
        assert_success(r)
        ctx.pop("sexp_complex_node", None)

    # ----- create_sexp_node: do-nothing operator with no arguments -----

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

    def test_cleanup_created_tree():
        node = ctx.get("sexp_created_node")
        if node is None:
            raise SkipTest("No created sexp node")
        r = client.call_tool("detach_sexp_node", {"node": node, "delete": True})
        assert_success(r)
        ctx.pop("sexp_created_node", None)

    # ----- create_sexp_node: operator with arguments -----

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

    def test_cleanup_args_tree():
        node = ctx.get("sexp_args_node")
        if node is None:
            raise SkipTest("No sexp args node")
        r = client.call_tool("detach_sexp_node", {"node": node, "delete": True})
        assert_success(r)
        ctx.pop("sexp_args_node", None)

    # ----- create_sexp_node: composing sub-expressions via 'node' arguments -----

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

    def test_cleanup_composed_tree():
        node = ctx.get("sexp_composed_node")
        if node is None:
            raise SkipTest("No composed sexp node")
        r = client.call_tool("detach_sexp_node", {"node": node, "delete": True})
        assert_success(r)
        ctx.pop("sexp_composed_node", None)
        ctx.pop("sexp_sub_node", None)

    # ----- create_sexp_node regression: non-root node arg is rejected -----

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

    # ----- create_sexp_node regression: type-error rollback preserves referenced tree -----

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

    tests = [
        ("sexp_roundtrip_text_to_sexp_simple", test_text_to_sexp_simple),
        ("sexp_roundtrip_sexp_to_text_simple", test_sexp_to_text_simple),
        ("sexp_roundtrip_get_sexp_node_simple", test_get_sexp_node_simple),
        ("sexp_roundtrip_walk_sexp_tree_simple", test_walk_sexp_tree_simple),
        ("sexp_roundtrip_cleanup_simple_tree", test_cleanup_simple_tree),
        ("sexp_roundtrip_text_to_sexp_complex", test_text_to_sexp_complex),
        ("sexp_roundtrip_walk_sexp_tree_depth", test_walk_sexp_tree_depth),
        ("sexp_roundtrip_sexp_to_text_complex", test_sexp_to_text_complex),
        ("sexp_roundtrip_cleanup_complex_tree", test_cleanup_complex_tree),
        ("sexp_roundtrip_create_sexp_node_simple", test_create_sexp_node_simple),
        ("sexp_roundtrip_get_created_sexp_node", test_get_created_sexp_node),
        ("sexp_roundtrip_sexp_to_text_created", test_sexp_to_text_created),
        ("sexp_roundtrip_cleanup_created_tree", test_cleanup_created_tree),
        ("sexp_roundtrip_create_sexp_node_with_args", test_create_sexp_node_with_args),
        ("sexp_roundtrip_sexp_to_text_args", test_sexp_to_text_args),
        ("sexp_roundtrip_cleanup_args_tree", test_cleanup_args_tree),
        ("sexp_roundtrip_create_sexp_node_with_node_arg", test_create_sexp_node_with_node_arg),
        ("sexp_roundtrip_sexp_to_text_composed", test_sexp_to_text_composed),
        ("sexp_roundtrip_cleanup_composed_tree", test_cleanup_composed_tree),
        ("sexp_roundtrip_create_sexp_node_non_root_rejected", test_create_sexp_node_non_root_rejected),
        ("sexp_roundtrip_cleanup_nonroot_tree", test_cleanup_nonroot_tree),
        ("sexp_roundtrip_create_sexp_node_type_error_preserves_referenced_tree",
         test_create_sexp_node_type_error_preserves_referenced_tree),
        ("sexp_roundtrip_cleanup_preserved_tree", test_cleanup_preserved_tree),
    ]
    for name, func in tests:
        suite.add(name, func)


if __name__ == "__main__":
    run_module_standalone(register, "SEXP round-trip and creation tests")
