"""Generate the fixed DublinFlight city-data v1 contract, entirely offline.

Run acquire_osm.py once beforehand. Only generated-data and Content\\Data outputs
are written; no LAZ, images, Unreal APIs or original derivatives are processed.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import io
import json
import math
from pathlib import Path
import re
import time
import traceback
import unittest

import numpy as np
from scipy import ndimage
from scipy.interpolate import LinearNDInterpolator
from scipy.spatial import cKDTree
import shapely
from shapely.geometry import LineString, Point, Polygon, box
from shapely.geometry.polygon import orient
from shapely.ops import polygonize, split, unary_union
from pyproj import Transformer

ORIGIN = np.array([315989., 234393.])
EXTENT = 768
HALF = EXTENT / 2
HALO = 50
WATER_H = 1.1
CORE = box(*(ORIGIN - HALF), *(ORIGIN + HALF))
HALO_BOX = box(*(ORIGIN - HALF - HALO), *(ORIGIN + HALF + HALO))
ATTRIBUTION = [
    "2015 Aerial Laser and Photogrammetry Survey of Dublin City — Debra F. Laefer, "
    "Saleh Abuwarda, Anh-Vu Vo, Linh Truong-Hong, Hamid Gharibi. CC BY 4.0. "
    "https://archive.nyu.edu/handle/2451/38684 ; "
    "https://creativecommons.org/licenses/by/4.0/ . "
    "Modified: cropped cached raster, robust ground interpolation and simplified roof reconstruction.",
    "© OpenStreetMap contributors. ODbL 1.0. https://www.openstreetmap.org/copyright . "
    "Modified: projected, clipped, partitioned footprints and reconstructed geometry. "
    "Keep source and derived database attribution/share-alike obligations when distributing.",
    "Water H=1.1m is an artistic provisional tide, not a surveyed water level. "
    "Facades, foundation depth, bridge thickness and unresolved roofs are approximate.",
]


def workspace():
    return Path(__file__).resolve().parent.parent


def write_json(path, value, compact=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, ensure_ascii=False, allow_nan=False,
                      separators=(",", ":") if compact else None,
                      indent=None if compact else 2)
    pending = path.with_suffix(path.suffix + ".pending")
    pending.write_text(text + "\n", encoding="utf-8")
    pending.replace(path)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def survey_to_ue(points):
    result = np.asarray(points, dtype=float).copy()
    result[..., 0] = (result[..., 0] - ORIGIN[0]) * 100
    result[..., 1] = (ORIGIN[1] - result[..., 1]) * 100
    result[..., 2] *= 100
    return result


def uv_global(x, y):
    return [(x - (ORIGIN[0] - HALF)) / EXTENT,
            ((ORIGIN[1] + HALF) - y) / EXTENT]


def polygons(geometry):
    if geometry.is_empty:
        return []
    if geometry.geom_type == "Polygon":
        return [geometry]
    return [p for g in getattr(geometry, "geoms", []) for p in polygons(g)]


def source_id(element):
    return f"osm/{element['type']}/{element['id']}"


def present(tags, key):
    return tags.get(key, "no") not in ("no", "false", "0", "")


def project_line(geometry, transform):
    if not geometry or any(p is None or "lon" not in p or "lat" not in p for p in geometry):
        raise ValueError("missing/incomplete member geometry")
    lon, lat = np.array([(p["lon"], p["lat"]) for p in geometry]).T
    return np.column_stack(transform.transform(lon, lat))


def element_polygon(element, transform):
    if element["type"] == "way":
        points = project_line(element.get("geometry"), transform)
        if len(points) < 4 or np.linalg.norm(points[0] - points[-1]) > .05:
            raise ValueError("open/nonpolygon way")
        geometry = Polygon(points)
    elif element["type"] == "relation" and element.get("tags", {}).get("type") == "multipolygon":
        lines = {"outer": [], "inner": []}
        for member in element.get("members", []):
            if member["type"] != "way":
                continue
            role = member.get("role") or "outer"
            if role in lines:
                lines[role].append(LineString(project_line(member.get("geometry"), transform)))
        outer = list(polygonize(unary_union(lines["outer"])))
        inner = list(polygonize(unary_union(lines["inner"])))
        if not outer:
            raise ValueError("relation has no assembled outer rings")
        # Every member segment must belong to an assembled boundary; do not
        # accept a partial relation that happens to contain one closed ring.
        for role, rings in (("outer", outer), ("inner", inner)):
            assembled = unary_union([p.boundary for p in rings])
            if lines[role] and unary_union(lines[role]).difference(assembled).length > .01:
                raise ValueError(f"relation has unclosed {role} segments")
        geometry = unary_union(outer).difference(unary_union(inner))
    else:
        raise ValueError("non-multipolygon grouping relation")
    if not geometry.is_valid:
        raise ValueError("invalid polygon topology; deferred rather than silently repaired")
    geometry = shapely.set_precision(geometry, .001)
    if not geometry.is_valid or geometry.is_empty:
        raise ValueError("polygon precision reduction failed")
    return geometry


def normalize_buildings(elements, transform):
    records, diagnostics, suppressed = [], [], set()
    for element in sorted(elements, key=lambda e: (e["type"] != "relation", e["id"])):
        tags = element.get("tags", {})
        if not (present(tags, "building") or present(tags, "building:part")):
            continue
        if any(present(tags, k) for k in ("demolished:building", "abandoned:building")) or \
                tags.get("building") in ("construction", "ruins"):
            diagnostics.append({"id": source_id(element), "status": "excluded_lifecycle"})
            continue
        try:
            geometry = element_polygon(element, transform)
        except ValueError as error:
            diagnostics.append({"id": source_id(element), "status": "deferred_geometry",
                                "reason": str(error)})
            continue
        if not geometry.intersects(HALO_BOX):
            continue
        records.append({"id": source_id(element), "tags": tags, "geometry": geometry,
                        "part": present(tags, "building:part"), "source": element})
        if element["type"] == "relation":
            suppressed.update(f"osm/way/{m['ref']}" for m in element.get("members", [])
                              if m["type"] == "way" and m.get("role", "outer") in ("outer", ""))
    # All mapped outlines remain in the ground-exclusion mask, including
    # deferred volumes and parent areas later replaced by detailed parts.
    mask = unary_union([r["geometry"].intersection(HALO_BOX) for r in records])
    accepted = []
    claimed = Polygon()
    priority = lambda r: (not r["part"], r["source"]["type"] != "relation",
                          r["geometry"].area, r["id"])
    for record in sorted(records, key=priority):
        identity, geometry = record["id"], record["geometry"]
        if record["tags"].get("wikidata") == "Q1134365" or (
                "spire of dublin" in record["tags"].get("name", "").lower() and
                geometry.distance(Point(315903.5, 234672.5)) < 10):
            diagnostics.append({"id": identity, "status": "replaced_by_verified_spire_landmark"})
            continue
        if identity in suppressed and record["source"]["type"] == "way":
            diagnostics.append({"id": identity, "status": "duplicate_relation_member"})
            continue
        if not geometry.intersects(CORE):
            continue
        if not HALO_BOX.covers(geometry):
            diagnostics.append({"id": identity, "status": "deferred_beyond_halo"})
            continue
        clipped = geometry.intersection(CORE)
        residual = shapely.set_precision(clipped.difference(claimed), .001)
        removed = clipped.area - residual.area
        if residual.area < 1:
            diagnostics.append({"id": identity, "status": "duplicate_or_covered",
                                "removedAreaM2": round(removed, 3)})
            continue
        components = sorted(polygons(residual), key=lambda p: (-p.area, p.centroid.x, p.centroid.y))
        for index, component in enumerate(components):
            if component.area < 1:
                diagnostics.append({"id": identity, "status": "deferred_sliver",
                                    "areaM2": round(component.area, 4)})
                continue
            accepted.append({**record, "id": identity + (f"/volume/{index}" if len(components) > 1 else ""),
                             "geometry": orient(component, sign=1),
                             "cropClosure": not CORE.covers(geometry),
                             "overlapRemovedM2": round(removed, 3)})
        claimed = unary_union([claimed, residual])
    ids = [r["id"] for r in accepted]
    if len(ids) != len(set(ids)):
        raise ValueError("duplicate output IDs")
    return accepted, mask, diagnostics


def metres(value):
    if value is None:
        return None
    match = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*(m|metres?|meters?|ft|')?\s*", str(value))
    if not match:
        return None
    result = float(match[1])
    return result * .3048 if match[2] in ("ft", "'") else result


def hydrology(elements, transform):
    waters, islands, bridge_areas, bridge_lines, diagnostics = [], [], [], [], []
    source_records = []
    for element in elements:
        tags = element.get("tags", {})
        is_water = tags.get("natural") == "water" or tags.get("waterway") == "riverbank"
        is_island = tags.get("place") == "island"
        is_bridge_area = tags.get("man_made") == "bridge"
        if is_water or is_island or is_bridge_area:
            try:
                geometry = element_polygon(element, transform).intersection(HALO_BOX)
                if not geometry.is_empty:
                    record = {"id": source_id(element), "tags": tags, "geometry": geometry}
                    (waters if is_water else islands if is_island else bridge_areas).append(record)
                    source_records.append({"id": record["id"], "tags": tags,
                                           "areaWithinHaloM2": round(geometry.area, 2)})
            except ValueError as error:
                diagnostics.append({"id": source_id(element), "status": "deferred_water_or_bridge",
                                    "reason": str(error)})
        elif element["type"] == "way" and tags.get("highway") and present(tags, "bridge"):
            try:
                line = LineString(project_line(element.get("geometry"), transform))
            except ValueError as error:
                diagnostics.append({"id": source_id(element), "status": "deferred_bridge_line",
                                    "reason": str(error)})
                continue
            width = metres(tags.get("width"))
            method = "osm_width"
            if not width:
                lanes = metres(tags.get("lanes"))
                width = lanes * 3.1 + 1.5 if lanes else (
                    3.5 if tags.get("highway") in ("footway", "cycleway", "path", "pedestrian") else 9.)
                method = "assumed_width_from_lanes_or_highway"
            bridge_lines.append({"id": source_id(element), "tags": tags,
                                 "geometry": line.buffer(min(max(width, 2), 45) / 2, cap_style=2),
                                 "widthMethod": method, "widthM": width})
        elif element["type"] == "way" and tags.get("waterway") == "river":
            line = LineString(project_line(element.get("geometry"), transform)).intersection(HALO_BOX)
            source_records.append({"id": source_id(element), "tags": tags,
                                   "kind": "river_centerline_validation_only",
                                   "lengthWithinHaloM": round(line.length, 2)})
    water = unary_union([w["geometry"] for w in waters])
    water = water.difference(unary_union([i["geometry"] for i in islands]))
    # An area outline describes the whole bridge; its carriageways/footways
    # must not become overlapping slab copies.
    bridge_union = unary_union([b["geometry"] for b in bridge_areas])
    for bridge in bridge_lines:
        if bridge["geometry"].intersection(bridge_union).area > bridge["geometry"].area * .4:
            diagnostics.append({"id": bridge["id"], "status": "bridge_line_covered_by_area"})
            continue
        residual = bridge["geometry"].difference(bridge_union)
        if residual.intersection(water).area < 2:
            continue
        bridge_areas.append({**bridge, "geometry": residual})
        bridge_union = unary_union([bridge_union, residual])
    bridges = []
    claimed = Polygon()
    for bridge in sorted(bridge_areas, key=lambda b: b["id"]):
        geometry = bridge["geometry"].intersection(CORE).difference(claimed)
        for index, polygon in enumerate(polygons(geometry)):
            if polygon.area > 2 and polygon.intersection(water).area > 1:
                bridges.append({**bridge, "id": "bridge/" + bridge["id"] + f"/{index}",
                                "geometry": orient(shapely.set_precision(polygon, .001), sign=1)})
        claimed = unary_union([claimed, geometry])
    return water, bridges, diagnostics, source_records


class Raster:
    def __init__(self, path):
        with np.load(path) as data:
            self.bounds = data["bounds"].astype(float)
            self.cell = float(data["cell"])
            if self.cell != 1:
                raise ValueError("expected legacy 1m cached raster")
            x0, y0 = (ORIGIN - HALF - HALO).astype(int)
            width = int(EXTENT + HALO * 2)
            ix, iy = np.array([x0, y0]) - self.bounds[:2].astype(int)
            crop = np.s_[iy:iy + width + 1, ix:ix + width + 1]
            self.surface = data["surface"][crop].copy()
            self.valid = data["valid"][crop].copy()
            self.count = data["count"][crop].copy()
        self.x = np.arange(x0, x0 + self.surface.shape[1], dtype=float)
        self.y = np.arange(y0, y0 + self.surface.shape[0], dtype=float)
        self.xx, self.yy = np.meshgrid(self.x, self.y)

    def samples(self, geometry, erosion=1):
        inside = geometry.buffer(-erosion)
        if inside.is_empty or inside.area < 8:
            inside = geometry.buffer(-.25)
        minx, miny, maxx, maxy = geometry.bounds
        ix0, iy0 = np.maximum(np.floor([minx - self.x[0], miny - self.y[0]]).astype(int), 0)
        ix1, iy1 = np.minimum(np.ceil([maxx - self.x[0], maxy - self.y[0]]).astype(int) + 1,
                             [len(self.x), len(self.y)])
        selection = np.s_[iy0:iy1, ix0:ix1]
        xx, yy = self.xx[selection], self.yy[selection]
        mask = shapely.contains_xy(inside, xx, yy)
        measured = (mask & self.valid[selection] & (self.count[selection] >= 8) &
                    np.isfinite(self.surface[selection]))
        return (np.column_stack([xx[measured], yy[measured]]),
                self.surface[selection][measured].astype(float),
                int(mask.sum()), int(measured.sum()))


class Ground:
    def __init__(self, x, y, heights, valid, exclusion, min_samples=4):
        xx, yy = np.meshgrid(x, y)
        allowed = valid & np.isfinite(heights) & (heights > -.5) & (heights < 22)
        allowed &= ~shapely.intersects_xy(exclusion, xx, yy)
        points, values = [], []
        # Local lower quartiles reduce cars, canopy and residual facades without
        # mistaking the minimum under a roof for ground.
        for row in range(0, len(y), 8):
            for col in range(0, len(x), 8):
                selection = np.s_[row:row + 8, col:col + 8]
                keep = allowed[selection]
                if keep.sum() < min_samples:
                    continue
                z = heights[selection][keep]
                low = np.percentile(z, 25)
                cluster = z <= low + .5
                points.append([np.median(xx[selection][keep][cluster]),
                               np.median(yy[selection][keep][cluster])])
                values.append(low)
        points, values = np.asarray(points, dtype=float), np.asarray(values, dtype=float)
        if len(points) < 12:
            raise ValueError("insufficient building-excluded bare-ground support")
        distances, indices = cKDTree(points).query(points, k=min(12, len(points)))
        median = np.median(values[indices], axis=1)
        mad = np.median(np.abs(values[indices] - median[:, None]), axis=1)
        keep = (values < median + np.maximum(1.5, mad * 3)) & (values > median - 3)
        broad_distance, broad_indices = cKDTree(points).query(points, k=min(128, len(points)))
        broad_low = np.percentile(values[broad_indices], 20, axis=1)
        # Current OSM courtyards can contain a demolished 2015 rooftop. A
        # 12-neighbour median alone accepts such an entire elevated island.
        # Require support from the wider low surface with a gentle grade bound.
        keep &= values <= broad_low + 1.5 + .035 * broad_distance[:, -1]
        points, values = points[keep], values[keep]
        self.points, self.values = points, values
        self.tree = cKDTree(points)
        self.interpolator = LinearNDInterpolator(points, values)
        self.stats = {"eligibleRasterCells": int(allowed.sum()),
                      "lowSupportedNodes": len(points),
                      "rejectedElevatedOrDisconnectedNodes": int((~keep).sum()),
                      "nodeHeightRangeM": np.round([values.min(), values.max()], 3).tolist(),
                      "buildingBufferM": 4, "blockM": 8, "blockPercentile": 25,
                      "candidateSource": "Measured max-raster surface outside building/water/deck buffers; cached class2 terrain NOT used",
                      "wideSupportRule": "128 nearby low nodes; reject >p20 +1.5m +0.035*supportRadius"}

    def evaluate(self, coordinates, return_distance=False):
        coordinates = np.asarray(coordinates)
        heights = np.asarray(self.interpolator(coordinates))
        distances, indices = self.tree.query(coordinates)
        heights = np.where(np.isfinite(heights), heights, self.values[indices])
        return (heights, distances) if return_distance else heights


def robust_fit(design, z):
    beta = np.linalg.lstsq(design, z, rcond=None)[0]
    for _ in range(7):
        residual = z - design @ beta
        sigma = max(.15, 1.4826 * np.median(np.abs(residual - np.median(residual))))
        weights = np.minimum(1., 1.5 * sigma / np.maximum(np.abs(residual), 1e-6))
        beta = np.linalg.lstsq(design * np.sqrt(weights[:, None]),
                               z * np.sqrt(weights), rcond=None)[0]
    residual = z - design @ beta
    return beta, float(np.median(np.abs(residual))), float(np.mean(np.abs(residual) < .75))


def fit_roof(record, raster, ground):
    geometry, tags = record["geometry"], record["tags"]
    xy, z, cells, samples = raster.samples(geometry)
    origin = np.array(geometry.centroid.coords[0])
    ground_h, ground_distance = ground.evaluate([origin], return_distance=True)
    ground_h = float(ground_h[0])
    evidence = {"id": record["id"], "name": tags.get("name", ""), "samples": samples,
                "interiorCells": cells, "coverage": round(samples / max(cells, 1), 4),
                "groundM": round(ground_h, 3), "groundSupportDistanceM": round(float(ground_distance[0]), 2),
                "sourceTags": tags, "cropClosure": record.get("cropClosure", False),
                "overlapRemovedM2": record.get("overlapRemovedM2", 0),
                "footprintAreaM2": round(geometry.area, 3),
                "holes": len(geometry.interiors)}
    adequate = samples >= 12 and samples / max(cells, 1) >= .7
    if adequate:
        median = float(np.median(z))
        spread = max(.4, float(np.median(np.abs(z - median))))
        keep = np.abs(z - median) <= max(2., spread * 4)
        xy, z = xy[keep], z[keep]
        adequate = len(z) >= 12 and np.median(z) > ground_h + 2.2
    if adequate:
        local = xy - origin
        design = np.column_stack([np.ones(len(z)), local])
        beta, residual, support = robust_fit(design, z)
        evidence.update({"roofMedianM": round(float(np.median(z)), 3),
                         "planeResidualMedianM": round(residual, 3),
                         "planeSupport075m": round(support, 3)})
        slope = np.linalg.norm(beta[1:])
        ring_xy = np.vstack([np.asarray(r.coords)[:-1] for r in [geometry.exterior, *geometry.interiors]])
        plane_at_edges = beta[0] + (ring_xy - origin) @ beta[1:]
        if samples >= 30 and support >= .75 and residual <= .45 and slope <= .8 and \
                plane_at_edges.min() > ground_h + 2 and \
                plane_at_edges.max() < np.percentile(z, 99) + 2:
            roof = lambda points: beta[0] + (np.asarray(points) - origin) @ beta[1:]
            method = "lidar_robust_plane" if slope >= .035 else "lidar_near_flat_plane"
            return roof, [], method, .85 if samples >= 60 else .7, evidence
        # A two-plane ridge is accepted only when it improves the supported fit
        # substantially. The ridge becomes a constrained triangulation edge.
        if len(z) >= 60:
            _, axes = np.linalg.eigh(np.cov(local.T))
            best = None
            for axis in axes.T:
                u = local @ axis
                for offset in np.quantile(u, [.4, .5, .6]):
                    ridge_design = np.column_stack([design, np.abs(u - offset)])
                    fit, error, supported = robust_fit(ridge_design, z)
                    edges = np.column_stack([np.ones(len(ring_xy)), ring_xy - origin,
                                             np.abs((ring_xy - origin) @ axis - offset)]) @ fit
                    if fit[3] < -.08 and abs(fit[3]) < 1.5 and supported >= .72 and \
                            error < min(.5, residual * .7) and edges.min() > ground_h + 2 and \
                            edges.max() < np.percentile(z, 99) + 2:
                        if best is None or error < best[0]:
                            best = (error, fit, axis.copy(), float(offset), supported)
            if best:
                error, fit, axis, offset, supported = best
                def ridge_roof(points):
                    local_points = np.asarray(points) - origin
                    return fit[0] + local_points @ fit[1:3] + fit[3] * np.abs(local_points @ axis - offset)
                along = np.array([-axis[1], axis[0]])
                centre = origin + axis * offset
                ridge = LineString([centre - along * 2000, centre + along * 2000])
                evidence.update({"ridgeResidualMedianM": round(error, 3),
                                 "ridgeSupport075m": round(supported, 3)})
                return ridge_roof, [ridge], "lidar_supported_two_plane_ridge", .8, evidence
        level = float(np.median(z))
        evidence["lostRoofDetails"] = "Multi-plane/ornamental/vegetated or ambiguous raster: flat median cap; no claim of detailed roof recovery."
        return lambda points: np.full(len(points), level), [], "lidar_robust_median_flat_coarse", .55, evidence
    height = metres(tags.get("height"))
    if height and 2 < height < 160:
        method, confidence = "osm_height_fallback_no_supported_2015_roof", .4
    else:
        levels = metres(tags.get("building:levels"))
        height = levels * 3.2 if levels and 0 < levels <= 40 else None
        method, confidence = "osm_levels_assumed_3.2m_flat_fallback", .25
    if height is None:
        raise ValueError(f"no supported roof and no usable height/levels; samples={samples}")
    if samples >= 12 and np.median(z) < ground_h + 2.2:
        evidence["dateConflict"] = "Mapped footprint lacks elevated 2015 raster support; OSM fallback is not measured geometry."
    evidence["assumedHeightAboveIndependentGroundM"] = height
    return lambda points: np.full(len(points), ground_h + height), [], method, confidence, evidence


def fit_bridge(record, raster):
    geometry = record["geometry"]
    xy, z, _, _ = raster.samples(geometry, erosion=1.5)
    valid = (z > WATER_H + 1) & (z < 25)
    xy, z = xy[valid], z[valid]
    if len(z) < 8:
        raise ValueError("bridge lacks supported LiDAR deck height")
    origin = np.array(geometry.centroid.coords[0])
    _, axes = np.linalg.eigh(np.cov((xy - origin).T))
    axis = axes[:, -1]
    u = (xy - origin) @ axis
    centres, lows = [], []
    for begin in np.arange(u.min(), u.max() + 1, 5):
        selection = (u >= begin) & (u < begin + 5)
        if selection.sum() >= 3:
            centres.append(float(np.median(u[selection])))
            lows.append(float(np.percentile(z[selection], 25)))
    level = float(np.median(lows)) if len(lows) >= 3 else float(np.percentile(z, 25))
    detail = {"id": record["id"], "sourceTags": record["tags"],
              "samples": len(z), "lowSupportBins5m": len(lows),
              "widthMethod": record.get("widthMethod", "osm_bridge_area"),
              "heightM": round(level, 3), "assumedThicknessM": .7,
              "lostRoofDetails": "Railings/piers/trusses not reconstructed; low-raster deck with approximate thickness."}
    if len(lows) >= 6:
        scale = max(float(np.ptp(centres)) / 2, 1.)
        normalized = np.asarray(centres) / scale
        design = np.column_stack([np.ones(len(lows)), normalized, normalized ** 2])
        beta, residual, support = robust_fit(design, np.asarray(lows))
        is_footbridge = (
            "penny" in record["tags"].get("name", "").lower() or
            record["tags"].get("highway") in ("footway", "path", "pedestrian"))
        if is_footbridge and beta[2] < -.3 and abs(beta[2]) < 4 and residual < .45 and support >= .75:
            def curve(points):
                coordinate = (np.asarray(points) - origin) @ axis / scale
                return np.clip(beta[0] + beta[1] * coordinate + beta[2] * coordinate ** 2,
                               np.percentile(z, 5), np.percentile(z, 80))
            perpendicular = np.array([-axis[1], axis[0]])
            ridges = [LineString([origin + axis * c - perpendicular * 1000,
                                  origin + axis * c + perpendicular * 1000])
                      for c in np.arange(u.min() + 2.5, u.max(), 5)]
            detail.update({"curveResidualMedianM": round(residual, 3),
                           "curveSupport075m": round(support, 3)})
            return curve, ridges, "bridge_lidar_low_supported_arch_assumed_0.7m_slab", detail
    return (lambda points: np.full(len(points), level)), [], \
        "bridge_lidar_low_quartile_deck_assumed_0.7m_slab", detail


class MeshBuilder:
    def __init__(self, pivot=None):
        self.pivot = np.asarray(pivot if pivot is not None else [0, 0, 0], dtype=float)
        self.vertices, self.uv, self.triangles, self.materials = [], [], [], []
        self.lookup = {}

    def add_triangle(self, points, uvs, material=0):
        for point, uv in zip(points, uvs):
            position = tuple(np.round(survey_to_ue(point) - self.pivot, 2))
            texcoord = tuple(np.round(uv, 6))
            key = (position, texcoord, material)
            if key not in self.lookup:
                self.lookup[key] = len(self.vertices)
                self.vertices.append(list(position))
                self.uv.append(list(texcoord))
            self.triangles.append(self.lookup[key])
        self.materials.append(material)

    def payload(self, materials=False):
        result = {"verticesCm": self.vertices, "triangles": self.triangles, "uv": self.uv}
        if materials:
            result["materialIds"] = self.materials
        return result


def ccw_triangle(triangle):
    xy = np.array(triangle.exterior.coords[:3], dtype=float)
    cross = np.linalg.det(np.stack([xy[1] - xy[0], xy[2] - xy[0]]))
    return xy if cross > 0 else xy[[0, 2, 1]]


def solid_mesh(geometry, roof, bottom, ridges=()):
    pieces = [geometry]
    for ridge in ridges:
        pieces = [part for piece in pieces for part in polygons(split(piece, ridge))]
    # Union keeps ridge intersections on boundary rings, so roof, wall and
    # underside caps agree at every seam rather than creating T-junctions.
    boundary = orient(unary_union(pieces), sign=1)
    if boundary.geom_type != "Polygon":
        raise ValueError("unexpected disconnected solid")
    centre = np.array(boundary.centroid.coords[0])
    base = bottom if callable(bottom) else lambda points: np.full(len(points), bottom)
    pivot = np.round(survey_to_ue([*centre, float(base([centre])[0])]), 2)
    mesh = MeshBuilder(pivot)
    for piece in pieces:
        for triangle in shapely.constrained_delaunay_triangles(piece).geoms:
            xy = ccw_triangle(triangle)
            top = np.column_stack([xy, roof(xy)])
            mesh.add_triangle(top, [uv_global(*p) for p in xy], 0)
    for piece in pieces:
        for triangle in shapely.constrained_delaunay_triangles(piece).geoms:
            xy = ccw_triangle(triangle)[[0, 2, 1]]
            mesh.add_triangle(np.column_stack([xy, base(xy)]),
                              [uv_global(*p) for p in xy], 2)
    for ring in [boundary.exterior, *boundary.interiors]:
        points = np.array(ring.coords, dtype=float)
        u = 0.
        for first, second in zip(points[:-1], points[1:]):
            height = roof(np.array([first, second]))
            low = base(np.array([first, second]))
            if np.any(height <= low + .2):
                raise ValueError("roof intersects or approaches bottom closure")
            edge = float(np.linalg.norm(second - first))
            a, b = [*first, low[0]], [*second, low[1]]
            c, d = [*second, height[1]], [*first, height[0]]
            coords = [[u, 0], [u + edge, 0], [u + edge, height[1] - low[1]],
                      [u, height[0] - low[0]]]
            mesh.add_triangle([a, b, c], [coords[0], coords[1], coords[2]], 1)
            mesh.add_triangle([a, c, d], [coords[0], coords[2], coords[3]], 1)
            u += edge
    return mesh.payload(materials=True), pivot.tolist()


def validate_mesh(mesh, closed=False, check_roofs=True):
    vertices = np.asarray(mesh["verticesCm"], dtype=float)
    uv = np.asarray(mesh["uv"], dtype=float)
    raw_indices = mesh["triangles"]
    if len(raw_indices) % 3 or any(not isinstance(i, int) for i in raw_indices):
        raise ValueError("triangles must be a flat integer array divisible by three")
    if len(vertices) == 0:
        if raw_indices or len(uv):
            raise ValueError("inconsistent empty mesh")
        return {"vertices": 0, "triangles": 0}
    if vertices.shape[1:] != (3,) or uv.shape != (len(vertices), 2):
        raise ValueError("vertex/UV dimensions mismatch")
    if not np.isfinite(vertices).all() or not np.isfinite(uv).all():
        raise ValueError("nonfinite positions/UV")
    indices = np.asarray(raw_indices, dtype=np.int64).reshape(-1, 3)
    if indices.size == 0 or indices.min() < 0 or indices.max() >= len(vertices):
        raise ValueError("out-of-range or empty mesh indices")
    triangles = vertices[indices]
    normals = -np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    areas2 = np.linalg.norm(normals, axis=1)
    if np.any(areas2 < .001):
        raise ValueError(f"zero-area triangles: {int((areas2 < .001).sum())}")
    material = np.asarray(mesh.get("materialIds", np.zeros(len(indices), dtype=int)))
    if material.shape != (len(indices),) or np.any(~np.isin(material, [0, 1, 2])):
        raise ValueError("invalid material IDs")
    if check_roofs and np.any(normals[material == 0, 2] <= 0):
        raise ValueError("roof does not face up in UE clockwise convention")
    result = {"vertices": len(vertices), "triangles": len(indices),
              "minimumTriangleAreaCm2": round(float(areas2.min() / 2), 6)}
    if closed:
        _, welded = np.unique(vertices, axis=0, return_inverse=True)
        faces = welded[indices]
        directed = np.concatenate([faces[:, [0, 1]], faces[:, [1, 2]], faces[:, [2, 0]]])
        edges, counts = np.unique(np.sort(directed, axis=1), axis=0, return_counts=True)
        if np.any(counts != 2):
            raise ValueError(f"nonmanifold/open geometric edges: {int((counts != 2).sum())}")
        direction = np.where(directed[:, 0] < directed[:, 1], 1, -1)
        _, edge_map = np.unique(np.sort(directed, axis=1), axis=0, return_inverse=True)
        if np.any(np.bincount(edge_map, weights=direction) != 0):
            raise ValueError("inconsistent face orientation across solid edges")
        volume = -float(np.einsum("ij,ij->i", triangles[:, 0],
                                 np.cross(triangles[:, 1], triangles[:, 2])).sum() / 6)
        if volume <= 0:
            raise ValueError("nonpositive UE clockwise signed volume")
        result.update({"weldedVertices": int(welded.max() + 1), "closedEdges": len(edges),
                       "volumeM3": round(volume / 1e6, 5)})
    return result


def planar_water(geometry):
    mesh = MeshBuilder()
    for polygon in polygons(geometry.intersection(CORE)):
        for triangle in shapely.constrained_delaunay_triangles(polygon).geoms:
            xy = ccw_triangle(triangle)
            mesh.add_triangle(np.column_stack([xy, np.full(3, WATER_H)]),
                              [uv_global(*p) for p in xy])
    return mesh.payload()


def terrain_mesh(ground, water):
    x = np.arange(-HALF, HALF + 1, 2) + ORIGIN[0]
    y = ORIGIN[1] - np.arange(-HALF, HALF + 1, 2)
    xx, yy = np.meshgrid(x, y)
    xy = np.column_stack([xx.ravel(), yy.ravel()])
    heights, distances = ground.evaluate(xy, return_distance=True)
    # Unsupported interiors are interpolated, never sampled from roof-labelled
    # class 2. All regular slots remain present for deterministic runtime chunks.
    heights = ndimage.gaussian_filter(heights.reshape(xx.shape), sigma=.8).ravel()
    spire_distance = np.linalg.norm(xy - [315903.5, 234672.5], axis=1)
    spire_blend = np.clip((6 - spire_distance) / 3, 0, 1)
    heights = heights * (1 - spire_blend) + 6.45 * spire_blend
    positions = survey_to_ue(np.column_stack([xy, heights]))
    uv = np.column_stack([(xx.ravel() - x[0]) / EXTENT, (y[0] - yy.ravel()) / EXTENT])
    size = len(x)
    row, col = np.mgrid[:size - 1, :size - 1]
    a = (row * size + col).ravel()
    triangles = np.concatenate([np.column_stack([a, a + size, a + 1]),
                                np.column_stack([a + 1, a + size, a + size + 1])])
    # Retain grid slots but remove triangles whose centroids are in real water.
    # Shoreline quantization is <= one 2m cell, not an imagery-derived river.
    centres = xy[triangles].mean(axis=1)
    cut = shapely.intersects_xy(water, centres[:, 0], centres[:, 1])
    triangles = triangles[~cut]
    payload = {"verticesCm": np.round(positions, 2).tolist(),
               "triangles": triangles.ravel().tolist(), "uv": np.round(uv, 6).tolist()}
    stats = {"gridShape": [size, size], "spacingM": 2, "waterCutTriangles": int(cut.sum()),
             "heightRangeM": np.round([heights.min(), heights.max()], 3).tolist(),
             "supportDistanceP50P95MaxM": np.round(np.percentile(distances, [50, 95, 100]), 2).tolist(),
             "interpolatedSlotsFurtherThan40m": int((distances > 40).sum()),
             "shorelineMethod": "OSM water-polygon triangle-centroid cut; <=2.83m cell-diagonal approximation",
             "underwaterVertices": "Unused grid slots have interpolated bank heights, no bed/bathymetry claim",
             "buildingInteriorSamplesUsed": 0, "certifiedBareEarth": False,
             "spireGradeAnchor": "Prior report H6.45m; artistic 3m-radius level pad blending to interpolated ground by6m to avoid floating restored base"}
    return payload, stats


def spire():
    xy = np.array([315903.5, 234672.5])
    base, tip, segments = 6.45, 124.609, 40
    pivot = survey_to_ue([*xy, base])
    mesh = MeshBuilder(pivot)
    angles = np.linspace(0, 2 * np.pi, segments, endpoint=False)
    bottom = xy + 1.5 * np.column_stack([np.cos(angles), np.sin(angles)])
    top = xy + .08 * np.column_stack([np.cos(angles), np.sin(angles)])
    u = 0.
    for i in range(segments):
        j = (i + 1) % segments
        a, b = [*bottom[i], base], [*bottom[j], base]
        c, d = [*top[j], tip], [*top[i], tip]
        next_u = u + float(np.linalg.norm(bottom[j] - bottom[i]))
        mesh.add_triangle([a, b, c], [[u, 0], [next_u, 0], [next_u, tip - base]], 1)
        mesh.add_triangle([a, c, d], [[u, 0], [next_u, tip - base], [u, tip - base]], 1)
        u = next_u
        mesh.add_triangle([[*xy, base], b, a],
                          [uv_global(*xy), uv_global(*bottom[j]), uv_global(*bottom[i])], 2)
        mesh.add_triangle([[*xy, tip], d, c],
                          [uv_global(*xy), uv_global(*top[i]), uv_global(*top[j])], 0)
    return {"id": "landmark/spire", "name": "The Spire",
            "confidence": .9, "heightMethod": "prior_verified_report_raw_tip_procedural_taper",
            "pivotCm": pivot.tolist(), **mesh.payload(materials=True)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    root = workspace()
    output = root / "DublinFlight" / "Content" / "Data" / "dublin-city.json"
    handoff_path = root / "DublinFlight" / "Saved" / "Automation" / "data-generation-handoff.json"
    generated = root / "generated-data"
    if args.validate_only:
        payload = json.loads(output.read_text(encoding="utf-8"))
        print(json.dumps(validate_output(payload)))
        return
    started = time.monotonic()
    handoff = {"schema_version": 1, "status": "in_progress", "stage": "offline_generation",
               "output": "Content\\Data\\dublin-city.json", "blockers": [], "counts": {},
               "tests": {"status": "pending"}, "updatedUtc": datetime.now(timezone.utc).isoformat()}
    def stage(name, **extra):
        handoff.update(stage=name, updatedUtc=datetime.now(timezone.utc).isoformat(), **extra)
        write_json(handoff_path, handoff)
        print(name, flush=True)
    try:
        stage("verifying_cached_sources")
        surface_path = root / "artwork" / "surface.npz"
        baseline = sha256(surface_path)
        transform = Transformer.from_crs(4326, 29903, always_xy=True)
        osm, sources = {}, {}
        for name in ("buildings", "hydrology"):
            source = generated / "sources" / f"osm-{name}.json"
            record = json.loads((generated / "sources" / f"osm-{name}-provenance.json").read_text(encoding="utf-8"))
            if source.stat().st_size > 10_000_000 or sha256(source) != record["sha256"]:
                raise ValueError(f"{name} source failed size/hash verification")
            osm[name] = json.loads(source.read_text(encoding="utf-8"))["elements"]
            sources[name] = record
        records, building_mask, diagnostics = normalize_buildings(osm["buildings"], transform)
        water, bridges, hydro_diagnostics, hydro_sources = hydrology(osm["hydrology"], transform)
        diagnostics.extend(hydro_diagnostics)
        stage("cleaning_ground", counts={"normalizedBuildingVolumes": len(records),
                                        "bridgeFootprints": len(bridges)},
              surfaceBoundsEN=[314950, 233400, 316950, 235350], sources=sources)
        raster = Raster(surface_path)
        bridge_mask = unary_union([b["geometry"] for b in bridges])
        exclusion = unary_union([building_mask.buffer(4), water.buffer(2), bridge_mask.buffer(2),
                                 Point(315903.5, 234672.5).buffer(6)])
        ground = Ground(raster.x, raster.y, raster.surface,
                        raster.valid & (raster.count >= 8), exclusion)
        terrain, terrain_stats = terrain_mesh(ground, water)
        water_mesh = planar_water(water)
        stage("reconstructing_closed_buildings", ground={**ground.stats, **terrain_stats})
        buildings, evidence = [], []
        for index, record in enumerate(records):
            try:
                roof, ridges, method, confidence, detail = fit_roof(record, raster, ground)
                boundary_xy = np.array(record["geometry"].exterior.coords)
                bottom = float(np.min(ground.evaluate(boundary_xy))) - 1
                detail["bottomMethod"] = "independent_boundary_ground_min_minus_1m_foundation"
                minimum_height = metres(record["tags"].get("min_height"))
                if minimum_height and 0 < minimum_height < 150:
                    bottom = float(np.median(ground.evaluate(boundary_xy))) + minimum_height
                    detail["bottomMethod"] = "osm_min_height_above_independent_ground"
                mesh, pivot = solid_mesh(record["geometry"], roof, bottom, ridges)
                validation = validate_mesh(mesh, closed=True)
                buildings.append({"id": record["id"], "name": record["tags"].get("name", ""),
                                  "confidence": confidence, "heightMethod": method,
                                  "pivotCm": pivot, **mesh})
                evidence.append({**detail, "heightMethod": method, "confidence": confidence,
                                 "bottomM": round(bottom, 3), "validation": validation,
                                 "roofPlanarPartitionCount": len(ridges) + 1})
            except (ValueError, shapely.errors.GEOSException) as error:
                diagnostics.append({"id": record["id"], "status": "deferred_unsafe_or_unsupported_volume",
                                    "reason": str(error)})
            if (index + 1) % 200 == 0:
                stage("reconstructing_closed_buildings",
                      counts={"processed": index + 1, "accepted": len(buildings),
                              "diagnosticRecords": len(diagnostics)})
        stage("separating_bridge_decks_and_spire")
        for record in bridges:
            try:
                roof, ridges, method, detail = fit_bridge(record, raster)
                mesh, pivot = solid_mesh(record["geometry"], roof,
                                         lambda points: roof(points) - .7, ridges)
                validation = validate_mesh(mesh, closed=True)
                buildings.append({"id": record["id"], "name": record["tags"].get("name", "Liffey bridge deck"),
                                  "confidence": .65, "heightMethod": method,
                                  "pivotCm": pivot, **mesh})
                evidence.append({**detail, "heightMethod": method,
                                 "validation": validation})
            except (ValueError, shapely.errors.GEOSException) as error:
                diagnostics.append({"id": record["id"], "status": "deferred_unsafe_bridge",
                                    "reason": str(error)})
        buildings.append(spire())
        payload = {"schemaVersion": 1, "originEN": ORIGIN.astype(int).tolist(),
                   "extentMeters": EXTENT, "attribution": ATTRIBUTION,
                   "terrain": terrain, "water": water_mesh, "buildings": buildings}
        stage("validating_all_geometry")
        validation = validate_output(payload)
        stage("running_regression_tests")
        test_stream = io.StringIO()
        suite = unittest.defaultTestLoader.discover(str(generated), pattern="test_city_pipeline.py")
        test_result = unittest.TextTestRunner(stream=test_stream, verbosity=2).run(suite)
        tests = {"status": "passed" if test_result.wasSuccessful() else "failed",
                 "run": test_result.testsRun, "failures": len(test_result.failures),
                 "errors": len(test_result.errors), "output": test_stream.getvalue()}
        write_json(generated / "test-results.json", tests)
        if not test_result.wasSuccessful():
            raise ValueError("regression tests failed: " + test_stream.getvalue())
        if sha256(surface_path) != baseline:
            raise ValueError("original surface hash changed during generation")
        stage("writing_atomic_output", validation=validation)
        write_json(output, payload, compact=True)
        # Validate persisted bytes rather than only in-memory structures.
        validate_output(json.loads(output.read_text(encoding="utf-8")))
        operation = transform.get_last_used_operation()
        counts = {**validation, "heightMethods": dict(Counter(b["heightMethod"] for b in buildings)),
                  "diagnosticStatuses": dict(Counter(d["status"] for d in diagnostics)),
                  "deferredVolumes": sum(d["status"].startswith("deferred") for d in diagnostics),
                  "buildingsWithHoles": sum(e.get("holes", 0) > 0 for e in evidence),
                  "cropClosedVolumes": sum(e.get("cropClosure", False) for e in evidence)}
        caveats = [
            "2015-03-26 LiDAR versus acquired 2026 OSM; changed structures and source registration are not independently surveyed.",
            "Numeric EPSG:29903 legacy convention retained. LAS custom false-origin discrepancy (+0.32,+0.08m) and legacy lower-left bin placement remain; no silent per-layer offsets.",
            "Ground uses low supported exterior surface samples only, 4m building exclusion and wider low-support screening; cached class2 terrain is not used. Interpolation beneath buildings is inferred and support distances are recorded.",
            "Ground can contain residual unmapped features; ground node heights are bounded and robustly screened, not certified bare-earth DTM.",
            "Water H=1.1m is an artistic provisional tide. Water is OSM polygon geometry with holes/islands retained; no bathymetry is inferred.",
            "Terrain shoreline uses 2m grid triangle-centroid cut; bank discrepancy <=2.83m; exact OSM water geometry retained.",
            "Complex roofs downgraded to footprint-constrained flat robust median with explicit per-volume method; ornamental details, trees and facade detail are not recovered from max-raster data.",
            "Bridge decks are separate low-raster-height, approximate-width/0.7m-thick closed slabs; a supported footbridge arch can be fitted. Piers, railings and trusses are not reconstructed.",
            "The 1m maximum-return/median raster can blend bridge railings, vehicles and deck; low-quantile bridge heights are supported visual proxies, not certified deck levels.",
            "Boundary building footprints are clipped and CLOSED at the exact 768m core to keep roof UVs in [0,1]; artificial crop closure walls and 1m foundations are flagged.",
            "OSM outlines are planimetrically partitioned: detailed parts win, relation-member duplicates suppressed, uncovered parent residuals retained; vertically stacked parts become a coarse non-overlapping XY partition.",
            "Meshes have intentional UV/hard-edge seam vertices. Closure is tested after exact positional welding; every geometric edge has two oppositely directed incident triangles.",
            "Wall UVs are metres: U cumulative boundary distance, V height above the local bottom; roof UVs remain global top-down normalized coordinates. Facades are generic, not measured.",
            "UE clockwise winding: outward normal = cross(C-A,B-A); signed volume is NEGATIVE of conventional right-hand determinant formula.",
            "Spire restored from verified prior handoff/report, not remeasured: base H6.45m, tip H124.609m, diameter3m, tip radius0.08m.",
            "OSM Spire shell suppressed in favour of one restored landmark; an artistic 3m-radius H6.45m grade pad blends out by6m, preventing a floating base.",
            "No orthophoto, Esri imagery, imagery masks, LAZ, native source/config, editor or asset modifications were made by this pipeline.",
            "This verifies offline geometry only, not saved-map appearance, collision, destruction or frame-rate acceptance.",
        ]
        provenance = {
            "schemaVersion": 1, "generatedUtc": datetime.now(timezone.utc).isoformat(),
            "originEN": ORIGIN.tolist(), "extentMeters": EXTENT, "coreBoundsEN": list(CORE.bounds),
            "haloBoundsEN": list(HALO_BOX.bounds), "crs": "EPSG:29903",
            "transformOperation": operation.definition, "transformAccuracyM": operation.accuracy,
            "surface": {"path": "artwork\\surface.npz", "sha256": baseline,
                        "sourceBoundsEN": raster.bounds.tolist(), "haloShapeYX": list(raster.surface.shape),
                        "cellM": raster.cell, "measuredHaloFraction": round(float(raster.valid.mean()), 6)},
            "sources": sources, "hydrologySources": hydro_sources,
            "spireSource": {"path": "snowglobe\\provenance.json",
                            "sha256": sha256(root / "snowglobe" / "provenance.json"),
                            "field": "spire_reconstruction",
                            "verification": "Read report and city-data-handoff; no new raw-point measurement"},
            "attribution": ATTRIBUTION, "ground": {**ground.stats, **terrain_stats},
            "counts": counts, "buildings": evidence, "diagnostics": diagnostics, "caveats": caveats,
            "tests": {k: v for k, v in tests.items() if k != "output"},
            "output": {"path": "DublinFlight\\Content\\Data\\dublin-city.json",
                       "bytes": output.stat().st_size, "sha256": sha256(output)},
            "runtimeSeconds": round(time.monotonic() - started, 2),
            "dependencies": {"numpy": np.__version__, "shapely": shapely.__version__,
                             "GEOS": shapely.geos_version_string},
        }
        write_json(generated / "city-provenance.json", provenance)
        np.savez_compressed(generated / "clean-ground-support.npz",
                            pointsEN=ground.points, heightsM=ground.values)
        stage("complete", status="complete",
              counts=counts, validation={"status": "passed", **validation},
              tests=provenance["tests"],
              ground=provenance["ground"], sourceSurface=provenance["surface"],
              outputBytes=output.stat().st_size, outputSha256=sha256(output),
              runtimeSeconds=provenance["runtimeSeconds"],
              provenance="generated-data\\city-provenance.json", caveats=caveats)
        print(json.dumps({"bytes": output.stat().st_size, "seconds": provenance["runtimeSeconds"],
                          "counts": counts}, indent=2))
    except Exception as error:
        stage("failed", status="blocked", blockers=[str(error)], traceback=traceback.format_exc())
        raise


def validate_output(payload):
    if payload["schemaVersion"] != 1 or payload["originEN"] != [315989, 234393] or payload["extentMeters"] != 768:
        raise ValueError("fixed coordinate/schema contract changed")
    terrain = validate_mesh(payload["terrain"])
    water = validate_mesh(payload["water"])
    ids = [b["id"] for b in payload["buildings"]]
    if len(ids) != len(set(ids)):
        raise ValueError("nonunique building IDs")
    stats = [validate_mesh(b, closed=True) for b in payload["buildings"]]
    for b in payload["buildings"]:
        pivot = np.asarray(b["pivotCm"])
        if pivot.shape != (3,) or not np.isfinite(pivot).all():
            raise ValueError("invalid building pivot")
        world = np.asarray(b["verticesCm"]) + pivot
        if np.any(np.abs(world[:, :2]) > HALF * 100 + .1):
            raise ValueError(f"{b['id']} outside exact core")
        faces = np.asarray(b["triangles"]).reshape(-1, 3)
        roof_indices = np.unique(faces[np.asarray(b["materialIds"]) == 0])
        uv = np.asarray(b["uv"])[roof_indices]
        if np.any(uv < -1e-6) or np.any(uv > 1 + 1e-6):
            raise ValueError("roof UV outside core")
        expected = .5 + world[roof_indices, :2] / (EXTENT * 100)
        if not np.allclose(uv, expected, atol=1e-6):
            raise ValueError("roof UV does not follow global topdown contract")
    return {"terrainVertices": terrain["vertices"], "terrainTriangles": terrain["triangles"],
            "waterVertices": water["vertices"], "waterTriangles": water["triangles"],
            "buildingVolumes": len(stats), "buildingVertices": sum(s["vertices"] for s in stats),
            "buildingTriangles": sum(s["triangles"] for s in stats),
            "bridgeVolumes": sum(b["id"].startswith("bridge/") for b in payload["buildings"]),
            "closedSolidFailuresInOutput": 0}


if __name__ == "__main__":
    main()
