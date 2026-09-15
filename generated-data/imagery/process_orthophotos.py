"""Render an exact, north-up 768 m RGB crop, without importing anything into Unreal."""

import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, PngImagePlugin
from pyproj import CRS
from scipy.ndimage import find_objects, label
from scipy.sparse import csr_matrix
import tifffile

from acquire_orthophotos import (
    ROOT, WORK, PROJECT, HANDOFF, MANIFEST, TILES, now, progress, sha256, write_json,
)


BOUNDS = (315605.0, 234009.0, 316373.0, 234777.0)
ORIGIN = (315989.0, 234393.0)
SIZE = 4096
SPACING = 768.0 / SIZE
NATIVE_SPACING = 0.05
PNG = PROJECT / "Content" / "Data" / "dublin-ortho.png"
SIDECAR = PNG.with_name("dublin-ortho-georeference.json")
CHECKS = (
    ("O'Connell Bridge / project origin", 315989.0, 234393.0),
    ("Spire axis / recovered recognition anchor", 315903.5, 234672.5),
    ("Contract calibration: 100 m east of origin", 316089.0, 234393.0),
    ("Four-tile junction / seam registration", 316000.0, 234500.0),
)


def pixel_to_grid(col, row):
    return BOUNDS[0] + (col + 0.5) * SPACING, BOUNDS[3] - (row + 0.5) * SPACING


def grid_to_pixel(easting, northing):
    return ((easting - BOUNDS[0]) / SPACING - 0.5,
            (BOUNDS[3] - northing) / SPACING - 0.5)


def area_weights(source_count, destination_count, start=0, stop=None):
    """Exact fractional-pixel box integrals; each destination row sums to one."""
    stop = destination_count if stop is None else stop
    scale = source_count / destination_count
    first_source = math.floor(start * scale)
    last_source = math.ceil(stop * scale)
    rows, cols, values = [], [], []
    for output in range(start, stop):
        left, right = output * scale, (output + 1) * scale
        for source in range(math.floor(left), math.ceil(right)):
            weight = (min(right, source + 1) - max(left, source)) / scale
            if weight > 0:
                rows.append(output - start)
                cols.append(source - first_source)
                values.append(weight)
    matrix = csr_matrix((values, (rows, cols)), dtype=np.float64,
                        shape=(stop - start, last_source - first_source))
    return matrix, first_source, last_source


def rgb_bands(rgbi):
    if rgbi.shape[-1] != 4:
        raise ValueError("Expected red, green, blue, infrared bands.")
    return rgbi[..., :3]


def open_sources():
    inspection = json.loads((WORK / "source-inspection.json").read_text(encoding="utf-8"))
    if set(t["tile"] for t in inspection["tiles"]) != set(TILES):
        raise RuntimeError("Inspected source set is not the exact four requested tiles.")
    sources = []
    for record in inspection["tiles"]:
        tile_id = record["tile"]
        path = WORK / "originals" / tile_id / (tile_id + ".tif")
        world_path = path.with_suffix(".tfw")
        expected = {Path(f["path"]).name: f["sha256"] for f in record["files"]}
        if sha256(path) != expected[path.name] or sha256(world_path) != expected[world_path.name]:
            raise RuntimeError(f"An original changed after inspection: {tile_id}")
        with tifffile.TiffFile(path) as image:
            page = image.pages[0]
            geo = image.geotiff_metadata
            if page.shape != (10000, 10000, 4) or page.dtype != np.uint8:
                raise RuntimeError(f"Unexpected source dimensions/bands/bit depth: {tile_id}")
            if not page.is_memmappable or geo["ProjectedCSTypeGeoKey"] != 29902:
                raise RuntimeError(f"Unexpected TIFF encoding or actual projected CRS: {tile_id}")
            if geo["GTRasterTypeGeoKey"] != 1:
                raise RuntimeError("Expected PixelIsArea GeoTIFF convention.")
            tie = geo["ModelTiepoint"]
            scale = geo["ModelPixelScale"]
            west = tie[3] - tie[0] * scale[0]
            north = tie[4] + tie[1] * scale[1]
            expected_tfw = [scale[0], 0, 0, -scale[1],
                            west + scale[0] / 2, north - scale[1] / 2]
            actual_tfw = [float(v) for v in world_path.read_text().split()]
            if not np.allclose(expected_tfw, actual_tfw, atol=1e-8, rtol=0):
                raise RuntimeError(f"GeoTIFF and TFW disagree: {tile_id}")
            if not np.allclose(scale[:2], [NATIVE_SPACING] * 2, atol=1e-12, rtol=0):
                raise RuntimeError("Unexpected native pixel spacing.")
            sw = [float(v) for v in tile_id.split("_")]
            if not np.allclose([west, north - 500], sw, atol=1e-8, rtol=0):
                raise RuntimeError(f"Tile name disagrees with embedded georeference: {tile_id}")
            nodata_tag = page.tags.get("GDAL_NODATA")
            nodata = float(nodata_tag.value) if nodata_tag else None
        sources.append({"tile": tile_id, "path": path, "west": west, "north": north,
                        "east": west + 500, "south": north - 500, "nodata": nodata,
                        "data": tifffile.memmap(path, mode="r"), "inspection": record})
    return sources


