"""Reference and discovery tests for read-only MCP tools.

Covers ship classes/types, weapon classes, species, IFFs, intel entries,
personas, fonts, talking heads, SEXP metadata, reference notes, scripting
documentation, mod info, mission directory, root paths, subsystem name
helpers, and the coordinate transform tool.

All cross-test state in this file is intra-area: discovery tests stash a
representative name in suite.ctx and lookup tests consume it shortly after.
Nothing here writes ctx keys that other areas need.
"""

from mcp_test_lib import (
    assert_has_key,
    assert_in,
    assert_is_list,
    assert_non_empty_list,
    assert_success,
    assert_true,
    run_module_standalone,
    SkipTest,
    tool_data,
    tool_text,
)


def register(suite, client):
    ctx = suite.ctx

    # ----- Ship/Weapon -----

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

    # ----- Species/IFF/Intel -----

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

    # ----- Personas/UI -----

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

    # ----- SEXP Metadata -----

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

    # ----- Reference Notes -----

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

    # ----- Scripting -----

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

    # ----- Misc -----

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
        if "position" in d:
            pos = d["position"]
            assert_true(abs(pos["x"] - 100) < 0.01, f"x: {pos['x']}")
            assert_true(abs(pos["y"] - 200) < 0.01, f"y: {pos['y']}")
            assert_true(abs(pos["z"] - 300) < 0.01, f"z: {pos['z']}")

    tests = [
        ("reference_list_ship_types", test_list_ship_types),
        ("reference_get_ship_type", test_get_ship_type),
        ("reference_list_ship_classes", test_list_ship_classes),
        ("reference_list_ship_classes_filtered_species", test_list_ship_classes_filtered_species),
        ("reference_list_ship_classes_filtered_type", test_list_ship_classes_filtered_type),
        ("reference_get_ship_class", test_get_ship_class),
        ("reference_get_ship_class_model_details", test_get_ship_class_model_details),
        ("reference_list_weapon_classes", test_list_weapon_classes),
        ("reference_list_weapon_classes_filtered", test_list_weapon_classes_filtered),
        ("reference_get_weapon_class", test_get_weapon_class),
        ("reference_list_species", test_list_species),
        ("reference_list_iffs", test_list_iffs),
        ("reference_get_iff", test_get_iff),
        ("reference_list_intel_entries", test_list_intel_entries),
        ("reference_get_intel_entry", test_get_intel_entry),
        ("reference_list_persona_types", test_list_persona_types),
        ("reference_list_personas", test_list_personas),
        ("reference_list_talking_heads", test_list_talking_heads),
        ("reference_list_fonts", test_list_fonts),
        ("reference_list_sexp_categories", test_list_sexp_categories),
        ("reference_list_sexp_operators", test_list_sexp_operators),
        ("reference_list_sexp_operators_filtered_category", test_list_sexp_operators_filtered_category),
        ("reference_list_sexp_operators_search", test_list_sexp_operators_search),
        ("reference_get_sexp_operator", test_get_sexp_operator),
        ("reference_get_sexp_argument_type_list", test_get_sexp_argument_type_list),
        ("reference_get_sexp_argument_type", test_get_sexp_argument_type),
        ("reference_list_sexp_argument_values", test_list_sexp_argument_values),
        ("reference_get_sexp_return_type_list", test_get_sexp_return_type_list),
        ("reference_get_sexp_return_type", test_get_sexp_return_type),
        ("reference_list_reference_notes", test_list_reference_notes),
        ("reference_list_reference_notes_search", test_list_reference_notes_search),
        ("reference_get_reference_note", test_get_reference_note),
        ("reference_list_scripting_elements", test_list_scripting_elements),
        ("reference_list_scripting_elements_filtered", test_list_scripting_elements_filtered),
        ("reference_get_scripting_element", test_get_scripting_element),
        ("reference_search_scripting_children", test_search_scripting_children),
        ("reference_search_scripting_children_typed", test_search_scripting_children_typed),
        ("reference_list_scripting_hooks", test_list_scripting_hooks),
        ("reference_list_scripting_hooks_filtered", test_list_scripting_hooks_filtered),
        ("reference_list_scripting_enums", test_list_scripting_enums),
        ("reference_get_scripting_misc_conditions", test_get_scripting_misc_conditions),
        ("reference_get_scripting_misc_globalvars", test_get_scripting_misc_globalvars),
        ("reference_get_mod_info", test_get_mod_info),
        ("reference_list_missions", test_list_missions),
        ("reference_get_root_paths", test_get_root_paths),
        ("reference_subsystem_names_compare", test_subsystem_names_compare),
        ("reference_subsystem_names_equal", test_subsystem_names_equal),
        ("reference_coordinate_transform", test_coordinate_transform),
    ]
    for name, func in tests:
        suite.add(name, func)


if __name__ == "__main__":
    run_module_standalone(register, "Reference and discovery tests")
