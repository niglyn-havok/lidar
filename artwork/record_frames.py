import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import time

from PIL import Image


ROOT = Path(__file__).resolve().parent
CANVAS = ROOT / "working-canvas.png"
FRAMES = ROOT / "frames"
STOP = ROOT / "stop-recording.flag"


def initialize():
    if not CANVAS.exists():
        image = Image.new("RGB", (4800, 3600), "#f8f5ef")
        image.save(CANVAS)


def record():
    initialize()
    FRAMES.mkdir(exist_ok=True)
    if STOP.exists():
        raise FileExistsError("Recording stop marker already exists.")
    start = time.monotonic()
    deadline = start
    count = len(list(FRAMES.glob("frame-*.png")))
    with (FRAMES / "timeline.jsonl").open("a", encoding="utf-8") as log:
        while True:
            raw = CANVAS.read_bytes()
            with Image.open(io.BytesIO(raw)) as source:
                image = source.convert("RGB")
                image.thumbnail((1920, 1440), Image.Resampling.LANCZOS)
                name = f"frame-{count:05d}.png"
                image.save(FRAMES / name)
            log.write(json.dumps({
                "file": name,
                "utc": datetime.now(timezone.utc).isoformat(),
                "elapsed_seconds": round(time.monotonic() - start, 3),
                "source_sha256": hashlib.sha256(raw).hexdigest(),
            }) + "\n")
            log.flush()
            print(f"Saved {name}", flush=True)
            count += 1
            if STOP.exists():
                break
            deadline += 30
            while time.monotonic() < deadline and not STOP.exists():
                time.sleep(min(1, max(0, deadline - time.monotonic())))


def animate():
    import cv2
    import numpy as np

    paths = sorted(FRAMES.glob("frame-*.png"))
    if not paths:
        raise FileNotFoundError("No recorded frames.")
    images = []
    previous = None
    durations = []
    for path in paths:
        with Image.open(path) as source:
            image = source.convert("RGB")
            signature = hashlib.sha256(image.tobytes()).hexdigest()
            if signature == previous:
                durations[-1] += 120
                continue
            image.thumbnail((1280, 960), Image.Resampling.LANCZOS)
            images.append(image.copy())
            durations.append(500)
            previous = signature
    durations = [min(1800, duration) for duration in durations]
    durations[-1] = 2500
    images[0].save(
        ROOT / "dublin-drawing-timelapse.gif", save_all=True,
        append_images=images[1:], duration=durations, loop=0, disposal=2,
    )
    video_path = ROOT / "dublin-drawing-timelapse.mp4"
    fps = 24
    writer = cv2.VideoWriter(str(video_path), cv2.VideoWriter_fourcc(*"mp4v"), fps, images[0].size)
    if not writer.isOpened():
        raise RuntimeError("The installed video encoder could not open the MP4 output.")
    try:
        for image, duration in zip(images, durations):
            bgr = cv2.cvtColor(np.asarray(image), cv2.COLOR_RGB2BGR)
            for _ in range(round(duration / 1000 * fps)):
                writer.write(bgr)
    finally:
        writer.release()
    timeline = [json.loads(line) for line in (FRAMES / "timeline.jsonl").read_text().splitlines()]
    gaps = []
    for before, after in zip(timeline, timeline[1:]):
        elapsed = (datetime.fromisoformat(after["utc"]) - datetime.fromisoformat(before["utc"])).total_seconds()
        if elapsed > 45:
            gaps.append({"from_utc": before["utc"], "to_utc": after["utc"],
                         "gap_seconds": round(elapsed, 2)})
    report = {
        "recorded_frames": len(paths), "distinct_frames": len(images),
        "capture_interval_seconds": 30, "animation_size": images[0].size,
        "recording_gaps": gaps,
        "notes": "Frames are snapshots of the saved working image, not screen recordings. Identical snapshots are condensed in the animation. Crash-related gaps have not been fabricated or backfilled.",
    }
    (ROOT / "recording-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("operation", choices=["initialize", "record", "animate"])
    args = parser.parse_args()
    {"initialize": initialize, "record": record, "animate": animate}[args.operation]()
