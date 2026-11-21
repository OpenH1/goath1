/*
SHIELD_GLOW.C

The energy shield's glow (Video Setup > Graphics > Shield Glow,
display.shield_glow): as a unit's shield flares (hit, charging, falling),
it lights what is around it, as a plasma bolt does, in the shield's own
color: a Spartan's gold, an Elite's blue. display.shield_glow_intensity
"light_show" makes it reach three times as far, and at full strength from a
third of the flare (a light is no brighter than full: the game's lights'
colors go to 1).

The flare's strength is the shield shader's own: the intensity its plasma is
drawn with (its intensity source, an object function of the unit, to its
exponent), so the glow rises and falls with the flare seen on the body. Its
color is the shader's edge color at full brightness. The light is an
ordinary dynamic light of the game's (light_new_unattached, which
object_lights.c's light_port_glow_set colors and sizes), drawn with a light
definition the map already has: no new assets. The units that glow are those
near any local player's camera, the same for each of a split screen's views
(lights_preprocess_scene runs for each), so each keeps its one light.

A shield that is a model's region rather than a modifier shader (a Jackal's
hand-held shield, the Covenant's standing field generators) glows too, as
long as it stands: a model with a region named "shield", while that region
is not destroyed and its permutation is not one of its off ones ("~..."),
glows steadily at its "shield light" marker, in its change color C (the
shield's tint), and a standing one (scenery) at least as far as its own size.
The nearest of all of them glow, at most SHIELD_GLOW_MAXIMUM.

Only the drawing changes: nothing the game simulates, nothing sent between
machines.
*/

#include "cseries.h"
#include "real_math.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "render/render.h"
#include "rasterizer/rasterizer.h"
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"
#include "units/units.h"
#include "camera/observer.h"
#include "game/players.h"
#include "objects/object_lights.h"
#include "objects/object_types.h"
#include "models/model_definitions.h"
#include "network_shield_effects.h"

#include <math.h>
#include <string.h>

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
const char *config_string(const char *name);
unsigned long config_changes(void);

/* object_lights.c (port) */
long light_port_glow_definition_get(void);
boolean light_port_glow_set(long light_index, long object_index, real_rgb_color const *color, real radius);
/* port/linux/game/overshield_effect.c */
real overshield_effect_flare(long unit_index);
real_rgb_color const *overshield_effect_glow_color(long unit_index);

enum
{
	SHIELD_GLOW_MAXIMUM = 8,
	_shader_type_transparent_plasma_glow = 10, /* (shader_get_and_verify_type's plasma) */
};

/* units this far from the camera and nearer glow (world units) */
#define SHIELD_GLOW_RANGE 25.f
/* how far the glow reaches, world units: at the flare's least and its most */
#define SHIELD_GLOW_RADIUS_MINIMUM 0.5f
#define SHIELD_GLOW_RADIUS_MAXIMUM 1.4f
/* a flare fainter than this lights nothing */
#define SHIELD_GLOW_THRESHOLD 0.03f
/* a model's shield region's steady glow: its strength, and how far it
reaches (world units; a standing shield's at least its own bounding radius,
to SHIELD_GLOW_HELD_RADIUS_MAXIMUM) */
#define SHIELD_GLOW_HELD_STRENGTH 0.55f
#define SHIELD_GLOW_HELD_RADIUS 1.0f
#define SHIELD_GLOW_HELD_RADIUS_MAXIMUM 3.0f
/* the candidates looked at each frame, of which the nearest glow */
#define SHIELD_GLOW_CANDIDATES 64

/* the plasma shader's fields after its header (rasterizer_xbox_plasma_energy.c's) */
struct shield_glow_plasma
{
	byte reserved00[4];
	short intensity_exponent_source;
	short pad06;
	real intensity_exponent;
	byte reserved0C[0x2C];
	real perpendicular_alpha;
	real_rgb_color perpendicular_color;
	real parallel_alpha;
	real_rgb_color parallel_color;
};

struct shield_glow
{
	long unit_index; /* NONE: free */
	long light_index;
	unsigned long frame;
};

/* an object that glows this frame: how near, how bright (to 1), its color,
radius, and where its light is (a node, and the offset in its space) */
struct shield_glow_candidate
{
	long object_index;
	real distance;
	real strength;
	real radius;
	real_rgb_color color;
	short node_index;
	real_point3d offset;
};

