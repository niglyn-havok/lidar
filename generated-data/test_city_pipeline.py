import unittest

import numpy as np
from shapely.geometry import LineString, Polygon, box

from city_pipeline import (Ground, ORIGIN, fit_bridge, normalize_buildings, planar_water,
                           solid_mesh, spire, survey_to_ue, validate_mesh)


class IdentityTransform:
    def transform(self, x, y):
        return x, y


def way(identity, polygon, **tags):
    return {"type": "way", "id": identity, "tags": tags,
            "geometry": [{"lon": x, "lat": y} for x, y in polygon.exterior.coords]}


class CoordinatesTests(unittest.TestCase):
    def test_calibration_and_spire(self):
        points = [[315989, 234393, 0], [316089, 234393, 0],
                  [315989, 234493, 0], [315989, 234393, 10],
                  [315903.5, 234672.5, 124.609]]
        expected = [[0, 0, 0], [10000, 0, 0], [0, -10000, 0], [0, 0, 1000],
                    [-8550, -27950, 12460.9]]
        np.testing.assert_allclose(survey_to_ue(points), expected, atol=1e-8)
        np.testing.assert_array_equal(np.asarray(points)[0], [315989, 234393, 0])


class SolidTests(unittest.TestCase):
    def setUp(self):
        x, y = ORIGIN
        self.polygon = Polygon([(x, y), (x + 20, y), (x + 20, y + 20), (x, y + 20)],
                               [[(x + 5, y + 5), (x + 5, y + 15),
                                 (x + 15, y + 15), (x + 15, y + 5)]])

    def test_hole_roof_normals_and_positive_closed_volume(self):
        mesh, pivot = solid_mesh(self.polygon, lambda xy: np.full(len(xy), 12.), 2.)
        stats = validate_mesh(mesh, closed=True)
        self.assertAlmostEqual(stats["volumeM3"], self.polygon.area * 10, places=3)
        xyz = np.asarray(mesh["verticesCm"]) + pivot
        indices = np.asarray(mesh["triangles"]).reshape(-1, 3)
        roof = indices[np.asarray(mesh["materialIds"]) == 0]
        triangles = xyz[roof]
        up = -np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
        self.assertTrue(np.all(up[:, 2] > 0))
        self.assertAlmostEqual(up[:, 2].sum() / 20000, self.polygon.area, places=5)
        centres = triangles.mean(axis=1)
        self.assertFalse(np.any((centres[:, 0] > 500) & (centres[:, 0] < 1500) &
                                (centres[:, 1] < -500) & (centres[:, 1] > -1500)))

    def test_wall_uvs_are_cumulative_metres_not_texture_repeats(self):
        mesh, _ = solid_mesh(self.polygon, lambda xy: np.full(len(xy), 12.), 2.)
        xyz, uv = np.asarray(mesh["verticesCm"]), np.asarray(mesh["uv"])
        faces = np.asarray(mesh["triangles"]).reshape(-1, 3)
        walls = faces[np.asarray(mesh["materialIds"]) == 1]
        wall_vertices = np.unique(walls)
        np.testing.assert_allclose(uv[wall_vertices, 1], xyz[wall_vertices, 2] / 100, atol=1e-6)
        self.assertAlmostEqual(uv[wall_vertices, 0].max(), 80.)
        for face in walls:
            a, b = face[:2]
            if xyz[a, 2] == xyz[b, 2] == 0:
                self.assertAlmostEqual(uv[b, 0] - uv[a, 0],
                                       np.linalg.norm(xyz[b, :2] - xyz[a, :2]) / 100, places=5)

    def test_supported_ridge_preserves_hole_and_closure(self):
        x, y = ORIGIN
        roof = lambda xy: 15 - .3 * np.abs(xy[:, 0] - x - 10)
        ridge = LineString([(x + 10, y - 10), (x + 10, y + 30)])
        mesh, _ = solid_mesh(self.polygon, roof, 2, [ridge])
        self.assertGreater(validate_mesh(mesh, closed=True)["volumeM3"], 0)

    def test_curved_bridge_top_and_bottom_are_closed(self):
        x, y = ORIGIN
        shape = box(x, y, x + 20, y + 5)
        roof = lambda xy: 7 - .02 * (np.asarray(xy)[:, 0] - x - 10) ** 2
        ridges = [LineString([(x + offset, y - 10), (x + offset, y + 15)])
                  for offset in (5, 10, 15)]
        mesh, _ = solid_mesh(shape, roof, lambda xy: roof(xy) - .7, ridges)
        self.assertAlmostEqual(validate_mesh(mesh, closed=True)["volumeM3"], shape.area * .7, places=3)

    def test_man_made_bridge_area_can_recover_supported_arch(self):
        x, y = ORIGIN
        shape = box(x - 25, y - 2, x + 25, y + 2)
        xx, yy = np.meshgrid(np.arange(x - 24, x + 25), np.arange(y - 1, y + 2))
        xy = np.column_stack([xx.ravel(), yy.ravel()])
        heights = 7.5 - .004 * (xy[:, 0] - x) ** 2
        class SyntheticRaster:
            def samples(self, geometry, erosion=1):
                return xy, heights, len(xy), len(xy)
        roof, ridges, method, _ = fit_bridge(
            {"id": "bridge/test", "geometry": shape,
             "tags": {"name": "Ha'penny Bridge", "man_made": "bridge", "bridge:structure": "arch"}},
            SyntheticRaster())
        self.assertIn("supported_arch", method)
        self.assertGreater(len(ridges), 0)
        self.assertGreater(roof([[x, y]])[0], roof([[x + 20, y]])[0])

    def test_spire_closed_exact_tip_and_base(self):
        mesh = spire()
        validate_mesh(mesh, closed=True)
        xyz = np.asarray(mesh["verticesCm"]) + mesh["pivotCm"]
        np.testing.assert_array_equal(mesh["pivotCm"], [-8550, -27950, 645])
        self.assertAlmostEqual(xyz[:, 2].max(), 12460.9)
        self.assertAlmostEqual(xyz[:, 2].min(), 645)
        faces = np.asarray(mesh["triangles"]).reshape(-1, 3)
        wall_vertices = np.unique(faces[np.asarray(mesh["materialIds"]) == 1])
        uv = np.asarray(mesh["uv"])[wall_vertices]
        self.assertAlmostEqual(uv[:, 1].max(), (12460.9 - 645) / 100, places=5)
        self.assertAlmostEqual(uv[:, 0].max(), 40 * 3 * np.sin(np.pi / 40), places=5)

    def test_non_integer_nested_and_out_of_range_indices_rejected(self):
        mesh, _ = solid_mesh(self.polygon, lambda xy: np.full(len(xy), 12.), 2.)
        for bad in ([0., 1., 2.], [[0, 1, 2]], [0, 1], [0, 1, 999999]):
            with self.subTest(indices=bad), self.assertRaises(ValueError):
                validate_mesh({**mesh, "triangles": bad})

    def test_missing_face_rejected(self):
        mesh, _ = solid_mesh(self.polygon, lambda xy: np.full(len(xy), 12.), 2.)
        mesh["triangles"] = mesh["triangles"][3:]
        mesh["materialIds"] = mesh["materialIds"][1:]
        with self.assertRaisesRegex(ValueError, "nonmanifold"):
            validate_mesh(mesh, closed=True)

    def test_water_island_is_not_filled(self):
        mesh = planar_water(self.polygon)
        stats = validate_mesh(mesh)
        self.assertGreater(stats["triangles"], 0)
        xyz = np.asarray(mesh["verticesCm"])
        triangles = xyz[np.asarray(mesh["triangles"]).reshape(-1, 3)]
        area = np.linalg.norm(np.cross(triangles[:, 1] - triangles[:, 0],
                                      triangles[:, 2] - triangles[:, 0]), axis=1).sum() / 20000
        self.assertAlmostEqual(area, self.polygon.area, places=5)


