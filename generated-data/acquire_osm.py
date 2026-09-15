"""Bounded, sequential public-OSM acquisition; generation itself is offline."""
import argparse
import hashlib
import json
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

from pyproj import Transformer

MAX_BYTES = 10_000_000
ENDPOINTS = ("https://overpass-api.de/api/interpreter",
             "https://overpass.kumi.systems/api/interpreter")
ORIGIN = (315989, 234393)
HALF_HALO = 434


def workspace():
    return Path(__file__).resolve().parent.parent


def queries():
    transform = Transformer.from_crs(29903, 4326, always_xy=True)
    corners = [transform.transform(ORIGIN[0] + x, ORIGIN[1] + y)
               for x in (-HALF_HALO, HALF_HALO)
               for y in (-HALF_HALO, HALF_HALO)]
    bbox = (min(p[1] for p in corners), min(p[0] for p in corners),
            max(p[1] for p in corners), max(p[0] for p in corners))
    bounds = ",".join(f"{v:.7f}" for v in bbox)
    selectors = {
        "buildings": ['way["building"]', 'relation["building"]',
                      'way["building:part"]', 'relation["building:part"]'],
        "hydrology": ['way["waterway"="riverbank"]',
                      'relation["waterway"="riverbank"]',
                      'way["natural"="water"]', 'relation["natural"="water"]',
                      'way["waterway"="river"]', 'way["highway"]',
                      'way["man_made"="bridge"]',
                      'relation["man_made"="bridge"]',
                      'way["place"="island"]', 'relation["place"="island"]'],
    }
    return {
        name: "[out:json][timeout:40][maxsize:67108864];(" +
              "".join(f"{selector}({bounds});" for selector in group) +
              ");out body geom;"
        for name, group in selectors.items()
    }, bbox


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--refresh", action="store_true")
    args = parser.parse_args()
    root = workspace()
    sources = root / "generated-data" / "sources"
    sources.mkdir(parents=True, exist_ok=True)
    all_queries, bbox = queries()
    records = {}
    for name, query in all_queries.items():
        target = sources / f"osm-{name}.json"
        record_path = sources / f"osm-{name}-provenance.json"
        if target.exists() and record_path.exists() and not args.refresh:
            record = json.loads(record_path.read_text(encoding="utf-8"))
            digest = hashlib.sha256(target.read_bytes()).hexdigest()
            if record["sha256"] != digest or record["query"] != query:
                raise ValueError(f"{name}: cached source hash/query mismatch")
            records[name] = record
            print(f"{name}: verified cache {target.stat().st_size} bytes", flush=True)
            continue
        errors = []
        for attempt in range(3):
            if attempt:
                time.sleep(20 * attempt)
            endpoint = ENDPOINTS[min(attempt, 1)]
            request = Request(endpoint + "?" + urlencode({"data": query}),
                              headers={"User-Agent": "DublinFlight-bounded-derivative/1.0",
                                       "Accept": "application/json",
                                       "Accept-Encoding": "identity"})
            try:
                start = time.monotonic()
                with urlopen(request, timeout=65) as response:
                    size = response.headers.get("Content-Length")
                    if size and int(size) > MAX_BYTES:
                        raise ValueError(f"response too large: {size}")
                    body = response.read(MAX_BYTES + 1)
                    if len(body) > MAX_BYTES:
                        raise ValueError("response exceeded strict 10MB cap")
                data = json.loads(body)
                if data.get("remark") or not isinstance(data.get("elements"), list):
                    raise ValueError(f"incomplete OSM response: {data.get('remark')}")
                record = {
                    "source": "OpenStreetMap", "endpoint": endpoint, "query": query,
                    "bboxSouthWestNorthEast": bbox,
                    "coreBoundsEN": [315605, 234009, 316373, 234777],
                    "haloBoundsEN": [315555, 233959, 316423, 234827],
                    "acquiredUtc": datetime.now(timezone.utc).isoformat(),
                    "osmBaseTimestampUtc": data.get("osm3s", {}).get("timestamp_osm_base"),
                    "bytes": len(body), "sha256": hashlib.sha256(body).hexdigest(),
                    "elements": len(data["elements"]),
                    "seconds": round(time.monotonic() - start, 3),
                    "attempts": attempt + 1, "earlierErrors": errors,
                    "responseLimitBytes": MAX_BYTES,
                    "attribution": "© OpenStreetMap contributors",
                    "license": "ODbL 1.0",
                    "licenseUrl": "https://www.openstreetmap.org/copyright",
                }
                target.write_bytes(body)
                record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
                records[name] = record
                print(f"{name}: {len(data['elements'])} elements, {len(body)} bytes", flush=True)
                break
            except (HTTPError, URLError, TimeoutError, ValueError) as error:
                errors.append(str(error))
                print(f"{name}: attempt {attempt + 1} failed: {error}", flush=True)
        else:
            raise RuntimeError(f"{name}: bounded retries exhausted: {errors}")
    print(json.dumps({name: {key: value[key] for key in ("bytes", "elements", "sha256")}
                      for name, value in records.items()}))


if __name__ == "__main__":
    main()
