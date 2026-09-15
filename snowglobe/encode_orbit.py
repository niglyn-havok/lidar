"""Encode and fully decode-check the completed 288-frame orbit."""
import argparse
import json
import subprocess
from pathlib import Path

import imageio_ffmpeg
import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
EXPECTED_FRAMES = 288
FPS = 24


def main(landscape=False):
    ORBIT = ROOT / ("orbit-landscape" if landscape else "orbit")
    filename = "Dublin - Snow Globe - 12s Landscape Orbit.mp4" if landscape else "Dublin - Snow Globe - 12s Orbit.mp4"
    OUTPUT = ROOT / filename
    TEMPORARY = ORBIT / "orbit-encoding.mp4"
    SIZE = (1920, 1080) if landscape else (1080, 1280)
    state = json.loads((ORBIT / "final-status.json").read_text())
    if state["state"] != "complete":
        raise RuntimeError(f"Rendering has not finished: {state['state']}")
    for frame in range(1, EXPECTED_FRAMES + 1):
        path = ORBIT / "frames" / f"frame-{frame:04d}.png"
        with Image.open(path) as image:
            image.load()
            if image.size != SIZE:
                raise ValueError(f"Frame {frame}: unexpected dimensions {image.size}")
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-framerate", str(FPS), "-start_number", "1",
        "-i", str(ORBIT / "frames" / "frame-%04d.png"),
        "-frames:v", str(EXPECTED_FRAMES), "-an",
        "-c:v", "libx264", "-preset", "slow", "-crf", "16",
        "-vf", "scale=out_color_matrix=bt709:out_range=tv",
        "-pix_fmt", "yuv420p", "-colorspace", "bt709",
        "-color_primaries", "bt709", "-color_trc", "bt709",
        "-movflags", "+faststart",
        "-metadata", "title=Dublin - A City Held in Glass - 12 Second Orbit",
        "-metadata", "comment=LiDAR: Laefer et al. 2015, CC BY 4.0. Imagery: Esri, Vantor, Earthstar Geographics, GIS User Community. See ABOUT.txt.",
        str(TEMPORARY),
    ]
    subprocess.run(command, check=True)
    reader = imageio_ffmpeg.read_frames(str(TEMPORARY), pix_fmt="rgb24")
    metadata = next(reader)
    count = 0
    selected = {}
    first = previous = None
    differences = []
    for raw in reader:
        array = np.frombuffer(raw, dtype=np.uint8).reshape(SIZE[1], SIZE[0], 3)
        sample = array[::8, ::8].astype(np.float32)
        if first is None:
            first = sample.copy()
        if previous is not None:
            differences.append(float(np.abs(sample - previous).mean()))
        previous = sample.copy()
        if count in [0, 48, 96, 144, 192, 240, 287]:
            selected[count] = Image.fromarray(array.copy())
        count += 1
    if count != EXPECTED_FRAMES:
        raise RuntimeError(f"Decoded {count} frames, expected {EXPECTED_FRAMES}")
    if abs(metadata["fps"] - FPS) > .001 or abs(metadata["duration"] - 12) > .01:
        raise RuntimeError(f"Unexpected timing: {metadata}")
    if tuple(metadata["size"]) != SIZE:
        raise RuntimeError(f"Unexpected encoded dimensions: {metadata}")
    if min(differences) <= .05:
        raise RuntimeError("Detected repeated/frozen adjacent frames")
    seam_difference = float(np.abs(previous - first).mean())
    median_difference = float(np.median(differences))
    if seam_difference > median_difference * 2.5:
        raise RuntimeError("Loop seam is inconsistent with normal frame-to-frame motion")
    # Keep the first six evenly spaced views as a compact inspection artifact.
    thumbnail_height = round(360 * SIZE[1] / SIZE[0])
    row_height = thumbnail_height + 23
    sheet = Image.new("RGB", (1080, row_height * 2), (10, 24, 30))
    draw = ImageDraw.Draw(sheet)
    for index, frame in enumerate([0, 48, 96, 144, 192, 240]):
        x, y = index % 3 * 360, index // 3 * row_height
        image = selected[frame].resize((360, thumbnail_height), Image.Resampling.LANCZOS)
        sheet.paste(image, (x, y))
        draw.text((x + 15, y + thumbnail_height + 4), f"{frame / FPS:.0f}s / {frame * 360 / EXPECTED_FRAMES:.0f} degrees",
                  fill=(205, 211, 195))
    sheet.save(ORBIT / "video-contact-sheet.jpg", quality=95)
    TEMPORARY.replace(OUTPUT)
    report = {
        "file": OUTPUT.name, "duration_seconds": metadata["duration"],
        "fps": metadata["fps"], "decoded_frames": count,
        "resolution": list(metadata["size"]), "codec": metadata["codec"],
        "bytes": OUTPUT.stat().st_size, "rotation_degrees": 360,
        "median_adjacent_frame_difference": median_difference,
        "loop_seam_difference": seam_difference,
        "all_frames_decoded": True, "framing_matches_preview": not landscape,
        "preview_elevation_preserved": True, "landscape": landscape,
        "text_free": landscape,
        "render_seconds": state["elapsed_seconds"],
    }
    (ORBIT / "video-manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--landscape", action="store_true")
    main(landscape=parser.parse_args().landscape)
