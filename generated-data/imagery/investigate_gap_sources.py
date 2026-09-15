"""Bounded primary-source metadata/HEAD investigation; never download imagery archives."""

from datetime import datetime, timezone
import hashlib
import html
import json
import re
from urllib.parse import urljoin, urlparse

import requests

from acquire_orthophotos import HANDOFF, WORK, now, progress, write_json


def get_small(url, params=None):
    with requests.get(url, params=params, stream=True, timeout=(10, 20)) as response:
        response.raise_for_status()
        body = bytearray()
        for chunk in response.iter_content(65536):
            body.extend(chunk)
            if len(body) > 2_000_000:
                raise RuntimeError("Metadata exceeds 2 MB bound.")
        return bytes(body), response.url


def main():
    baseline = json.loads((WORK / "partial-followup-baseline.json").read_text(encoding="utf-8"))
    result = {"schema_version": 1, "checked_at_utc": now(), "archive_bodies_downloaded": 0,
              "remaining_archive_budget_bytes": baseline["additional_archive_budget_bytes"],
              "sources": [], "candidate_archive_heads": [], "catalog_queries": [], "errors": []}
    urls = {
        "nyu_collection": "https://archive.nyu.edu/handle/2451/38684",
        "nyu_components": "https://serv.cusp.nyu.edu/projects/ALSDublin15/html/data-components.html",
        "nyu_survey": "https://serv.cusp.nyu.edu/projects/ALSDublin15/ALSDublin15.html",
        "tailte_aerial": "https://tailte.ie/map-shop/professional-map-products/aerial-imagery-maps-and-data/",
        "tailte_rights": "https://tailte.ie/map-shop/map-licences-and-copyright/",
        "tailte_open_data": "https://tailte.ie/services/open-data/",
        "smartdublin_docklands": "https://data.smartdublin.ie/dataset/3d-data-hack-dublin-resources",
    }
    documents = {}
    for name, url in urls.items():
        try:
            body, resolved = get_small(url)
            documents[name] = body.decode("utf-8", errors="replace")
            text = re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", " ", documents[name])))
            tokens = ("CC BY", "Creative Commons", "NonCommercial", "Copyright Licence", "purchase",
                      "15cm", "20cm", "ECW", "partially covered", "orthorectified")
            result["sources"].append({"name": name, "url": url, "resolved_url": resolved,
                                      "response_bytes": len(body), "html_sha256": hashlib.sha256(body).hexdigest(),
                                      "present_tokens": [token for token in tokens if token.lower() in text.lower()]})
        except Exception as exc:
            result["errors"].append({"url": url, "error": str(exc)})
        write_json(WORK / "gap-source-investigation.json", result)
    collection = documents.get("nyu_collection", "")
    links = [html.unescape(link) for link in re.findall(r'href=["\']([^"\']+)["\']', collection)]
    suffixes = ("_misc_other.zip", "_misc_rgb.zip", "_misc_oblique.zip", "_doc.zip")
    selected = sorted({urljoin(urls["nyu_collection"], link) for link in links if link.endswith(suffixes)})
    for url in selected:
        try:
            if urlparse(url).hostname != "archive.nyu.edu":
                raise RuntimeError("Unexpected archive host.")
            with requests.head(url, allow_redirects=True, timeout=(10, 20)) as response:
                response.raise_for_status()
                size = int(response.headers.get("Content-Length", 0))
                result["candidate_archive_heads"].append({
                    "url": url, "head_bytes": size, "checked_at_utc": now(),
                    "within_remaining_budget": 0 < size <= result["remaining_archive_budget_bytes"],
                    "license_basis": urls["nyu_collection"],
                    "gap_coverage_verified": False, "download_performed": False})
        except Exception as exc:
            result["errors"].append({"url": url, "error": str(exc)})
        write_json(WORK / "gap-source-investigation.json", result)
    for base in ("https://data.gov.ie", "https://data.smartdublin.ie"):
        url = base + "/api/3/action/package_search"
        try:
            body, resolved = get_small(url, {"q": 'orthophoto OR "aerial imagery"', "rows": 12})
            payload = json.loads(body)
            records = payload["result"]
            result["catalog_queries"].append({
                "url": resolved, "total_matching_records": records["count"],
                "bounded_results": [{"title": item["title"], "name": item["name"],
                                     "license_id": item.get("license_id"), "license_title": item.get("license_title"),
                                     "organization": (item.get("organization") or {}).get("title"),
                                     "resource_formats": sorted({r.get("format", "") for r in item.get("resources", [])})}
                                    for item in records["results"]]})
        except Exception as exc:
            result["errors"].append({"url": url, "error": str(exc)})
        write_json(WORK / "gap-source-investigation.json", result)
    result["elapsed_since_followup_start_seconds"] = (
        datetime.now(timezone.utc) - datetime.fromisoformat(baseline["started_at_utc"])).total_seconds()
    write_json(WORK / "gap-source-investigation.json", result)
    print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    main()