def read_window(sources, source_row_start, source_row_stop):
    """Assemble only the required 5 cm rows, crossing tile boundaries before filtering."""
    width = round((BOUNDS[2] - BOUNDS[0]) / NATIVE_SPACING)
    height = source_row_stop - source_row_start
    data = np.zeros((height, width, 4), dtype=np.uint8)
    coverage = np.zeros((height, width), dtype=np.uint8)
    declared_invalid = np.zeros((height, width), dtype=bool)
    for source in sources:
        tile_x = round((source["west"] - BOUNDS[0]) / NATIVE_SPACING)
        tile_y = round((BOUNDS[3] - source["north"]) / NATIVE_SPACING)
        x0, x1 = max(0, tile_x), min(width, tile_x + 10000)
        y0 = max(source_row_start, tile_y)
        y1 = min(source_row_stop, tile_y + 10000)
        if x1 <= x0 or y1 <= y0:
            continue
        destination = (slice(y0 - source_row_start, y1 - source_row_start), slice(x0, x1))
        patch = source["data"][y0 - tile_y:y1 - tile_y, x0 - tile_x:x1 - tile_x]
        data[destination] = patch
        coverage[destination] += 1
        if source["nodata"] is not None:
            # A per-band sentinel only masks pixels missing all RGB bands, not a zero IR value.
            declared_invalid[destination] = np.all(patch[..., :3] == source["nodata"], axis=2)
    return data, coverage, declared_invalid


