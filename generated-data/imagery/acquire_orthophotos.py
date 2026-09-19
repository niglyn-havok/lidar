"""Bounded, restartable acquisition of the four licensed DublinFlight ortho tiles."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import html
import json
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import tempfile
import threading
import time
from urllib.parse import urlparse
import zipfile

import requests


ROOT = Path(__file__).resolve().parents[2]
WORK = Path(__file__).resolve().parent
PROJECT = ROOT / "DublinFlight"
PROGRESS = WORK / "progress.json"
MANIFEST = WORK / "source-manifest.json"
HANDOFF = PROJECT / "Saved" / "Automation" / "imagery-generation-handoff.json"
TILES = ("315500_234000", "315500_234500", "316000_234000", "316000_234500")
BUDGET = 2_000_000_000
LOCK = threading.Lock()
COLLECTION = "https://archive.nyu.edu/handle/2451/38684"
SURVEY = "https://serv.cusp.nyu.edu/projects/ALSDublin15/ALSDublin15.html"
LICENSE = "https://creativecommons.org/licenses/by/4.0/"
AUTHORS = ["Debra F. Laefer", "Saleh Abuwarda", "Anh-Vu Vo",
           "Linh Truong-Hong", "Hamid Gharibi"]


def now():
    return datetime.now(timezone.utc).isoformat()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    staged = path.with_suffix(path.suffix + ".pending")
    staged.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n",
                      encoding="utf-8")
    staged.replace(path)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def progress(status, **fields):
    with LOCK:
        state = json.loads(PROGRESS.read_text(encoding="utf-8")) if PROGRESS.exists() else {}
        state.update(status=status, updated_at_utc=now(), **fields)
        write_json(PROGRESS, state)


def official_text(url):
    with requests.get(url, timeout=(20, 90)) as response:
        response.raise_for_status()
        if len(response.content) > 2_000_000:
            raise RuntimeError(f"Unexpected metadata size: {url}")
        text = html.unescape(re.sub(r"<[^>]+>", " ", response.text))
        text = re.sub(r"\s+", " ", text)
        return text, hashlib.sha256(response.content).hexdigest(), response.url


def retry_metadata(action):
    for attempt in range(3):
        try:
            return action()
        except (requests.Timeout, requests.ConnectionError, requests.HTTPError) as exc:
            code = getattr(getattr(exc, "response", None), "status_code", None)
            if attempt == 2 or (code and code not in (408, 429, 500, 502, 503, 504)):
                raise
            time.sleep(15 * (attempt + 1))


def prepare():
    source = json.loads((PROJECT / "Saved" / "Automation" / "city-data-handoff.json")
                        .read_text(encoding="utf-8"))
    candidates = source["imagery_reuse_and_replacement"]["preferred_replacement"]["core_and_halo_archives"]
    selected = {entry["tile"]: entry["url"] for entry in candidates if entry["tile"] in TILES}
    if set(selected) != set(TILES):
        raise RuntimeError("Authoritative handoff does not identify exactly the four required tiles.")
    records = []
    progress("verifying_official_rights")
    for url, tokens in (
        (COLLECTION, ["Attribution 4.0", "CC BY 4.0", *AUTHORS]),
        (SURVEY, ["March 26", "2015", "5cm", "infrared"]),
        (LICENSE, ["Attribution", "4.0", "Share", "Adapt"]),
    ):
        text, digest, resolved = retry_metadata(lambda: official_text(url))
        if not all(token in text for token in tokens):
            raise RuntimeError(f"Official licence/description verification failed: {url}")
        records.append({"url": url, "resolved_url": resolved, "checked_at_utc": now(),
                        "html_sha256": digest, "verified_tokens": tokens})
    tiles = []
    for tile in TILES:
        url = selected[tile]
        package_id = re.search(r"nyu_2451_(\d+)_orthophoto", url).group(1)
        record_url = f"https://archive.nyu.edu/handle/2451/{package_id}?mode=full"
        text, digest, resolved = retry_metadata(lambda: official_text(record_url))
        if "CC BY 4.0" not in text or ("orthophoto" not in text.lower()):
            raise RuntimeError(f"Tile-specific orthophoto rights are not established: {record_url}")
        records.append({"url": record_url, "resolved_url": resolved, "checked_at_utc": now(),
                        "html_sha256": digest, "rights": "CC BY 4.0",
                        "contains_orthophoto_component": True})

        def head():
            with requests.head(url, allow_redirects=True, timeout=(20, 90)) as response:
                response.raise_for_status()
                size = int(response.headers.get("Content-Length", "0"))
                if size <= 0 or "archive.nyu.edu" != urlparse(response.url).hostname:
                    raise RuntimeError(f"Unknown size or unexpected download host: {url}")
                return size, response.url, dict(response.headers)

        size, resolved_url, headers = retry_metadata(head)
        name = Path(urlparse(url).path).name
        reuse = next((p for p in (WORK / "downloads" / name, ROOT / "archives" / name)
                      if p.exists()), None)
        tiles.append({"tile": tile, "record_url": record_url, "archive_url": url,
                      "resolved_archive_url": resolved_url, "head_bytes": size,
                      "head_at_utc": now(),
                      "http_headers": {k: v for k, v in headers.items()
                                       if k.lower() in ("content-length", "content-type", "etag",
                                                        "last-modified", "accept-ranges")},
                      "archive_path": str((reuse or WORK / "downloads" / name).relative_to(ROOT)),
                      "reused_existing": bool(reuse)})
        progress("verifying_archive_sizes", verified_tiles=tiles, official_verification=records)
        print(json.dumps({"tile": tile, "head_bytes": size, "rights": "CC BY 4.0"}), flush=True)
    total = sum(tile["head_bytes"] for tile in tiles)
    if total > BUDGET:
        raise RuntimeError(f"Four archives need {total} bytes, exceeding {BUDGET}-byte budget.")
    if shutil.disk_usage(WORK).free < total + 2_000_000_000 + 1_000_000_000:
        raise RuntimeError("Insufficient free space for unchanged archives, TIFFs and output.")
    result = {"schema_version": 1, "prepared_at_utc": now(),
              "title": "2015 Aerial Laser and Photogrammetry Survey of Dublin City",
              "authors": AUTHORS, "capture_date": "2015-03-26",
              "source_publisher": "NYU Faculty Digital Archive",
              "license": {"name": "CC BY 4.0", "url": LICENSE,
                          "attribution_required": True, "indicate_changes": True,
                          "no_endorsement": True, "no_additional_restrictions": True},
              "collection_url": COLLECTION, "survey_description_url": SURVEY,
              "native_resolution_m": 0.05, "bands": ["red", "green", "blue", "infrared"],
              "archive_total_bytes": total, "archive_budget_bytes": BUDGET,
              "maximum_concurrent_downloads": 2, "maximum_sequential_retries": 2,
              "official_verification": records, "tiles": tiles}
    write_json(MANIFEST, result)
    progress("ready_to_download", archive_total_bytes=total, manifest_path=str(MANIFEST.relative_to(ROOT)))
    print(json.dumps({"archive_total_bytes": total, "within_budget": True}), flush=True)


def download_once(tile):
    path = ROOT / tile["archive_path"]
    path.parent.mkdir(parents=True, exist_ok=True)
    expected = tile["head_bytes"]
    if path.exists():
        if path.stat().st_size != expected or not zipfile.is_zipfile(path):
            raise RuntimeError(f"Existing archive invalid; preserved unchanged: {path}")
    else:
        staged = path.with_suffix(path.suffix + ".part")
        # The server has previously ignored Range; never append an unverified partial response.
        with requests.get(tile["archive_url"], stream=True, timeout=(20, 120),
                          headers={"Accept-Encoding": "identity"}) as response:
            response.raise_for_status()
            length = int(response.headers.get("Content-Length", 0))
            if length != expected:
                raise RuntimeError(f"GET size {length} differs from HEAD {expected}: {path.name}")
            downloaded = 0
            checkpoint = time.monotonic()
            with staged.open("wb") as output:
                for chunk in response.iter_content(chunk_size=1024 * 1024):
                    if downloaded + len(chunk) > expected:
                        raise RuntimeError(f"Archive body exceeds approved size: {path.name}")
                    output.write(chunk)
                    downloaded += len(chunk)
                    if time.monotonic() - checkpoint > 15:
                        with LOCK:
                            state = json.loads(PROGRESS.read_text(encoding="utf-8"))
                            state.setdefault("download_bytes", {})[tile["tile"]] = downloaded
                            state["updated_at_utc"] = now()
                            write_json(PROGRESS, state)
                        checkpoint = time.monotonic()
            if downloaded != expected or not zipfile.is_zipfile(staged):
                raise RuntimeError(f"Incomplete or non-ZIP body: {path.name}")
        staged.replace(path)
    completed = {**tile, "archive_sha256": sha256(path), "download_completed_at_utc": now(),
                 "actual_archive_bytes": path.stat().st_size}
    with LOCK:
        state = json.loads(PROGRESS.read_text(encoding="utf-8"))
        done = {entry["tile"]: entry for entry in state.get("completed_downloads", [])}
        done[tile["tile"]] = completed
        state.update(status="downloading", updated_at_utc=now(), completed_downloads=list(done.values()))
        write_json(PROGRESS, state)
        write_json(WORK / "provenance" / f"{tile['tile']}-download.json", completed)
    print(json.dumps({"download_complete": tile["tile"], "bytes": expected,
                      "sha256": completed["archive_sha256"]}), flush=True)
    return completed


def download():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if tuple(t["tile"] for t in manifest["tiles"]) != TILES:
        raise RuntimeError("Manifest tile set changed.")
    if sum(t["head_bytes"] for t in manifest["tiles"]) > BUDGET:
        raise RuntimeError("Manifest exceeds budget.")
    progress("downloading")
    failures = []
    with ThreadPoolExecutor(max_workers=2) as pool:
        pending = {pool.submit(download_once, tile): tile for tile in manifest["tiles"]}
        for future in as_completed(pending):
            try:
                future.result()
            except (requests.RequestException, OSError, RuntimeError) as exc:
                failures.append((pending[future], str(exc)))
    # Retries are globally sequential after the two initial workers have finished.
    for tile, error in failures:
        for retry in range(2):
            progress("retry_wait", retry_tile=tile["tile"], retry_number=retry + 1, last_error=error)
            time.sleep(20 * (retry + 1))
            try:
                download_once(tile)
                break
            except (requests.RequestException, OSError, RuntimeError) as exc:
                error = str(exc)
                if retry == 1:
                    raise RuntimeError(f"{tile['tile']} failed after two sequential retries: {error}") from exc
    progress("downloads_complete", next_action="Inspect TIFF/TFW and source checksums before rendering.")


def restore_archive(tile, path, expected_hash):
    expected_size = tile["head_bytes"]
    if path.exists():
        if (not path.is_file() or path.stat().st_size != expected_size or
                sha256(path) != expected_hash or not zipfile.is_zipfile(path)):
            raise RuntimeError(f"Existing archive differs from retained provenance; preserved: {path}")
        print(f"Verified cache: {path.name}", flush=True)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    staged = None
    try:
        with requests.get(tile["archive_url"], stream=True, timeout=(20, 120),
                          headers={"Accept-Encoding": "identity"}) as response:
            response.raise_for_status()
            resolved = urlparse(response.url)
            if resolved.scheme != "https" or resolved.hostname != "archive.nyu.edu":
                raise RuntimeError("Archive redirect leaves the recorded official HTTPS host.")
            if int(response.headers.get("Content-Length", "0")) != expected_size:
                raise RuntimeError(f"GET size differs from retained manifest: {path.name}")
            downloaded, digest = 0, hashlib.sha256()
            checkpoint = time.monotonic()
            with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".",
                                             suffix=".part", delete=False) as output:
                staged = Path(output.name)
                for chunk in response.iter_content(chunk_size=1024 * 1024):
                    downloaded += len(chunk)
                    if downloaded > expected_size:
                        raise RuntimeError(f"Archive body exceeds retained size: {path.name}")
                    output.write(chunk)
                    digest.update(chunk)
                    if time.monotonic() - checkpoint > 15:
                        print(f"{path.name}: {downloaded}/{expected_size} bytes", flush=True)
                        checkpoint = time.monotonic()
            if downloaded != expected_size:
                raise RuntimeError(f"Incomplete archive size: {path.name}")
            if digest.hexdigest() != expected_hash:
                raise RuntimeError(f"Archive SHA-256 differs from retained provenance: {path.name}")
            if not zipfile.is_zipfile(staged):
                raise RuntimeError(f"Retained archive is not a ZIP: {path.name}")
        # Publish without overwriting a file another process created during the transfer.
        os.link(staged, path)
        print(f"Restored {path.name}: {expected_size} bytes, SHA-256 {expected_hash}", flush=True)
    finally:
        if staged is not None:
            staged.unlink()


def restore():
    """Restore pinned ZIP caches only; never change historical metadata or originals."""
    manifest = json.loads(MANIFEST.read_bytes())
    tiles = manifest["tiles"]
    if tuple(tile["tile"] for tile in tiles) != TILES:
        raise RuntimeError("Retained manifest must contain exactly the four original tiles.")
    if any(type(tile["head_bytes"]) is not int or tile["head_bytes"] <= 0 for tile in tiles):
        raise RuntimeError("Retained archive sizes must be positive integers.")
    total = sum(tile["head_bytes"] for tile in tiles)
    if total != manifest["archive_total_bytes"] or total > BUDGET:
        raise RuntimeError("Retained archive total differs or exceeds the download budget.")
    jobs = []
    for tile in tiles:
        record = json.loads((WORK / "provenance" / f"{tile['tile']}-download.json").read_bytes())
        for key in ("tile", "archive_url", "archive_path", "head_bytes"):
            if record[key] != tile[key]:
                raise RuntimeError(f"Retained manifest/provenance mismatch: {tile['tile']} {key}")
        digest = record["archive_sha256"]
        if not re.fullmatch(r"[0-9a-f]{64}", digest) or record["actual_archive_bytes"] != tile["head_bytes"]:
            raise RuntimeError(f"Invalid retained size/SHA-256: {tile['tile']}")
        url = urlparse(tile["archive_url"])
        if (url.scheme != "https" or url.hostname != "archive.nyu.edu" or
                url.username or url.password or url.port not in (None, 443)):
            raise RuntimeError("Restoration only permits the recorded official HTTPS archive host.")
        path = WORK / "downloads" / Path(url.path).name
        declared = ROOT.joinpath(*PureWindowsPath(tile["archive_path"]).parts)
        if (path.suffix != ".zip" or path.resolve() != declared.resolve() or
                not path.resolve().is_relative_to(WORK.resolve()) or path.is_symlink()):
            raise RuntimeError(f"Archive destination is not the intended imagery cache: {declared}")
        jobs.append((tile, path, digest))
    needed = sum(tile["head_bytes"] for tile, path, _ in jobs if not path.exists())
    if shutil.disk_usage(WORK).free < needed:
        raise RuntimeError("Insufficient free space for the missing archive caches.")
    for tile, path, digest in jobs:
        retry_metadata(lambda: restore_archive(tile, path, digest))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=["prepare", "download", "restore"])
    args = parser.parse_args()
    if args.stage == "restore":
        restore()
        return
    try:
        {"prepare": prepare, "download": download}[args.stage]()
    except Exception as exc:
        progress("blocked", blocker=str(exc))
        write_json(HANDOFF, {"schema_version": 1, "status": "blocked",
                            "ready_for_import": False, "blocker": str(exc),
                            "updated_at_utc": now(), "progress_file": str(PROGRESS.relative_to(ROOT)),
                            "unreal_actions_performed": False})
        raise


if __name__ == "__main__":
    main()
