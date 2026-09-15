"""Prepare georeferenced imagery and LiDAR for the Dublin glass study."""
import json
from pathlib import Path

import numpy as np
import requests
from PIL import Image
from pyproj import Transformer
from scipy.ndimage import map_coordinates

ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "assets"
ASSETS.mkdir(exist_ok=True)
CENTER = (315989.0, 234393.0)
RADIUS = 760
BOUNDS = (CENTER[0] - 800, CENTER[1] - 800,
          CENTER[0] + 800, CENTER[1] + 800)
SESSION = requests.Session()
SESSION.headers["User-Agent"] = "DublinLidarGlobe/1.0 (local artistic visualization)"


def fetch_sources():
    base = "https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer"
    response = SESSION.get(base, params={"f": "json"}, timeout=90)
    response.raise_for_status()
    service = response.json()
    (ASSETS / "imagery-service.json").write_text(json.dumps(service, indent=2))
    print("Imagery:", service.get("copyrightText"), flush=True)
    # Request native Web Mercator imagery, then rectify to the LiDAR grid locally.
    mercator = Transformer.from_crs(29903, 3857, always_xy=True)
    corners = np.array([mercator.transform(x, y) for x in BOUNDS[::2] for y in BOUNDS[1::2]])
    box = (*corners.min(axis=0), *corners.max(axis=0))
    response = SESSION.get(base + "/export", params={
        "bbox": ",".join(map(str, box)), "bboxSR": 3857, "imageSR": 3857,
        "size": "2048,2048", "format": "jpg", "f": "json",
    }, timeout=120)
    response.raise_for_status()
    metadata = response.json()
    if "error" in metadata:
        raise RuntimeError(metadata["error"])
    (ASSETS / "imagery-georeference.json").write_text(json.dumps(metadata, indent=2))
    image = SESSION.get(metadata["href"], timeout=120)
    image.raise_for_status()
    (ASSETS / "source-aerial.jpg").write_bytes(image.content)
    source = np.array(Image.open(ASSETS / "source-aerial.jpg"))
    yy, xx = np.mgrid[0:2048, 0:2048]
    mx, my = mercator.transform(BOUNDS[0] + (xx + .5) * 1600 / 2048,
                               BOUNDS[3] - (yy + .5) * 1600 / 2048)
    extent = metadata["extent"]
    u = (mx - extent["xmin"]) / (extent["xmax"] - extent["xmin"]) * source.shape[1] - .5
    v = (extent["ymax"] - my) / (extent["ymax"] - extent["ymin"]) * source.shape[0] - .5
    rectified = np.stack([map_coordinates(source[:, :, c], [v, u], order=1, mode="nearest")
                          for c in range(3)], axis=-1)
    Image.fromarray(rectified).save(ASSETS / "dublin-aerial.jpg", quality=97)
    with Image.open(ASSETS / "dublin-aerial.jpg") as im:
        im.resize((1024, 1024)).save(ASSETS / "aerial-preview.jpg")


if __name__ == "__main__":
    fetch_sources()
