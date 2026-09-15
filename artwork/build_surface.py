import json
from pathlib import Path
import time

import cv2
import laspy
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage


ROOT = Path(__file__).resolve().parent
DATA = ROOT.parent
CELL = 1.0
BOUNDS = (314950, 233400, 316950, 235350)
NX = int((BOUNDS[2] - BOUNDS[0]) / CELL)
NY = int((BOUNDS[3] - BOUNDS[1]) / CELL)


def font(size, face="GARA.TTF"):
    return ImageFont.truetype(str(Path("C:\\Windows\\Fonts") / face), size)


def publish(image):
    temporary = ROOT / "working-canvas.tmp.png"
    image.save(temporary)
    temporary.replace(ROOT / "working-canvas.png")


def draft(surface, processed, total):
    small = surface[::4, ::4]
    valid = np.isfinite(small)
    yy, xx = np.indices(small.shape)
    x = (xx * 4 + BOUNDS[0] - 315950) * 0.82
    y = (yy * 4 + BOUNDS[1] - 234375) * 0.82
    z = np.where(valid, small, 0)
    px = np.rint(2400 + (x * .866 + y * .5) * 1.78).astype(int)
    py = np.rint(1870 + (x * .288675 - y * .5 - z * 1.35) * 1.78).astype(int)
    canvas = np.full((3600, 4800, 3), (248, 245, 239), np.uint8)
    order = np.argsort((-y + x * .577).ravel())
    flat_valid = valid.ravel()[order]
    xp = px.ravel()[order][flat_valid]
    yp = py.ravel()[order][flat_valid]
    zp = z.ravel()[order][flat_valid]
    tone = np.clip(197 - zp * 2.2, 70, 194).astype(np.uint8)
    for dx, dy in [(0, 0), (1, 0), (0, 1)]:
        canvas[yp + dy, xp + dx] = np.column_stack((tone, tone + 4, tone + 2))
    image = Image.fromarray(canvas)
    draw = ImageDraw.Draw(image)
    draw.text((360, 285), "D U B L I N", font=font(124), fill="#283b3b")
    draw.text((367, 456), "THE CITY, IN A BILLION POINTS", font=font(30, "bahnschrift.ttf"), fill="#7c8179")
    draw.line((367, 3255, 4433, 3255), fill="#b7b8aa", width=2)
    draw.text((367, 3320), f"LIDAR SURFACE STUDY   /   {processed:02d} OF {total:02d} TILES",
              font=font(28, "bahnschrift.ttf"), fill="#7c8179")
    publish(image)


def main():
    start = time.monotonic()
    maximum = np.full(NX * NY, -np.inf, np.float32)
    ground = np.full(NX * NY, np.inf, np.float32)
    count = np.zeros(NX * NY, np.uint32)
    metadata = []
    source = json.loads((DATA / "manifest.json").read_text())
    tiles = []
    for tile in source["tiles"]:
        x0, y0, x1, y1 = tile["bounds_irish_grid"]
        if x1 >= BOUNDS[0] and x0 <= BOUNDS[2] and y1 >= BOUNDS[1] and y0 <= BOUNDS[3]:
            tiles.append(tile)
    tiles.sort(key=lambda t: (int(t["tile_id"].split("_")[1]), int(t["tile_id"].split("_")[0])), reverse=True)
    total_points = 0
    kept_points = 0
    for tile_number, tile in enumerate(tiles, 1):
        path = DATA / "tiles" / tile["laz_filename"]
        print(f"{tile_number}/{len(tiles)} decoding {path.name}", flush=True)
        classes = np.zeros(256, np.int64)
        with laspy.open(path) as reader:
            for points in reader.chunk_iterator(2_000_000):
                x = np.asarray(points.x)
                y = np.asarray(points.y)
                z = np.asarray(points.z)
                classification = np.asarray(points.classification)
                total_points += len(points)
                classes += np.bincount(classification, minlength=256)
                keep = ((x >= BOUNDS[0]) & (x < BOUNDS[2]) &
                        (y >= BOUNDS[1]) & (y < BOUNDS[3]) &
                        (z > -4) & (z < 170))
                ix = ((x[keep] - BOUNDS[0]) / CELL).astype(np.int32)
                iy = ((y[keep] - BOUNDS[1]) / CELL).astype(np.int32)
                heights = z[keep].astype(np.float32)
                index = iy * NX + ix
                np.maximum.at(maximum, index, heights)
                count += np.bincount(index, minlength=NX * NY).astype(np.uint32)
                is_ground = classification[keep] == 2
                np.minimum.at(ground, index[is_ground], heights[is_ground])
                kept_points += len(heights)
        metadata.append({"tile": tile["tile_id"], "points": int(classes.sum()),
                         "classes": {str(i): int(n) for i, n in enumerate(classes) if n}})
        draft(maximum.reshape(NY, NX), tile_number, len(tiles))
        print(f"  {total_points:,} points read; {time.monotonic() - start:.1f}s elapsed", flush=True)
    raw = maximum.reshape(NY, NX)
    valid = np.isfinite(raw)
    indices = ndimage.distance_transform_edt(~valid, return_distances=False, return_indices=True)
    filled = raw[tuple(indices)]
    smooth = ndimage.median_filter(filled, size=3).astype(np.float32)
    ground = ground.reshape(NY, NX)
    has_ground = np.isfinite(ground)
    indices = ndimage.distance_transform_edt(~has_ground, return_distances=False, return_indices=True)
    terrain = ndimage.median_filter(ground[tuple(indices)], size=5).astype(np.float32)
    np.savez_compressed(
        ROOT / "surface.npz", surface=smooth, raw=raw, terrain=terrain,
        valid=valid, count=count.reshape(NY, NX), bounds=np.array(BOUNDS),
        cell=np.array(CELL),
    )
    report = {
        "read_points": total_points, "selected_points": kept_points,
        "bounds_irish_grid": BOUNDS, "cell_metres": CELL,
        "valid_cells": int(valid.sum()), "total_cells": int(valid.size),
        "elapsed_seconds": round(time.monotonic() - start, 1), "tiles": metadata,
        "method": "Maximum measured return per 1m cell, 3x3 median denoising. Ground from class 2 returns. Empty cells interpolated from nearest measured cell; no external geometry used.",
    }
    (ROOT / "surface-provenance.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "tiles"}, indent=2))
    normalized = np.clip((smooth - terrain) / 55, 0, 1)
    diagnostic = (255 * (1 - normalized)).astype(np.uint8)
    Image.fromarray(np.flipud(diagnostic)).save(ROOT / "surface-diagnostic.png")


if __name__ == "__main__":
    main()
