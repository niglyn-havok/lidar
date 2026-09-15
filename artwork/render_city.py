import argparse
import json
from pathlib import Path
import time

import cv2
import moderngl
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage


ROOT = Path(__file__).resolve().parent
PAPER = np.array([248, 245, 239], dtype=np.float32)
INK = np.array([35, 54, 56], dtype=np.float32)


def font(size, filename="GARA.TTF"):
    return ImageFont.truetype(str(Path("C:\\Windows\\Fonts") / filename), size)


def publish(image, filename):
    image.save(ROOT / filename)
    temporary = ROOT / "working-canvas.tmp.png"
    image.save(temporary)
    temporary.replace(ROOT / "working-canvas.png")
    preview = image.copy()
    preview.thumbnail((1600, 1200), Image.Resampling.LANCZOS)
    preview.save(ROOT / "preview.png")


VERTEX = """
#version 330
in vec3 in_position;
uniform vec2 resolution;
uniform vec2 origin;
uniform float scale;
uniform float height_scale;
out vec3 world;
void main() {
    world = in_position;
    float u = (world.x + world.y) * 0.70710678;
    float v = (-world.x + world.y) * 0.40824829 + world.z * 0.81649658 * height_scale;
    float d = (world.x - world.y) * 0.57735027 + world.z * 0.57735027;
    vec2 pixel = origin + vec2(u, v) * scale;
    gl_Position = vec4(pixel / resolution * 2.0 - 1.0, -d / 4000.0, 1.0);
}
"""

FRAGMENT = """
#version 330
in vec3 world;
layout(location = 0) out vec4 normal_height;
layout(location = 1) out vec4 position;
void main() {
    vec3 n = normalize(cross(dFdx(world), dFdy(world)));
    if (n.z < 0.0) n = -n;
    normal_height = vec4(n, world.z);
    position = vec4(world, 1.0);
}
"""


def geometry_pass(size, step, height_scale):
    data = np.load(ROOT / "surface.npz")
    surface = data["surface"][::step, ::step].copy()
    measured = data["valid"][::step, ::step]
    support = ndimage.uniform_filter(measured.astype(np.float32), size=15) > .12
    support = ndimage.binary_fill_holes(ndimage.binary_closing(support, iterations=8))
    bounds = data["bounds"]
    ny, nx = surface.shape
    yy, xx = np.indices((ny, nx), dtype=np.float32)
    x = xx * step + bounds[0] - 315950
    y = yy * step + bounds[1] - 234375
    vertices = np.stack((x, y, surface), axis=-1).astype("f4")
    a = (np.arange(ny - 1)[:, None] * nx + np.arange(nx - 1)[None, :]).ravel()
    indices = np.stack((a, a + 1, a + nx, a + 1, a + nx + 1, a + nx), axis=-1).astype("u4")
    # Trim the incomplete survey fringe; the drawing's core remains entirely LiDAR-derived.
    cell_x = x[:-1, :-1].ravel()
    cell_y = y[:-1, :-1].ravel()
    keep = ((cell_x > -855) & (cell_x < 875) & (cell_y > -880) & (cell_y < 660))
    keep &= (support[:-1, :-1] & support[1:, :-1] &
             support[:-1, 1:] & support[1:, 1:]).ravel()
    indices = indices[keep]
    context = moderngl.create_standalone_context()
    program = context.program(vertex_shader=VERTEX, fragment_shader=FRAGMENT)
    vertex_buffer = context.buffer(vertices.tobytes())
    index_buffer = context.buffer(indices.tobytes())
    vao = context.vertex_array(program, [(vertex_buffer, "3f", "in_position")], index_buffer)
    width, height = size
    textures = [context.texture(size, 4, dtype="f4") for _ in range(2)]
    depth = context.depth_renderbuffer(size)
    target = context.framebuffer(textures, depth)
    target.use()
    target.clear(0, 0, 0, 0, depth=1)
    context.enable(moderngl.DEPTH_TEST)
    program["resolution"] = size
    program["origin"] = (width * .5, height * .49)
    program["scale"] = width / 2900
    program["height_scale"] = height_scale
    vao.render()
    normal = np.frombuffer(target.read(attachment=0, components=4, dtype="f4"), np.float32).reshape(height, width, 4)
    position = np.frombuffer(target.read(attachment=1, components=4, dtype="f4"), np.float32).reshape(height, width, 4)
    normal = np.flipud(normal).copy()
    position = np.flipud(position).copy()
    context.release()
    return normal, position


def draw_letterspaced(draw, xy, text, face, fill, spacing):
    x, y = xy
    for char in text:
        draw.text((x, y), char, font=face, fill=fill)
        x += draw.textlength(char, font=face) + spacing
    return x


