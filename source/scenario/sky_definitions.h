/*
SKY_DEFINITIONS.H

header included in hcex build.
*/

#ifndef __SKY_DEFINITIONS_H
#define __SKY_DEFINITIONS_H
#pragma once

/* ---------- headers */

#include "tag_files/tag_files.h"

/* ---------- constants */

#define SKY_DEFINITION_TAG 'sky '

/* ---------- macros */

#define sky_definition_get(index) ((struct sky *)tag_get(SKY_DEFINITION_TAG, (index)))

/* ---------- structures */

struct sky_atmospheric_fog
{
	real_rgb_color color;
	byte unused0C[8];
	real maximum_density;
	real start_distance;
	real opaque_distance;
};

typedef char sky_atmospheric_fog_size_assert[
	sizeof(struct sky_atmospheric_fog) == 0x20 ? 1 : -1];

/* port: a sky's light (the Halo CE sky tag's lights block, 116 bytes; read
by the path tracing's measurement, port/linux/game/pt_measure.c): its lens
flare, its flags (bit 0 affects exteriors, bit 1 interiors), its color and
power, its test distance, its direction (yaw and pitch, radians: toward the
light) and its angular diameter (radians) */
struct sky_light
{
	struct tag_reference lens_flare;
	char lens_flare_marker_name[32];
	byte unused30[0x4C - 0x30];
	unsigned long flags;
	real_rgb_color color;
	real power;
	real test_distance;
	byte unused64[4];
	real direction[2];
	real diameter;
};

typedef char sky_light_size_assert[
	sizeof(struct sky_light) == 0x74 ? 1 : -1];

struct sky
{
	struct tag_reference model;
	struct tag_reference animation_graph;
	byte unused20[0x38 - 0x20];
	/* port: the radiosity's ambient light, indoors and outdoors */
	real_rgb_color indoor_ambient_radiosity_color;
	real indoor_ambient_radiosity_power;
	real_rgb_color outdoor_ambient_radiosity_color;
	real outdoor_ambient_radiosity_power;
	struct sky_atmospheric_fog outdoor_fog;
	struct sky_atmospheric_fog indoor_fog;
	struct tag_reference indoor_fog_screen;
	/* port: the rest of the tag (208 bytes): its shader functions and
	animations (not decoded) and its lights (struct sky_light) */
	byte unusedA8[4];
	struct tag_block shader_functions;
	struct tag_block animations;
	struct tag_block lights;
};

typedef char sky_outdoor_fog_offset_assert[
	offsetof(struct sky, outdoor_fog) == 0x58 ? 1 : -1];
typedef char sky_lights_offset_assert[
	offsetof(struct sky, lights) == 0xC4 ? 1 : -1];

typedef char sky_indoor_fog_offset_assert[
	offsetof(struct sky, indoor_fog) == 0x78 ? 1 : -1];
typedef char sky_indoor_fog_screen_offset_assert[
	offsetof(struct sky, indoor_fog_screen) == 0x98 ? 1 : -1];

/* ---------- prototypes/EXAMPLE.C */

/* ---------- globals */

/* ---------- public code */

#endif // __SKY_DEFINITIONS_H
