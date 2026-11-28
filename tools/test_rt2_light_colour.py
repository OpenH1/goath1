"""The ray-traced lighting's one colour rule and one emission unit (port/linux/game/lt_light_colour.h): every light
source's colour reaches linear light through lt_light_colour.h alone, the radiosity tool's data in its one unit
(LT_TOOL_K), and no per-source k comes back. Reads the sources only (no build, no rig); the rule's numbers checked by
a C compiler where one is on the PATH."""

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "port/linux/game"
# the light sources: each reads a designer's colour (or a drawn glow) and makes a light of it
SOURCES = ["lt_emitters.c", "lt_lights_build.c", "lt_lights.c", "world_rt.c", "dynamic_lights.c"]
# (a gamma in these files that is no light's colour: the classic rasterizer's copies' share of one light's colour)
ALLOWED_GAMMA = {("dynamic_lights.c", "powf((real)count, -1.0f / 2.2f)")}
# the radiosity tool's colours: read anywhere in the sources, each in a function that takes it through
# lt_light_colour_tool; and the functions that make the tool's lights (the light tags' radiosity among them)
TOOL_COLOURS = ["color_of_emitted_light", "ambient_color", "radiosity_color"]
TOOL_FUNCTIONS = {"lt_emitters.c": "lt_emitters_rule", "lt_lights_build.c": "lt_suns",
                  "dynamic_lights.c": "dynamic_lights_baked_add"}
# (the files that make lights of colours; lt_lights.c only gathers them)
CONVERTERS = ["lt_emitters.c", "lt_lights_build.c", "world_rt.c", "dynamic_lights.c"]
# the per-source fits the one unit replaced
GONE = ["WORLD_RT_LAMP_K", "DYNAMIC_LIGHTS_BAKED_K ", "lt_lights_real_sun_k", "lt_lights_real_sky_k",
        "LT_EMITTERS_EXITANCE_TO_RADIANCE", "GLOW_K", "XGPU_HDR_CAMERA_CONTRAST"]


def code(text):
    """the text without its comments"""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def functions(text):
    """each top-level function's body by its name (a brace at the line's start opens and closes one)"""
    out, name, body = {}, None, []
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line == "{" and i:
            match = re.search(r"(\w+)\s*\(", lines[i - 1]) or re.search(r"(\w+)\s*\(", " ".join(lines[max(0, i - 3):i]))
            name, body = (match.group(1) if match else "?"), []
        elif line == "}" and name:
            out[name] = "\n".join(body)
            name = None
        elif name:
            body.append(line)
    return out


def failures():
    found = []
    for file in SOURCES:
        text = code((GAME / file).read_text())
        for line in text.split("\n"):
            if re.search(r"\b2\.2f?\b", line) and not any(f == file and a in line for f, a in ALLOWED_GAMMA):
                found.append(f"{file}: a colour taken to linear past lt_light_colour.h: {line.strip()}")
        bodies = functions(text)
        for name, body in bodies.items():
            if file in CONVERTERS and (any(c in body for c in TOOL_COLOURS) or TOOL_FUNCTIONS.get(file) == name) and \
                    "lt_light_colour_tool" not in body:
                found.append(f"{file}: {name} reads the tool's colour without lt_light_colour_tool")
        if file in TOOL_FUNCTIONS and TOOL_FUNCTIONS[file] not in bodies:
            found.append(f"{file}: {TOOL_FUNCTIONS[file]} not found (renamed? update this test)")
        if file in CONVERTERS and "lt_light_colour" not in text:
            found.append(f"{file}: lt_light_colour.h unused")
    for path in [*(GAME / f for f in SOURCES), ROOT / "port/linux/vulkan/lt/indirect.glsl",
                 ROOT / "port/linux/src/xgpu_hdr.h", ROOT / "port/linux/vulkan/xgpu_vulkan_stream.c"]:
        text = code(path.read_text())
        found += [f"{path.name}: a per-source k is back: {k.strip()}" for k in GONE if k in text]
    glow = re.search(r"hit\.glow\s*=\s*([^;]*);", code((ROOT / "port/linux/vulkan/lt/indirect.glsl").read_text()))
    if not glow or not glow.group(1).strip().startswith("hit.albedo * pow("):
        found.append("indirect.glsl: a model's glow hit not its drawn radiance by 1 (world_rt.c's lamps')")
    return found


RULE_CHECK = r"""
#include <stdio.h>
#include <math.h>
#include "lt_light_colour.h"
int main(void)
{
	float colours[4][3] = { { 1.0f, 0.5f, 0.1f }, { 0.57f, 0.55f, 0.71f }, { 0.79f, 0.52f, 1.0f }, { 0.3f, 0.3f, 0.3f } };
	int i, c, bad = 0;
	for (i = 0; i < 4; i++)
	{
		float tool[3], drawn[3], scaled[3], twice[3];
		lt_light_colour_tool(colours[i], tool);
		lt_light_colour_drawn(colours[i], drawn);
		for (c = 0; c < 3; c++)
			scaled[c] = 2.0f * colours[i][c];
		lt_light_colour_tool(scaled, twice);
		/* the tool's luminance kept, the screen's hue, linear in the colour's scale */
		bad |= fabsf(lt_light_luminance(tool) - lt_light_luminance(colours[i])) > 1e-5f;
		for (c = 0; c < 3; c++)
		{
			bad |= fabsf(tool[c] * lt_light_luminance(drawn) - drawn[c] * lt_light_luminance(tool)) > 1e-5f;
			bad |= fabsf(drawn[c] - powf(colours[i][c], 2.2f)) > 1e-6f;
			bad |= fabsf(twice[c] - 2.0f * tool[c]) > 1e-5f;
		}
	}
	bad |= fabsf(LT_TOOL_K - 1.0f / 3.14159265f) > 1e-7f;
	puts(bad ? "rule wrong" : "rule ok");
	return bad;
}
"""


def rule_check():
    cc = shutil.which("cc")
    if not cc:
        return "no C compiler: the rule's numbers not checked"
    with tempfile.TemporaryDirectory() as folder:
        source, binary = Path(folder) / "rule.c", Path(folder) / "rule"
        source.write_text(RULE_CHECK)
        subprocess.run([cc, "-std=gnu89", "-Wall", "-Werror", "-I", str(GAME), str(source), "-lm", "-o", str(binary)],
                       check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout)
        return result.stdout.strip()


def test_every_light_colour_through_the_rule():
    assert failures() == []


def test_rule_numbers():
    rule_check()


if __name__ == "__main__":
    problems = failures()
    print("\n".join(problems) or "every light colour through lt_light_colour.h")
    print(rule_check())
    sys.exit(1 if problems else 0)
