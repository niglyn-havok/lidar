"""Start a nonblocking MCP-controlled render with persistent progress reporting."""
import json
import time
from pathlib import Path

import bpy

ROOT = Path(__file__).resolve().parent
VARIANT = globals().get("ORBIT_VARIANT", "portrait")
if VARIANT not in {"portrait", "landscape"}:
    raise ValueError(f"Unknown orbit variant: {VARIANT}")
ORBIT = ROOT / ("orbit-landscape" if VARIANT == "landscape" else "orbit")
MODE = globals().get("RENDER_MODE", "final")
if MODE not in {"proof", "final"}:
    raise ValueError(f"Unknown render mode: {MODE}")
SCENE = bpy.context.scene
expected_size = (1920, 1080) if VARIANT == "landscape" else (1080, 1280)
if (SCENE.render.resolution_x, SCENE.render.resolution_y) != expected_size:
    raise ValueError(f"The active scene does not match {VARIANT} resolution.")
START = time.time()
FRAME_START = START
FRAMES_WRITTEN = []
STATUS = ORBIT / f"{MODE}-status.json"


def status(state):
    payload = {"state": state, "mode": MODE, "frame": SCENE.frame_current,
               "frames_written": FRAMES_WRITTEN,
               "elapsed_seconds": round(time.time() - START, 2),
               "updated_unix": time.time()}
    temporary = STATUS.with_suffix(".tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n")
    temporary.replace(STATUS)


def before(scene, *args):
    global FRAME_START
    FRAME_START = time.time()
    status("rendering")


def written(scene, *args):
    FRAMES_WRITTEN.append({"frame": scene.frame_current,
                           "seconds": round(time.time() - FRAME_START, 2)})
    status("rendering")


def finished(scene, *args):
    status("complete")


def cancelled(scene, *args):
    status("cancelled")


for handlers, handler in [
    (bpy.app.handlers.render_pre, before),
    (bpy.app.handlers.render_write, written),
    (bpy.app.handlers.render_complete, finished),
    (bpy.app.handlers.render_cancel, cancelled),
]:
    for previous in list(handlers):
        if getattr(previous, "_dublin_orbit", False):
            handlers.remove(previous)
    handler._dublin_orbit = True
    handlers.append(handler)

if MODE == "proof":
    SCENE.render.use_overwrite = True
    SCENE.render.resolution_percentage = 50
    SCENE.cycles.samples = 48
    SCENE.frame_step = 72
    SCENE.render.filepath = str(ORBIT / "proofs" / "proof-")
else:
    SCENE.render.use_overwrite = False
    SCENE.render.resolution_percentage = 100
    SCENE.cycles.samples = 64
    SCENE.frame_step = 1
    SCENE.render.filepath = str(ORBIT / "frames" / "frame-")
SCENE.frame_start, SCENE.frame_end = 1, 288
SCENE.frame_set(1)
status("starting")
bpy.ops.render.render("INVOKE_DEFAULT", animation=True)
print(f"{MODE.upper()}_RENDER_STARTED")
