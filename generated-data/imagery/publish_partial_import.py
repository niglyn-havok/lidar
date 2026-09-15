"""Publish honest partial-image coverage without changing the RGB PNG or its georeference."""

import json
import math

import numpy as np
from PIL import Image, PngImagePlugin
from scipy.ndimage import binary_erosion, find_objects, label

from acquire_orthophotos import HANDOFF, PROJECT, ROOT, WORK, now, progress, sha256, write_json
from process_orthophotos import BOUNDS, NATIVE_SPACING, PNG, SIDECAR, SIZE, SPACING, open_sources


MASK = PROJECT / "Content" / "Data" / "dublin-ortho-validity.png"
CONTRACT = WORK / "partial-import-contract.json"


def native_gap(source_list, component):
    box = component["bounds_pixel_edges_xyxy"]
    native_size = round(768 / NATIVE_SPACING)
    scale = SPACING / NATIVE_SPACING
    for padding in (16, 64, 256):
        x0 = max(0, math.floor(box[0] * scale) - padding)
        y0 = max(0, math.floor(box[1] * scale) - padding)
        x1 = min(native_size, math.ceil(box[2] * scale) + padding)
        y1 = min(native_size, math.ceil(box[3] * scale) + padding)
        white = np.zeros((y1 - y0, x1 - x0), dtype=bool)
        for source in source_list:
            tx = round((source["west"] - BOUNDS[0]) / NATIVE_SPACING)
            ty = round((BOUNDS[3] - source["north"]) / NATIVE_SPACING)
            left, right = max(x0, tx), min(x1, tx + 10000)
            top, bottom = max(y0, ty), min(y1, ty + 10000)
            if left < right and top < bottom:
                patch = source["data"][top - ty:bottom - ty, left - tx:right - tx]
                white[top - y0:bottom - y0, left - x0:right - x0] = np.all(patch == 255, axis=2)
        labels, count = label(white)
        sizes = np.bincount(labels.ravel())
        if count == 0:
            raise RuntimeError("Previously validated gap disappeared from original TIFFs.")
        identifier = int(np.argmax(sizes[1:]) + 1)
        rows, cols = find_objects(labels)[identifier - 1]
        touches_crop = ((rows.start == 0 and y0 != 0) or
                        (cols.start == 0 and x0 != 0) or
                        (rows.stop == white.shape[0] and y1 != native_size) or
                        (cols.stop == white.shape[1] and x1 != native_size))
        if touches_crop:
            continue
        gx0, gy0, gx1, gy1 = x0 + cols.start, y0 + rows.start, x0 + cols.stop, y0 + rows.stop
        per_tile = []
        for source in source_list:
            tx = round((source["west"] - BOUNDS[0]) / NATIVE_SPACING)
            ty = round((BOUNDS[3] - source["north"]) / NATIVE_SPACING)
            left, right = max(gx0, tx), min(gx1, tx + 10000)
            top, bottom = max(gy0, ty), min(gy1, ty + 10000)
            if left >= right or top >= bottom:
                continue
            inside = labels[top - y0:bottom - y0, left - x0:right - x0] == identifier
            ys, xs = np.nonzero(inside)
            if len(xs):
                per_tile.append({
                    "tile": source["tile"],
                    "source_pixel_bounds_xyxy": [int(left - tx + xs.min()), int(top - ty + ys.min()),
                                                  int(left - tx + xs.max() + 1), int(top - ty + ys.max() + 1)],
                    "gap_native_pixels": int(len(xs))})
        pixels = int(sizes[identifier])
        return {
            "core_native_pixel_bounds_xyxy": [gx0, gy0, gx1, gy1],
            "irish_grid_bounds_wsen_m": [BOUNDS[0] + gx0 * NATIVE_SPACING,
                                         BOUNDS[3] - gy1 * NATIVE_SPACING,
                                         BOUNDS[0] + gx1 * NATIVE_SPACING,
                                         BOUNDS[3] - gy0 * NATIVE_SPACING],
            "native_connected_component_pixels": pixels,
            "area_m2": round(pixels * NATIVE_SPACING ** 2, 6),
            "source_tiles": per_tile, "output_fully_void_component": component,
            "bounds_convention": "Half-open pixel-edge bounds [left,top,right,bottom], zero-based, north-first. Rectangles enclose the gap; not every enclosed pixel is void.",
            "confidence": "High for this large boundary-connected component; not a publisher-provided nodata declaration.",
            "evidence": [
                "Every component sample is exactly [255,255,255,255], including infrared.",
                "The region is spatially continuous, crosses many expected roofs/streets and touches the core boundary.",
                "Sharp clipped survey-footprint boundaries are visible in the unmodified ortho preview, not roof outlines.",
                f"Component covers {pixels} native 5 cm samples; small saturated roof pixels are classified separately.",
                "Official NYU documentation states that peripheral tiles can be only partially covered."]}
    raise RuntimeError("Gap exceeded bounded native inspection windows; do not publish incomplete gap bounds.")


