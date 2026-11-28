"""display.ambient_occlusion "ray_traced"'s scene (port/linux/include/world_rt.h, port/linux/game/world_rt.c,
docs/world-ao.md "Hardware ray queries"): a mesh's triangles turned to face out, the box, the instances' transforms
and owners; a fake world's frames: the meshes made once a map as they are first needed, the instances on the objects'
nodes, the static scenery as the structure is, everything let go of at the map's end; debug.rt_render_geometry's render mesh
in the collision mesh's place from the frame after (its opaque triangles, then its alpha-tested ones, with their
materials and textures' slots), the collision mesh let go of then; each BSP's meshes kept by index."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

CASES = ["arithmetic", "frame", "render", "model", "skin", "permutation"]
GAME = "port/linux/game/world_rt.c"
CONTROLS = [
    # every node's collision mesh its first permutation's, whatever the object has (a dead Jackal's shield, #1311)
    ("\t\t\tint permutation = node_index < permutation_count ? world_rt.permutations[node_index] : 0;\n",
     "\t\t\tint permutation = 0;\n", "permutation"),
    # a mesh wound inward kept so (the backend's front faces then the inside)
    ("\tif (volume < -(double)least)\n", "\tif (volume > (double)least)\n", "arithmetic"),
    # an owner that is the datum index itself (object 0's the structure's 0)
    ("(unsigned long)(object_index & 0xFFFF) + 1;", "(unsigned long)(object_index & 0xFFFF);", "arithmetic"),
    # the box's centre left at the node's origin
    ("\t\t\tposition += frame->axes[column][row] * sphere[column];\n", "", "arithmetic"),
    # the static scenery the field and its pages hold traced as an object (its occlusion twice)
    ("unsigned long owner = object->baked ? 0 : world_rt_owner(object->id);",
     "unsigned long owner = world_rt_owner(object->id);", "frame"),
    # ... or left out (rays through the trees and rocks)
    ("\t\tdefinition = world_rt_definition(object->definition);\n",
     "\t\tif (object->baked)\n\t\t\tcontinue;\n\t\tdefinition = world_rt_definition(object->definition);\n", "frame"),
    # a definition's meshes made again each time it is drawn
    ("\t\tif (entry->definition == definition)\n\t\t\treturn entry;\n", "", "frame"),
    # the structure's mesh not made again for another BSP
    ("\tif (bsp_index == world_rt.bsp_index)\n\t{", "\tif (world_rt.structure)\n\t{", "frame"),
    # a box for anything with no collision geometry (an emitter's, the sky's ring's sphere)
    ("\tif (!any && item && entry->node_count > 0 && entry->sphere[3]", "\tif (!any && entry->node_count > 0 && entry->sphere[3]", "frame"),
    # the instances handed again in the same frame
    ("\tif (world_rt.framed && world_rt.frame == frame)\n\t\treturn;", "", "frame"),
    # the meshes kept past the map's end
    ("\tif (world_rt.sent)\n\t\thalo_world_ao_mesh(0, 0, NULL);", "", "frame"),
    # debug.rt_render_geometry's render mesh made with the switch off
    ("\tif (world_rt.render_geometry)\n\t{\n\t\tif (world_rt.bsps[bsp_index].render)",
     "\tif (1)\n\t{\n\t\tif (world_rt.bsps[bsp_index].render)", "frame"),
    # ... or in the same frame as the collision mesh (a load waits for it)
    ("\tworld_rt.collision_count = count;\n",
     "\tworld_rt.collision_count = count;\n\tif (world_rt.render_due)\n\t\tworld_rt_structure_render();\n", "render"),
    # ... or its materials carried again by another BSP's mesh (the renderer has them)
    ("\t\tif (!memcmp(&world_rt.materials[index], &key, sizeof(key)))", "\t\tif (0)", "render"),
    # ... or its textures sent every frame, or not again as one loads
    ("\t\tif (slot->sent && header == slot->header)", "\t\tif (slot->sent)", "render"),
    ("\t\tif (slot->sent && header == slot->header)", "\t\tif (header == slot->header)", "render"),
    # ... or the alpha-tested material's alpha map not its slot
    ("\t\tworld_rt_slot(&source->maps[source->alpha_map], 0) : 0;", "\t\t0 : 0;", "render"),
    # a model's triangles left in its model's space (not each node's), or not turned to face out
    ("world_rt_transformed(inverses[node_index], mesh.vertices[vertex].position, 1, out->position);", "", "model"),
    ("\tif (facing < 0.0f)\n", "\tif (0)\n", "model"),
    # its collision meshes made beside them
    ("\tif (!any)\n\t\ttriangles = world_rt_collision_meshes(",
     "\tif (1)\n\t\ttriangles += world_rt_collision_meshes(", "model"),
    # a skinned model split by node (its joints torn), or its pose without its nodes' default inverses, or not
    # into its first node's space, or its instance not naming its pose (the backend refits nothing)
    ("\tif (world_rt_blended(mesh.vertices, mesh.vertex_count))", "\tif (0)", "skin"),
    ("\t\tworld_rt_compose(node, definition->inverses[node_index], node);\n", "", "skin"),
    ("\t\tworld_rt_compose(inverse, node, pose->transforms[node_index]);",
     "\t\tmemcpy(pose->transforms[node_index], node, sizeof(node));", "skin"),
    ("\t\t\tinstance->pad = (unsigned int)++pose_count;\n\t\t\t/* (measuring",
     "\t\t\t++pose_count;\n\t\t\t/* (measuring", "skin"),
    # the octahedral map's lower half not folded
    ("\tif (vector[2] < 0.0f)\n", "\tif (0)\n", "arithmetic"),
    # ... or not made again for another BSP
    ("\t\tworld_rt.render_due = 1;", "\t\tworld_rt.render_due = world_rt.next_mesh <= 2;", "render"),
    # ... or its collision mesh kept beside it
    ("\t\thalo_world_ao_mesh_release(world_rt.bsps[bsp_index].collision);", "", "render"),
    # a BSP switched back to made again (a10's start: 0, 7, 0)
    ("\tif (world_rt.bsps[bsp_index].collision)\n\t{", "\tif (0)\n\t{", "frame"),
    ("\t\tif (world_rt.bsps[bsp_index].render)\n", "\t\tif (0)\n", "render"),
]


def generated(fault=None):
    game = read(GAME)
    if fault:
        game = mutated(game, *fault)
    headers = [(name, read(f"port/linux/include/{name}"))
               for name in ("world_ao.h", "world_proxies_source.h", "world_rt.h")]
    return tuple(headers) + (("under_test.inc", game),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("world_rt", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("world_rt", generated((before, after))), case)
    assert status == CHECK_FAILED, output
