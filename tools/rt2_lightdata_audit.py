#!/usr/bin/env python3
"""Every lighting input of a map that only Bungie's lightmap tool used, and how much of it there is: the audit
behind port/assets/emitters/README.md's "Lighting the lightmap alone used". A level the ray-traced lighting lights
live drops its lightmap, so any of these the live pass does not turn into light is lost there.

    python3 tools/rt2_lightdata_audit.py maps/*.map              # a table a map, then the totals
    python3 tools/rt2_lightdata_audit.py maps/*.map --json out.json

It reads each Xbox map's tags (tools/lightmap_fingerprint.py's TagCache) and lists:
- lights: every light tag (ligh) attached to a placed object (scenery, light fixtures, machines, controls, ...)
  whose radiosity intensity (0xD4) is above 0, each placement once, with what the game makes of it: a light datum
  only for a tag that is dynamic or has a lens flare (object_lights.c's light_new), and the live conversion
  (dynamic_lights.c's dynamic_lights_fixtures) only for a datum with a cone; the rest are lost live;
- shaders: every shader with radiosity power, used by a BSP's lightmap materials (the emitter solve / tag emitters
  take those) or not (on objects: the object lamps take a glowing part's self-illumination, not the power);
- skies: each sky's indoor ambient radiosity (0x38 colour, 0x44 power: the bake's interior clusters' ambient),
  outdoor ambient and its lights' flags (bit 0 affects exteriors, bit 1 affects interiors);
- BSPs: clusters, how many see a lit sky (lt_lights.c's rule for a level lit live) and the shipped emitter file.
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lightmap_fingerprint import TagCache, SCENARIO_STRUCTURE_BSPS  # noqa: E402

EMITTERS = Path(__file__).resolve().parent.parent / "port" / "assets" / "emitters"

# scenario_definitions.h: the placement blocks (each element's position at +8) and their palettes
PLACEMENTS = [  # (kind, block offset, element size)
    ("scenery", 0x210, 0x48), ("biped", 0x228, 0x78), ("vehicle", 0x240, 0x78), ("equipment", 0x258, 0x28),
    ("weapon", 0x270, 0x5C), ("machine", 0x294, 0x40), ("control", 0x2AC, 0x40), ("light_fixture", 0x2C4, 0x58),
    ("sound_scenery", 0x2DC, 0x28),
]
PALETTE_ENTRY = 0x30
SCENARIO_SKIES = 0x30
# object_definitions.h: the attachments block, each 0x48 (its type a tag reference)
OBJECT_ATTACHMENTS, ATTACHMENT_SIZE = 0x140, 0x48
# the light tag (object_lights.c's point_light_definition; radiosity the lightmap tool's)
LIGHT_FLAGS, LIGHT_RADIUS, LIGHT_COS_FALLOFF, LIGHT_COS_CUTOFF = 0x0, 0x4, 0x1C, 0x20
LIGHT_COLOR_LOWER, LIGHT_LENS_FLARE = 0x48, 0xAC
LIGHT_RADIOSITY_INTENSITY, LIGHT_RADIOSITY_COLOR = 0xD4, 0xD8
# shader_definitions.h's shader_radiosity_properties
SHADER_RADIOSITY_FLAGS, SHADER_POWER, SHADER_COLOR, SHADER_TINT = 0x0, 0x4, 0x8, 0x14
# the sky tag: indoor/outdoor ambient radiosity, its lights (lt_lights.c's lt_sky_light)
SKY_INDOOR_COLOR, SKY_INDOOR_POWER, SKY_OUTDOOR_COLOR, SKY_OUTDOOR_POWER = 0x38, 0x44, 0x48, 0x54
SKY_LIGHTS, SKY_LIGHT_SIZE = 0xC4, 0x74
# structure_bsp: world bounds, lightmaps (materials 0x100 each, shader reference at +0), clusters (0x68 each)
BSP_WORLD_BOUNDS, BSP_LIGHTMAPS, BSP_CLUSTERS, CLUSTER_SIZE = 0xC8, 0x104, 0x134, 0x68
NONE = 0xFFFFFFFF


def group(cache, tag_index):
    instances, _, _, count = struct.unpack_from("<IiIi", cache.memory, 0)
    index = tag_index & 0xFFFF
    if tag_index in (-1, NONE) or not 0 <= index < count:
        return None, None, None
    entry = instances + index * 0x20
    groups = struct.unpack_from("<4s4s4s", cache.memory, entry - 0x803A6000)
    names = [g[::-1].decode("latin-1") for g in groups]
    return names, cache.name(cache.u32(entry + 0x10)), cache.u32(entry + 0x14)


def all_tags(cache):
    instances, _, _, count = struct.unpack_from("<IiIi", cache.memory, 0)
    for index in range(count):
        names, name, address = group(cache, index | 0xE0000000 if False else index)
        if names:
            yield index, names, name, address


def f32(cache, address, count=1):
    values = struct.unpack_from(f"<{count}f", cache.memory, address - 0x803A6000)
    return values if count > 1 else values[0]


def block(cache, address):
    return cache.i32(address), cache.u32(address + 4)


def ref_index(cache, address):
    return cache.i32(address + 0xC)


def luminance(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def light_info(cache, tag_index):
    names, name, address = group(cache, tag_index)
    flags = cache.u32(address + LIGHT_FLAGS)
    intensity = f32(cache, address + LIGHT_RADIOSITY_INTENSITY)
    color = f32(cache, address + LIGHT_RADIOSITY_COLOR, 3)
    falloff, cutoff = f32(cache, address + LIGHT_COS_FALLOFF), f32(cache, address + LIGHT_COS_CUTOFF)
    flare = ref_index(cache, address + LIGHT_LENS_FLARE)
    dynamic = bool(flags & 1)
    has_flare = flare not in (-1,)
    datum = dynamic or has_flare
    cone = -1.0 < cutoff < 1.0
    if not intensity > 0:
        live = "none"
    elif dynamic:
        live = "real (the game's dynamic light)"
    elif not datum:
        live = "LOST (no light datum: no lens flare, not dynamic)"
    elif cone:
        live = "real (a50 spot conversion)"
    else:
        live = "LOST (point: the a50 conversion takes cones only)"
    return {"tag": name, "flags": flags, "dynamic": dynamic, "lens_flare": has_flare, "radius": f32(cache, address + LIGHT_RADIUS),
            "radiosity_intensity": intensity, "radiosity_color": list(color), "cos_falloff": falloff,
            "cos_cutoff": cutoff, "cone": cone, "live": live,
            "power": intensity * luminance(color) if intensity > 0 else 0.0}


def bsp_table(cache, map_name):
    scenario = cache.scenario()
    skies = []
    count, address = block(cache, scenario + SCENARIO_SKIES)
    for index in range(count):
        skies.append(ref_index(cache, address + index * 0x10))
    sky_lit = [sky_info(cache, s)["lit"] if s != -1 else False for s in skies]
    bsps = []
    for index, structure, _ in cache.bsps():
        bounds = f32(cache, structure + BSP_WORLD_BOUNDS, 6)
        clusters, clusters_address = block(cache, structure + BSP_CLUSTERS)
        exterior = 0
        for c in range(clusters):
            sky = cache.i16(clusters_address + c * CLUSTER_SIZE)
            if 0 <= sky < len(sky_lit) and sky_lit[sky]:
                exterior += 1
        shaders = set()
        lightmaps, lightmaps_address = block(cache, structure + BSP_LIGHTMAPS)
        for lm in range(lightmaps):
            materials, materials_address = block(cache, lightmaps_address + lm * 0x20 + 0x14)
            for m in range(materials):
                shaders.add(ref_index(cache, materials_address + m * 0x100) & 0xFFFF)
        emitters = EMITTERS / f"{map_name}_bsp{index}.json"
        live = exterior >= 0.5 * clusters and clusters > 0
        bsps.append({"bsp": index, "bounds": list(bounds), "clusters": clusters, "exterior": exterior,
                     "shaders": shaders, "emitter_file": emitters.exists(),
                     "live": "live (sky)" if live else "live (solved lamps)" if emitters.exists() else
                     "lightmap kept (interior views); live in exterior views" if exterior else "lightmap kept"})
    return skies, bsps


def sky_info(cache, tag_index):
    names, name, address = group(cache, tag_index)
    indoor_color, indoor_power = f32(cache, address + SKY_INDOOR_COLOR, 3), f32(cache, address + SKY_INDOOR_POWER)
    outdoor_color, outdoor_power = f32(cache, address + SKY_OUTDOOR_COLOR, 3), f32(cache, address + SKY_OUTDOOR_POWER)
    lights = []
    count, laddress = block(cache, address + SKY_LIGHTS)
    lit = 0 < outdoor_power < 1e6
    for index in range(count):
        element = laddress + index * SKY_LIGHT_SIZE
        flags = cache.u32(element + 0x4C)
        color, power = f32(cache, element + 0x50, 3), f32(cache, element + 0x5C)
        diameter = f32(cache, element + 0x70)
        lights.append({"flags": flags, "exteriors": bool(flags & 1), "interiors": bool(flags & 2),
                       "color": list(color), "power": power, "diameter_deg": math.degrees(diameter)})
        if power > 0 and flags & 1:
            lit = True
    return {"tag": name, "indoor_ambient": {"color": list(indoor_color), "power": indoor_power},
            "outdoor_ambient": {"color": list(outdoor_color), "power": outdoor_power}, "lights": lights, "lit": lit}


def in_bounds(bounds, point):
    x0, x1, y0, y1, z0, z1 = bounds
    return x0 <= point[0] <= x1 and y0 <= point[1] <= y1 and z0 <= point[2] <= z1


def audit(path):
    cache = TagCache(path)
    map_name = Path(path).stem
    scenario = cache.scenario()
    skies, bsps = bsp_table(cache, map_name)
    lights = []
    light_cache = {}
    for kind, offset, size in PLACEMENTS:
        count, address = block(cache, scenario + offset)
        pcount, paddress = block(cache, scenario + offset + 0xC)
        palette = [ref_index(cache, paddress + i * PALETTE_ENTRY) for i in range(pcount)]
        for index in range(count):
            element = address + index * size
            entry = cache.i16(element)
            if not 0 <= entry < len(palette) or palette[entry] == -1:
                continue
            names, object_name, object_address = group(cache, palette[entry])
            if not object_address:
                continue
            position = f32(cache, element + 8, 3)
            acount, aaddress = block(cache, object_address + OBJECT_ATTACHMENTS)
            for a in range(acount):
                attachment = aaddress + a * ATTACHMENT_SIZE
                tag = ref_index(cache, attachment)
                gnames, _, _ = group(cache, tag)
                if not gnames or gnames[0] != "ligh":
                    continue
                if tag not in light_cache:
                    light_cache[tag] = light_info(cache, tag)
                info = light_cache[tag]
                if not info["radiosity_intensity"] > 0:
                    continue
                inside = [b["bsp"] for b in bsps if in_bounds(b["bounds"], position)]
                lights.append({"object": object_name, "kind": kind, "placement": index,
                               "marker": cache.name(attachment + 0x10), "position": list(position),
                               "bsps": inside, **info})
    shaders = []
    for index, names, name, address in all_tags(cache):
        if "shdr" not in names:
            continue
        power = f32(cache, address + SHADER_POWER)
        if not power > 0:
            continue
        used = [b["bsp"] for b in bsps if index in b["shaders"]]
        color = f32(cache, address + SHADER_COLOR, 3)
        shaders.append({"tag": name, "group": names[0], "power": power, "color": list(color),
                        "flags": cache.u32(address) & 0xFFFF, "tint": list(f32(cache, address + SHADER_TINT, 3)),
                        "bsps": used})
    sky_rows = [sky_info(cache, s) for s in skies if s != -1]
    for b in bsps:
        del b["shaders"]
    return {"map": map_name, "bsps": bsps, "skies": sky_rows, "lights": lights, "shaders": shaders}


def report(result):
    out = [f"== {result['map']}"]
    for b in result["bsps"]:
        out.append(f"  bsp{b['bsp']}: {b['clusters']} clusters, {b['exterior']} see a lit sky; {b['live']}")
    for s in result["skies"]:
        ia = s["indoor_ambient"]
        out.append(f"  sky {s['tag']}: indoor ambient {ia['power']:.3g} x {[round(c, 3) for c in ia['color']]}, "
                   f"outdoor {s['outdoor_ambient']['power']:.3g}; lights " +
                   ", ".join(f"{l['power']:.3g}{' ext' if l['exteriors'] else ''}{' int' if l['interiors'] else ''}"
                             f" {l['diameter_deg']:.1f}deg" for l in s["lights"]))
    groups = {}
    for l in result["lights"]:
        key = (l["tag"], l["live"], tuple(l["bsps"]))
        g = groups.setdefault(key, {"n": 0, "power": 0.0, "objects": set(), "l": l})
        g["n"] += 1
        g["power"] += l["power"]
        g["objects"].add(f"{l['kind']}:{l['object'].split(chr(92))[-1]}")
    for (tag, live, bsps), g in sorted(groups.items(), key=lambda kv: -kv[1]["power"]):
        l = g["l"]
        out.append(f"  light {tag} x{g['n']} bsp{list(bsps)}: radiosity {l['radiosity_intensity']:.3g} x "
                   f"{[round(c, 2) for c in l['radiosity_color']]}, cone {l['cos_falloff']:.2f}/{l['cos_cutoff']:.2f}, "
                   f"flare {int(l['lens_flare'])} dyn {int(l['dynamic'])} -> {live} [{', '.join(sorted(g['objects']))}]")
    for s in sorted(result["shaders"], key=lambda s: -s["power"]):
        out.append(f"  shader {s['group']} {s['tag']}: power {s['power']:.4g} flags {s['flags']:#x} "
                   f"{'bsp' + str(s['bsps']) if s['bsps'] else 'NOT in a BSP'}")
    return "\n".join(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("maps", nargs="+")
    parser.add_argument("--json")
    args = parser.parse_args(argv)
    results = []
    for path in args.maps:
        try:
            result = audit(path)
        except Exception as failure:  # one map that does not read: say so, go on
            print(f"{path}: {failure}", file=sys.stderr)
            continue
        results.append(result)
        print(report(result))
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