def main():
    baseline_path = WORK / "partial-followup-baseline.json"
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    for name, expected in baseline["preserve_unchanged_sha256"].items():
        if sha256(PROJECT / "Content" / "Data" / name) != expected:
            raise RuntimeError(f"Preserved image/georeference changed: {name}")
    original_hashes = {PNG.name: sha256(PNG), SIDECAR.name: sha256(SIDECAR)}
    handoff = json.loads(HANDOFF.read_text(encoding="utf-8"))
    coverage = handoff["coverage"]
    with Image.open(WORK / "core-validity.png") as image:
        if image.mode != "L" or image.size != (SIZE, SIZE):
            raise RuntimeError("Validated fractional coverage mask dimensions/type changed.")
        fractional = np.asarray(image).copy()
    fully_known = fractional == 255
    # One source-output texel guard excludes bilinear RGB taps adjacent to a white/partial texel.
    safe_known = binary_erosion(fully_known, structure=np.ones((3, 3), dtype=bool), border_value=1)
    binary = np.where(safe_known, 255, 0).astype(np.uint8)
    text = PngImagePlugin.PngInfo()
    text.add_text("MaskSemantics", "255=known-source RGB with one-texel boundary guard; 0=explicit generic neutral material")
    text.add_text("Sampling", "Linear data, point/nearest sampling, no mipmaps, clamp, no interpolation or blending")
    Image.fromarray(binary).save(MASK, pnginfo=text)
    with Image.open(MASK) as image:
        if image.mode != "L" or image.size != (SIZE, SIZE) or not np.array_equal(np.asarray(image), binary):
            raise RuntimeError("Published coverage mask round-trip failed.")
    sources = open_sources()
    gaps = [{"gap_id": f"gap-{index + 1}", **native_gap(sources, component)}
            for index, component in enumerate(coverage["large_white_void_components"])]
    reported_large_pixels = sum(gap["native_connected_component_pixels"] for gap in gaps)
    contract = {
        "schema_version": 1, "created_at_utc": now(),
        "ready_for_partial_import": True, "full_coverage": False,
        "final_complete_visual_gate_passed": False,
        "image_preserved_unchanged": original_hashes,
        "mask_path": str(MASK.relative_to(ROOT)), "mask_bytes": MASK.stat().st_size,
        "mask_sha256": sha256(MASK), "dimensions_wh": [SIZE, SIZE], "mode": "L",
        "bits_per_pixel": 8, "pixel_spacing_m": SPACING, "extent_en_m": list(BOUNDS),
        "top_origin": True, "alignment": "Exactly the same pixel centres and extent as dublin-ortho.png; no Y flip.",
        "uv_formulas": ["u=0.5+Xcm/76800", "v=0.5+Ycm/76800"],
        "mask_values": {"255": "Use original licensed RGB imagery.",
                        "0": "Use explicitly generic neutral rooftop/ground; not measured colour and not invented replacement imagery."},
        "mask_derivation": "Threshold validated fractional coverage at exactly 255, then erode valid pixels by a 3x3 footprint (one output texel, 18.75 cm) only around internal invalid/partial coverage. This suppresses bilinear RGB white fringes.",
        "edge_policy": {
            "mask_srgb": False, "mask_filter": "nearest / point", "mask_mipmaps": "disabled",
            "mask_address_u_v": "clamp", "compression": "Lossless single-channel data; preserve exact 0/255.",
            "shader": "Point-sample mask at shared UV; branch/step at 0.5. No interpolated mask, no feathering, no temporal/dither blend.",
            "rgb_sampling": "Point or bilinear LOD0, clamp. The one-texel mask guard covers bilinear LOD0 support; higher RGB mip levels/anisotropic footprints are NOT certified by this mask.",
            "outside_core": "Do not stretch or repeat the 768 m texture; use neutral material outside the exact extent."},
        "valid_output_pixels_before_guard": int(fully_known.sum()),
        "valid_output_pixels_after_guard": int(safe_known.sum()),
        "extra_neutral_boundary_guard_pixels": int(fully_known.sum() - safe_known.sum()),
        "shader_imagery_fraction_after_guard": float(safe_known.mean()),
        "conservative_native_usable_fraction": coverage["usable_imagery_fraction_conservative"],
        "gaps": gaps, "large_gap_native_pixels": reported_large_pixels,
        "small_white_or_saturated_native_pixels": coverage["all_four_bands_white_native_pixels"] - reported_large_pixels,
        "confidence_limit": "Large components are high-confidence survey gaps. Tiny all-band-white pixels could be real saturation; conservatively neutral, never inferred imagery.",
        "credit_text": handoff["attribution"],
        "credits_placement": "Consumer HUD/about later; preserve creator/source/CC BY 4.0 links and modification notice.",
        "coverage_warning_not_engine_blocker": "Partial rendering is approved with this mask and explicitly generic neutral materials. Missing source coverage is not an engine/tools failure.",
        "validation": {"passed": True, "binary_values": np.unique(binary).tolist(),
                       "no_valid_pixel_comes_from_partial_or_void_mask": bool(np.all(~safe_known | fully_known)),
                       "original_image_and_georeference_unchanged": True,
                       "large_gap_count": len(gaps)}}
    if len(gaps) != 4 or contract["small_white_or_saturated_native_pixels"] < 0:
        raise RuntimeError("Gap accounting failed.")
    write_json(CONTRACT, contract)
    for name, expected in original_hashes.items():
        if sha256(PROJECT / "Content" / "Data" / name) != expected:
            raise RuntimeError("Unexpected modification to immutable imagery output.")
    handoff.update(
        status="ready_for_partial_import", ready_for_partial_import=True,
        ready_for_import=True, full_coverage=False, final_complete_visual_gate_passed=False,
        updated_at_utc=now(), coverage_warning=(handoff.get("coverage_warning") or handoff.get("blocker") or
                                               "Known partial source coverage; generic neutral material is required in masked regions."),
        blocker=None, complete_usable_coverage=False,
        validity_image_path=str(MASK.relative_to(ROOT)),
        partial_import_contract_path=str(CONTRACT.relative_to(ROOT)),
        mask_contract=contract, engine_or_tools_blocker=False,
        import_coordinator_note="Proceed with honest partial city import. Shared full-core roof/ground UVs; use binary point-sampled validity and explicitly generic neutral material where zero. Full visual-completeness gate remains unpassed.")
    write_json(HANDOFF, handoff)
    progress("ready_for_partial_import", ready_for_import=True, ready_for_partial_import=True,
             full_coverage=False, blocker=None, final_complete_visual_gate_passed=False,
             partial_import_contract_path=str(CONTRACT.relative_to(ROOT)),
             next_action="Coordinator may import partial imagery now; bounded licensed gap-fill source investigation continues.")
    print(json.dumps({"ready_for_partial_import": True, "full_coverage": False,
                      "mask_path": str(MASK.relative_to(ROOT)), "mask_bytes": MASK.stat().st_size,
                      "native_usable_fraction": coverage["usable_imagery_fraction_conservative"],
                      "shader_imagery_fraction": contract["shader_imagery_fraction_after_guard"],
                      "large_gap_native_pixels": reported_large_pixels,
                      "small_white_or_saturated_pixels": contract["small_white_or_saturated_native_pixels"]}), flush=True)


if __name__ == "__main__":
    main()