def compose(normals, positions, style):
    height, width = normals.shape[:2]
    ratio = width / 4800
    valid = positions[:, :, 3] > .5
    normal = normals[:, :, :3]
    # Geometry edges: screen-space depth discontinuities and changes in surface orientation.
    depth = (positions[:, :, 0] - positions[:, :, 1] + positions[:, :, 2]) * .57735027
    depth_dx = cv2.Sobel(depth, cv2.CV_32F, 1, 0, ksize=3) / 8
    depth_dy = cv2.Sobel(depth, cv2.CV_32F, 0, 1, ksize=3) / 8
    depth_edge = np.sqrt(depth_dx ** 2 + depth_dy ** 2)
    smooth_normal = cv2.GaussianBlur(normal, (0, 0), .85 * ratio + .25)
    normal_dx = cv2.Sobel(smooth_normal, cv2.CV_32F, 1, 0, ksize=3) / 8
    normal_dy = cv2.Sobel(smooth_normal, cv2.CV_32F, 0, 1, ksize=3) / 8
    change = np.sqrt(np.sum(normal_dx ** 2 + normal_dy ** 2, axis=2))
    silhouette = cv2.morphologyEx(valid.astype(np.uint8), cv2.MORPH_GRADIENT, np.ones((3, 3), np.uint8)) > 0
    edges = ((depth_edge > 2.6 / ratio) | (change > .22)) & valid
    edges |= silhouette
    line = edges.astype(np.float32)
    if style == "contours":
        line *= .62
    # Sparse parallel hatching on steep faces; its placement comes from measured geometry.
    yy, xx = np.indices((height, width), dtype=np.float32)
    steep = np.clip((.74 - normal[:, :, 2]) * 1.5, 0, 1)
    facing = np.clip(.58 - (normal[:, :, 0] * -.35 + normal[:, :, 1] * -.7 + normal[:, :, 2] * .7), 0, 1)
    hatch = (np.mod(xx + yy * .57, max(5, 8 * ratio)) < max(.9, 1.05 * ratio)).astype(np.float32)
    hatch *= steep * facing * valid * .50
    shading = np.clip((1 - normal[:, :, 2]) * .08 + facing * .055, 0, .14) * valid
    ink_amount = np.clip(np.maximum(line * .74, hatch) + shading, 0, .88)
    if style == "contours":
        ink_amount *= .7
    canvas = PAPER[None, None, :] + (INK - PAPER)[None, None, :] * ink_amount[:, :, None]
    image = Image.fromarray(np.uint8(np.clip(canvas, 0, 255)))
    draw = ImageDraw.Draw(image)
    margin = round(335 * ratio)
    draw_letterspaced(draw, (margin, round(233 * ratio)), "DUBLIN",
                     font(round(157 * ratio)), "#263e40", round(22 * ratio))
    draw_letterspaced(draw, (margin + 5, round(444 * ratio)), "A CITY IN A BILLION POINTS",
                     font(round(26 * ratio), "bahnschrift.ttf"), "#77857d", round(5 * ratio))
    draw.text((width - margin, round(281 * ratio)), "01 /  CITY STUDIES",
              font=font(round(26 * ratio), "bahnschrift.ttf"), fill="#617671", anchor="ra")
    draw.text((width - margin, round(328 * ratio)), "DUBLIN  /  MARCH 2015",
              font=font(round(23 * ratio), "bahnschrift.ttf"), fill="#8a9289", anchor="ra")
    footer = round(3260 * ratio)
    draw.line((margin, footer, width - margin, footer), fill="#bdc0b2", width=max(1, round(ratio)))
    draw.text((margin, footer + round(44 * ratio)), "THE LIFFEY & THE CITY CENTRE",
              font=font(round(29 * ratio), "bahnschrift.ttf"), fill="#344d4b")
    draw.text((margin, footer + round(92 * ratio)), "Isometric study from airborne laser returns",
              font=font(round(30 * ratio), "GARAIT.TTF"), fill="#738077")
    draw.text((width - margin, footer + round(48 * ratio)), "NO MAPS. NO PHOTOGRAPHS. ONLY LIGHT.",
              font=font(round(22 * ratio), "bahnschrift.ttf"), fill="#627872", anchor="ra")
    draw.text((width - margin, footer + round(91 * ratio)), "NYU Dublin LiDAR  /  CC BY 4.0",
              font=font(round(23 * ratio), "bahnschrift.ttf"), fill="#8b958a", anchor="ra")
    draw.text((margin, footer + round(166 * ratio)),
              "Data: D. F. Laefer, S. Abuwarda, A.-V. Vo, L. Truong-Hong & H. Gharibi",
              font=font(round(18 * ratio), "bahnschrift.ttf"), fill="#8b958a")
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--width", type=int, default=3200)
    parser.add_argument("--step", type=int, default=1)
    parser.add_argument("--height-scale", type=float, default=1.0)
    parser.add_argument("--style", choices=["ink", "contours"], default="ink")
    parser.add_argument("--reuse", action="store_true")
    args = parser.parse_args()
    started = time.monotonic()
    height = round(args.width * .75)
    cache = ROOT / f"render-buffers-{args.width}.npz"
    if args.reuse:
        saved = np.load(cache)
        normals, positions = saved["normal"], saved["position"]
    else:
        normals, positions = geometry_pass((args.width, height), args.step, args.height_scale)
        np.savez_compressed(cache, normal=normals, position=positions)
    image = compose(normals, positions, args.style)
    filename = f"dublin-isometric-{args.style}-{args.width}.png"
    publish(image, filename)
    print(json.dumps({"image": filename, "size": image.size,
                      "elapsed_seconds": round(time.monotonic() - started, 1)}))


if __name__ == "__main__":
    main()
