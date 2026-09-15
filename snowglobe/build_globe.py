"""Run inside the managed Blender instance through blender-control MCP."""
import json
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / "assets"
RNG = np.random.default_rng(20260908)


def material(name, color, metallic=0, roughness=.5):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    shader = mat.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*color, 1)
    shader.inputs["Metallic"].default_value = metallic
    shader.inputs["Roughness"].default_value = roughness
    return mat, shader


def mesh_object(name, vertices, faces, mat=None):
    mesh = bpy.data.meshes.new(name)
    if isinstance(vertices, np.ndarray) and isinstance(faces, np.ndarray):
        mesh.vertices.add(len(vertices))
        mesh.vertices.foreach_set("co", vertices.astype(np.float32).ravel())
        mesh.loops.add(faces.size)
        mesh.loops.foreach_set("vertex_index", faces.astype(np.int32).ravel())
        mesh.polygons.add(len(faces))
        mesh.polygons.foreach_set("loop_start", np.arange(len(faces), dtype=np.int32) * faces.shape[1])
        mesh.polygons.foreach_set("loop_total", np.full(len(faces), faces.shape[1], dtype=np.int32))
        mesh.update()
    else:
        mesh.from_pydata(vertices, [], faces)
        mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    if mat:
        mesh.materials.append(mat)
    return obj


def lathe(name, profile, mat, segments=256):
    vertices = [(r * math.cos(a * math.tau / segments),
                 r * math.sin(a * math.tau / segments), z)
                for r, z in profile for a in range(segments)]
    faces = [(j * segments + i, j * segments + (i + 1) % segments,
              (j + 1) * segments + (i + 1) % segments, (j + 1) * segments + i)
             for j in range(len(profile) - 1) for i in range(segments)]
    obj = mesh_object(name, vertices, faces, mat)
    for p in obj.data.polygons:
        p.use_smooth = True
    return obj


def torus(name, radius, minor, z, mat):
    bpy.ops.mesh.primitive_torus_add(major_segments=256, minor_segments=16,
                                    location=(0, 0, z), major_radius=radius,
                                    minor_radius=minor)
    obj = bpy.context.object
    obj.name = name
    obj.data.materials.append(mat)
    for p in obj.data.polygons:
        p.use_smooth = True
    return obj


def aim(obj, target):
    obj.rotation_euler = (Vector(target) - obj.location).to_track_quat("-Z", "Y").to_euler()


def area(name, location, target, energy, color, size, size_y=None):
    data = bpy.data.lights.new(name, "AREA")
    data.energy, data.color = energy, color
    if size_y is None:
        data.shape, data.size = "DISK", size
    else:
        data.shape, data.size, data.size_y = "RECTANGLE", size, size_y
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    obj.location = location
    aim(obj, target)
    return obj


def aerial_material():
    mat, shader = material("CITY | rectified aerial rooftops and streets", (.3, .3, .3), roughness=.79)
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    uv = nodes.new("ShaderNodeTexCoord")
    image = nodes.new("ShaderNodeTexImage")
    image.image = bpy.data.images.load(str(ASSETS / "dublin-aerial.jpg"), check_existing=True)
    image.interpolation = "Linear"
    links.new(uv.outputs["UV"], image.inputs["Vector"])
    hue = nodes.new("ShaderNodeHueSaturation")
    hue.inputs["Saturation"].default_value = 1.1
    hue.inputs["Value"].default_value = .94
    links.new(image.outputs["Color"], hue.inputs["Color"])
    links.new(hue.outputs["Color"], shader.inputs["Base Color"])
    noise = nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 260
    links.new(uv.outputs["Object"], noise.inputs["Vector"])
    bump = nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = .16
    bump.inputs["Distance"].default_value = .003
    links.new(noise.outputs["Fac"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], shader.inputs["Normal"])
    return mat


