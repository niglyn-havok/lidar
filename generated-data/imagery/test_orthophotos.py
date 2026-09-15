"""Small mathematical tests plus checks against the actual generated image and originals."""

import json
import io
import math
from pathlib import Path
import stat
import unittest
import zipfile

import numpy as np
from PIL import Image

from acquire_orthophotos import (
    AUTHORS, BUDGET, HANDOFF, MANIFEST, ROOT, TILES, WORK, now, progress, sha256, write_json,
)
from process_orthophotos import (
    BOUNDS, PNG, SIDECAR, SIZE, SPACING, area_weights, grid_to_pixel,
    independent_pixel, open_sources, pixel_to_grid, read_window, rgb_bands,
)
from inspect_orthophotos import approved_members, scoped_target, validate_member_path


class ExtractionTests(unittest.TestCase):
    def test_rejects_absolute_drive_unc_and_traversal_paths(self):
        for name in ("/315500_234000.tif", "\\315500_234000.tif",
                     "C:\\315500_234000.tif", "C:315500_234000.tif",
                     "\\\\server\\share\\315500_234000.tif",
                     "../315500_234000.tif", "folder/../315500_234000.tif",
                     "folder\\..\\315500_234000.tif",
                     "folder/./315500_234000.tif", "folder/.. /315500_234000.tif",
                     "folder/315500_234000.tif:stream", "folder/\x00hidden"):
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                validate_member_path(zipfile.ZipInfo(name))

    def test_rejects_zip_symlinks(self):
        member = zipfile.ZipInfo("folder/315500_234000.tif")
        member.create_system = 3
        member.external_attr = (stat.S_IFLNK | 0o777) << 16
        with self.assertRaises(RuntimeError):
            validate_member_path(member)

    def test_approved_pair_only(self):
        with io.BytesIO() as buffer:
            with zipfile.ZipFile(buffer, "w") as bundle:
                bundle.writestr("source/", b"")
                bundle.writestr("source/315500_234000.tif", b"test")
                bundle.writestr("source/315500_234000.tfw", b"0.05")
            buffer.seek(0)
            with zipfile.ZipFile(buffer) as bundle:
                self.assertEqual(set(approved_members(bundle, "315500_234000")),
                                 {"315500_234000.tif", "315500_234000.tfw"})

    def test_rejects_unexpected_missing_duplicate_and_unsafe_archive_entries(self):
        variants = [
            ["folder/315500_234000.tif"],
            ["folder/315500_234000.tif", "folder/315500_234000.tfw", "folder/unapproved.txt"],
            ["folder/315500_234000.tif", "other/315500_234000.tif", "folder/315500_234000.tfw"],
            ["../315500_234000.tif", "folder/315500_234000.tfw"],
            ["folder/315500_234000.tif", "folder/315500_234000.tfw", "/unapproved/"],
        ]
        for names in variants:
            with self.subTest(names=names), io.BytesIO() as buffer:
                with zipfile.ZipFile(buffer, "w") as bundle:
                    for name in names:
                        bundle.writestr(name, b"data")
                buffer.seek(0)
                with zipfile.ZipFile(buffer) as bundle, self.assertRaises(RuntimeError):
                    approved_members(bundle, "315500_234000")

    def test_destination_is_only_approved_in_scope_file(self):
        target = scoped_target("315500_234000", "315500_234000.tif")
        self.assertTrue(target.resolve().is_relative_to(WORK.resolve()))
        self.assertEqual(scoped_target("315500_234000", "315500_234000.tif", staged=True).suffix, ".part")
        for tile, name in (("../escape", "315500_234000.tif"),
                           ("315500_234000", "../315500_234000.tif"),
                           ("315500_234000", "unapproved.png")):
            with self.subTest(tile=tile, name=name), self.assertRaises(RuntimeError):
                scoped_target(tile, name)