static struct shield_glow shield_glows[SHIELD_GLOW_MAXIMUM];
static unsigned long shield_glow_frame = 0;

static real shield_glow_scale = 1.f;

/* display.shield_glow, and its intensity (display.shield_glow_intensity):
"default", or "light_show", three times as bright and as far; in a network
game, the host's (network_shield_effects.c) */
static boolean shield_glow_enabled(
	void)
{
	static unsigned long changes = (unsigned long)-1;
	static boolean enabled = FALSE;
	short effects = network_shield_effects();

	if (effects != NONE)
	{
		changes = (unsigned long)-1;
		shield_glow_scale = effects == _shield_effects_light_show ||
			effects == _shield_effects_light_show_overshield ? 3.f : 1.f;
		return effects != _shield_effects_stock && effects != _shield_effects_overshield;
	}
	if (changes != config_changes())
	{
		char const *intensity = config_string("display.shield_glow_intensity");

		changes = config_changes();
		enabled = config_boolean("display.shield_glow") != 0;
		shield_glow_scale = intensity && !strcmp(intensity, "light_show") ? 3.f : 1.f;
	}
	return enabled;
}

/* a glow's light, on the object's node (NONE: none, or the map has no light
definition to draw it with) */
static long shield_glow_light_new(
	struct shield_glow_candidate const *candidate)
{
	long definition_index = light_port_glow_definition_get();

	if (definition_index == NONE)
		return NONE;
	return light_new_unattached(definition_index, candidate->object_index, candidate->node_index, &candidate->offset,
		global_forward3d, 0.0f);
}

/* how far a point is from the nearest local player's camera */
static real shield_glow_camera_distance(
	real_point3d const *point)
{
	real nearest = SHIELD_GLOW_RANGE * 2.f;
	short local_player_index;

	for (local_player_index = local_player_get_next(NONE); local_player_index != NONE;
		local_player_index = local_player_get_next(local_player_index))
	{
		struct observer_result const *camera = observer_get_camera(local_player_index);
		real distance = camera ? distance3d(point, &camera->position) : nearest;

		if (distance < nearest)
			nearest = distance;
	}
	return nearest;
}

/* a unit's flare now: its strength (0: none) and color */
static real shield_glow_flare(
	long unit_index,
	real_rgb_color *color)
{
	struct object_datum *object = object_try_and_get(unit_index);
	struct object_definition *definition;
	struct shader *shader;
	struct shield_glow_plasma const *plasma;
	short source;
	real value;
	real peak;

	if (!object || TEST_FLAG(object->object.flags, _object_invisible_bit))
		return 0.f;
	definition = object_definition_get(object->definition_index);
	if (!definition || definition->object.modifier_shader.index == NONE)
		return 0.f;
	shader = shader_definition_get(definition->object.modifier_shader.index);
	if (!shader || shader->base.type != _shader_type_transparent_plasma_glow)
		return 0.f;
	plasma = (struct shield_glow_plasma const *)((byte const *)shader + sizeof(struct shader));
	source = plasma->intensity_exponent_source;
	if (source < 1 || source > 4)
		return 0.f;
	value = object->object.outgoing_function_values[source - 1];
	/* (an overshield: lit as it rises, then whole while it lasts) */
	{
		real overshield = overshield_effect_flare(unit_index);

		if (overshield >= 0.f)
			value = overshield;
	}
	if (!(value > 0.f))
		return 0.f;
	value = (real)pow((double)value, (double)plasma->intensity_exponent);
	if (!(value > SHIELD_GLOW_THRESHOLD))
		return 0.f;

	*color = plasma->perpendicular_color;
	/* (an overshield: its own color) */
	if (overshield_effect_flare(unit_index) >= 0.f)
		*color = *overshield_effect_glow_color(unit_index);
	peak = MAX(color->red, MAX(color->green, color->blue));
	if (!(peak > 0.f))
		return 0.f;
	color->red /= peak;
	color->green /= peak;
	color->blue /= peak;
	if (unit_get(unit_index)->unit.active_camouflage > 0.f)
		value *= 1.f - unit_get(unit_index)->unit.active_camouflage;
	return PIN(value, 0.f, 1.f);
}