def facade_material():
    mat, shader = material("CITY | limestone brick and restrained window rhythm", (.29, .25, .20), roughness=.72)
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    geo = nodes.new("ShaderNodeNewGeometry")
    separate = nodes.new("ShaderNodeSeparateXYZ")
    links.new(geo.outputs["Position"], separate.inputs[0])
    # Detail is decorative: the LiDAR, not this shader, defines all buildings.
    phase = nodes.new("ShaderNodeMath")
    phase.operation = "MULTIPLY_ADD"
    phase.inputs[1].default_value = 1
    phase.inputs[2].default_value = 0
    links.new(separate.outputs["X"], phase.inputs[0])
    combine = nodes.new("ShaderNodeCombineXYZ")
    links.new(separate.outputs["X"], combine.inputs["X"])
    links.new(separate.outputs["Y"], combine.inputs["Y"])
    links.new(separate.outputs["Z"], combine.inputs["Z"])
    mapping = nodes.new("ShaderNodeVectorMath")
    mapping.operation = "SCALE"
    mapping.inputs["Scale"].default_value = 1
    links.new(combine.outputs[0], mapping.inputs[0])
    noise = nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 3.5
    noise.inputs["Detail"].default_value = 1
    links.new(mapping.outputs[0], noise.inputs["Vector"])
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = .2
    ramp.color_ramp.elements[0].color = (.13, .105, .08, 1)
    ramp.color_ramp.elements[1].position = .8
    ramp.color_ramp.elements[1].color = (.56, .48, .37, 1)
    links.new(noise.outputs["Fac"], ramp.inputs[0])
    brickvec = nodes.new("ShaderNodeCombineXYZ")
    xy = nodes.new("ShaderNodeMath")
    xy.operation = "ADD"
    links.new(separate.outputs["X"], xy.inputs[0])
    links.new(separate.outputs["Y"], xy.inputs[1])
    links.new(xy.outputs[0], brickvec.inputs["X"])
    links.new(separate.outputs["Z"], brickvec.inputs["Y"])
    brick = nodes.new("ShaderNodeTexBrick")
    brick.inputs["Scale"].default_value = 1
    brick.inputs["Mortar Size"].default_value = .006
    brick.inputs["Mortar Smooth"].default_value = .0005
    brick.inputs["Brick Width"].default_value = .028
    brick.inputs["Row Height"].default_value = .034
    brick.inputs["Color1"].default_value = (.025, .04, .045, 1)
    brick.inputs["Color2"].default_value = (.13, .16, .16, 1)
    links.new(ramp.outputs["Color"], brick.inputs["Mortar"])
    links.new(brickvec.outputs[0], brick.inputs["Vector"])
    links.new(brick.outputs["Color"], shader.inputs["Base Color"])
    return mat


def water_material():
    mat, shader = material("LIFFEY | deep green water with capillary ripples", (.019, .095, .085), metallic=.24, roughness=.18)
    shader.inputs["IOR"].default_value = 1.333
    shader.inputs["Coat Weight"].default_value = .5
    shader.inputs["Coat Roughness"].default_value = .13
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    geo = nodes.new("ShaderNodeTexCoord")
    mapping = nodes.new("ShaderNodeVectorMath")
    mapping.operation = "MULTIPLY"
    mapping.inputs[1].default_value = (1.2, 7, 2)
    links.new(geo.outputs["Object"], mapping.inputs[0])
    noise = nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 120
    noise.inputs["Detail"].default_value = 2
    noise.inputs["Roughness"].default_value = .65
    links.new(mapping.outputs[0], noise.inputs["Vector"])
    bump = nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = .24
    bump.inputs["Distance"].default_value = .002
    links.new(noise.outputs["Fac"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], shader.inputs["Normal"])
    return mat


