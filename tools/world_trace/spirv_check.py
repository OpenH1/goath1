"""spirv_check.py <directory> <module.spv> <tolerance>: the SPIR-V emitter's ray step through the distance-field
scene (port/linux/src/xgpu_spirv.c's world_trace, in the module xgpu_spirv_world_trace_test writes) run on the CPU
(tools/world_ao_light/spirv_run.py) at the made-up scene's rays (scene.c's rays.json, the texture's bytes in
cascades.bin, the level's field's in field_table.bin and field_atlas.bin) and compared with its reference there (port/linux/game/world_cascades.c's world_trace_ray): whether
each hits, what, and how far. Prints the largest difference of t; exits 1 if a ray differs past the tolerance."""
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "world_ao_light"))
import spirv_run  # noqa: E402

SPAN = 65  # (xgpu.h's XGPU_WORLD_CASCADE_SPAN)
CASCADES = 4
ATLAS_SIDE = 16 * 9  # (the field's atlas: XGPU_WORLD_SDF_ATLAS bricks of XGPU_WORLD_SDF_BRICK_SPAN voxels a side)


def main():
    directory = Path(sys.argv[1])
    data = (directory / "cascades.bin").read_bytes()
    texels = [value - 256 if value > 127 else value for value in data]
    scene = json.loads((directory / "rays.json").read_text())
    words = Path(sys.argv[2]).read_bytes()
    module = spirv_run.Module([int.from_bytes(words[at:at + 4], "little") for at in range(0, len(words), 4)])
    tolerance = float(sys.argv[3])
    size = (SPAN, SPAN, SPAN * CASCADES)
    table = (directory / "field_table.bin").read_bytes()
    atlas = [value - 256 if value > 127 else value for value in (directory / "field_atlas.bin").read_bytes()]
    cells = [int(value) for value in scene["field"][1][:3]]
    atlas_size = (ATLAS_SIDE, ATLAS_SIDE, int(scene["field"][1][3]) * 9)

    def filtered(texels, extent, channels, uvw):
        # (signed bytes, snorm, filtered, clamped to the edge)
        base, fraction = [], []
        for axis in range(3):
            coordinate = uvw[axis] * extent[axis] - 0.5
            floor_value = math.floor(coordinate)
            base.append(int(floor_value))
            fraction.append(coordinate - floor_value)
        result = [0.0] * channels
        for corner in range(8):
            at, weight = [], 1.0
            for axis in range(3):
                bit = (corner >> axis) & 1
                at.append(min(max(base[axis] + bit, 0), extent[axis] - 1))
                weight *= fraction[axis] if bit else 1.0 - fraction[axis]
            texel = ((at[2] * extent[1] + at[1]) * extent[0] + at[0]) * channels
            for part in range(channels):
                result[part] += weight * max(texels[texel + part] / 127.0, -1.0)
        return result

    def sample(name, uvw):
        if name == "world_sdf_table":
            # (read by texel: the table's four bytes as RGBA8)
            at = ((uvw[2] * cells[1] + uvw[1]) * cells[0] + uvw[0]) * 4
            return [spirv_run.f32(table[at + part] / 255.0) for part in range(4)]
        if name == "world_sdf":
            # (the field's atlas: R8 snorm)
            return [spirv_run.f32(filtered(atlas, atlas_size, 1, uvw)[0]), 0.0, 0.0, 1.0]
        # (the cascades: two signed bytes a texel, RG8 snorm)
        assert name == "world_cascades", name
        result = filtered(texels, size, 2, uvw)
        return [spirv_run.f32(result[0]), spirv_run.f32(result[1]), 0.0, 1.0]

    worst, unlike = 0.0, 0
    for index, ray in enumerate(scene["rays"]):
        inputs = {"origin": ray["origin"], "direction": ray["direction"],
                  "parameters": [ray["t_max"], float(ray["mask"]), 0.0, 0.0]}
        for cascade in range(CASCADES):
            inputs[f"cascade{cascade}"] = scene["block"][cascade]
        inputs["field0"] = scene["field"][0]
        inputs["field1"] = [float(value) for value in scene["field"][1]]
        colour = spirv_run.run(module, inputs, sample)
        difference = abs(colour[1] - ray["t"])
        worst = max(worst, difference)
        if int(colour[0]) != ray["hit"] or int(colour[2]) != ray["kind"] or difference > tolerance:
            unlike += 1
            if unlike <= 10:
                print(f"ray {index}: {colour[:3]}, the reference's {ray['hit']} {ray['kind']} {ray['t']}")
    print(f"{len(scene['rays'])} rays ({sum(ray['hit'] for ray in scene['rays'])} hit), {unlike} differ, "
          f"the largest difference of t {worst:.3g}")
    return 1 if unlike else 0


if __name__ == "__main__":
    sys.exit(main())
