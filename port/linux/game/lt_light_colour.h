/*
LT_LIGHT_COLOUR.H

The ray-traced lighting's one colour rule and one emission unit
(~/dev/rt2-unified-design.md, 2C; port/assets/emitters/README.md, "Units").
Every light source's colour is read here: the level's glowing surfaces
(lt_emitters.c), the sky's lights and dome (lt_lights_build.c's suns), the
light tags' radiosity (dynamic_lights.c's baked lights), the game's own
lights as drawn (lt_lights_build.c's list), and the objects' lamps
(world_rt.c, with their change colours). tools/test_rt2_light_colour.py
fails where a light source's colour is taken to linear anywhere else.

A designer chose each colour as it looks on screen, so its hue is the
screen's: c^2.2, the linear light that shows as c. How bright it is
follows the source's own unit system:
- the radiosity tool's data (lt_light_colour_tool): the tool multiplies the
  colour by the power linearly, so its luminance is lum(c) x power; the
  colour linearized keeping that luminance,
  c_lin = lum(c) x c^2.2 / lum(c^2.2), then x power x LT_TOOL_K;
- a colour as drawn (lt_light_colour_drawn): a glow on screen, or a
  game light as the classic light model adds it (BRIGHTNESS x c^2.2): the
  screen's own light, c^2.2, its luminance lum(c^2.2).
Both are lt_light_colour with that luminance.

LT_TOOL_K, the one constant: the radiosity tool's powers are exitances and
irradiances (a glowing shader's exitance M = colour x power; a sky light's
irradiance facing it, colour x power; the sky's outdoor ambient the
irradiance under the whole open dome; a light tag's intensity, the
irradiance at a unit's distance on its axis). The pass and the lightmap
carry radiance: the light a white diffuse surface shows is its irradiance
over pi (lt/e1.frag's emitter_light: radiance x cos x cos' / d^2 / pi), and
an emitter's radiance its exitance over pi. So every tool power is taken
to the pass's units by 1 / pi; nothing is fitted. Checked, not refitted,
against b30's bake (the truth renderer, ~/dev/rt2-calibration.json's
real_sky): the sky's (fill and dome) fitted k 0.339 against 0.318; a50's
floodlights' (the old per-light fit) 0.617 for their 1.8 against 0.573;
the sun's fitted 0.596 is the bake's extra contrast (its sunlit over shade
5.88 against the traced 3.78), the camera's light curve's job
(xgpu_hdr.h's XGPU_HDR_CAMERA_LIGHT_CONTRAST), not the sun's power.

A glowing surface with no radiosity data (an object's self-illumination,
an additive part) emits what it shows: its drawn colour is its radiance,
by 1, the same whether a light sample reaches it (world_rt.c's lamps) or a
ray hits it (lt/indirect.glsl's hit.glow).

Plain C (gnu89), no game headers: lt_lights_build.c's light-list unit
includes it too (with its powf, the game's own).
*/

#ifndef LT_LIGHT_COLOUR_H
#define LT_LIGHT_COLOUR_H

#include <math.h>

/* the radiosity tool's powers in the pass's units: 1 / pi (above) */
#define LT_TOOL_K 0.31830989f

/* the linear light's luminance (Rec. 709) */
static inline float lt_light_luminance(float const c[3])
{
	return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/* an image's value (a texel, as the screen shows it) to linear: an albedo */
static inline float lt_image_linear(float value)
{
	return value > 0.0f ? powf(value, 2.2f) : 0.0f;
}

/* (its inverse: the image's value that shows as linear) */
static inline float lt_image_encoded(float linear)
{
	return linear > 0.0f ? powf(linear, 1.0f / 2.2f) : 0.0f;
}

/* the one rule: colour (as on screen) linear with luminance's luminance,
its hue the screen's (c^2.2); 0 where it has none */
static inline void lt_light_colour(float const colour[3], float luminance, float out[3])
{
	float screen[3], shown;
	int channel;

	for (channel = 0; channel < 3; channel++)
		screen[channel] = lt_image_linear(colour[channel]);
	shown = lt_light_luminance(screen);
	for (channel = 0; channel < 3; channel++)
		out[channel] = shown > 0.0f && luminance > 0.0f ? screen[channel] * (luminance / shown) : 0.0f;
}

/* a colour of the radiosity tool's data: the tool's luminance, lum(c) */
static inline void lt_light_colour_tool(float const colour[3], float out[3])
{
	lt_light_colour(colour, lt_light_luminance(colour), out);
}

/* a colour as drawn: the screen's light, c^2.2 (lt_light_colour at the
screen's own luminance, without the round trip) */
static inline void lt_light_colour_drawn(float const colour[3], float out[3])
{
	int channel;

	for (channel = 0; channel < 3; channel++)
		out[channel] = lt_image_linear(colour[channel]);
}

#endif