def render(sources):
    native_size = round(768 / NATIVE_SPACING)
    horizontal, _, _ = area_weights(native_size, SIZE)
    output = np.empty((SIZE, SIZE, 3), dtype=np.uint8)
    validity = np.empty((SIZE, SIZE), dtype=np.uint8)
    diagnostics = {"native_core_pixels": native_size ** 2, "covered_once_native_pixels": 0,
                   "uncovered_native_pixels": 0, "overlapping_native_pixels": 0,
                   "declared_nodata_native_pixels": 0, "all_four_bands_zero_native_pixels": 0,
                   "all_four_bands_white_native_pixels": 0,
                   "all_rgb_zero_native_pixels": 0, "all_rgb_white_native_pixels": 0,
                   "ir_zero_with_nonzero_rgb_native_pixels": 0}
    for start in range(0, SIZE, 64):
        stop = min(SIZE, start + 64)
        vertical, y0, y1 = area_weights(native_size, SIZE, start, stop)
        raw, coverage, invalid = read_window(sources, y0, y1)
        diagnostics["covered_once_native_pixels"] += int(np.count_nonzero(coverage == 1))
        diagnostics["uncovered_native_pixels"] += int(np.count_nonzero(coverage == 0))
        diagnostics["overlapping_native_pixels"] += int(np.count_nonzero(coverage > 1))
        diagnostics["declared_nodata_native_pixels"] += int(np.count_nonzero(invalid))
        rgb = rgb_bands(raw)
        rgb_zero = np.all(rgb == 0, axis=2)
        diagnostics["all_rgb_zero_native_pixels"] += int(np.count_nonzero(rgb_zero))
        diagnostics["all_rgb_white_native_pixels"] += int(np.count_nonzero(np.all(rgb == 255, axis=2)))
        diagnostics["all_four_bands_zero_native_pixels"] += int(np.count_nonzero(np.all(raw == 0, axis=2)))
        white_void = np.all(raw == 255, axis=2)
        diagnostics["all_four_bands_white_native_pixels"] += int(np.count_nonzero(white_void))
        diagnostics["ir_zero_with_nonzero_rgb_native_pixels"] += int(
            np.count_nonzero((raw[..., 3] == 0) & ~rgb_zero))
        if np.any(coverage != 1) or np.any(invalid):
            raise RuntimeError(f"Missing, overlapping or nodata source coverage in rows {start}:{stop}.")
        if np.any(np.all(raw == 0, axis=2)):
            raise RuntimeError("Possible undeclared all-band-zero void in requested core; do not silently fill.")
        for band in range(3):
            x_filtered = horizontal.dot(rgb[..., band].T).T
            result = vertical.dot(x_filtered)
            output[start:stop, :, band] = np.floor(np.clip(result, 0, 255) + 0.5).astype(np.uint8)
        valid_area = vertical.dot(horizontal.dot((~white_void).T).T)
        validity[start:stop] = np.floor(np.clip(valid_area, 0, 1) * 255 + 0.5).astype(np.uint8)
        if start % 512 == 0:
            progress("rendering", completed_output_rows=stop, total_output_rows=SIZE)
    diagnostics["raster_extent_coverage_fraction"] = (
        diagnostics["covered_once_native_pixels"] / diagnostics["native_core_pixels"])
    diagnostics["inferred_white_void_fraction"] = (
        diagnostics["all_four_bands_white_native_pixels"] / diagnostics["native_core_pixels"])
    diagnostics["usable_imagery_fraction_conservative"] = 1 - diagnostics["inferred_white_void_fraction"]
    diagnostics["inferred_white_void_area_m2"] = (
        diagnostics["all_four_bands_white_native_pixels"] * NATIVE_SPACING ** 2)
    diagnostics["usable_imagery_area_m2_conservative"] = (
        (diagnostics["native_core_pixels"] - diagnostics["all_four_bands_white_native_pixels"])
        * NATIVE_SPACING ** 2)
    diagnostics["inferred_void_detection"] = (
        "All four source channels equal 255. Large white regions are visually confirmed survey "
        "gaps touching the core boundary. This conservative rule may also exclude tiny saturated "
        "pixels; the publisher supplied no validity/nodata mask. Zero IR is never used as alpha.")
    diagnostics["fully_void_output_pixels"] = int(np.count_nonzero(validity == 0))
    diagnostics["partially_covered_output_pixels"] = int(np.count_nonzero((validity > 0) & (validity < 255)))
    diagnostics["fully_covered_output_pixels"] = int(np.count_nonzero(validity == 255))
    labels, count = label(validity == 0)
    sizes = np.bincount(labels.ravel())
    slices = find_objects(labels)
    large_components = []
    for identifier in range(1, count + 1):
        if sizes[identifier] < 64:
            continue
        rows, cols = slices[identifier - 1]
        large_components.append({
            "output_pixels": int(sizes[identifier]),
            "bounds_pixel_edges_xyxy": [cols.start, rows.start, cols.stop, rows.stop],
            "bounds_en_m": [BOUNDS[0] + cols.start * SPACING, BOUNDS[3] - rows.stop * SPACING,
                            BOUNDS[0] + cols.stop * SPACING, BOUNDS[3] - rows.start * SPACING],
            "touches_core_boundary": (rows.start == 0 or cols.start == 0 or
                                      rows.stop == SIZE or cols.stop == SIZE)})
    diagnostics["large_white_void_components"] = sorted(large_components,
                                                       key=lambda item: -item["output_pixels"])
    diagnostics["complete_usable_coverage"] = diagnostics["all_four_bands_white_native_pixels"] == 0
    diagnostics["coverage_interpretation"] = (
        "Raster extent coverage is NOT usable imagery coverage. White source-survey gaps remain "
        "unchanged in the RGB output and are separately quantified/masked; no fill is invented. "
        "No source GDAL_NODATA is declared; all-four-band-zero pixels are additionally rejected. "
        "Shadows, occlusions and survey radiometric artifacts remain.")
    return output, diagnostics, validity


