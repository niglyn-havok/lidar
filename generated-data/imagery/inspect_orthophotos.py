"""Extract only approved TIFF/TFW members and inspect unchanged survey metadata."""

import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
import stat
import zipfile

import tifffile

from acquire_orthophotos import ROOT, WORK, MANIFEST, TILES, sha256, write_json, progress, now


def jsonable(value):
    if hasattr(value, "tolist"):
        return value.tolist()
    if isinstance(value, dict):
        return {str(k): jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [jsonable(v) for v in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return str(value)


def validate_member_path(member):
    name = member.orig_filename
    normalized = name.replace("\\", "/")
    parts = normalized.rstrip("/").split("/")
    if (not name or name != member.filename or "\x00" in name or
            normalized.startswith("/") or PureWindowsPath(name).drive or
            any(part in ("", ".", "..") or ":" in part or
                part.endswith((" ", ".")) for part in parts)):
        raise RuntimeError(f"Unsafe ZIP member path: {name!r}")
    kind = stat.S_IFMT(member.external_attr >> 16)
    if kind not in (0, stat.S_IFREG, stat.S_IFDIR):
        raise RuntimeError(f"Non-regular ZIP member is not allowed: {name!r}")
    return PurePosixPath(normalized)


def approved_members(bundle, tile_id):
    if tile_id not in TILES:
        raise RuntimeError(f"Unexpected extraction tile: {tile_id}")
    expected = {f"{tile_id}.tif", f"{tile_id}.tfw"}
    selected = {}
    for member in bundle.infolist():
        path = validate_member_path(member)
        if member.is_dir():
            continue
        if path.name not in expected:
            raise RuntimeError(f"Unexpected file in orthophoto archive: {member.filename}")
        if path.name in selected:
            raise RuntimeError(f"Duplicate expected ZIP member: {path.name}")
        limit = 500_000_000 if path.suffix == ".tif" else 16_384
        if not 0 < member.file_size <= limit:
            raise RuntimeError(f"Unexpected expanded member size: {path.name}")
        selected[path.name] = member
    if set(selected) != expected:
        raise RuntimeError(f"Archive must contain exactly the TIFF/TFW pair for {tile_id}")
    return selected


def scoped_target(tile_id, basename, staged=False):
    if tile_id not in TILES or basename not in (f"{tile_id}.tif", f"{tile_id}.tfw"):
        raise RuntimeError("Extraction destination is not an approved TIFF/TFW.")
    target = WORK / "originals" / tile_id / basename
    if staged:
        target = target.with_suffix(target.suffix + ".part")
    if not target.resolve().is_relative_to(WORK.resolve()):
        raise RuntimeError(f"Extraction destination escapes imagery subtree: {target}")
    return target


def inspect():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    progress("inspecting_source_georeference")
    results = []
    for tile in manifest["tiles"]:
        tile_id = tile["tile"]
        if tile_id not in TILES:
            raise RuntimeError("Unexpected tile.")
        source_checksums = ROOT / "metadata" / f"{tile_id}-source-checksums.json"
        hashes = json.loads(source_checksums.read_text(encoding="utf-8"))["manifests"]["sha256"]["tile_rgbi"]
        if set(hashes) != {f"{tile_id}.tfw", f"{tile_id}.tif"}:
            raise RuntimeError(f"Checksum manifest must identify exactly the approved TIFF/TFW pair: {tile_id}")
        destination = scoped_target(tile_id, f"{tile_id}.tif").parent
        archive = ROOT / tile["archive_path"]
        files = []
        with zipfile.ZipFile(archive) as bundle:
            selected = approved_members(bundle, tile_id)
            destination.mkdir(parents=True, exist_ok=True)
            for name, expected_hash in hashes.items():
                basename = Path(name).name
                member = selected[basename]
                target = scoped_target(tile_id, basename)
                if not target.exists():
                    staged = scoped_target(tile_id, basename, staged=True)
                    with bundle.open(member) as source, staged.open("wb") as output:
                        shutil.copyfileobj(source, output, length=4 * 1024 * 1024)
                    if sha256(staged) != expected_hash:
                        raise RuntimeError(f"Published SHA-256 does not match: {basename}")
                    staged.replace(target)
                actual_hash = sha256(target)
                if actual_hash != expected_hash:
                    raise RuntimeError(f"Existing extracted original differs; preserved: {target}")
                files.append({"path": str(target.relative_to(ROOT)), "bytes": target.stat().st_size,
                              "zip_member_path": member.filename, "zip_member_path_validated": True,
                              "destination_within_imagery_subtree": True,
                              "sha256": actual_hash, "published_sha256": expected_hash,
                              "published_checksum_source": str(source_checksums.relative_to(ROOT)),
                              "published_hash_matches": True, "zip_crc_checked_on_extraction": True})
        tiff = destination / f"{tile_id}.tif"
        tfw = destination / f"{tile_id}.tfw"
        with tifffile.TiffFile(tiff) as image:
            page = image.pages[0]
            tags = {tag.name: jsonable(tag.value) for tag in page.tags.values()
                    if tag.name in {"ImageWidth", "ImageLength", "BitsPerSample", "Compression",
                                    "PhotometricInterpretation", "SamplesPerPixel", "PlanarConfiguration",
                                    "ExtraSamples", "SampleFormat", "Software", "DateTime",
                                    "ModelPixelScaleTag", "ModelTiepointTag", "ModelTransformationTag",
                                    "GeoKeyDirectoryTag", "GeoDoubleParamsTag", "GeoAsciiParamsTag",
                                    "GDAL_NODATA", "GDAL_METADATA", "ICCProfile"}}
            if "ICCProfile" in tags:
                tags["ICCProfile"] = "Present; source profile retained in unchanged original."
            result = {"tile": tile_id, "files": files, "archive_path": tile["archive_path"],
                      "inspected_at_utc": now(), "shape": list(page.shape), "dtype": str(page.dtype),
                      "axes": page.axes, "geotiff_metadata": jsonable(image.geotiff_metadata),
                      "tags": tags, "tfw_text": tfw.read_text(encoding="utf-8"),
                      "extraction_policy": "All ZIP paths validated; reject absolute/drive/UNC/traversal/ambiguous paths and special files. Only the exact TIFF/TFW pair is streamed to checked in-scope paths; no extractall.",
                      "tfw_A_D_B_E_C_F": [float(v) for v in tfw.read_text().split()],
                      "page_count": len(image.pages), "is_memmappable": page.is_memmappable}
        results.append(result)
        write_json(WORK / "provenance" / f"{tile_id}-inspection.json", result)
        progress("inspecting_source_georeference", inspected_tiles=[r["tile"] for r in results])
        print(json.dumps({key: result[key] for key in ("tile", "shape", "dtype", "axes",
                                                     "geotiff_metadata", "tags",
                                                     "tfw_A_D_B_E_C_F", "is_memmappable")}), flush=True)
    write_json(WORK / "source-inspection.json", {"schema_version": 1, "tiles": results})
    progress("source_inspection_complete", next_action="Render using actual TIFF/TFW and validate core coverage.")


if __name__ == "__main__":
    try:
        inspect()
    except Exception as exc:
        progress("blocked", blocker=str(exc))
        raise
