"""Negative-path tests: invalid params, missing params, out-of-range
indices, nonexistent entities, range-validation rejection.

These tests verify that the MCP server reports errors gracefully when
given malformed or impossible input, rather than crashing or silently
succeeding.  None of them require a particular mission state beyond an
empty mission, so they are safe to run after the CRUD/SEXP areas.
"""

from mcp_test_lib import (
    assert_error,
    assert_in,
    assert_success,
    assert_true,
    MCPError,
    run_module_standalone,
    tool_text,
)


def register(suite, client):

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
        # May return error or success with parse errors; just verify it
        # doesn't crash and returns a response.
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

    # ----- Additional get/delete error tests -----

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

    # ----- Range validation (event creation) -----

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
        ("negative_unknown_tool", test_unknown_tool),
        ("negative_get_message_missing_param", test_get_message_missing_param),
        ("negative_get_message_nonexistent", test_get_message_nonexistent),
        ("negative_set_timeout_too_low", test_set_timeout_too_low),
        ("negative_set_timeout_too_high", test_set_timeout_too_high),
        ("negative_create_message_duplicate", test_create_message_duplicate),
        ("negative_delete_message_nonexistent", test_delete_message_nonexistent),
        ("negative_create_jump_node_missing_position", test_create_jump_node_missing_position),
        ("negative_text_to_sexp_invalid", test_text_to_sexp_invalid),
        ("negative_create_sexp_node_invalid_operator", test_create_sexp_node_invalid_operator),
        ("negative_create_sexp_node_wrong_arg_type", test_create_sexp_node_wrong_arg_type),
        ("negative_create_sexp_node_invalid_number_value", test_create_sexp_node_invalid_number_value),
        ("negative_create_sexp_node_argument_invalid_number", test_create_sexp_node_argument_invalid_number),
        ("negative_detach_sexp_node_invalid", test_detach_sexp_node_invalid),
        ("negative_swap_messages_out_of_range", test_swap_messages_out_of_range),
        ("negative_move_event_out_of_range", test_move_event_out_of_range),
        ("negative_get_sexp_operator_nonexistent", test_get_sexp_operator_nonexistent),
        ("negative_coordinate_transform_missing_params", test_coordinate_transform_missing_params),
        ("negative_get_scripting_misc_invalid_section", test_get_scripting_misc_invalid_section),
        ("negative_get_event_nonexistent", test_get_event_nonexistent),
        ("negative_get_goal_nonexistent", test_get_goal_nonexistent),
        ("negative_get_jump_node_nonexistent", test_get_jump_node_nonexistent),
        ("negative_get_waypoint_list_nonexistent", test_get_waypoint_list_nonexistent),
        ("negative_get_sexp_variable_nonexistent", test_get_sexp_variable_nonexistent),
        ("negative_delete_event_nonexistent", test_delete_event_nonexistent),
        ("negative_delete_goal_nonexistent", test_delete_goal_nonexistent),
        ("negative_update_event_nonexistent", test_update_event_nonexistent),
        ("negative_get_ship_class_nonexistent", test_get_ship_class_nonexistent),
        ("negative_get_weapon_class_nonexistent", test_get_weapon_class_nonexistent),
        ("negative_create_event_repeat_count_zero", test_create_event_repeat_count_zero),
        ("negative_create_event_interval_negative", test_create_event_interval_negative),
        ("negative_create_event_chain_delay_invalid", test_create_event_chain_delay_invalid),
    ]
    for name, func in tests:
        suite.add(name, func)


if __name__ == "__main__":
    run_module_standalone(register, "Negative-path tests")
