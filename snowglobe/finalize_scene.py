"""Finishing, editorial compositor, alternate camera and portable scene packaging."""
import json
from pathlib import Path

import bmesh
import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parent


def main():
    scene = bpy.context.scene
    for name in ["LABEL | soft brass grazing light", "CAMERA | city detail"]:
        previous = bpy.data.objects.get(name)
        if previous:
            bpy.data.objects.remove(previous, do_unlink=True)
    for tree in list(bpy.data.node_groups):
        if tree.name.startswith("DUBLIN | print finishing"):
            bpy.data.node_groups.remove(tree, do_unlink=True)
    glass = bpy.data.objects["GLOBE | closed two-surface optical glass"]
    bm = bmesh.new()
    bm.from_mesh(glass.data)
    bmesh.ops.remove_doubles(bm, verts=list(bm.verts), dist=.00001)
    # Collapsing the two polar rings leaves a non-rendering edge between poles.
    bmesh.ops.delete(bm, geom=[e for e in bm.edges if e.is_wire], context="EDGES")
    bm.to_mesh(glass.data)
    bm.free()
    glass.data.update()
    # The key illuminates the inlaid lettering without a distracting glass card.
    front = Vector((19, -29, 0)).normalized()
    lamp = bpy.data.lights.new("LABEL | soft brass grazing light", "AREA")
    lamp.energy, lamp.shape, lamp.size, lamp.size_y = 95, "RECTANGLE", 5, 1
    lamp.color = (1, .82, .56)
    obj = bpy.data.objects.new(lamp.name, lamp)
    scene.collection.objects.link(obj)
    obj.location = front * 11 + Vector((0, 0, 2.5))
    target = front * 7.75 + Vector((0, 0, -.56))
    obj.rotation_euler = (target - obj.location).to_track_quat("-Z", "Y").to_euler()
    obj.visible_glossy = False
    obj.visible_transmission = False
    detail_data = bpy.data.cameras.new("CAMERA | city detail")
    detail = bpy.data.objects.new("CAMERA | city detail", detail_data)
    scene.collection.objects.link(detail)
    detail.location = (10, -15, 19)
    detail.rotation_euler = (Vector((0, 0, .15)) - detail.location).to_track_quat("-Z", "Y").to_euler()
    detail_data.type = "ORTHO"
    detail_data.ortho_scale = 16.8
    # Blender 5.2 uses an explicit compositor node group.
    tree = bpy.data.node_groups.new("DUBLIN | print finishing - disable compositing for clean beauty", "CompositorNodeTree")
    tree.interface.new_socket(name="Image", in_out="OUTPUT", socket_type="NodeSocketColor")
    scene.compositing_node_group = tree
    source = tree.nodes.new("CompositorNodeRLayers")
    source.location = (-500, 100)
    overlay = tree.nodes.new("CompositorNodeImage")
    overlay.image = bpy.data.images.load(str(ROOT / "assets" / "editorial-overlay.png"), check_existing=True)
    overlay.location = (-500, -100)
    scale = tree.nodes.new("CompositorNodeScale")
    scale.inputs["Type"].default_value = "Render Size"
    scale.inputs["Frame Type"].default_value = "Stretch"
    scale.location = (-280, -100)
    tree.links.new(overlay.outputs["Image"], scale.inputs["Image"])
    alpha = tree.nodes.new("CompositorNodeAlphaOver")
    alpha.location = (-50, 100)
    tree.links.new(source.outputs["Image"], alpha.inputs["Background"])
    tree.links.new(scale.outputs["Image"], alpha.inputs["Foreground"])
    output = tree.nodes.new("NodeGroupOutput")
    output.location = (180, 100)
    tree.links.new(alpha.outputs["Image"], output.inputs["Image"])
    scene.render.use_compositing = True
    groups = {}
    for name in ["01 | MEASURED DUBLIN", "02 | OPTICAL GLASS", "03 | TURNED PLINTH",
                 "04 | CLOUDS AND SNOW", "05 | PHOTOGRAPHIC STUDIO", "06 | CAMERAS"]:
        collection = bpy.data.collections.get(name)
        if collection is None:
            collection = bpy.data.collections.new(name)
            scene.collection.children.link(collection)
        groups[name] = collection
    for obj in list(scene.objects):
        if obj.type == "CAMERA":
            key = "06 | CAMERAS"
        elif obj.name.startswith(("DUBLIN |", "CANOPY |", "LANDMARK |", "CITY SECTION |")):
            key = "01 | MEASURED DUBLIN"
        elif obj.name.startswith("GLOBE |"):
            key = "02 | OPTICAL GLASS"
        elif obj.name.startswith(("PLINTH |", "BRASS")):
            key = "03 | TURNED PLINTH"
        elif obj.name.startswith(("CLOUD |", "ATMOSPHERE |", "SNOW |")):
            key = "04 | CLOUDS AND SNOW"
        else:
            key = "05 | PHOTOGRAPHIC STUDIO"
        for old in list(obj.users_collection):
            old.objects.unlink(obj)
        groups[key].objects.link(obj)
    scene.render.resolution_x = 3200
    scene.render.resolution_y = 3800
    scene.render.resolution_percentage = 100
    scene.cycles.samples = 256
    scene.cycles.adaptive_threshold = .012
    scene.cycles.use_denoising = True
    scene.render.image_settings.color_depth = "16"
    scene.render.filepath = str(ROOT / "Dublin - A City Held in Glass.png")
    scene["README"] = "Hero camera and final print compositor are active. For an unlettered render disable Post Processing > Compositing. Alternate city-detail camera is in collection 06."
    scene["SOURCE DATUM"] = "Irish National Grid EPSG:29903; elevations Malin Head; centre E315989 N234393."
    scene["SURVEY DATE"] = "2015-03-26; imagery is not asserted to be contemporaneous."
    scene["OPTICAL APPROXIMATIONS"] = "Refractive two-surface glass with shadow-ray caustic approximation. Interior glossy visibility reduced to suppress distracting multiple reflections."
    bpy.ops.file.pack_all()
    for image in bpy.data.images:
        if image.source == "FILE" and image.packed_file:
            image.filepath = "//assets/" + Path(image.filepath).name
    bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / "Dublin - OConnell Bridge - Snow Globe.blend"))
    report = {
        "blend_path": bpy.data.filepath,
        "objects": len(scene.objects),
        "mesh_vertices": sum(len(m.vertices) for m in bpy.data.meshes if m.users),
        "mesh_polygons": sum(len(m.polygons) for m in bpy.data.meshes if m.users),
        "packed_images": [i.name for i in bpy.data.images if i.packed_file],
        "unpacked_file_images": [i.name for i in bpy.data.images if i.source == "FILE" and not i.packed_file],
        "camera": scene.camera.name,
        "final_resolution": [scene.render.resolution_x, scene.render.resolution_y],
        "cycles_samples": scene.cycles.samples,
        "glass_shell_thickness": .035,
        "bridge_center_model_xy": [0, 0],
    }
    (ROOT / "scene-manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