/* a model's shield region, standing now (a Jackal's, a field
generator's): TRUE, its color and where its light goes; FALSE if the object
has none, or it is destroyed or off */
static boolean shield_glow_held(
	long object_index,
	struct shield_glow_candidate *candidate)
{
	struct object_datum *object = object_try_and_get(object_index);
	struct object_definition *definition;
	struct model *model;
	struct model_region *region = NULL;
	struct model_region_permutation *permutation;
	short region_index, marker_index, instance_index;
	real_rgb_color color;
	real peak;

	if (!object || TEST_FLAG(object->object.flags, _object_invisible_bit))
		return FALSE;
	definition = object_definition_get(object->definition_index);
	if (!definition || definition->object.model.index == NONE)
		return FALSE;
	model = model_definition_get(definition->object.model.index);
	if (!model)
		return FALSE;
	for (region_index = 0; region_index < MIN(model->regions.count, MAXIMUM_REGIONS_PER_OBJECT); region_index++)
	{
		region = TAG_BLOCK_GET_ELEMENT(&model->regions, region_index, struct model_region);
		if (!_stricmp(region->name, "shield"))
			break;
	}
	if (region_index >= MIN(model->regions.count, MAXIMUM_REGIONS_PER_OBJECT) ||
		TEST_FLAG(object->object.regions_destroyed_flags, region_index) ||
		object->object.region_permutations[region_index] >= region->permutations.count)
	{
		return FALSE;
	}
	permutation = TAG_BLOCK_GET_ELEMENT(&region->permutations, object->object.region_permutations[region_index],
		struct model_region_permutation);
	if (permutation->name[0] == '~')
		return FALSE;
	/* (its tint: change color C, at full brightness) */
	color = object->object.outgoing_change_colors[2];
	peak = MAX(color.red, MAX(color.green, color.blue));
	if (!(peak > 0.f))
		return FALSE;
	candidate->color.red = color.red / peak;
	candidate->color.green = color.green / peak;
	candidate->color.blue = color.blue / peak;
	candidate->strength = SHIELD_GLOW_HELD_STRENGTH;
	if (TEST_FLAG(_object_mask_unit, object_get_type(object_index)) && unit_get(object_index)->unit.active_camouflage > 0.f)
		candidate->strength *= 1.f - unit_get(object_index)->unit.active_camouflage;
	candidate->radius = SHIELD_GLOW_HELD_RADIUS;
	if (object_get_type(object_index) == _object_type_scenery)
		candidate->radius = PIN(object->object.bounding_sphere_radius, SHIELD_GLOW_HELD_RADIUS,
			SHIELD_GLOW_HELD_RADIUS_MAXIMUM);
	/* (its "shield light" marker, the region's own where it has one: else the
	root node) */
	candidate->node_index = 0;
	candidate->offset = *(real_point3d const *)global_zero_vector3d;
	for (marker_index = 0; marker_index < model->markers.count; marker_index++)
	{
		struct model_marker *marker = TAG_BLOCK_GET_ELEMENT(&model->markers, marker_index, struct model_marker);
		boolean found = FALSE;

		if (_stricmp(marker->name, "shield light"))
			continue;
		for (instance_index = 0; instance_index < marker->instances.count; instance_index++)
		{
			struct model_marker_instance *instance = TAG_BLOCK_GET_ELEMENT(&marker->instances, instance_index,
				struct model_marker_instance);

			if (instance->node_index >= model->nodes.count)
				continue;
			if (!found || instance->region_index == region_index)
			{
				candidate->node_index = instance->node_index;
				candidate->offset = instance->translation;
				found = TRUE;
			}
			if (instance->region_index == region_index)
				break;
		}
		break;
	}
	return candidate->strength > SHIELD_GLOW_THRESHOLD;
}

static struct shield_glow *shield_glow_find(
	long unit_index,
	boolean make)
{
	struct shield_glow *free_glow = NULL;
	short index;

	for (index = 0; index < SHIELD_GLOW_MAXIMUM; index++)
	{
		if (shield_glows[index].unit_index == unit_index)
			return &shield_glows[index];
		if (shield_glows[index].unit_index == NONE && !free_glow)
			free_glow = &shield_glows[index];
	}
	if (!make || !free_glow)
		return NULL;
	free_glow->unit_index = unit_index;
	free_glow->light_index = NONE;
	return free_glow;
}