def build_city():
    data = np.load(ASSETS / "city-grid.npz")
    h, water, green, canopy = (data[k] for k in ["height", "water", "green", "canopy"])
    ny, nx = h.shape
    yy, xx = np.mgrid[-800:801, -800:801]
    vertices = np.column_stack((xx.ravel() * .01, yy.ravel() * .01, h.ravel() * .01 + .132))
    ids = np.arange(nx * ny, dtype=np.int32).reshape(ny, nx)
    faces = np.stack((ids[:-1, :-1], ids[:-1, 1:], ids[1:, 1:], ids[1:, :-1]), axis=-1).reshape(-1, 4)
    circle = ((xx[:-1, :-1] + .5) ** 2 + (yy[:-1, :-1] + .5) ** 2) < 760 ** 2
    faces = faces[circle.ravel()]
    mat = aerial_material()
    used, compact_faces = np.unique(faces, return_inverse=True)
    obj = mesh_object("DUBLIN | measured 1m LiDAR surface - 1520m diameter",
                      vertices[used], compact_faces.reshape(-1, 4), mat)
    obj["source"] = "Supplied 2015 Dublin LiDAR / 1m measured-return surface"
    obj["center_irish_grid"] = [315989.0, 234393.0]
    obj["geographic_scale"] = "1 Blender unit = 100 surveyed metres; no height exaggeration"
    obj.data.materials.append(facade_material())
    obj.data.materials.append(water_material())
    foliage, fs = material("CITY | living canopy - Irish summer greens", (.055, .16, .033), roughness=.88)
    fs.inputs["Subsurface Weight"].default_value = .055
    obj.data.materials.append(foliage)
    heights = h.ravel()[faces]
    material_ids = np.zeros(len(faces), np.int32)
    material_ids[np.ptp(heights, axis=1) > 2.8] = 1
    is_water = water.ravel()[faces].sum(axis=1) >= 3
    material_ids[is_water] = 2
    is_leaf = canopy.ravel()[faces].sum(axis=1) >= 3
    material_ids[is_leaf] = 3
    obj.data.polygons.foreach_set("material_index", material_ids)
    uv = obj.data.uv_layers.new(name="Irish National Grid | orthorectified XY")
    coords = vertices[faces.ravel(), :2] / 16 + .5
    uv.data.foreach_set("uv", coords.astype(np.float32).ravel())
    # Measured-canopy leaf clusters add a broken silhouette, not arbitrary trees.
    choices = np.flatnonzero((canopy & ((xx ** 2 + yy ** 2) < 753 ** 2)).ravel())
    choices = RNG.choice(choices, size=min(22000, len(choices)), replace=False)
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=1)
    prototype = bpy.context.object
    leaf_v = np.array([v.co[:] for v in prototype.data.vertices])
    leaf_f = np.array([p.vertices[:] for p in prototype.data.polygons], dtype=np.int32)
    bpy.data.objects.remove(prototype, do_unlink=True)
    centers = vertices[choices].copy()
    centers[:, 2] += .003
    sizes = RNG.uniform(.006, .020, (len(choices), 1, 1))
    leaf_vertices = (leaf_v[None, :, :] * sizes + centers[:, None, :]).reshape(-1, 3)
    leaf_faces = (leaf_f[None, :, :] + np.arange(len(choices))[:, None, None] * len(leaf_v)).reshape(-1, 3)
    leaves = mesh_object("CANOPY | LiDAR-located leaf clusters", leaf_vertices, leaf_faces, foliage)
    for c in [(.045, .115, .026), (.095, .19, .043), (.12, .205, .04), (.031, .09, .021)]:
        leaves.data.materials.append(material("CANOPY | botanical variation", c, roughness=.86)[0])
    leaves.data.polygons.foreach_set("material_index", np.repeat(RNG.integers(0, 5, len(choices), dtype=np.int32), len(leaf_f)))
    steel, _ = material("LANDMARK | satin stainless steel", (.45, .5, .53), metallic=.82, roughness=.24)
    base_z, tip_z = 6.45 * .01 + .132, 124.609 * .01 + .132
    bpy.ops.mesh.primitive_cone_add(vertices=48, radius1=.015, radius2=.0008,
                                    depth=tip_z - base_z,
                                    location=(-.855, 2.795, (tip_z + base_z) / 2))
    spire = bpy.context.object
    spire.name = "LANDMARK | The Spire - restored from raw measured tip"
    spire.data.materials.append(steel)
    for p in spire.data.polygons:
        p.use_smooth = True
    spire["measured_tip_elevation_metres"] = 124.609
    angles = np.linspace(0, math.tau, 1600, endpoint=False)
    edge_xy = np.column_stack((np.cos(angles), np.sin(angles))) * 7.599
    edge_indices = np.clip(np.rint(edge_xy * 100 + 800).astype(int), 0, 1600)
    edge_z = h[edge_indices[:, 1], edge_indices[:, 0]] * .01 + .132
    skirt_v = np.vstack((np.column_stack((edge_xy, np.full(1600, .115))),
                         np.column_stack((edge_xy, edge_z))))
    skirt_i = np.arange(1600)
    skirt_f = np.column_stack((skirt_i, (skirt_i + 1) % 1600,
                               (skirt_i + 1) % 1600 + 1600, skirt_i + 1600))
    section, _ = material("CITY SECTION | vertical cut through the survey", (.075, .085, .075), roughness=.8)
    mesh_object("CITY SECTION | closed LiDAR perimeter", skirt_v, skirt_f, section)
    print(f"CITY_READY: {len(faces):,} quads, {len(choices):,} canopy clusters", flush=True)


