"""Add deterministic per-building sRGB8 tint/style colours, never alter geometry.

Run from any directory: python <workspace>\\generated-data\\apply_facades.py
Use --check-only to verify persisted outputs without writing. Only building
colorsRGBA, facade provenance and the two automation handoffs are written.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import re
import unittest

import numpy as np

STYLE_CODES = {
    "red_brick": 1, "brown_brick": 2, "cool_stone": 3, "warm_stone": 4,
    "cream_render": 5, "grey_render": 6, "modern_glazing": 7, "metal_structure": 8,
}
CSS_COLOURS = {
    "white": (255, 255, 255), "black": (0, 0, 0), "red": (255, 0, 0),
    "brown": (165, 42, 42), "grey": (128, 128, 128), "gray": (128, 128, 128),
    "lightgrey": (211, 211, 211), "lightgray": (211, 211, 211),
    "darkgrey": (169, 169, 169), "darkgray": (169, 169, 169),
    "aquamarine": (127, 255, 212), "beige": (245, 245, 220),
    "silver": (192, 192, 192), "tan": (210, 180, 140),
    "ivory": (255, 255, 240), "wheat": (245, 222, 179),
    "maroon": (128, 0, 0), "navy": (0, 0, 128), "blue": (0, 0, 255),
    "green": (0, 128, 0), "yellow": (255, 255, 0), "orange": (255, 165, 0),
}


def encode(value, sorted_keys=False):
    return (json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=sorted_keys,
                       separators=(",", ":")) + "\n").encode("utf-8")


def encode_city(value, newline):
    return encode(value)[:-1] + newline


def digest(value):
    return hashlib.sha256(value).hexdigest()


def without_colours(city):
    return {**city, "buildings": [
        {k: v for k, v in b.items() if k != "colorsRGBA"} for b in city["buildings"]]}


def geometry_digest(city):
    fields = ("id", "pivotCm", "verticesCm", "triangles", "uv", "materialIds")
    geometry = {k: city[k] for k in ("schemaVersion", "originEN", "extentMeters", "terrain", "water")}
    geometry["buildings"] = [{k: b[k] for k in fields} for b in city["buildings"]]
    return digest(encode(geometry, sorted_keys=True))


def parse_colour(value):
    if isinstance(value, (tuple, list)):
        if len(value) == 3 and all(type(v) is int and 0 <= v <= 255 for v in value):
            return tuple(value)
        raise ValueError("RGB components must be exactly three integer bytes")
    if not isinstance(value, str):
        raise ValueError("colour must be a whitelisted CSS name, hex or integer RGB")
    text = value.strip().lower()
    if text in CSS_COLOURS:
        return CSS_COLOURS[text]
    if re.fullmatch(r"#[0-9a-f]{6}", text):
        return tuple(int(text[i:i + 2], 16) for i in (1, 3, 5))
    if re.fullmatch(r"#[0-9a-f]{3}", text):
        return tuple(int(c * 2, 16) for c in text[1:])
    match = re.fullmatch(r"rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)", text)
    if match:
        return parse_colour([int(v) for v in match.groups()])
    raise ValueError("unknown/non-CSS colour; no exact RGB claim made")


def inferred_rgb(profile, identity, profiles):
    base = parse_colour(profiles[profile]["colours_srgb_hex"][0])
    # Only inferred tints receive a tiny deterministic brightness variation.
    delta = hashlib.sha256(identity.encode("utf-8")).digest()[0] % 7 - 3
    return tuple(max(0, min(255, channel + delta)) for channel in base)


def choose_profile(material, colour_text, rgb, suggested):
    if material is None:
        return suggested
    text = str(material).lower()
    if "glass" in text or "glazing" in text:
        return "modern_glazing"
    if any(word in text for word in ("steel", "metal", "aluminium", "aluminum")):
        return "metal_structure"
    if "brick" in text:
        if str(colour_text).strip().lower() == "brown":
            return "brown_brick"
        if str(colour_text).strip().lower() == "red":
            return "red_brick"
        if rgb is not None:
            return "brown_brick" if max(rgb) < 135 else "red_brick"
        return suggested if suggested in ("red_brick", "brown_brick") else "red_brick"
    if any(word in text for word in ("stone", "granite", "marble")):
        if rgb is not None and rgb[0] - rgb[2] >= 12:
            return "warm_stone"
        return suggested if suggested in ("cool_stone", "warm_stone") else "cool_stone"
    if any(word in text for word in ("render", "plaster", "stucco", "concrete")):
        if rgb is not None and rgb[0] - rgb[2] >= 12:
            return "cream_render"
        return suggested if suggested in ("cream_render", "grey_render") else "grey_render"
    raise ValueError("unsupported asserted material; contextual style explicitly inferred")


def resolve(building, evidence, profiles):
    identity = building["id"]
    if identity == "landmark/spire":
        return [192, 192, 192, 8], {
            "profile": "metal_structure", "materialSource": "USER_ASSERTED_STEEL_SPIRE",
            "colourSource": "USER_REQUESTED_SILVER_GRAY", "facadeEvidenceConfidence": "explicit_user_override",
            "sourceReference": "Current authorised implementation request; no new material survey",
        }, []
    if identity.startswith("bridge/"):
        return [128, 128, 128, 8], {
            "profile": "metal_structure", "materialSource": "NON_FACADE_UNUSED",
            "colourSource": "NON_FACADE_UNUSED", "facadeEvidenceConfidence": "not_applicable",
            "note": "Bridge material ignores vertex colour; this is not a surveyed bridge-material assertion.",
        }, []
    inherited = evidence.get("inherited_candidates", {})
    material = evidence.get("known_material")
    colour = evidence.get("known_colour")
    material_source = "SOURCE_ASSERTED" if material is not None else "CONTEXTUAL_INFERENCE"
    colour_source = "SOURCE_ASSERTED" if colour is not None else "INFERRED_PROFILE_TINT"
    material_reference = evidence.get("field_sources", {}).get("known_material")
    colour_reference = evidence.get("field_sources", {}).get("known_colour")
    if material is None and "building:material" in inherited:
        material = inherited["building:material"]["value"]
        material_source = "INHERITED_CANDIDATE"
        material_reference = inherited["building:material"]
    if colour is None and "building:colour" in inherited:
        colour = inherited["building:colour"]["value"]
        colour_source = "INHERITED_CANDIDATE"
        colour_reference = inherited["building:colour"]
    suggested = (evidence.get("palette_suggestion") or {}).get("palette_profile")
    if suggested not in STYLE_CODES:
        raise ValueError(f"{identity}: missing/unknown inferred profile")
    rgb, errors = None, []
    if colour is not None:
        try:
            rgb = parse_colour(colour)
        except ValueError as error:
            errors.append({"id": identity, "field": "colour", "rejectedValue": colour,
                           "reason": str(error), "fallback": "explicitly inferred profile tint"})
            colour_source = "REJECTED_COLOUR_INFERRED_TINT"
    try:
        profile = choose_profile(material, colour, rgb, suggested)
    except ValueError as error:
        errors.append({"id": identity, "field": "material", "rejectedValue": material,
                       "reason": str(error), "fallback": "labelled contextual profile"})
        profile, material_source = suggested, "REJECTED_MATERIAL_CONTEXTUAL_INFERENCE"
    if rgb is None:
        rgb = inferred_rgb(profile, identity, profiles)
    confidence = ("source_asserted_not_currently_surveyed" if material_source == "SOURCE_ASSERTED"
                  else "parent_candidate_unverified" if material_source == "INHERITED_CANDIDATE"
                  else "inferred_not_survey_truth")
    record = {
        "profile": profile, "materialSource": material_source, "colourSource": colour_source,
        "materialAssertion": material, "colourAssertion": colour,
        "materialReference": material_reference, "colourReference": colour_reference,
        "sourceEvidenceLevel": evidence.get("evidencelevel"),
        "facadeEvidenceConfidence": confidence,
        "geometryConfidence": building["confidence"],
        "geometryConfidenceIsNotFacadeConfidence": True,
        "startDate": evidence.get("start_date"), "referenceIds": evidence.get("reference_ids", []),
    }
    return [*rgb, STYLE_CODES[profile]], record, errors


def decorate(city, evidence):
    entries = evidence["buildings"]
    ids = [b["id"] for b in city["buildings"]]
    if len(set(ids)) != len(ids) or set(ids) != set(entries):
        raise ValueError("facade evidence must match every existing building ID exactly")
    before = encode(without_colours(city))
    original_geometry = geometry_digest(city)
    buildings, provenance, errors = [], {}, []
    for building in city["buildings"]:
        rgba, record, problems = resolve(building, entries[building["id"]], evidence["palette_profiles"])
        if len(rgba) != 4 or any(type(v) is not int or not 0 <= v <= 255 for v in rgba) or rgba[3] not in range(1, 9):
            raise ValueError("invalid sRGB byte/style encoding")
        buildings.append({**building, "colorsRGBA": [rgba[:] for _ in building["verticesCm"]]})
        provenance[building["id"]] = {"rgba": rgba, **record}
        errors.extend(problems)
    result = {**city, "buildings": buildings}
    if encode(without_colours(result)) != before or geometry_digest(result) != original_geometry:
        raise ValueError("non-colour content or bit-exact geometry changed")
    return result, provenance, errors


def summarise(city, records):
    ordinary = [b for b in city["buildings"] if not b["id"].startswith(("bridge/", "landmark/"))]
    counts = Counter(records[b["id"]]["materialSource"] for b in ordinary)
    colour_counts = Counter(records[b["id"]]["colourSource"] for b in ordinary)
    area, material_area, colour_area = 0., Counter(), Counter()
    for building in ordinary:
        positions = np.asarray(building["verticesCm"], dtype=float)
        indices = np.asarray(building["triangles"], dtype=np.int64).reshape(-1, 3)
        walls = positions[indices[np.asarray(building["materialIds"]) == 1]]
        value = float(np.linalg.norm(np.cross(walls[:, 1] - walls[:, 0],
                                             walls[:, 2] - walls[:, 0]), axis=1).sum() / 20000)
        area += value
        material_area[records[building["id"]]["materialSource"]] += value
        colour_area[records[building["id"]]["colourSource"]] += value
    return {
        "buildingIds": len(city["buildings"]), "ordinaryBuildingIds": len(ordinary),
        "nonFacadeOrLandmarkIds": len(city["buildings"]) - len(ordinary),
        "colouredVertices": sum(len(b["colorsRGBA"]) for b in city["buildings"]),
        "styleCounts": dict(Counter(r["profile"] for r in records.values())),
        "materialSourceCounts": dict(counts), "colourSourceCounts": dict(colour_counts),
        "materialSourcePercentOfBuildings": {k: round(v * 100 / len(ordinary), 3) for k, v in counts.items()},
        "colourSourcePercentOfBuildings": {k: round(v * 100 / len(ordinary), 3) for k, v in colour_counts.items()},
        "wallAreaM2": round(area, 2),
        "materialSourcePercentOfWallArea": {k: round(v * 100 / area, 3) for k, v in material_area.items()},
        "colourSourcePercentOfWallArea": {k: round(v * 100 / area, 3) for k, v in colour_area.items()},
        "areaBasis": "Ordinary-building wall triangles only; not imagery coverage. Bridge/Spire, roofs and foundation caps excluded.",
    }


def write_bytes(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_suffix(path.suffix + ".pending")
    pending.write_bytes(data)
    pending.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    city_path = root / "DublinFlight" / "Content" / "Data" / "dublin-city.json"
    evidence_path = root / "generated-data" / "facade-evidence.json"
    provenance_path = city_path.with_name("facade-provenance.json")
    automation = root / "DublinFlight" / "Saved" / "Automation"
    handoff_path = automation / "facade-implementation-handoff.json"
    city_bytes, evidence_bytes = city_path.read_bytes(), evidence_path.read_bytes()
    city, evidence = json.loads(city_bytes), json.loads(evidence_bytes)
    newline = b"\r\n" if city_bytes.endswith(b"\r\n") else b"\n"
    if len(city["buildings"]) != 1044:
        raise ValueError("expected the fixed 1044-building artifact")
    original_sha = digest(encode_city(without_colours(city), newline))
    if original_sha != evidence["city_json"]["sha256"]:
        raise ValueError("non-colour city bytes no longer match the authorised evidence baseline")
    coloured, records, errors = decorate(city, evidence)
    generated_bytes = encode_city(coloured, newline)
    if args.check_only:
        if generated_bytes != city_bytes:
            raise ValueError("persisted colours differ from deterministic expected output")
        provenance = json.loads(provenance_path.read_bytes())
        if provenance["outputCitySHA256"] != digest(city_bytes):
            raise ValueError("facade provenance output hash mismatch")
        print(json.dumps({"status": "passed", "geometryUnchanged": True,
                          "idempotentCityBytes": True, "citySha256": digest(city_bytes),
                          "buildingIds": len(records)}))
        return
    stream = io.StringIO()
    suite = unittest.defaultTestLoader.discover(str(root / "generated-data"), pattern="test_apply_facades.py")
    result = unittest.TextTestRunner(stream=stream, verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise ValueError("facade tests failed:\n" + stream.getvalue())
    tests = {"status": "passed", "run": result.testsRun, "failures": 0, "errors": 0}
    previous = json.loads(provenance_path.read_bytes()) if provenance_path.exists() else {}
    generated_utc = previous.get("generatedUtc", datetime.now(timezone.utc).isoformat())
    summary = summarise(coloured, records)
    provenance = {
        "schemaVersion": 1, "generatedUtc": generated_utc, "change": "Additive building colorsRGBA only",
        "inputEvidenceSHA256": digest(evidence_bytes), "originalGeometricCitySHA256": original_sha,
        "geometryDigestSHA256": geometry_digest(city), "outputCitySHA256": digest(generated_bytes),
        "outputCityBytes": len(generated_bytes), "styleCodes": STYLE_CODES,
        "encoding": "RGB is sRGB8 bytes. Alpha is integer STYLE CODE1..8, not opacity;255 reserved and unused.",
        "interpretation": "Only wall shader reads these values; roof/bridge/foundation shaders ignore them.",
        "summary": summary, "buildings": records, "diagnostics": errors, "tests": tests,
        "sourceDates": {k: v["provenance"].get("osmBaseTimestampUtc")
                        for k, v in enumerate(evidence.get("cached_osm_sources", []))},
        "references": {k: {field: v[field] for field in
                          ("publisher", "url", "date", "survey_date", "checked_on") if field in v}
                       for k, v in evidence.get("sources", {}).items()},
        "caveats": [
            "OSM/NIAH/Council facts and inherited candidates are not a present-day facade survey.",
            "Rejected non-CSS descriptions do not become purported exact colours; fallback tint is explicitly inferred.",
            "Known/inherited valid colours are exact parsed bytes, never jittered. Only inferred profile tints vary by<=3byte brightness using SHA256(building ID).",
            "One RGBA per building is repeated across render vertices; this cannot express mixed-material elevations or individual shopfronts.",
            "Geometry confidence is retained for provenance only and is not facade evidence confidence.",
            "No personal address, faith or other unused OSM tags are copied into this renderer sidecar.",
        ],
    }
    # A second pure pass must reproduce both the full payload and resolution.
    repeated, repeated_records, repeated_errors = decorate(coloured, evidence)
    if encode_city(repeated, newline) != generated_bytes or records != repeated_records or errors != repeated_errors:
        raise ValueError("processor is not idempotent")
    write_bytes(city_path, generated_bytes)
    write_bytes(provenance_path, encode(provenance))
    persisted = json.loads(city_path.read_bytes())
    if digest(encode_city(without_colours(persisted), newline)) != original_sha or geometry_digest(persisted) != geometry_digest(city):
        raise ValueError("persisted geometry changed")
    data_handoff_path = automation / "data-generation-handoff.json"
    data_handoff = json.loads(data_handoff_path.read_bytes())
    data_handoff.update(
        outputBytes=len(generated_bytes), outputSha256=digest(generated_bytes),
        originalGeometricCitySHA256=original_sha, geometryDigestSHA256=geometry_digest(city),
        facadeAddition={"status": "complete", "note": "Facade colour addition, NOT geometry regeneration",
                        "generatedUtc": generated_utc,
                        "provenance": "Content\\Data\\facade-provenance.json", "tests": tests,
                        "colouredVertices": summary["colouredVertices"], "buildingIds": len(records)})
    handoff_text = json.dumps(data_handoff, ensure_ascii=False, allow_nan=False, indent=2) + "\n"
    write_bytes(data_handoff_path, handoff_text.replace("\n", "\r\n").encode("utf-8"))
    handoff = {
        "status": "complete", "change": "Additive facade colours only; not geometry regeneration",
        "completedUtc": datetime.now(timezone.utc).isoformat(),
        "output": "Content\\Data\\dublin-city.json", "outputBytes": len(generated_bytes),
        "outputSha256": digest(generated_bytes), "originalGeometricCitySHA256": original_sha,
        "geometryDigestSHA256": geometry_digest(city), "nonColourBytesBitExactUnchanged": True,
        "countsUnchanged": data_handoff["counts"], "summary": summary, "tests": tests,
        "idempotence": "Second full pure pass byte-identical, persisted non-colour bytes match original SHA",
        "provenance": "Content\\Data\\facade-provenance.json",
        "provenanceSHA256": digest(provenance_path.read_bytes()),
        "diagnosticCount": len(errors), "diagnostics": errors,
        "sourceEvidence": "generated-data\\facade-evidence.json",
        "cacheCaveat": "Full JSON SHA changed although geometry did not; parent must reload/refresh any cached city data to consume colorsRGBA.",
        "peerUpdatesSent": 0, "editorNativeImageryChanges": False,
    }
    write_bytes(handoff_path, encode(handoff))
    print(json.dumps({"status": "complete", "tests": tests, "summary": summary,
                      "diagnostics": errors, "bytes": len(generated_bytes),
                      "sha256": digest(generated_bytes), "originalGeometricCitySHA256": original_sha}, indent=2))


if __name__ == "__main__":
    main()