/* each frame, before the lights are gathered (lights_preprocess_scene) */
void shield_glow_update(
	void)
{
	static boolean initialized = FALSE;
	struct object_iterator iterator;
	short index;

	if (!initialized)
	{
		for (index = 0; index < SHIELD_GLOW_MAXIMUM; index++)
			shield_glows[index].unit_index = NONE;
		initialized = TRUE;
	}
	shield_glow_frame++;
	if (shield_glow_enabled())
	{
		static struct shield_glow_candidate candidates[SHIELD_GLOW_CANDIDATES];
		short count = 0, kept;

		/* (the shields near the cameras: a unit's flaring, a model's shield
		region standing) */
		object_iterator_new(&iterator, _object_mask_unit | _object_mask_scenery, 0);
		while (object_iterator_next(&iterator) && count < SHIELD_GLOW_CANDIDATES)
		{
			struct object_datum *object = object_get(iterator.index);
			struct shield_glow_candidate *candidate = &candidates[count];
			real flare;

			candidate->distance = shield_glow_camera_distance(&object->object.bounding_sphere_center);
			if (candidate->distance > SHIELD_GLOW_RANGE)
				continue;
			candidate->object_index = iterator.index;
			flare = TEST_FLAG(_object_mask_unit, object_get_type(iterator.index)) ?
				shield_glow_flare(iterator.index, &candidate->color) : 0.f;
			if (flare > SHIELD_GLOW_THRESHOLD)
			{
				candidate->strength = flare;
				candidate->radius = SHIELD_GLOW_RADIUS_MINIMUM + (SHIELD_GLOW_RADIUS_MAXIMUM - SHIELD_GLOW_RADIUS_MINIMUM) * flare;
				candidate->node_index = 0;
				candidate->offset = *(real_point3d const *)global_zero_vector3d;
			}
			else if (!shield_glow_held(iterator.index, candidate))
			{
				continue;
			}
			count++;
		}
		/* (the nearest first: at most SHIELD_GLOW_MAXIMUM glow) */
		for (index = 1; index < count; index++)
		{
			struct shield_glow_candidate moved = candidates[index];
			short at = index;

			while (at > 0 && candidates[at - 1].distance > moved.distance)
			{
				candidates[at] = candidates[at - 1];
				at--;
			}
			candidates[at] = moved;
		}
		if (count > SHIELD_GLOW_MAXIMUM)
			count = SHIELD_GLOW_MAXIMUM;
		/* (those that glowed and are no longer among them let go first, so
		that each of them has a slot) */
		for (index = 0; index < SHIELD_GLOW_MAXIMUM; index++)
		{
			struct shield_glow *glow = &shield_glows[index];

			if (glow->unit_index == NONE)
				continue;
			for (kept = 0; kept < count && candidates[kept].object_index != glow->unit_index; kept++)
				;
			if (kept < count)
				continue;
			if (glow->light_index != NONE)
				light_port_glow_set(glow->light_index, glow->unit_index, global_real_rgb_black, 0.f);
			glow->unit_index = NONE;
			glow->light_index = NONE;
		}
		for (kept = 0; kept < count; kept++)
		{
			struct shield_glow_candidate *candidate = &candidates[kept];
			struct shield_glow *glow = shield_glow_find(candidate->object_index, TRUE);
			real_rgb_color color = candidate->color;

			if (!glow)
				continue;
			if (glow->light_index == NONE)
				glow->light_index = shield_glow_light_new(candidate);
			/* (brighter, to full, every channel alike: the shield's color
			kept) */
			{
				real strength = MIN(candidate->strength * shield_glow_scale, 1.f);

				color.red *= strength;
				color.green *= strength;
				color.blue *= strength;
			}
			if (glow->light_index != NONE &&
				light_port_glow_set(glow->light_index, candidate->object_index, &color,
					candidate->radius * shield_glow_scale))
			{
				glow->frame = shield_glow_frame;
			}
			else
			{
				glow->light_index = NONE;
			}
		}
	}
	/* (a glow not kept this frame: dark, and let go; its light goes a moment
	after, as the game's own passing lights do) */
	for (index = 0; index < SHIELD_GLOW_MAXIMUM; index++)
	{
		struct shield_glow *glow = &shield_glows[index];

		if (glow->unit_index == NONE || glow->frame == shield_glow_frame)
			continue;
		if (glow->light_index != NONE)
			light_port_glow_set(glow->light_index, glow->unit_index, global_real_rgb_black, 0.f);
		glow->unit_index = NONE;
		glow->light_index = NONE;
	}
}