def build_base():
    gold, gs = material("HARDWARE | satin champagne brass", (.55, .33, .105), metallic=.82, roughness=.24)
    gs.inputs["Coat Weight"].default_value = .2
    dark, ds = material("BASE | ebonised walnut", (.025, .017, .011), metallic=.12, roughness=.29)
    ds.inputs["Coat Weight"].default_value = .38
    ds.inputs["Coat Roughness"].default_value = .2
    nodes, links = dark.node_tree.nodes, dark.node_tree.links
    tex = nodes.new("ShaderNodeTexNoise")
    tex.inputs["Scale"].default_value = 5
    tex.inputs["Detail"].default_value = 3
    coord = nodes.new("ShaderNodeTexCoord")
    mapping = nodes.new("ShaderNodeVectorMath")
    mapping.operation = "MULTIPLY"
    mapping.inputs[1].default_value = (1, 1, 45)
    links.new(coord.outputs["Object"], mapping.inputs[0])
    links.new(mapping.outputs[0], tex.inputs["Vector"])
    ramp = nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = (.009, .006, .004, 1)
    ramp.color_ramp.elements[1].color = (.065, .032, .013, 1)
    links.new(tex.outputs["Fac"], ramp.inputs[0])
    links.new(ramp.outputs["Color"], ds.inputs["Base Color"])
    lathe("PLINTH | bespoke turned walnut", [
        (0, -1.12), (7.45, -1.12), (7.69, -1.10), (7.82, -1.04),
        (7.88, -.94), (7.87, -.86), (7.75, -.79), (7.72, -.68),
        (7.72, -.35), (7.79, -.25), (7.92, -.19), (7.94, -.11),
        (7.87, -.045), (7.65, .0), (0, .0)], dark)
    torus("BRASS | lower pinstripe", 7.81, .024, -.90, gold)
    torus("BRASS | upper reveal", 7.88, .026, -.12, gold)
    torus("BRASS | glass retaining bead", 7.676, .048, .036, gold)
    stone, _ = material("CITY SECTION | dark limestone", (.095, .103, .087), roughness=.73)
    lathe("CITY SECTION | circular survey cut", [(0, -.02), (7.60, -.02), (7.60, .118), (0, .118)], stone)
    # Small inlays on the retaining rim provide a precise instrument-like finish.
    for angle in np.linspace(0, math.tau, 72, endpoint=False):
        bpy.ops.mesh.primitive_cube_add(size=1, location=(7.73 * math.cos(angle), 7.73 * math.sin(angle), -.025))
        tick = bpy.context.object
        tick.name = "BRASS | radial graduation"
        tick.scale = (.055 if round(angle / math.tau * 72) % 6 == 0 else .027, .009, .003)
        tick.rotation_euler.z = angle
        tick.data.materials.append(gold)
    return gold