def independent_pixel(sources, col, row):
    """Direct 2-D weighted integration, independent of the sparse separable render."""
    e0 = BOUNDS[0] + col * SPACING
    e1 = e0 + SPACING
    n1 = BOUNDS[3] - row * SPACING
    n0 = n1 - SPACING
    result = np.zeros(3, dtype=np.float64)
    area = 0.0
    tiles = []
    for source in sources:
        west, east = max(e0, source["west"]), min(e1, source["east"])
        south, north = max(n0, source["south"]), min(n1, source["north"])
        if east <= west or north <= south:
            continue
        tiles.append(source["tile"])
        x0 = max(0, math.floor((west - source["west"]) / NATIVE_SPACING))
        x1 = min(10000, math.ceil((east - source["west"]) / NATIVE_SPACING))
        y0 = max(0, math.floor((source["north"] - north) / NATIVE_SPACING))
        y1 = min(10000, math.ceil((source["north"] - south) / NATIVE_SPACING))
        for sy in range(y0, y1):
            for sx in range(x0, x1):
                px0 = source["west"] + sx * NATIVE_SPACING
                py1 = source["north"] - sy * NATIVE_SPACING
                weight = (max(0, min(e1, px0 + NATIVE_SPACING) - max(e0, px0))
                          * max(0, min(n1, py1) - max(n0, py1 - NATIVE_SPACING)))
                result += source["data"][sy, sx, :3] * weight
                area += weight
    if not math.isclose(area, SPACING ** 2, abs_tol=1e-8):
        raise RuntimeError("Control pixel area lacks complete independent source coverage.")
    return np.floor(result / area + 0.5).astype(np.uint8), tiles


def validate(output, sources, coverage):
    if output.shape != (SIZE, SIZE, 3) or output.dtype != np.uint8:
        raise RuntimeError("Output shape/type mismatch.")
    histograms = [np.bincount(output[..., band].ravel(), minlength=256).tolist() for band in range(3)]
    statistics = []
    for band, name in enumerate(["red", "green", "blue"]):
        values = output[..., band]
        statistics.append({"band": name, "minimum": int(values.min()), "maximum": int(values.max()),
                           "mean": float(values.mean()), "stddev": float(values.std()),
                           "percentiles_1_5_50_95_99": np.percentile(values, [1, 5, 50, 95, 99]).tolist(),
                           "occupied_histogram_bins": int(np.count_nonzero(histograms[band]))})
        if values.std() < 5 or np.count_nonzero(histograms[band]) < 64:
            raise RuntimeError("Output is empty or unexpectedly flat.")
    known = []
    for name, easting, northing in CHECKS:
        colf, rowf = grid_to_pixel(easting, northing)
        col, row = math.floor(colf + 0.5), math.floor(rowf + 0.5)
        expected, contributing = independent_pixel(sources, col, row)
        actual = output[row, col]
        difference = int(np.max(np.abs(actual.astype(int) - expected.astype(int))))
        if difference > 1:
            raise RuntimeError(f"Independent source projection check failed at {name}")
        back = pixel_to_grid(colf, rowf)
        if not np.allclose(back, (easting, northing), atol=1e-9, rtol=0):
            raise RuntimeError("Pixel-centre inverse projection check failed.")
        known.append({"name": name, "grid_en_m": [easting, northing],
                      "pixel_centre_index_float_col_row": [colf, rowf],
                      "containing_pixel_col_row": [col, row], "pixel_centre_en_m": list(pixel_to_grid(col, row)),
                      "uv_top_origin": [(easting - BOUNDS[0]) / 768, (BOUNDS[3] - northing) / 768],
                      "ue_xy_cm": [(easting - ORIGIN[0]) * 100, -(northing - ORIGIN[1]) * 100],
                      "contributing_tiles": contributing, "output_rgb": actual.tolist(),
                      "independent_source_rgb": expected.tolist(), "maximum_channel_error": difference,
                      "validation_scope": "Coordinate orientation/registration and RGB source sampling, not surveyed GCP accuracy."})
    black = int(np.count_nonzero(np.all(output == 0, axis=2)))
    if black:
        raise RuntimeError(f"Unexpected empty black output pixels: {black}")
    return {"passed": True, "processing_validation_passed": True,
            "complete_usable_coverage_passed": coverage["complete_usable_coverage"],
            "coverage": coverage, "histograms_rgb_256_bins": histograms,
            "channel_statistics": statistics, "all_rgb_black_output_pixels": black,
            "all_rgb_white_output_pixels": int(np.count_nonzero(np.all(output == 255, axis=2))),
            "known_coordinate_checks": known,
            "known_coordinate_accuracy_caution": (
                "These are recovered project coordinates, not independent certified control points. "
                "Aerial building lean, shadow and historical change are not corrected.")}


