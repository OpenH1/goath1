/*
SHIELD_GLOW_HELD.C

A model's shield region's glow (port/linux/game/shield_glow.c
shield_glow_held): a Jackal's hand-held shield, a field generator's standing
one. The objects, their definitions and models are made up here, with the
fields the function reads.
*/

#include "harness.h"
#include <math.h>
#include <strings.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define MAXIMUM_REGIONS_PER_OBJECT 8
#define TAG_STRING_LENGTH 31
#define _stricmp strcasecmp
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->address)[index])
#define model_definition_get(index) (&models[index])
#define unit_get(index) (&units[index])

typedef struct { real red, green, blue; } real_rgb_color;
typedef struct { real i, j, k, w; } real_quaternion;
struct tag_block { long count; void *address; };
enum { _object_invisible_bit = 0 };
enum { _object_type_biped = 0, _object_type_vehicle, _object_type_scenery = 6 };
enum { _object_mask_unit = FLAG(_object_type_biped) | FLAG(_object_type_vehicle) };

struct model_marker_instance { byte region_index, permutation_index, node_index, pad; real_point3d translation;
	real_quaternion rotation; };
struct model_marker { char name[32]; struct tag_block instances; };
struct model_region_permutation { char name[TAG_STRING_LENGTH + 1]; };
struct model_region { char name[TAG_STRING_LENGTH + 1]; struct tag_block permutations; };
struct model { struct tag_block markers, nodes, regions; };
struct object_definition { struct { struct { long index; } model; } object; };
struct object_datum
{
	long definition_index;
	struct
	{
		unsigned long flags;
		unsigned short regions_destroyed_flags;
		byte region_permutations[MAXIMUM_REGIONS_PER_OBJECT];
		real_rgb_color outgoing_change_colors[4];
		real bounding_sphere_radius;
	} object;
};
struct unit_datum { struct { real active_camouflage; } unit; };

static const real_point3d zero_point;
#define global_zero_vector3d (&zero_point)

static struct model models[1];
static struct object_definition definitions[1];
static struct object_datum objects[2];
static struct unit_datum units[2];
static short types[2];

static struct object_datum *object_try_and_get(long index) { return index >= 0 && index < 2 ? &objects[index] : NULL; }
static struct object_definition *object_definition_get(long index) { return &definitions[index]; }
static short object_get_type(long index) { return types[index]; }

#include "under_test.inc"

static struct model_region regions[2];
static struct model_region_permutation body_permutations[1] = { { "base" } };
static struct model_region_permutation shield_permutations[3] = { { "base" }, { "~shield_off" }, { "~damaged" } };
static struct model_marker_instance instances[2];
static struct model_marker markers[2];

/* a model of a body and a shield region, the "shield light" marker on node
3 in the shield's region (and on node 1 in the body's); object 0 a unit (a
Jackal), object 1 scenery (a field generator), both its */
static void setup(void)
{
	memset(objects, 0, sizeof(objects));
	memset(units, 0, sizeof(units));
	strcpy(regions[0].name, "body");
	regions[0].permutations = (struct tag_block){ 1, body_permutations };
	strcpy(regions[1].name, "shield");
	regions[1].permutations = (struct tag_block){ 3, shield_permutations };
	instances[0] = (struct model_marker_instance){ 0, 0, 1, 0, { 9.0f, 9.0f, 9.0f }, { 0 } };
	instances[1] = (struct model_marker_instance){ 1, 0, 3, 0, { 0.1f, 0.2f, 0.3f }, { 0 } };
	strcpy(markers[0].name, "head");
	markers[0].instances = (struct tag_block){ 0, NULL };
	strcpy(markers[1].name, "shield light");
	markers[1].instances = (struct tag_block){ 2, instances };
	models[0] = (struct model){ { 2, markers }, { 5, NULL }, { 2, regions } };
	definitions[0].object.model.index = 0;
	types[0] = _object_type_biped;
	types[1] = _object_type_scenery;
	for (int i = 0; i < 2; i++)
	{
		objects[i].definition_index = 0;
		objects[i].object.outgoing_change_colors[2] = (real_rgb_color){ 0.065f, 0.105f, 0.5f };
		objects[i].object.bounding_sphere_radius = 2.0f;
	}
}

#define NEAR(a, b) (fabsf((a) - (b)) < 1.0e-4f)

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	struct shield_glow_candidate candidate;

	setup();
	CASE("jackal")
	{
		/* its shield standing: lit at the marker's instance in its region,
		in change color C at full brightness, steadily, as far as a unit's */
		CHECK(shield_glow_held(0, &candidate), "a Jackal's standing shield glows");
		CHECK(candidate.node_index == 3 && NEAR(candidate.offset.x, 0.1f) && NEAR(candidate.offset.z, 0.3f),
			"at the shield region's marker: node %d", candidate.node_index);
		CHECK(NEAR(candidate.color.blue, 1.0f) && NEAR(candidate.color.red, 0.13f) && NEAR(candidate.color.green, 0.21f),
			"its tint at full brightness: %f %f %f", candidate.color.red, candidate.color.green, candidate.color.blue);
		CHECK(NEAR(candidate.strength, SHIELD_GLOW_HELD_STRENGTH) && NEAR(candidate.radius, SHIELD_GLOW_HELD_RADIUS),
			"steady: %f, radius %f", candidate.strength, candidate.radius);
		return 0;
	}
	CASE("destroyed")
	{
		objects[0].object.regions_destroyed_flags = FLAG(1);
		CHECK(!shield_glow_held(0, &candidate), "a destroyed shield does not glow");
		return 0;
	}
	CASE("off")
	{
		objects[1].object.region_permutations[1] = 1;
		CHECK(!shield_glow_held(1, &candidate), "a generator switched off (~shield_off) does not glow");
		return 0;
	}
	CASE("no-shield")
	{
		strcpy(regions[1].name, "arm");
		CHECK(!shield_glow_held(0, &candidate), "a model with no shield region does not glow");
		return 0;
	}
	CASE("untinted")
	{
		objects[0].object.outgoing_change_colors[2] = (real_rgb_color){ 0.0f, 0.0f, 0.0f };
		CHECK(!shield_glow_held(0, &candidate), "no tint, no glow");
		return 0;
	}
	CASE("standing-radius")
	{
		/* scenery: as far as its own size, within the bounds */
		CHECK(shield_glow_held(1, &candidate) && NEAR(candidate.radius, 2.0f), "its size: %f", candidate.radius);
		objects[1].object.bounding_sphere_radius = 10.0f;
		CHECK(shield_glow_held(1, &candidate) && NEAR(candidate.radius, SHIELD_GLOW_HELD_RADIUS_MAXIMUM),
			"no further than the most: %f", candidate.radius);
		return 0;
	}
	CASE("camouflage")
	{
		units[0].unit.active_camouflage = 0.5f;
		CHECK(shield_glow_held(0, &candidate) && NEAR(candidate.strength, SHIELD_GLOW_HELD_STRENGTH * 0.5f),
			"dimmed by camouflage: %f", candidate.strength);
		return 0;
	}
	CHECK(FALSE, "unknown case %s", case_name);
	return 1;
}
