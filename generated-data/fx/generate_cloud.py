"""Original deterministic soft cloud sprite; no external source imagery."""
from pathlib import Path
import math
import random
from PIL import Image

SIZE = 256
rng = random.Random(90210)
octaves = []
for grid_size in (4, 8, 16, 32, 64):
    coarse = Image.new("L", (grid_size, grid_size))
    coarse.putdata([rng.randrange(256) for _ in range(grid_size * grid_size)])
    octaves.append(coarse.resize((SIZE, SIZE), Image.Resampling.BICUBIC))

image = Image.new("RGBA", (SIZE, SIZE))
pixels = []
for y in range(SIZE):
    for x in range(SIZE):
        u, v = (x + .5) / SIZE * 2 - 1, (y + .5) / SIZE * 2 - 1
        noise = sum(o.getpixel((x, y)) / 255 * w for o, w in zip(octaves, (.42, .27, .17, .09, .05)))
        radius = math.hypot(u, v)
        edge = max(0, min(1, (1 - radius) * 4))
        density = max(0, min(1, (noise - .27) * 2.7)) * edge * edge
        shade = int(255 * max(.12, min(1, .30 + .8 * noise - .16 * v)))
        pixels.append((shade, shade, shade, int(255 * density)))
image.putdata(pixels)
image.save(Path(__file__).with_name("T_ImpactCloud.png"))
print("Generated original 256x256 RGBA T_ImpactCloud.png")
