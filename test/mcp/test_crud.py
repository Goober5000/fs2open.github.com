"""CRUD lifecycle tests for mission entities.

Each entity type gets one omnibus test that exercises create -> list -> get
-> update -> verify -> swap -> move -> delete -> verify-empty.  The body of
each test wraps everything in try/finally so the cleanup runs even when an
intermediate assertion fails — keeping the mission state usable for the
rest of the suite.

Covers: messages, events, goals, command briefing stages, fiction viewer
stages, debriefing stages, jump nodes, waypoints, and SEXP variables.

Note: the jump-node test gates on whether update_jump_node exists, via
client.tool_names.  This used to read suite.ctx["tool_names"] populated by
the connectivity test, but tool_names now lives on the client (lazily
discovered) so this file can run standalone without that dependency.
"""

from mcp_test_lib import (
    assert_equal,
    assert_in,
    assert_is_list,
    assert_success,
    assert_true,
    run_module_standalone,
    tool_data,
)


def register(suite, client):

    # ----- Messages -----

    def test_messages_crud():
        created = []
        try:
            # Create A (with filename fields, to test preservation across update)
            r = client.call_tool("create_message", {
                "name": "Test Message A",
                "message": "Hello world",
                "talking_head": "test_head.ani",
                "voice_filename": "test_voice.wav",
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

            # Filename preservation: filenames set at create should survive an
            # update that doesn't mention them.  Catches the silent-clobber bug
            # where get_optional_filename(..., null_if_invalid=false) returns ""
            # for missing params and lets the handler overwrite the stored value.
            assert_equal(d.get("talking_head"), "test_head.ani",
                "talking_head should be preserved across update")
            assert_equal(d.get("voice_filename"), "test_voice.wav",
                "voice_filename should be preserved across update")

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

    # ----- Events -----

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

    # ----- Goals -----

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

    # ----- Command Briefing Stages -----

    def test_cmd_brief_stages_crud():
        try:
            r = client.call_tool("create_cmd_brief_stage", {
                "text": "Stage 1: Approach the target",
                "animation_filename": "test_anim.ani",
                "voice_filename": "test_voice.wav",
            })
            assert_success(r)

            r = client.call_tool("create_cmd_brief_stage", {
                "text": "Stage 2: Engage hostiles"
            })
            assert_success(r)

            r = client.call_tool("list_cmd_brief_stages")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            assert_true(len(d) >= 2, f"Expected at least 2 stages, got {len(d)}")

            r = client.call_tool("get_cmd_brief_stage", {"index": 1})
            assert_success(r)
            d = tool_data(r)
            assert_in("Approach", d.get("text", ""), "stage text")

            r = client.call_tool("update_cmd_brief_stage", {
                "index": 1, "text": "Stage 1: Updated approach"
            })
            assert_success(r)

            r = client.call_tool("get_cmd_brief_stage", {"index": 1})
            assert_success(r)
            d = tool_data(r)
            assert_in("Updated", d.get("text", ""), "updated stage text")

            # Filename preservation across update
            assert_equal(d.get("animation_filename"), "test_anim.ani",
                "animation_filename should be preserved across update")
            assert_equal(d.get("voice_filename"), "test_voice.wav",
                "voice_filename should be preserved across update")

            r = client.call_tool("swap_cmd_brief_stages", {"index_a": 1, "index_b": 2})
            assert_success(r)

            r = client.call_tool("move_cmd_brief_stage", {"from_index": 2, "to_index": 1})
            assert_success(r)

        finally:
            for _ in range(5):  # safety bound
                try:
                    r = client.call_tool("list_cmd_brief_stages")
                    d = tool_data(r)
                    if not d:
                        break
                    client.call_tool("delete_cmd_brief_stage", {"index": len(d)})
                except Exception:
                    break

    # ----- Fiction Viewer Stages -----

    def test_fiction_viewer_stages_crud():
        try:
            r = client.call_tool("create_fiction_viewer_stage", {
                "story_filename": "test_story_a.txt",
                "font_filename": "test_font.fnt",
                "voice_filename": "test_voice.wav",
                "background_640": "test_bg640.pcx",
                "background_1024": "test_bg1024.pcx",
            })
            assert_success(r)

            r = client.call_tool("create_fiction_viewer_stage", {
                "story_filename": "test_story_b.txt"
            })
            assert_success(r)

            r = client.call_tool("list_fiction_viewer_stages")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            assert_true(len(d) >= 2, f"Expected at least 2 stages, got {len(d)}")

            r = client.call_tool("get_fiction_viewer_stage", {"index": 1})
            assert_success(r)

            r = client.call_tool("update_fiction_viewer_stage", {
                "index": 1, "story_filename": "test_story_updated.txt"
            })
            assert_success(r)

            # Filename preservation: the four optional filenames should
            # survive an update that only touches story_filename.
            r = client.call_tool("get_fiction_viewer_stage", {"index": 1})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("font_filename"), "test_font.fnt",
                "font_filename should be preserved across update")
            assert_equal(d.get("voice_filename"), "test_voice.wav",
                "voice_filename should be preserved across update")
            assert_equal(d.get("background_640"), "test_bg640.pcx",
                "background_640 should be preserved across update")
            assert_equal(d.get("background_1024"), "test_bg1024.pcx",
                "background_1024 should be preserved across update")

            r = client.call_tool("swap_fiction_viewer_stages", {"index_a": 1, "index_b": 2})
            assert_success(r)

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

    # ----- Debriefing Stages -----

    def test_debriefing_stages_crud():
        try:
            r = client.call_tool("create_debriefing_stage", {
                "text": "Debrief stage 1: Mission success",
                "voice_filename": "test_debrief.wav",
            })
            assert_success(r)

            r = client.call_tool("create_debriefing_stage", {
                "text": "Debrief stage 2: Mission failure",
                "recommendation_text": "Try harder next time"
            })
            assert_success(r)

            r = client.call_tool("list_debriefing_stages")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            assert_true(len(d) >= 2, f"Expected at least 2 stages, got {len(d)}")

            r = client.call_tool("get_debriefing_stage", {"index": 1})
            assert_success(r)
            d = tool_data(r)
            assert_in("Mission success", d.get("text", ""), "debrief text")

            r = client.call_tool("update_debriefing_stage", {
                "index": 2,
                "recommendation_text": "Updated recommendation"
            })
            assert_success(r)

            r = client.call_tool("get_debriefing_stage", {"index": 2})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("recommendation_text"), "Updated recommendation")

            # Filename preservation: update stage 1's text without mentioning
            # voice_filename, and verify voice_filename survives.
            r = client.call_tool("update_debriefing_stage", {
                "index": 1,
                "text": "Debrief stage 1: Mission success (updated)"
            })
            assert_success(r)
            r = client.call_tool("get_debriefing_stage", {"index": 1})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("voice_filename"), "test_debrief.wav",
                "voice_filename should be preserved across update")

            r = client.call_tool("swap_debriefing_stages", {"index_a": 1, "index_b": 2})
            assert_success(r)

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

    # ----- Jump Nodes -----

    def test_jump_nodes_crud():
        created = []
        try:
            # Note: no model_filename preservation test for jump nodes — passing
            # a real model file at create would trigger model_load() and require
            # the file to actually exist on disk.  The create/update handlers
            # already guard against empty model_filename via "if (model_file &&
            # model_file[0])" so the immediate-load regression is covered there.
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

            r = client.call_tool("list_jump_nodes")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            names = [n.get("name") for n in d]
            assert_in("Test Node A", names)

            r = client.call_tool("get_jump_node", {"name": "Test Node B"})
            assert_success(r)
            d = tool_data(r)

            # Update is optional — gate on whether the tool exists.  This used
            # to read suite.ctx["tool_names"] populated by phase 0; that key
            # doesn't exist in standalone runs of test_crud.py, so we ask the
            # client (which lazily caches tools/list).
            if "update_jump_node" in client.tool_names:
                r = client.call_tool("update_jump_node", {
                    "name": "Test Node A",
                    "new_name": "Test Nd A",
                    "position": {"x": 50, "y": 50, "z": 50},
                    "display_name": "Updated Node"
                })
                assert_success(r)
                created[0] = "Test Nd A"

                r = client.call_tool("get_jump_node", {"name": "Test Nd A"})
                assert_success(r)

            r = client.call_tool("swap_jump_nodes", {"index_a": 1, "index_b": 2})
            assert_success(r)

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

    # ----- Waypoint Lists + Waypoints -----

    def test_waypoints_crud():
        created_lists = []
        try:
            r = client.call_tool("create_waypoint_list", {
                "name": "Test WPL A",
                "points": [{"x": 0, "y": 0, "z": 0}, {"x": 100, "y": 0, "z": 0}]
            })
            assert_success(r)
            created_lists.append("Test WPL A")

            r = client.call_tool("create_waypoint_list", {
                "name": "Test WPL B",
                "points": [{"x": 0, "y": 100, "z": 0}]
            })
            assert_success(r)
            created_lists.append("Test WPL B")

            r = client.call_tool("list_waypoint_lists")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            names = [w.get("name") for w in d]
            assert_in("Test WPL A", names)

            r = client.call_tool("get_waypoint_list", {"name": "Test WPL A"})
            assert_success(r)
            d = tool_data(r)
            wps = d.get("waypoints", d.get("points", []))
            assert_true(len(wps) >= 2, "Expected at least 2 waypoints")

            r = client.call_tool("update_waypoint_list", {
                "name": "Test WPL A",
                "new_name": "Test WPL Alpha"
            })
            assert_success(r)
            created_lists[0] = "Test WPL Alpha"

            r = client.call_tool("create_waypoint", {
                "list": "Test WPL Alpha",
                "position": {"x": 200, "y": 0, "z": 0}
            })
            assert_success(r)

            r = client.call_tool("update_waypoint", {
                "list": "Test WPL Alpha",
                "index": 3,
                "position": {"x": 300, "y": 0, "z": 0}
            })
            assert_success(r)

            r = client.call_tool("swap_waypoints", {
                "list": "Test WPL Alpha",
                "index_a": 1,
                "index_b": 2
            })
            assert_success(r)

            r = client.call_tool("move_waypoint", {
                "list": "Test WPL Alpha",
                "from_index": 2,
                "to_index": 1
            })
            assert_success(r)

            r = client.call_tool("delete_waypoint", {
                "list": "Test WPL Alpha",
                "index": 3
            })
            assert_success(r)

            r = client.call_tool("swap_waypoint_lists", {"index_a": 1, "index_b": 2})
            assert_success(r)

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

    # ----- SEXP Variables -----

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

            r = client.call_tool("list_sexp_variables")
            assert_success(r)
            d = tool_data(r)
            assert_is_list(d)
            names = [v.get("name") for v in d]
            assert_in("test_var_a", names)
            assert_in("test_var_b", names)

            r = client.call_tool("get_sexp_variable", {"name": "test_var_a"})
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("default_value"), "0", "default value")

            r = client.call_tool("update_sexp_variable", {
                "name": "test_var_a",
                "new_name": "test_var_alpha",
                "default_value": "42"
            })
            assert_success(r)
            created[0] = "test_var_alpha"

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

    # ----- Mission Info filename preservation -----

    def test_mission_info_filename_preservation():
        # Snapshot what we'll restore at cleanup time
        r = client.call_tool("get_mission_info")
        assert_success(r)
        original = tool_data(r)
        original_title = original.get("title", "")

        try:
            # Set the three filename fields plus a non-filename field
            r = client.call_tool("update_mission_info", {
                "title": "Filename Preservation Test",
                "loading_screen_640": "test_640.pcx",
                "loading_screen_1024": "test_1024.pcx",
                "squadron_logo_filename": "test_logo.pcx",
            })
            assert_success(r)

            r = client.call_tool("get_mission_info")
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("loading_screen_640"), "test_640.pcx",
                "loading_screen_640 should be set after update")
            assert_equal(d.get("loading_screen_1024"), "test_1024.pcx",
                "loading_screen_1024 should be set after update")
            assert_equal(d.get("squadron_logo_filename"), "test_logo.pcx",
                "squadron_logo_filename should be set after update")

            # Now update only a non-filename field — the three filenames must
            # survive.  Catches the silent-clobber bug where a missing
            # filename param gets normalized to "" and overwrites the stored
            # value.
            r = client.call_tool("update_mission_info", {
                "title": "Filename Preservation Test 2",
            })
            assert_success(r)

            r = client.call_tool("get_mission_info")
            assert_success(r)
            d = tool_data(r)
            assert_equal(d.get("title"), "Filename Preservation Test 2",
                "title should reflect the second update")
            assert_equal(d.get("loading_screen_640"), "test_640.pcx",
                "loading_screen_640 should be preserved across update")
            assert_equal(d.get("loading_screen_1024"), "test_1024.pcx",
                "loading_screen_1024 should be preserved across update")
            assert_equal(d.get("squadron_logo_filename"), "test_logo.pcx",
                "squadron_logo_filename should be preserved across update")

        finally:
            # Restore: clear the filename fields and the title
            try:
                client.call_tool("update_mission_info", {
                    "title": original_title,
                    "loading_screen_640": "",
                    "loading_screen_1024": "",
                    "squadron_logo_filename": "",
                })
            except Exception:
                pass

    suite.add("crud_messages", test_messages_crud)
    suite.add("crud_events", test_events_crud)
    suite.add("crud_goals", test_goals_crud)
    suite.add("crud_cmd_brief_stages", test_cmd_brief_stages_crud)
    suite.add("crud_fiction_viewer_stages", test_fiction_viewer_stages_crud)
    suite.add("crud_debriefing_stages", test_debriefing_stages_crud)
    suite.add("crud_jump_nodes", test_jump_nodes_crud)
    suite.add("crud_waypoints", test_waypoints_crud)
    suite.add("crud_sexp_variables", test_sexp_variables_crud)
    suite.add("crud_mission_info_filename_preservation", test_mission_info_filename_preservation)


if __name__ == "__main__":
    run_module_standalone(register, "CRUD lifecycle tests")