def build_glass():
    glass = bpy.data.materials.new("GLASS | optical shell - IOR 1.46 - 3cm model thickness")
    glass.use_nodes = True
    nodes, links = glass.node_tree.nodes, glass.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    shader = nodes.new("ShaderNodeBsdfPrincipled")
    shader.inputs["Base Color"].default_value = (.97, .995, 1, 1)
    shader.inputs["Roughness"].default_value = .025
    shader.inputs["IOR"].default_value = 1.46
    shader.inputs["Transmission Weight"].default_value = 1
    transparent = nodes.new("ShaderNodeBsdfTransparent")
    rays = nodes.new("ShaderNodeLightPath")
    mix = nodes.new("ShaderNodeMixShader")
    # Preserve full camera optics; transparent shadow rays approximate otherwise
    # prohibitively expensive diffuse light paths through a closed glass shell.
    links.new(rays.outputs["Is Shadow Ray"], mix.inputs[0])
    links.new(shader.outputs[0], mix.inputs[1])
    links.new(transparent.outputs[0], mix.inputs[2])
    links.new(mix.outputs[0], output.inputs["Surface"])
    absorb = nodes.new("ShaderNodeVolumeAbsorption")
    absorb.inputs["Color"].default_value = (.73, .92, .9, 1)
    absorb.inputs["Density"].default_value = .05
    links.new(absorb.outputs[0], output.inputs["Volume"])
    radius, center, cut = 8.30, 3.00, -.16
    theta = math.acos((cut - center) / radius)
    profile = [(radius * math.sin(t), center + radius * math.cos(t))
               for t in np.linspace(theta, 0, 160)]
    inner = radius - .035
    inner_theta = math.acos((cut - center) / inner)
    profile += [(inner * math.sin(t), center + inner * math.cos(t))
                for t in np.linspace(0, inner_theta, 160)]
    profile.append(profile[0])
    globe = lathe("GLOBE | closed two-surface optical glass", profile, glass, 320)
    globe["wall_thickness_model_units"] = .035
    globe["optics"] = "Physical camera-ray refraction and Fresnel reflection; shadow-ray caustic approximation"
    # The lathe travels upward on the exterior; its outward normals are correct.
    return globe


def cloud_material():
    mat = bpy.data.materials.new("ATMOSPHERE | soft fractal cloud vapor")
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    volume = nodes.new("ShaderNodeVolumePrincipled")
    volume.inputs["Color"].default_value = (.87, .94, 1, 1)
    volume.inputs["Anisotropy"].default_value = .25
    coord = nodes.new("ShaderNodeTexCoord")
    center = nodes.new("ShaderNodeVectorMath")
    center.operation = "SUBTRACT"
    center.inputs[1].default_value = (.5, .5, .5)
    links.new(coord.outputs["Generated"], center.inputs[0])
    length = nodes.new("ShaderNodeVectorMath")
    length.operation = "LENGTH"
    links.new(center.outputs[0], length.inputs[0])
    edge = nodes.new("ShaderNodeMapRange")
    edge.inputs["From Min"].default_value = .19
    edge.inputs["From Max"].default_value = .5
    edge.inputs["To Min"].default_value = 1
    edge.inputs["To Max"].default_value = 0
    links.new(length.outputs["Value"], edge.inputs["Value"])
    noise = nodes.new("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 7
    noise.inputs["Detail"].default_value = 5
    noise.inputs["Roughness"].default_value = .7
    links.new(coord.outputs["Generated"], noise.inputs["Vector"])
    ramp = nodes.new("ShaderNodeMapRange")
    ramp.inputs["From Min"].default_value = .46
    ramp.inputs["From Max"].default_value = .79
    ramp.inputs["To Min"].default_value = 0
    ramp.inputs["To Max"].default_value = 1.0
    links.new(noise.outputs["Fac"], ramp.inputs["Value"])
    mul = nodes.new("ShaderNodeMath")
    mul.operation = "MULTIPLY"
    links.new(edge.outputs[0], mul.inputs[0])
    links.new(ramp.outputs[0], mul.inputs[1])
    links.new(mul.outputs[0], volume.inputs["Density"])
    links.new(volume.outputs[0], output.inputs["Volume"])
    return mat


def build_atmosphere():
    mat = cloud_material()
    clouds = [
        ((-3.8, 2.2, 3.3), (2.3, .88, .45)),
        ((-2.2, 2.9, 3.45), (1.7, .72, .48)),
        ((3.4, 3.0, 4.7), (2.0, .73, .43)),
        ((4.6, 2.1, 4.65), (1.15, .64, .35)),
        ((-1.5, 3.0, 7.2), (2.1, .75, .20)),
    ]
    for i, (position, scale) in enumerate(clouds):
        bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, location=position)
        obj = bpy.context.object
        obj.name = f"CLOUD | suspended vapor wisp {i + 1:02}"
        obj.scale = scale
        obj.data.materials.append(mat)
    haze = bpy.data.materials.new("ATMOSPHERE | subtle interior aerial perspective")
    haze.use_nodes = True
    n = haze.node_tree.nodes
    n.clear()
    out = n.new("ShaderNodeOutputMaterial")
    volume = n.new("ShaderNodeVolumePrincipled")
    volume.inputs["Density"].default_value = .0025
    volume.inputs["Color"].default_value = (.64, .78, .83, 1)
    volume.inputs["Anisotropy"].default_value = .2
    haze.node_tree.links.new(volume.outputs[0], out.inputs["Volume"])
    bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=7.98, location=(0, 0, 3))
    air = bpy.context.object
    air.name = "ATMOSPHERE | contained clean air"
    air.data.materials.append(haze)
    # A sparse handful of drifting ice crystals; never a blizzard over the city.
    pearl, ps = material("SNOW | tiny suspended ice crystals", (.83, .91, .94), roughness=.22)
    ps.inputs["Subsurface Weight"].default_value = .12
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=1)
    temp = bpy.context.object
    v = np.array([p.co[:] for p in temp.data.vertices])
    f = np.array([p.vertices[:] for p in temp.data.polygons], dtype=np.int32)
    bpy.data.objects.remove(temp, do_unlink=True)
    points = RNG.uniform((-6, -5, 1), (6, 5, 9), (220, 3))
    points = points[np.linalg.norm(points - (0, 0, 3), axis=1) < 7.5]
    sizes = RNG.uniform(.007, .017, (len(points), 1, 1))
    mesh_object("SNOW | sparse floating crystals",
                (v[None] * sizes + points[:, None]).reshape(-1, 3),
                (f[None] + np.arange(len(points))[:, None, None] * len(v)).reshape(-1, 3), pearl)