class GridTests(unittest.TestCase):
    def test_exact_extent_and_spacing(self):
        self.assertEqual(SPACING, 0.1875)
        self.assertEqual(SIZE * SPACING, 768)
        self.assertEqual(BOUNDS, (315605, 234009, 316373, 234777))

    def test_northwest_pixel_centre(self):
        self.assertEqual(pixel_to_grid(0, 0), (315605.09375, 234776.90625))

    def test_southeast_pixel_centre(self):
        self.assertEqual(pixel_to_grid(4095, 4095), (316372.90625, 234009.09375))

    def test_origin_between_four_centre_pixels(self):
        self.assertEqual(grid_to_pixel(315989, 234393), (2047.5, 2047.5))

    def test_orientation_and_inverse(self):
        col, row = grid_to_pixel(316089, 234493)
        self.assertGreater(col, 2047.5)
        self.assertLess(row, 2047.5)
        np.testing.assert_allclose(pixel_to_grid(col, row), (316089, 234493), atol=1e-9, rtol=0)

    def test_ir_is_not_alpha(self):
        rgbi = np.array([[[40, 80, 120, 0], [1, 2, 3, 255]]], dtype=np.uint8)
        np.testing.assert_array_equal(rgb_bands(rgbi), [[[40, 80, 120], [1, 2, 3]]])

    def test_area_weights_normalized_and_preserve_constants(self):
        weights, start, stop = area_weights(15, 4)
        self.assertEqual((start, stop), (0, 15))
        np.testing.assert_allclose(np.asarray(weights.sum(axis=1)).ravel(), np.ones(4), atol=1e-15)
        np.testing.assert_allclose(weights.dot(np.full(15, 137)), np.full(4, 137), atol=1e-12)

    def test_exact_fractional_area_weights(self):
        weights, _, _ = area_weights(15, 4)
        np.testing.assert_allclose(weights.toarray()[0, :4], [4 / 15, 4 / 15, 4 / 15, 3 / 15])
        np.testing.assert_allclose(weights.dot(np.arange(15)), [1.4, 5.133333333333333,
                                                               8.866666666666667, 12.6], atol=1e-12)

    def test_chunked_filter_matches_whole_filter(self):
        whole, _, _ = area_weights(240, 64)
        raw = np.arange(240)
        expected = whole.dot(raw)
        pieces = []
        for start in range(0, 64, 16):
            weights, left, right = area_weights(240, 64, start, start + 16)
            pieces.extend(weights.dot(raw[left:right]))
        np.testing.assert_allclose(pieces, expected, atol=1e-12)

    def test_synthetic_seams_and_zero_ir_are_preserved(self):
        sources = []
        for tile, red in zip(TILES, (10, 20, 30, 40)):
            west, south = map(float, tile.split("_"))
            data = np.broadcast_to(np.array([red, 70, 90, 0], dtype=np.uint8), (10000, 10000, 4))
            sources.append({"tile": tile, "west": west, "east": west + 500,
                            "south": south, "north": south + 500, "nodata": None, "data": data})
        data, coverage, invalid = read_window(sources, 5539, 5541)
        self.assertTrue(np.all(coverage == 1))
        self.assertFalse(np.any(invalid))
        np.testing.assert_array_equal(data[:, 7899:7901, 0], [[20, 40], [10, 30]])
        self.assertTrue(np.all(data[..., 3] == 0))


class GeneratedArtifactTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sidecar = json.loads(SIDECAR.read_text(encoding="utf-8"))
        cls.manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        cls.sources = open_sources()
        with Image.open(PNG) as image:
            cls.mode, cls.size = image.mode, image.size
            cls.image_text = image.text
            cls.output = np.asarray(image).copy()

    def test_png_is_exact_rgb_and_hashed(self):
        self.assertEqual(self.mode, "RGB")
        self.assertEqual(self.size, (4096, 4096))
        self.assertEqual(sha256(PNG), self.sidecar["image_sha256"])
        self.assertEqual(PNG.stat().st_size, self.sidecar["image_bytes"])
        self.assertGreater(PNG.stat().st_size, 1_000_000)

    def test_sidecar_bounds_and_world_file(self):
        self.assertEqual(self.sidecar["extent_en_m"], list(BOUNDS))
        self.assertEqual(self.sidecar["world_file_A_D_B_E_C_F"],
                         [0.1875, 0, 0, -0.1875, 315605.09375, 234776.90625])
        self.assertEqual(self.sidecar["pixel_spacing_m"], [0.1875, 0.1875])
        contract = self.sidecar["ue_registration"]
        self.assertEqual(contract["consumers"], ["full-core roof", "full-core ground"])
        self.assertEqual(contract["uv_origin"], "top_left")
        self.assertEqual(contract["ue_uv"], ["u=0.5+Xcm/76800", "v=0.5+Ycm/76800"])

    def test_actual_source_crs_and_world_files(self):
        self.assertEqual(self.sidecar["source_crs"]["embedded_projected_epsg"], 29902)
        self.assertTrue(self.sidecar["working_registration"]["not_an_assertion_of_datum_equivalence"])
        for source in self.sources:
            geo = source["inspection"]["geotiff_metadata"]
            self.assertEqual(geo["ProjectedCSTypeGeoKey"], 29902)
            self.assertEqual(geo["GTRasterTypeGeoKey"], 1)
            self.assertIsNone(source["nodata"])
            self.assertTrue(all(f["published_hash_matches"] for f in source["inspection"]["files"]))

    def test_only_four_archives_and_bounded_total(self):
        tiles = self.manifest["tiles"]
        self.assertEqual(tuple(t["tile"] for t in tiles), TILES)
        total = sum((ROOT / t["archive_path"]).stat().st_size for t in tiles)
        self.assertEqual(total, self.manifest["archive_total_bytes"])
        self.assertLessEqual(total, BUDGET)
        for tile in tiles:
            self.assertIn("_orthophoto_", tile["archive_url"])
            self.assertEqual((ROOT / tile["archive_path"]).stat().st_size, tile["head_bytes"])
            download = json.loads((WORK / "provenance" / (tile["tile"] + "-download.json")).read_text())
            self.assertEqual(sha256(ROOT / tile["archive_path"]), download["archive_sha256"])
            with zipfile.ZipFile(ROOT / tile["archive_path"]) as bundle:
                members = approved_members(bundle, tile["tile"])
                self.assertEqual(len(members), 2)
        for source in self.sources:
            self.assertTrue(all(f["zip_member_path_validated"] and f["destination_within_imagery_subtree"]
                                for f in source["inspection"]["files"]))

    def test_coverage_no_empty_pixels(self):
        coverage = self.sidecar["validation"]["coverage"]
        self.assertEqual(coverage["native_core_pixels"], 15360 ** 2)
        self.assertEqual(coverage["covered_once_native_pixels"], 15360 ** 2)
        self.assertEqual(coverage["raster_extent_coverage_fraction"], 1.0)
        for key in ("uncovered_native_pixels", "overlapping_native_pixels",
                    "declared_nodata_native_pixels", "all_four_bands_zero_native_pixels"):
            self.assertEqual(coverage[key], 0)
        self.assertFalse(np.any(np.all(self.output == 0, axis=2)))

    def test_actual_white_source_gaps_are_detected_not_claimed_complete(self):
        coverage = self.sidecar["validation"]["coverage"]
        self.assertGreater(coverage["all_four_bands_white_native_pixels"], 1_000_000)
        self.assertFalse(coverage["complete_usable_coverage"])
        self.assertFalse(self.sidecar["validation"]["complete_usable_coverage_passed"])
        self.assertGreater(coverage["usable_imagery_fraction_conservative"], 0.9)
        self.assertLess(coverage["usable_imagery_fraction_conservative"], 0.99)
        self.assertEqual(len(coverage["large_white_void_components"]), 4)
        self.assertTrue(all(c["touches_core_boundary"] for c in coverage["large_white_void_components"]))
        with Image.open(WORK / "core-validity.png") as image:
            mask = np.asarray(image)
            self.assertEqual(image.mode, "L")
            self.assertEqual(image.size, (SIZE, SIZE))
            self.assertEqual(int(np.count_nonzero(mask == 0)), coverage["fully_void_output_pixels"])
            self.assertEqual(int(mask[0, 0]), 0)
            self.assertEqual(int(mask[2048, 2048]), 255)
        self.assertEqual(coverage["fully_void_output_pixels"] + coverage["fully_covered_output_pixels"] +
                         coverage["partially_covered_output_pixels"], SIZE ** 2)

    def test_histograms_reproduce_actual_output(self):
        validation = self.sidecar["validation"]
        for band in range(3):
            histogram = np.bincount(self.output[..., band].ravel(), minlength=256)
            np.testing.assert_array_equal(histogram, validation["histograms_rgb_256_bins"][band])
            self.assertEqual(int(histogram.sum()), SIZE ** 2)
            self.assertGreater(self.output[..., band].std(), 5)

    def test_three_known_coordinates_and_four_way_seam(self):
        checks = self.sidecar["validation"]["known_coordinate_checks"]
        self.assertGreaterEqual(len(checks), 3)
        for point in checks:
            col, row = point["containing_pixel_col_row"]
            expected, tiles = independent_pixel(self.sources, col, row)
            difference = np.abs(self.output[row, col].astype(int) - expected.astype(int))
            self.assertLessEqual(int(difference.max()), 1, point["name"])
        seam = checks[-1]
        self.assertEqual(len(seam["contributing_tiles"]), 4)

    def test_edges_and_seeded_pixels_against_independent_source_integral(self):
        random = np.random.default_rng(20150326)
        points = [(0, 0), (4095, 0), (0, 4095), (4095, 4095), (2106, 1477)]
        points += [tuple(map(int, p)) for p in random.integers(0, SIZE, size=(16, 2))]
        for col, row in points:
            expected, _ = independent_pixel(self.sources, col, row)
            difference = np.abs(self.output[row, col].astype(int) - expected.astype(int))
            self.assertLessEqual(int(difference.max()), 1, (col, row))

    def test_licence_and_attribution_are_carried_forward(self):
        provenance = self.sidecar["provenance"]
        self.assertEqual(provenance["authors"], AUTHORS)
        self.assertEqual(provenance["capture_date"], "2015-03-26")
        self.assertEqual(provenance["license"]["name"], "CC BY 4.0")
        self.assertEqual(provenance["license"]["url"], "https://creativecommons.org/licenses/by/4.0/")
        self.assertFalse(provenance["esri_imagery_used"])
        self.assertIn("18.75 cm", provenance["changes"])
        self.assertEqual(self.image_text["License"], provenance["license"]["url"])
        self.assertTrue(all(author in self.image_text["Attribution"] for author in AUTHORS))
        self.assertEqual(len(self.manifest["official_verification"]), 7)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    success = result.wasSuccessful()
    report = {"schema_version": 1, "completed_at_utc": now(), "passed": success,
              "tests_run": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
              "image_sha256": sha256(PNG) if PNG.exists() else None,
              "processor_sha256": sha256(WORK / "process_orthophotos.py"),
              "extractor_sha256": sha256(WORK / "inspect_orthophotos.py"),
              "test_script_sha256": sha256(Path(__file__)),
              "failure_details": [str(test) + "\n" + detail for test, detail in result.failures + result.errors]}
    write_json(WORK / "test-results.json", report)
    handoff = json.loads(HANDOFF.read_text(encoding="utf-8"))
    complete_coverage = handoff.get("complete_usable_coverage", False)
    partial_approved = handoff.get("ready_for_partial_import", False)
    ready = success and (complete_coverage or partial_approved)
    status = ("ready_for_partial_import" if success and partial_approved and not complete_coverage else
              "ready_for_import" if ready else
              "blocked_incomplete_source_coverage" if success else "tests_failed")
    handoff.update(status=status, ready_for_import=ready, updated_at_utc=now(),
                   tests=report, test_results_path=str((WORK / "test-results.json").relative_to(ROOT)))
    write_json(HANDOFF, handoff)
    progress(status, tests_passed=success,
             tests_run=result.testsRun,
             next_action=("Native editor coordinator may import the PNG using its sidecar." if ready else
                          "Bounded task stops at documented source-coverage blocker; candidate PNG is preserved."
                          if success else "Fix recorded test failures before importing."))
    raise SystemExit(0 if success else 1)