class GroundTests(unittest.TestCase):
    def test_unmapped_elevated_rooftop_island_is_rejected(self):
        x, y = np.arange(129.), np.arange(129.)
        heights = np.full((129, 129), 5.)
        heights[45:85, 45:85] = 18
        ground = Ground(x, y, heights, np.ones_like(heights, dtype=bool), Polygon())
        self.assertAlmostEqual(float(ground.evaluate([[60, 60]])[0]), 5.)
        self.assertLess(ground.values.max(), 8)

    def test_contaminated_class2_roofs_are_not_ground(self):
        x, y = np.arange(97.), np.arange(97.)
        xx, yy = np.meshgrid(x, y)
        heights = 5 + xx * .01
        building = box(25, 25, 70, 70)
        heights[25:71, 25:71] = 90
        heights[10:13, 10:13] = 30
        ground = Ground(x, y, heights, np.ones_like(heights, dtype=bool), building.buffer(4))
        values = ground.evaluate([[40, 40], [60, 60]])
        np.testing.assert_allclose(values, [5.4, 5.6], atol=.15)
        self.assertLess(ground.values.max(), 7)
        self.assertFalse(any(building.buffer(4).covers(__import__("shapely").Point(p))
                             for p in ground.points))

    def test_no_supported_ground_fails(self):
        with self.assertRaisesRegex(ValueError, "insufficient"):
            Ground(np.arange(20.), np.arange(20.), np.full((20, 20), 80.),
                   np.ones((20, 20), dtype=bool), Polygon())


