"""Build a measured city grid with imagery-derived surface classes."""
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi

ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "assets"
CX, CY = 315989, 234393
HALF = 800
RADIUS = 760


def main():
    data = np.load(ROOT.parent / "artwork" / "surface.npz")
    x0, y0 = data["bounds"][:2].astype(int)
    sx, sy = CX - HALF - x0, CY - HALF - y0
    crop = np.s_[sy:sy + 1601, sx:sx + 1601]
    height = data["surface"][crop].copy()
    valid = data["valid"][crop]
    image = Image.open(ASSETS / "dublin-aerial.jpg").convert("RGB")
    aerial = np.flipud(np.array(image.resize((1601, 1601), Image.Resampling.BILINEAR))) / 255.
    r, g, b = aerial.transpose(2, 0, 1)
    # A deliberately broad river corridor only limits segmentation; its banks
    # and bridge exclusions are extracted from the georectified photograph.
    corridor = Image.new("L", (1601, 1601))
    draw = ImageDraw.Draw(corridor)
    river_path = [(0, 1017), (240, 956), (440, 894), (640, 833),
                  (800, 793), (1090, 740), (1370, 752), (1600, 803)]
    draw.line(river_path, fill=255, width=200)
    corridor = np.flipud(np.array(corridor)) > 0
    dark = np.maximum.reduce([r, g, b]) < .115
    labels, count = ndi.label(dark & corridor)
    sizes = np.bincount(labels.ravel())
    water = (sizes[labels] > 2500) & (labels > 0)
    water = ndi.binary_fill_holes(ndi.binary_closing(water, iterations=2))
    water &= height < 8
    # Satellite shadows must not be classified as trees. Copper roofs are
    # mostly blue-green; living vegetation has stronger green than blue.
    green = (g > r * 1.12) & (g > b * 1.19) & (g > .065) & ~water
    green = ndi.binary_opening(green, iterations=1)
    local_ground = ndi.percentile_filter(height, percentile=12, size=31)
    canopy = green & ((height - local_ground) > 3.2)
    # Keep the measured bridges, but discard spurious water returns.
    height[water] = 1.1
    # Median rasterization spreads a needle into a small pyramid. Restore this
    # footprint to street level; the Blender script builds the surveyed needle.
    spire_x, spire_y = 315903 - (CX - HALF), 234672 - (CY - HALF)
    height[spire_y - 2:spire_y + 3, spire_x - 2:spire_x + 3] = 6.45
    yy, xx = np.mgrid[-HALF:HALF + 1, -HALF:HALF + 1]
    circle = xx * xx + yy * yy <= RADIUS ** 2
    report = {
        "center_irish_grid": [CX, CY], "crs": "EPSG:29903",
        "radius_metres": RADIUS, "diameter_metres": RADIUS * 2,
        "cell_metres": 1, "model_units_per_metre": .01,
        "measured_cells_inside_globe": int((valid & circle).sum()),
        "cells_inside_globe": int(circle.sum()),
        "coverage_fraction": float(valid[circle].mean()),
        "water_cells": int((water & circle).sum()),
        "vegetation_cells": int((green & circle).sum()),
        "canopy_cells": int((canopy & circle).sum()),
        "height_percentiles_metres": np.percentile(height[circle], [0, 25, 50, 95, 99, 100]).tolist(),
        "geometry_source": "../artwork/surface.npz, derived from supplied LAZ tiles",
        "color_source": "Esri World Imagery, locally reprojected from EPSG:3857 to EPSG:29903",
        "landcover_method": "Aerial color segmentation; river constrained to Liffey corridor, bridges excluded. Canopy requires both green color and local LiDAR relief.",
        "spire_reconstruction": "Tapered 3m-diameter needle at Irish Grid 315903.5,234672.5, tip 124.609m Malin Head from raw LiDAR; local street 6.45m.",
        "caveats": [
            "LiDAR captured March 26, 2015; imagery capture date not asserted and may differ.",
            "Nearest measured-cell interpolation retained in survey gaps.",
            "Facade microdetail, clouds, lighting and globe are artistic.",
            "Water shading is physically inspired, not measured river color.",
        ],
    }
    np.savez_compressed(ASSETS / "city-grid.npz", height=height, water=water,
                        green=green, canopy=canopy, valid=valid)
    Image.fromarray(np.flipud((water * 255).astype(np.uint8))).save(ASSETS / "water-mask.png")
    Image.fromarray(np.flipud((green * 255).astype(np.uint8))).save(ASSETS / "vegetation-mask.png")
    diagnostic = (aerial * 180).astype(np.uint8)
    diagnostic[water] = (25, 140, 210)
    diagnostic[canopy] = (85, 210, 65)
    Image.fromarray(np.flipud(diagnostic)).resize((1000, 1000)).save(ASSETS / "classification-preview.jpg")
    (ROOT / "provenance.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
