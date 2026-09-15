import copy
import unittest

from apply_facades import (STYLE_CODES, choose_profile, decorate, encode, encode_city, geometry_digest,
                           inferred_rgb, parse_colour, resolve, without_colours)


def fixture():
    profiles = {k: {"colours_srgb_hex": ["#808080"]} for k in STYLE_CODES}
    building = {"id": "osm/way/123", "name": "Test", "confidence": .6, "heightMethod": "unchanged",
                "pivotCm": [0, 0, 100], "verticesCm": [[0, 0, 0], [100, 0, 0], [0, -100, 200]],
                "triangles": [0, 1, 2], "uv": [[0, 0], [1, 0], [0, 2]], "materialIds": [1]}
    city = {"schemaVersion": 1, "originEN": [315989, 234393], "extentMeters": 768,
            "attribution": ["unchanged"], "terrain": {"sentinel": -0.0},
            "water": {"sentinel": "unchanged"}, "buildings": [building]}
    row = {"known_material": None, "known_colour": None, "inherited_candidates": {},
           "palette_suggestion": {"palette_profile": "red_brick"}, "evidencelevel": "INFERRED"}
    evidence = {"buildings": {building["id"]: row}, "palette_profiles": profiles}
    return city, evidence


class FacadeTests(unittest.TestCase):
    def test_original_windows_line_ending_is_preserved(self):
        city, _ = fixture()
        self.assertEqual(encode_city(city, b"\r\n"), encode(city)[:-1] + b"\r\n")

    def test_fixed_codes(self):
        self.assertEqual(list(STYLE_CODES.values()), list(range(1, 9)))
        self.assertEqual(STYLE_CODES["modern_glazing"], 7)
        self.assertEqual(STYLE_CODES["metal_structure"], 8)

    def test_css_hex_rgb_parse(self):
        for source, expected in [(" LiGhTgReY ", (211, 211, 211)), ("beige", (245, 245, 220)),
                                 ("#E96B39", (233, 107, 57)), ("#aBc", (170, 187, 204)),
                                 ("RGB( 1, 20, 255 )", (1, 20, 255))]:
            self.assertEqual(parse_colour(source), expected)

    def test_unknown_and_nonbyte_colours_rejected(self):
        for bad in ("red_brick_on_documented_upper_elevations", "mystery", "rgba(1,2,3,4)",
                    "rgb(256,1,2)", [1., 2, 3], [True, 2, 3], [1, 2, float("nan")]):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                parse_colour(bad)

    def test_asserted_colour_is_exact_never_jittered(self):
        city, evidence = fixture()
        row = evidence["buildings"]["osm/way/123"]
        row.update(known_material="brick", known_colour="#E96B39")
        result, records, _ = decorate(city, evidence)
        self.assertEqual(result["buildings"][0]["colorsRGBA"], [[233, 107, 57, 1]] * 3)
        self.assertEqual(records["osm/way/123"]["colourSource"], "SOURCE_ASSERTED")

    def test_direct_wins_over_inheritance(self):
        city, evidence = fixture()
        row = evidence["buildings"]["osm/way/123"]
        row.update(known_material="glass;metal", known_colour="white",
                   inherited_candidates={"building:material": {"value": "brick"},
                                         "building:colour": {"value": "red"}})
        result, _, _ = decorate(city, evidence)
        self.assertEqual(result["buildings"][0]["colorsRGBA"][0], [255, 255, 255, 7])

    def test_inherited_wins_over_context_and_is_not_jittered(self):
        city, evidence = fixture()
        evidence["buildings"]["osm/way/123"]["inherited_candidates"] = {
            "building:material": {"value": "stone"}, "building:colour": {"value": "beige"}}
        result, records, _ = decorate(city, evidence)
        self.assertEqual(result["buildings"][0]["colorsRGBA"][0], [245, 245, 220, 4])
        self.assertEqual(records["osm/way/123"]["materialSource"], "INHERITED_CANDIDATE")

    def test_unknown_colour_is_reported_and_fallback_labelled(self):
        city, evidence = fixture()
        evidence["buildings"]["osm/way/123"].update(known_material="brick", known_colour="unknown!")
        result, records, errors = decorate(city, evidence)
        self.assertEqual(len(errors), 1)
        self.assertEqual(records["osm/way/123"]["colourSource"], "REJECTED_COLOUR_INFERRED_TINT")
        self.assertEqual(result["buildings"][0]["colorsRGBA"][0][3], 1)

    def test_unknown_material_is_reported_not_asserted(self):
        city, evidence = fixture()
        evidence["buildings"]["osm/way/123"]["known_material"] = "unobtanium"
        _, records, errors = decorate(city, evidence)
        self.assertEqual(len(errors), 1)
        self.assertEqual(records["osm/way/123"]["materialSource"], "REJECTED_MATERIAL_CONTEXTUAL_INFERENCE")

    def test_stone_brick_and_glass_families(self):
        self.assertEqual(choose_profile("stone", "grey", (128, 128, 128), "cool_stone"), "cool_stone")
        self.assertEqual(choose_profile("stone", "beige", (245, 245, 220), "cool_stone"), "warm_stone")
        self.assertEqual(choose_profile("brick", "brown", (165, 42, 42), "red_brick"), "brown_brick")
        self.assertEqual(choose_profile("glass;metal", None, None, "red_brick"), "modern_glazing")

    def test_spire_is_silver_metal_not_brick(self):
        city, evidence = fixture()
        building = {**city["buildings"][0], "id": "landmark/spire"}
        rgba, record, errors = resolve(building, {}, evidence["palette_profiles"])
        self.assertEqual(rgba, [192, 192, 192, 8])
        self.assertEqual(record["profile"], "metal_structure")
        self.assertEqual(errors, [])

    def test_deterministic_tiny_inferred_variation(self):
        _, evidence = fixture()
        first = inferred_rgb("red_brick", "stable-id", evidence["palette_profiles"])
        self.assertEqual(first, inferred_rgb("red_brick", "stable-id", evidence["palette_profiles"]))
        self.assertTrue(all(abs(v - 128) <= 3 for v in first))

    def test_only_colour_addition_and_bit_exact_geometry(self):
        city, evidence = fixture()
        before = copy.deepcopy(city)
        result, _, _ = decorate(city, evidence)
        self.assertEqual(city, before)
        self.assertEqual(encode(without_colours(result)), encode(before))
        self.assertEqual(geometry_digest(result), geometry_digest(before))
        self.assertEqual(set(result["buildings"][0]) - set(before["buildings"][0]), {"colorsRGBA"})

    def test_idempotent_data_records_and_diagnostics(self):
        city, evidence = fixture()
        first, records, errors = decorate(city, evidence)
        second, second_records, second_errors = decorate(first, evidence)
        self.assertEqual(encode(first), encode(second))
        self.assertEqual(records, second_records)
        self.assertEqual(errors, second_errors)

    def test_missing_or_extra_evidence_ids_rejected(self):
        city, evidence = fixture()
        evidence["buildings"]["extra"] = {}
        with self.assertRaises(ValueError):
            decorate(city, evidence)


if __name__ == "__main__":
    unittest.main()
