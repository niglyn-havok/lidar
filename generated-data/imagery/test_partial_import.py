"""Verify the partial-import shader mask and immutable source-image contract."""

import json
import unittest

import numpy as np
from PIL import Image
from scipy.ndimage import binary_erosion

from acquire_orthophotos import HANDOFF, PROJECT, WORK, now, progress, sha256, write_json
from publish_partial_import import CONTRACT, MASK


class PartialImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
        with Image.open(MASK) as image:
            cls.mask = np.asarray(image).copy()
            cls.mode, cls.size = image.mode, image.size
        with Image.open(WORK / "core-validity.png") as image:
            cls.original_mask = np.asarray(image).copy()

    def test_mask_is_binary_aligned_and_hashed(self):
        self.assertEqual(self.mode, "L")
        self.assertEqual(self.size, (4096, 4096))
        np.testing.assert_array_equal(np.unique(self.mask), [0, 255])
        self.assertEqual(sha256(MASK), self.contract["mask_sha256"])
        self.assertEqual(self.contract["extent_en_m"], [315605, 234009, 316373, 234777])
        self.assertTrue(self.contract["top_origin"])

    def test_no_void_or_fractional_pixels_pass_and_bilinear_guard_matches(self):
        expected = binary_erosion(self.original_mask == 255, structure=np.ones((3, 3)), border_value=1)
        np.testing.assert_array_equal(self.mask == 255, expected)
        self.assertFalse(np.any((self.mask == 255) & (self.original_mask != 255)))
        self.assertEqual(int(self.mask[0, 0]), 0)
        self.assertEqual(int(self.mask[2048, 2048]), 255)

    def test_no_interpolation_contract_is_explicit(self):
        policy = self.contract["edge_policy"]
        self.assertFalse(policy["mask_srgb"])
        self.assertEqual(policy["mask_filter"], "nearest / point")
        self.assertEqual(policy["mask_mipmaps"], "disabled")
        self.assertIn("LOD0", policy["rgb_sampling"])

    def test_original_png_and_georeference_unchanged(self):
        baseline = json.loads((WORK / "partial-followup-baseline.json").read_text(encoding="utf-8"))
        for name, digest in baseline["preserve_unchanged_sha256"].items():
            self.assertEqual(sha256(PROJECT / "Content" / "Data" / name), digest)

    def test_four_gaps_have_native_and_irish_grid_bounds(self):
        self.assertEqual(len(self.contract["gaps"]), 4)
        for gap in self.contract["gaps"]:
            x0, y0, x1, y1 = gap["core_native_pixel_bounds_xyxy"]
            self.assertTrue(0 <= x0 < x1 <= 15360 and 0 <= y0 < y1 <= 15360)
            np.testing.assert_allclose(gap["irish_grid_bounds_wsen_m"],
                                       [315605+x0*0.05, 234777-y1*0.05,
                                        315605+x1*0.05, 234777-y0*0.05])
            self.assertEqual(sum(t["gap_native_pixels"] for t in gap["source_tiles"]),
                             gap["native_connected_component_pixels"])
            for tile in gap["source_tiles"]:
                left, top, right, bottom = tile["source_pixel_bounds_xyxy"]
                self.assertTrue(0 <= left < right <= 10000 and 0 <= top < bottom <= 10000)
        self.assertEqual(self.contract["large_gap_native_pixels"], 12374367)
        self.assertEqual(self.contract["small_white_or_saturated_native_pixels"], 23)

    def test_honest_partial_readiness_never_claims_full_coverage(self):
        handoff = json.loads(HANDOFF.read_text(encoding="utf-8"))
        self.assertTrue(handoff["ready_for_partial_import"])
        self.assertFalse(handoff["full_coverage"])
        self.assertFalse(handoff["final_complete_visual_gate_passed"])
        self.assertFalse(handoff["engine_or_tools_blocker"])
        self.assertIsNotNone(handoff["coverage_warning"])


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(PartialImportTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    report = {"completed_at_utc": now(), "passed": result.wasSuccessful(),
              "tests_run": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
              "mask_sha256": sha256(MASK), "contract_sha256": sha256(CONTRACT),
              "failure_details": [str(test) + "\n" + detail for test, detail in result.failures + result.errors]}
    write_json(WORK / "partial-import-test-results.json", report)
    handoff = json.loads(HANDOFF.read_text(encoding="utf-8"))
    handoff["partial_import_validation"] = report
    if not result.wasSuccessful():
        handoff.update(status="partial_import_tests_failed", ready_for_partial_import=False, ready_for_import=False)
    write_json(HANDOFF, handoff)
    progress(handoff["status"], partial_import_tests_passed=result.wasSuccessful(),
             partial_import_tests_run=result.testsRun)
    raise SystemExit(0 if result.wasSuccessful() else 1)