class NormalizationTests(unittest.TestCase):
    def test_osm_spire_does_not_duplicate_restored_landmark(self):
        shape = box(315902, 234671, 315905, 234674)
        records, mask, diagnostics = normalize_buildings(
            [way(96578181, shape, building="yes", wikidata="Q1134365")], IdentityTransform())
        self.assertEqual(records, [])
        self.assertGreater(mask.area, 0)
        self.assertEqual(diagnostics[0]["status"], "replaced_by_verified_spire_landmark")

    def test_parts_replace_parent_without_overlapping_shells(self):
        x, y = ORIGIN
        parent = box(x, y, x + 20, y + 20)
        part = box(x, y, x + 10, y + 20)
        elements = [way(1, parent, building="yes"), way(2, part, **{"building:part": "yes"}),
                    way(3, parent, building="yes")]
        records, mask, diagnostics = normalize_buildings(elements, IdentityTransform())
        self.assertEqual(len(records), 2)
        self.assertEqual(len(set(r["id"] for r in records)), 2)
        self.assertAlmostEqual(sum(r["geometry"].area for r in records), parent.area)
        self.assertEqual(records[0]["geometry"].intersection(records[1]["geometry"]).area, 0)
        self.assertAlmostEqual(mask.area, parent.area)
        self.assertTrue(any(d["status"] == "duplicate_or_covered" for d in diagnostics))

    def test_relation_suppresses_duplicate_member_way(self):
        x, y = ORIGIN
        shape = box(x, y, x + 20, y + 20)
        member = way(1, shape, building="yes")
        relation = {"type": "relation", "id": 9, "tags": {"type": "multipolygon", "building": "yes"},
                    "members": [{"type": "way", "ref": 1, "role": "outer",
                                 "geometry": member["geometry"]}]}
        records, _, diagnostics = normalize_buildings([member, relation], IdentityTransform())
        self.assertEqual([r["id"] for r in records], ["osm/relation/9"])
        self.assertTrue(any(d["status"] == "duplicate_relation_member" for d in diagnostics))


if __name__ == "__main__":
    unittest.main()
