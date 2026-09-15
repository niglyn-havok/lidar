"""Create a separate, loopable 12-second orbit from the saved hero camera."""
import json
import math
from pathlib import Path

import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parent
ORBIT = ROOT / "orbit"


def main():
    ORBIT.mkdir(exist_ok=True)
    (ORBIT / "frames").mkdir(exist_ok=True)
    (ORBIT / "proofs").mkdir(exist_ok=True)
    scene = bpy.context.scene
    if scene.camera.parent is not None:
        raise RuntimeError("Expected the original unparented hero camera.")
    camera = scene.camera
    original = camera.matrix_world.copy()
    rig = bpy.data.objects.new("ORBIT | linear 360 degrees in 12 seconds", None)
    bpy.data.collections["06 | CAMERAS"].objects.link(rig)
    camera.parent = rig
    camera.matrix_world = original
    rig.rotation_euler.z = 0
    rig.keyframe_insert(data_path="rotation_euler", index=2, frame=1)
    rig.rotation_euler.z = math.tau
    rig.keyframe_insert(data_path="rotation_euler", index=2, frame=289)
    for layer in rig.animation_data.action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                for curve in bag.fcurves:
                    for point in curve.keyframe_points:
                        point.interpolation = "LINEAR"
    scene.frame_start, scene.frame_end, scene.frame_step = 1, 288, 1
    scene.render.fps, scene.render.fps_base = 24, 1
    scene.render.resolution_x, scene.render.resolution_y = 1080, 1280
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.image_settings.color_depth = "8"
    scene.render.image_settings.compression = 20
    scene.render.filepath = str(ORBIT / "frames" / "frame-")
    scene.render.use_file_extension = True
    scene.render.use_overwrite = False
    scene.render.use_placeholder = False
    scene.render.use_persistent_data = True
    scene.cycles.samples = 64
    scene.cycles.adaptive_threshold = .035
    scene.cycles.use_animated_seed = False
    scene.cycles.seed = 19
    scene.cycles.use_denoising = True
    scene.render.use_compositing = True
    positions = []
    for frame in [1, 73, 145, 217, 289]:
        scene.frame_set(frame)
        bpy.context.view_layer.update()
        matrix = camera.matrix_world.copy()
        position = matrix.translation
        direction = -matrix.col[2].to_3d().normalized()
        target_direction = (Vector((0, 0, 3.5)) - position).normalized()
        assert abs(position.z - original.translation.z) < .0001
        assert direction.dot(target_direction) > .99999
        positions.append({"frame": frame, "position": list(position),
                          "angle_degrees": (frame - 1) * 360 / 288})
    loop_error = max(abs(camera.matrix_world[i][j] - original[i][j])
                     for i in range(4) for j in range(4))
    assert loop_error < .0001
    scene.frame_set(1)
    for light in scene.objects:
        if light.type == "LIGHT" and light.name.startswith("STRIP"):
            light_matrix = light.matrix_world.copy()
            light.parent = rig
            light.matrix_world = light_matrix
    scene["ORBIT"] = "12 seconds, 24fps, 288 frames. Frame 289 closes the loop but is not rendered. The camera and two reflection strips orbit; the snow globe and remaining studio stay stationary."
    scene["ORBIT START"] = "Original hero camera matrix, orthographic scale and elevation preserved."
    bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / "Dublin - Snow Globe - 12s Orbit.blend"))
    report = {
        "duration_seconds": 12, "fps": 24, "frames": 288,
        "resolution": [1080, 1280], "rotation_degrees": 360,
        "camera_height": original.translation.z,
        "camera_target_height": 3.5,
        "orthographic_scale": camera.data.ortho_scale,
        "start_matches_hero": True, "loop_matrix_error": loop_error,
        "reflection_strips_follow_camera": True,
        "key_views": positions, "samples": scene.cycles.samples,
        "blend_file": bpy.data.filepath,
    }
    (ORBIT / "orbit-settings.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