def setup_studio(gold):
    scene = bpy.context.scene
    floor, fs = material("STUDIO | midnight petrol lacquer", (.014, .034, .043), metallic=.22, roughness=.31)
    bpy.ops.mesh.primitive_plane_add(size=200, location=(0, 0, -1.14))
    bpy.context.object.name = "STUDIO | seamless reflective floor"
    bpy.context.object.data.materials.append(floor)
    scene.world.color = (.1, .1, .1)
    scene.world.use_nodes = True
    background = scene.world.node_tree.nodes.get("Background")
    background.inputs["Color"].default_value = (.20, .30, .40, 1)
    background.inputs["Strength"].default_value = .3
    area("SOFTBOX | warm silk key", (-10, -9, 16), (0, 0, 2), 1600, (1, .82, .62), 8, 11)
    area("STRIP | cool left glass contour", (-12, 1, 8), (0, 0, 4), 1600, (.55, .79, 1), .8, 11)
    area("STRIP | champagne right glass contour", (10, 4, 12), (0, 0, 5), 2100, (1, .8, .49), 1.0, 10)
    area("FILL | camera silk", (6, -16, 11), (0, 0, 1), 500, (.71, .85, 1), 9, 7)
    area("CITY | overhead daylight", (0, -1, 9), (0, 0, 0), 450, (1, .93, .79), 5)
    area("BACKDROP | pool of dusk", (0, 9, 8), (0, 7, -1.1), 1800, (.23, .48, .63), 9)
    sun_data = bpy.data.lights.new("SUN | late afternoon over the Liffey", "SUN")
    sun_data.energy = .85
    sun_data.angle = math.radians(9)
    sun_data.color = (1, .83, .63)
    sun = bpy.data.objects.new(sun_data.name, sun_data)
    bpy.context.collection.objects.link(sun)
    sun.rotation_euler = (math.radians(28), math.radians(-20), math.radians(-30))
    bpy.ops.object.camera_add(location=(19, -29, 21))
    camera = bpy.context.object
    camera.name = "CAMERA | hero - O'Connell Bridge at survey origin"
    aim(camera, (0, 0, 3.5))
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 22.6
    camera.data.lens = 55
    scene.camera = camera
    # Engraving follows the front tangent of the turned plinth.
    front = Vector((camera.location.x, camera.location.y, 0)).normalized()
    for text, size, z in [("D U B L I N", .31, -.56), ("O ' C O N N E L L   B R I D G E", .10, -.75)]:
        curve = bpy.data.curves.new("Engraved identity", "FONT")
        curve.body, curve.align_x, curve.align_y = text, "CENTER", "CENTER"
        curve.size, curve.extrude, curve.bevel_depth = size, .001, .0007
        obj = bpy.data.objects.new("BRASS LETTERING | " + text, curve)
        bpy.context.collection.objects.link(obj)
        obj.location = front * 7.754
        obj.location.z = z
        obj.rotation_euler = front.to_track_quat("Z", "Y").to_euler()
        curve.materials.append(gold)
    scene.render.engine = "CYCLES"
    preferences = bpy.context.preferences.addons["cycles"].preferences
    preferences.compute_device_type = "OPTIX"
    preferences.get_devices()
    for device in preferences.devices:
        device.use = device.type == "OPTIX"
    scene.cycles.device = "GPU"
    scene.cycles.samples = 64
    scene.cycles.use_denoising = True
    scene.cycles.adaptive_threshold = .03
    scene.cycles.max_bounces = 14
    scene.cycles.transmission_bounces = 10
    scene.cycles.transparent_max_bounces = 12
    scene.cycles.volume_bounces = 2
    scene.render.resolution_x = 1200
    scene.render.resolution_y = 1400
    scene.render.resolution_percentage = 75
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.look = "AgX - Medium High Contrast"
    scene.view_settings.exposure = .2
    scene.render.filepath = str(ROOT / "preview-02.png")
    for obj in bpy.context.scene.objects:
        if obj.type == "LIGHT" and not obj.name.startswith("STRIP"):
            obj.visible_glossy = False
            obj.visible_transmission = False
        if obj.name.startswith(("DUBLIN |", "CANOPY |", "LANDMARK |", "CLOUD |", "SNOW |")):
            obj.visible_glossy = False
    for screen in bpy.data.screens:
        for a in screen.areas:
            if a.type == "VIEW_3D":
                a.spaces.active.region_3d.view_perspective = "CAMERA"
                a.spaces.active.overlay.show_overlays = False
                a.spaces.active.clip_end = 500