def main():
    progress("processing")
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    sources = open_sources()
    output, coverage, validity = render(sources)
    validation = validate(output, sources, coverage)
    attribution = (manifest["title"] + " — " + ", ".join(manifest["authors"]) +
                   ". Survey: 26 March 2015. Source: NYU Faculty Digital Archive. CC BY 4.0.")
    modifications = (
        "Four source tiles mosaicked, cropped to the exact 768 m project core, infrared excluded "
        "(not alpha), and RGB area-resampled from 5 cm to 18.75 cm pixels. "
        "Original 8-bit RGB tone retained; no generative fill, sharpening, contrast or facade correction.")
    text = PngImagePlugin.PngInfo()
    text.add_itxt("Attribution", attribution)
    text.add_itxt("Source", manifest["collection_url"])
    text.add_itxt("License", manifest["license"]["url"])
    text.add_itxt("Modifications", modifications)
    PNG.parent.mkdir(parents=True, exist_ok=True)
    staged = PNG.with_suffix(".png.pending")
    Image.fromarray(output).save(staged, format="PNG", pnginfo=text, compress_level=6)
    with Image.open(staged) as check:
        if check.mode != "RGB" or check.size != (SIZE, SIZE) or not np.array_equal(np.asarray(check), output):
            raise RuntimeError("Saved PNG round-trip mismatch.")
    staged.replace(PNG)
    size = PNG.stat().st_size
    digest = sha256(PNG)
    metadata = {
        "schema_version": 1, "created_at_utc": now(), "image": PNG.name,
        "image_bytes": size, "image_sha256": digest, "dimensions_wh": [SIZE, SIZE],
        "mode": "RGB", "bits_per_channel": 8, "band_order": ["red", "green", "blue"],
        "fourth_source_band": "Infrared, deliberately not output and never interpreted as alpha",
        "extent_en_m": list(BOUNDS), "extent_order": ["west", "south", "east", "north"],
        "pixel_spacing_m": [SPACING, SPACING], "resolution_description": "18.75 cm output pixels, not a 5 cm PNG",
        "native_source_pixel_spacing_m": NATIVE_SPACING, "top_left_is_north_west": True,
        "pixel_convention": "PixelIsArea; image coordinates refer to pixel centres unless stated otherwise",
        "world_file_A_D_B_E_C_F": [SPACING, 0, 0, -SPACING,
                                  BOUNDS[0] + SPACING / 2, BOUNDS[3] - SPACING / 2],
        "gdal_geotransform": [BOUNDS[0], SPACING, 0, BOUNDS[3], 0, -SPACING],
        "pixel_to_grid": ["E = 315605 + (col + 0.5) * 0.1875",
                          "N = 234777 - (row + 0.5) * 0.1875"],
        "source_crs": {
            "embedded_projected_epsg": 29902, "name": CRS.from_epsg(29902).name,
            "canonical_projected_wkt": CRS.from_epsg(29902).to_wkt(),
            "raw_geokeys_retained_in": "source_tiles[].geotiff_metadata",
            "caution": "TIFF also carries user-defined GeographicTypeGeoKey=32767 and explicit Airy Modified ellipsoid parameters."},
        "working_registration": {
            "legacy_working_crs_label": "EPSG:29903 / TM75 / Irish Grid",
            "source_projected_crs_label": "EPSG:29902 / TM65 / Irish Grid",
            "operation": "Explicit identity in the shared numeric survey grid; no datum reprojection applied.",
            "reason": "Preserve the authoritative city-data-handoff numeric-grid contract shared with source LiDAR. Do not silently reproject one layer.",
            "not_an_assertion_of_datum_equivalence": True,
            "raw_las_offset_caveat": "Existing LAS custom CRS has +0.32 m easting/+0.08 m northing false-origin differences from canonical EPSG:29903; no per-layer correction applied.",
            "absolute_alignment_accuracy": "Not independently measured; pixel spacing is not survey/geodetic accuracy."},
        "ue_registration": {
            "origin_en_m": list(ORIGIN), "axis_basis": "+X east, +Y south, units centimetres",
            "consumers": ["full-core roof", "full-core ground"], "uv_origin": "top_left",
            "xy_bounds_cm": [-38400, -38400, 38400, 38400],
            "image_uv_top_origin": ["u=(E-315605)/768", "v=(234777-N)/768"],
            "ue_uv": ["u=0.5+Xcm/76800", "v=0.5+Ycm/76800"],
            "outside_core": "No halo pixels supplied. Do not stretch this 768 m image over an 800 m or 1600 m mesh."},
        "nodata": {"source_tag": None, "png_has_alpha": False,
                   "handling": "Verify geometric coverage; reject declared RGB nodata and all-four-band-zero voids. Separately flag all-four-band-white survey gaps, retain unchanged white RGB and legitimate zero IR.",
                   "inferred_coverage_mask": str((WORK / "core-validity.png").relative_to(ROOT)),
                   "mask_convention": "0 = inferred source white void, 255 = covered; intermediate values = covered area fraction times 255. This is a separate diagnostic, not an alpha interpretation of IR."},
        "resampling": {"method": "Exact separable area-weighted RGB integration",
                       "source_pixels_per_destination_axis": SPACING / NATIVE_SPACING,
                       "seams": "Join source windows before filtering; destination footprints can cross tile boundaries.",
                       "coordinate_approach": "Same inverse destination-to-source pixel-centre registration as legacy projection; exact area antialiasing replaces pointwise bilinear minification.",
                       "rounding": "floor(weighted_RGB + 0.5), clamp 0..255",
                       "colour_space": "Source 8-bit display RGB values retained; no source ICC profile or calibrated reflectance claim."},
        "provenance": {
            "creator_attribution": attribution, "authors": manifest["authors"],
            "capture_date": manifest["capture_date"], "processing_date": now()[:10],
            "derivative_creator": "GitHub Copilot for the DublinFlight project",
            "source_urls": [manifest["collection_url"], manifest["survey_description_url"]] +
                           [url for tile in manifest["tiles"] for url in (tile["record_url"], tile["archive_url"])],
            "license": manifest["license"], "changes": modifications,
            "distribution_notice": "Keep creator/source/licence attribution and modification notice with the image and in visible game/application credits; do not imply endorsement or impose additional licence restrictions.",
            "consumer_credit_placement": "HUD/about later, owned by the native editor coordinator; not implemented by this preprocessing task.",
            "esri_imagery_used": False, "originals_modified": False,
            "archive_total_bytes": manifest["archive_total_bytes"],
            "source_manifest": str(MANIFEST.relative_to(ROOT)),
            "source_inspection": str((WORK / "source-inspection.json").relative_to(ROOT))},
        "source_tiles": [{**source["inspection"],
                          "extent_en_m": [source["west"], source["south"], source["east"], source["north"]],
                         "band_interpretation": ["red", "green", "blue", "infrared"]}
                        for source in sources],
        "validation": validation,
    }
    write_json(SIDECAR, metadata)
    write_json(WORK / "validation.json", validation)
    Image.fromarray(validity).save(WORK / "core-validity.png")
    preview = Image.fromarray(output)
    preview.thumbnail((1024, 1024), Image.Resampling.LANCZOS)
    preview.save(WORK / "core-preview.png", pnginfo=text)
    blocker = None
    if not coverage["complete_usable_coverage"]:
        blocker = (
            f"The four licensed source GeoTIFFs contain {coverage['all_four_bands_white_native_pixels']} "
            f"all-band-white core pixels ({coverage['inferred_white_void_fraction'] * 100:.6f}% "
            f"or {coverage['inferred_white_void_area_m2']:.4f} m2), including "
            f"{len(coverage['large_white_void_components'])} large boundary-connected gaps. "
            "A complete accurately textured 768 m core cannot be made from these four source tiles. "
            "Candidate RGB crop and diagnostic mask are preserved; no fabricated/unlicensed fill "
            "or additional imagery downloads were performed.")
    progress("processed_awaiting_tests", image_path=str(PNG.relative_to(ROOT)),
             image_bytes=size, image_sha256=digest, coverage_fraction=coverage["raster_extent_coverage_fraction"],
             usable_imagery_fraction=coverage["usable_imagery_fraction_conservative"], blocker=blocker,
             next_action="Run test_orthophotos.py to finalize the coverage-aware handoff.")
    write_json(HANDOFF, {
        "schema_version": 1, "status": "processed_awaiting_tests", "ready_for_import": False,
        "updated_at_utc": now(), "image_path": str(PNG.relative_to(ROOT)),
        "georeference_path": str(SIDECAR.relative_to(ROOT)), "image_bytes": size,
        "image_sha256": digest, "dimensions_wh": [SIZE, SIZE], "extent_en_m": list(BOUNDS),
        "output_pixel_spacing_m": SPACING, "native_source_pixel_spacing_m": NATIVE_SPACING,
        "coverage": coverage, "license": manifest["license"], "attribution": attribution,
        "blocker": blocker, "candidate_png_available": True,
        "coverage_mask_path": str((WORK / "core-validity.png").relative_to(ROOT)),
        "complete_usable_coverage": coverage["complete_usable_coverage"],
        "source_crs": "Embedded EPSG:29902 TM65 / Irish Grid; shared numeric survey grid preserved",
        "registration_caution": metadata["working_registration"],
        "consumer_uv_contract": metadata["ue_registration"],
        "credit_display_handoff": metadata["provenance"]["consumer_credit_placement"],
        "visible_credits_required": True, "unreal_actions_performed": False,
        "import_coordinator_note": "NOT cleared as complete coverage while blocker is present. Candidate is exact RGB colour texture; white survey gaps are not fabricated. Any later import must use exact 768 m extent and sidecar UV formulas.",
        "provenance_directory": str((WORK / "provenance").relative_to(ROOT)),
        "validation_path": str((WORK / "validation.json").relative_to(ROOT)),
        "progress_file": str((WORK / "progress.json").relative_to(ROOT)),
        "archive_total_bytes": manifest["archive_total_bytes"],
    })
    print(json.dumps({"png": str(PNG.relative_to(ROOT)), "bytes": size,
                      "sha256": digest, "coverage_fraction": coverage["raster_extent_coverage_fraction"],
                      "status": "processed_awaiting_tests"}), flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        progress("blocked", blocker=str(exc))
        write_json(HANDOFF, {"schema_version": 1, "status": "blocked", "ready_for_import": False,
                            "updated_at_utc": now(), "blocker": str(exc),
                            "progress_file": str((WORK / "progress.json").relative_to(ROOT)),
                            "unreal_actions_performed": False})
        raise
