"""Make a text-free 16:9 variant without changing the portrait orbit on disk."""
import json
from pathlib import Path

import bpy
import numpy as np

ROOT = Path(__file__).resolve().parent
ORBIT = ROOT / "orbit-landscape"


def projected_bounds(obj, matrix):
    vertices = np.empty(len(obj.data.vertices) * 3, dtype=np.float32)
    obj.data.vertices.foreach_get("co", vertices)
    vertices = np.column_stack((vertices.reshape(-1, 3), np.ones(len(obj.data.vertices))))
    clip = vertices @ (np.array(matrix) @ np.array(obj.matrix_world)).T
    uv = clip[:, :2] / clip[:, 3:4] * .5 + .5
    return [uv.min(axis=0).tolist(), uv.max(axis=0).tolist()]


def main():
    scene = bpy.context.scene
    if bpy.app.is_job_running("RENDER"):
        raise RuntimeError("Wait for the current render before making the landscape copy.")
    if "ORBIT | linear 360 degrees in 12 seconds" not in scene.objects:
        raise RuntimeError("Load the existing portrait orbit scene first.")
    (ORBIT / "frames").mkdir(parents=True, exist_ok=True)
    (ORBIT / "proofs").mkdir(exist_ok=True)
    removed_text = []
    for obj in list(scene.objects):
        if obj.type == "FONT":
            removed_text.append(obj.name)
            bpy.data.objects.remove(obj, do_unlink=True)
    scene.render.use_compositing = False
    scene.compositing_node_group = None
    overlay = bpy.data.images.get("editorial-overlay.png")
    if overlay:
        bpy.data.images.remove(overlay, do_unlink=True)
    scene.render.resolution_x, scene.render.resolution_y = 1920, 1080
    scene.render.resolution_percentage = 100
    scene.camera.data.ortho_scale = 31.8
    scene.frame_start, scene.frame_end, scene.frame_step = 1, 288, 1
    scene.render.fps, scene.render.fps_base = 24, 1
    scene.cycles.samples = 64
    scene.cycles.adaptive_threshold = .035
    scene.render.filepath = str(ORBIT / "frames" / "frame-")
    scene.render.use_overwrite = False
    checks = []
    for frame in [1, 73, 145, 217]:
        scene.frame_set(frame)
        bpy.context.view_layer.update()
        camera = scene.camera
        depsgraph = bpy.context.evaluated_depsgraph_get()
        projection = camera.calc_matrix_camera(depsgraph, x=1920, y=1080)
        transform = projection @ camera.matrix_world.inverted()
        for name in ["GLOBE | closed two-surface optical glass", "PLINTH | bespoke turned walnut"]:
            bounds = projected_bounds(bpy.data.objects[name], transform)
            assert min(bounds[0]) > .035, (frame, name, bounds)
            assert max(bounds[1]) < .965, (frame, name, bounds)
            checks.append({"frame": frame, "object": name, "normalized_bounds": bounds})
    scene.frame_set(1)
    assert not any(obj.type == "FONT" for obj in scene.objects)
    assert not scene.render.use_compositing
    assert abs(scene.camera.matrix_world.translation.z - 21) < .0001
    scene["README"] = "Text-free landscape orbit: 1920x1080, 24fps, frames 1-288, 12 seconds. No text overlay or lettering. Original camera elevation retained; framing expanded for 16:9."
    bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / "Dublin - Snow Globe - 12s Landscape Orbit.blend"))
    report = {
        "duration_seconds": 12, "fps": 24, "frames": 288,
        "resolution": [1920, 1080], "rotation_degrees": 360,
        "camera_height": 21, "camera_target_height": 3.5,
        "orthographic_scale": scene.camera.data.ortho_scale,
        "removed_lettering": removed_text, "compositing_enabled": False,
        "text_free": True, "reflection_strips_follow_camera": True,
        "framing_checks": checks, "samples": 64,
        "blend_file": bpy.data.filepath,
    }
    (ORBIT / "orbit-settings.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