def main():
    # This script is run in a newly created, dedicated instance.
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.data.orphans_purge(do_recursive=True)
    build_city()
    gold = build_base()
    build_glass()
    build_atmosphere()
    setup_studio(gold)
    provenance = (ROOT / "provenance.json").read_text()
    bpy.data.texts.new("READ ME | geographic provenance and artistic approximations").write(provenance)
    bpy.data.texts.new("CREDITS | source attribution").write(
        "LiDAR: 2015 Aerial Laser and Photogrammetry Survey of Dublin City.\n"
        "Debra F. Laefer, Saleh Abuwarda, Anh-Vu Vo, Linh Truong-Hong, Hamid Gharibi. CC BY 4.0.\n"
        "Aerial imagery: Source: Esri, Vantor, Earthstar Geographics, and the GIS User Community.\n"
        "Derived artistic visualization. Survey: 26 March 2015. Imagery may be from a different date.\n"
        "Facade microdetail, water optical color, snow, atmosphere and globe are artistic.\n")
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / "Dublin - OConnell Bridge - Snow Globe.blend"))
    (ROOT / "checkpoint-build.json").write_text(json.dumps({
        "status": "scene-built", "objects": len(bpy.data.objects),
        "mesh_polygons": sum(len(m.polygons) for m in bpy.data.meshes),
        "blend": bpy.data.filepath,
    }, indent=2))
    print("SCENE_SAVED", flush=True)


if __name__ == "__main__":
    main()
