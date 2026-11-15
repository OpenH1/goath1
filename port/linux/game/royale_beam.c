/*
ROYALE_BEAM.C

Royale's glassing beam (royale.h), the shrinking circle's place: a
Covenant ship glasses the island from one side to the other.

The sweep: the beam comes from a side drawn from the game's seed (among the
map config's beam_sides), and its front, a straight line across the map's
bounds, crosses the map in the config's stages: each holds (the HUD counts
down "GLASSING IN 0:30") and then moves, from its start to its end (as
fractions of the map's length from that side) over its duration. The last
stage leaves the final area on the far side. Where the front is is a
function of the ticks since the PLAY phase began (royale_beam_status_at),
so every machine has the same front, run to run.

The damage: every player's unit behind the front (or out of the map's
bounds) takes the stage's damage per second, a share each tick, from the
host (a client's would be the host's twice). Downed players too; not the
gulag's, nor those in a Pelican.

What is seen and heard (each machine's own, nothing simulated):
- columns of energy at the front, from the ground to far up the sky, drawn
  with a Covenant contrail's material tinted orange and white-hot, swaying
  along the front, with fire and blasts at their feet;
- a glowing seam along the ground at the front, and an orange haze rising
  over the glassed side;
- the glassed side's light: a player behind the front sees the world tinted
  orange-red, the edges of their screen burning;
- a low hum, louder nearer the front;
- the HUD: "GLASSING IN 0:30" while it holds, "MOVE" and an arrow toward
  safety near or behind the front, and the screen's edge warning.
*/

#include "cseries.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_color_conversion.h"
#include "cache/texture_cache.h"
#include "effects/contrail_definitions.h"
#include "effects/effect_definitions.h"
#include "effects/effects.h"
#include "effects/particle_system_definitions.h"
#include "effects/particle_systems.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "networking/network_game_globals.h"
#include "objects/damage.h"
#include "objects/damage_effect_definitions.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "sound/game_sound.h"
#include "sound/sound_definitions.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"
#include "royale.h"
#include "royale_beam.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void platform_log(char const *format, ...);
void draw_quad(rectangle2d const *bounds, pixel32 color);

/* ---- the sweep (simulation) */

/* where the beam is, at a tick of its sweep */
struct royale_beam_status
{
	short stage; /* the stage holding or moving; the stage count once done */
	boolean moving;
	real front; /* fraction of the map's length glassed */
	real target; /* where this stage's move ends */
	long ticks_to_move; /* while holding; 0 moving or done */
	real damage_per_second; /* behind the front */
};

/* the stages' default damage, by stage, where the config gives none (as
the config's: a share of a unit's whole health, shields and body, a
second): rising so that the last stages are lethal in seconds */
static real const royale_beam_default_damage[ROYALE_MAXIMUM_BEAM_STAGES] =
	{ 0.02f, 0.04f, 0.07f, 0.11f, 0.16f, 0.24f, 0.34f, 0.48f };

/* the stages when the config has none: the island in five sweeps, the
final area the far 6% */
static struct royale_beam_stage const royale_beam_default_stages[] =
{
	{ 60.0f, 0.00f, 0.25f, 45.0f, 0.0f },
	{ 45.0f, 0.25f, 0.50f, 40.0f, 0.0f },
	{ 40.0f, 0.50f, 0.70f, 35.0f, 0.0f },
	{ 30.0f, 0.70f, 0.85f, 30.0f, 0.0f },
	{ 25.0f, 0.85f, 0.94f, 25.0f, 0.0f },
};

/* the sweep at a tick (0: the PLAY phase's first): each stage holds for its
delay, the front at its start, then moves to its end over its duration;
after the last the front stays at its end */
static void royale_beam_status_at(
	struct royale_beam_stage const *stages,
	short stage_count,
	real ticks_per_second,
	long ticks,
	struct royale_beam_status *status)
{
	short index;

	status->stage = stage_count;
	status->moving = FALSE;
	status->front = stage_count > 0 ? stages[0].start : 0.0f;
	status->target = status->front;
	status->ticks_to_move = 0;
	status->damage_per_second = 0.0f;
	if (ticks < 0)
		ticks = 0;
	for (index = 0; index < stage_count; index++)
	{
		struct royale_beam_stage const *stage = &stages[index];
		long hold = (long)(stage->delay * ticks_per_second + 0.5f);
		long move = (long)(stage->duration * ticks_per_second + 0.5f);
		real damage = stage->damage_per_second > 0.0f ?
			stage->damage_per_second : royale_beam_default_damage[index];

		if (move < 1)
			move = 1;
		status->damage_per_second = damage;
		if (ticks < hold)
		{
			status->stage = index;
			status->front = stage->start;
			status->target = stage->end;
			status->ticks_to_move = hold - ticks;
			return;
		}
		ticks -= hold;
		if (ticks < move)
		{
			status->stage = index;
			status->moving = TRUE;
			status->front = stage->start + (stage->end - stage->start) * (real)ticks / (real)move;
			status->target = stage->end;
			return;
		}
		ticks -= move;
		status->front = stage->end;
		status->target = stage->end;
	}
}

/* how far a point is behind the front (world units; negative ahead of it):
along the axis from the beam's side, past the front's line, or out of the
bounds, whichever is deeper */
static real royale_beam_depth_at(
	struct royale_bounds const *bounds,
	short side,
	real front,
	real x,
	real y)
{
	real length;
	real from_side;
	real depth;
	real outside;

	switch (side)
	{
	case ROYALE_SIDE_NORTH:
		length = bounds->maximum.y - bounds->minimum.y;
		from_side = bounds->maximum.y - y;
		break;
	case ROYALE_SIDE_EAST:
		length = bounds->maximum.x - bounds->minimum.x;
		from_side = bounds->maximum.x - x;
		break;
	case ROYALE_SIDE_SOUTH:
		length = bounds->maximum.y - bounds->minimum.y;
		from_side = y - bounds->minimum.y;
		break;
	case ROYALE_SIDE_WEST:
		length = bounds->maximum.x - bounds->minimum.x;
		from_side = x - bounds->minimum.x;
		break;
	default:
		return -1.0e9f;
	}
	depth = front * length - from_side;
	outside = MAX(MAX(bounds->minimum.x - x, x - bounds->maximum.x),
		MAX(bounds->minimum.y - y, y - bounds->maximum.y));
	return MAX(depth, outside);
}

/* the side the beam comes from: a hash of the game's seed (the global
random is left alone: it is the host's alone on a network game) among the
config's sides (all if none) */
static short royale_beam_choose_side(
	unsigned long seed,
	long sides)
{
	short allowed[NUMBER_OF_ROYALE_SIDES];
	short count = 0;
	short side;
	unsigned long hash;

	for (side = 0; side < NUMBER_OF_ROYALE_SIDES; side++)
	{
		if (!sides || (sides & (1L << side)))
			allowed[count++] = side;
	}
	if (count == 0)
		return ROYALE_SIDE_NORTH;
	hash = (seed ^ 0x9E3779B9UL) * 2654435761UL;
	hash ^= hash >> 15;
	hash *= 2246822519UL;
	hash ^= hash >> 13;
	return allowed[hash % (unsigned long)count];
}

/* ---- the beam's state */

enum
{
	ROYALE_BEAM_COLUMNS = 5,
	ROYALE_BEAM_SEAM_POINTS = 96,
	ROYALE_BEAM_FIRE_EFFECTS = 4,
	ROYALE_BEAM_BLAST_EFFECTS = 4,
};

/* the beam's damage's scale (damage.h: its flash, shake and sound by it; the
amount is the multiplier's) */
#define ROYALE_BEAM_DAMAGE_SCALE 0.08f
/* how far past the HUD's bounds the screen's edge is, a share of its size */
#define ROYALE_BEAM_HUD_EDGE 0.087f

/* the beam's tags, the first of each list the map has (b30's first) */
static char const *const royale_beam_contrail_names[] =
{
	"vehicles\\c gun turret\\c gun turret",
	"weapons\\plasma rifle\\plasma charged bolt",
	"weapons\\plasma rifle\\plasma",
	"weapons\\fuel rod gun\\tail",
	"vehicles\\banshee\\banshee wingtip contrail",
};
static char const *const royale_beam_fire_names[ROYALE_BEAM_FIRE_EFFECTS] =
{
	"effects\\burning large",
	"scenery\\emitters\\smoldering_debris\\effects\\smoldering_debris",
	"scenery\\debris\\rocks_smoldering\\effects\\smoking rock",
	"vehicles\\banshee\\effects\\partial damage",
};
static char const *const royale_beam_blast_names[ROYALE_BEAM_BLAST_EFFECTS] =
{
	"vehicles\\banshee\\fuel rod explosion",
	"weapons\\plasma grenade\\effects\\explosion",
	"vehicles\\pelican\\effects\\pelican explosion",
	"weapons\\fuel rod gun\\effects\\explosion",
};
/* (the explosions' effects all do damage: their particles and sound alone) */
static char const *const royale_beam_blast_particle_names[] =
{
	"effects\\particle systems\\explosion large",
	"vehicles\\pelican\\effects\\explosion_pelican",
	"weapons\\fuel rod gun\\effects\\hunter fuel rod explosion",
	"weapons\\plasma grenade\\effects\\explosion",
};
static char const *const royale_beam_blast_sound_names[] =
{
	"sound\\sfx\\weapons\\fuel rod gun\\explosion",
	"sound\\sfx\\weapons\\plasma grenade\\plasmagrenexpl",
	"sound\\sfx\\impulse\\impacts\\elite_sword_boom",
};
static char const *const royale_beam_hum_names[] =
{
	"sound\\sfx\\vehicles\\covydrop",
	"sound\\sfx\\ambience\\computers\\covy_uplink",
	"sound\\sfx\\weapons\\shield fx\\covy_shield",
};
static char const *const royale_beam_damage_names[] =
{
	"globals\\flaming_death",
	"globals\\falling",
	"globals\\distance",
};

static struct
{
	boolean running;
	struct royale_bounds bounds;
	short side;
	short stage_count;
	struct royale_beam_stage stages[ROYALE_MAXIMUM_BEAM_STAGES];
	real ticks_per_second;
	/* the match time the PLAY phase began at (NONE: not yet) */
	long play_start;
	long ticks;
	struct royale_beam_status status;
	/* the tags (NONE: the map has none) */
	long contrail;
	long fire_effect;
	long blast_effect;
	long blast_particles;
	long blast_sound;
	long hum;
	long damage;
	real damage_per_point; /* the damage tag's own, for a multiplier */
	boolean hum_playing;
	/* each local view's: the ground under the columns, and the seam */
	real column_ground[ROYALE_BEAM_COLUMNS];
	unsigned long effect_seed;
} royale_beam;

static long royale_beam_first_tag(
	long group,
	char const *const *names,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		long tag_index = tag_loaded(group, names[index]);

		if (tag_index != NONE)
			return tag_index;
	}
	return NONE;
}

/* an effect only seen and heard: none of its parts makes an object or does
damage (as effects.c judges a gameplay effect), so each machine can play
it alone */
static boolean royale_beam_effect_cosmetic(
	long effect_index)
{
	struct effect_definition *definition;
	long event_index;

	if (effect_index == NONE)
		return FALSE;
	definition = (struct effect_definition *)tag_get(EFFECT_DEFINITION_TAG, effect_index);
	for (event_index = 0; event_index < definition->events.count; event_index++)
	{
		struct effect_event_definition *event = TAG_BLOCK_GET_ELEMENT(
			&definition->events, event_index, struct effect_event_definition);
		long part_index;

		for (part_index = 0; part_index < event->parts.count; part_index++)
		{
			struct effect_part_definition *part = TAG_BLOCK_GET_ELEMENT(
				&event->parts, part_index, struct effect_part_definition);

			if (part->reference.index != NONE && (part->runtime_base_class_tag == OBJECT_DEFINITION_TAG ||
				part->runtime_base_class_tag == DAMAGE_EFFECT_DEFINITION_TAG))
			{
				return FALSE;
			}
		}
	}
	return TRUE;
}

static long royale_beam_first_cosmetic_effect(
	char const *const *names,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		long tag_index = tag_loaded(EFFECT_DEFINITION_TAG, names[index]);

		if (tag_index != NONE && royale_beam_effect_cosmetic(tag_index))
			return tag_index;
		if (tag_index != NONE)
			platform_log("royale beam: %s does damage or makes objects: not used", names[index]);
	}
	return NONE;
}

void royale_beam_start(
	struct royale_map_config const *config)
{
	static char const *const side_names[] = { "north", "east", "south", "west" };
	short index;

	csmemset(&royale_beam, 0, sizeof(royale_beam));
	royale_beam.play_start = NONE;
	royale_beam.hum = NONE;
	if (!config)
		return;
	royale_beam.bounds = config->bounds;
	if (royale_beam.bounds.maximum.x <= royale_beam.bounds.minimum.x ||
		royale_beam.bounds.maximum.y <= royale_beam.bounds.minimum.y)
	{
		/* (no bounds: the structure BSP's, a sixth in from each edge, as the
		insertion takes them) */
		struct structure_bsp *bsp = global_structure_bsp_get();
		real_rectangle3d world = bsp ? bsp->world_bounds : (real_rectangle3d){ { -60, 60, -60, 60, 0, 30 } };
		real inset_x = (world.x1 - world.x0) / 6.0f;
		real inset_y = (world.y1 - world.y0) / 6.0f;

		royale_beam.bounds.minimum.x = world.x0 + inset_x;
		royale_beam.bounds.maximum.x = world.x1 - inset_x;
		royale_beam.bounds.minimum.y = world.y0 + inset_y;
		royale_beam.bounds.maximum.y = world.y1 - inset_y;
		royale_beam.bounds.minimum.z = world.z0;
		royale_beam.bounds.maximum.z = world.z1;
	}
	royale_beam.stage_count = (short)MIN(config->beam_stage_count, ROYALE_MAXIMUM_BEAM_STAGES);
	if (royale_beam.stage_count > 0)
	{
		csmemcpy(royale_beam.stages, config->beam_stages, sizeof(royale_beam.stages[0]) * royale_beam.stage_count);
	}
	else
	{
		royale_beam.stage_count = (short)NUMBEROF(royale_beam_default_stages);
		csmemcpy(royale_beam.stages, royale_beam_default_stages, sizeof(royale_beam_default_stages));
	}
	/* tests (royale_test_setting): HALO_ROYALE_BEAM_SPEED runs the stages that many times faster
	(pictures and clips), HALO_ROYALE_BEAM_SIDE picks the side (0 north, 1
	east, 2 south, 3 west) */
	{
		char const *speed_text = royale_test_setting("HALO_ROYALE_BEAM_SPEED");
		real speed = speed_text ? (real)atof(speed_text) : 1.0f;

		if (speed > 0.0f && speed != 1.0f)
		{
			for (index = 0; index < royale_beam.stage_count; index++)
			{
				royale_beam.stages[index].delay /= speed;
				royale_beam.stages[index].duration /= speed;
			}
		}
	}
	royale_beam.side = royale_beam_choose_side(royale_match_seed(), config->beam_sides);
	if (royale_test_setting("HALO_ROYALE_BEAM_SIDE"))
		royale_beam.side = (short)(atoi(royale_test_setting("HALO_ROYALE_BEAM_SIDE")) & 3);
	royale_beam.ticks_per_second = (real)royale_seconds_to_ticks(100.0f) / 100.0f;
	if (royale_beam.ticks_per_second <= 0.0f)
		royale_beam.ticks_per_second = 30.0f;
	royale_beam.effect_seed = royale_match_seed() ^ 0x676C6173UL;

	royale_beam.contrail = royale_beam_first_tag(CONTRAIL_DEFINITION_TAG,
		royale_beam_contrail_names, (short)NUMBEROF(royale_beam_contrail_names));
	royale_beam.fire_effect = royale_beam_first_cosmetic_effect(royale_beam_fire_names, ROYALE_BEAM_FIRE_EFFECTS);
	royale_beam.blast_effect = royale_beam_first_cosmetic_effect(royale_beam_blast_names, ROYALE_BEAM_BLAST_EFFECTS);
	royale_beam.blast_particles = royale_beam_first_tag(PARTICLE_SYSTEM_DEFINITION_TAG,
		royale_beam_blast_particle_names, (short)NUMBEROF(royale_beam_blast_particle_names));
	royale_beam.blast_sound = royale_beam_first_tag(SOUND_DEFINITION_TAG,
		royale_beam_blast_sound_names, (short)NUMBEROF(royale_beam_blast_sound_names));
	royale_beam.hum = royale_beam_first_tag(LOOPING_SOUND_DEFINITION_TAG,
		royale_beam_hum_names, (short)NUMBEROF(royale_beam_hum_names));
	royale_beam.damage =royale_beam_first_tag(DAMAGE_EFFECT_DEFINITION_TAG,
		royale_beam_damage_names, (short)NUMBEROF(royale_beam_damage_names));
	royale_beam.damage_per_point = 0.0f;
	if (royale_beam.damage != NONE)
	{
		struct damage_definition const *damage =
			&((struct damage_effect_definition *)tag_get(DAMAGE_EFFECT_DEFINITION_TAG, royale_beam.damage))->damage;

		/* (what one hit of it does at ROYALE_BEAM_DAMAGE_SCALE: the scale
		keeps its screen flash and shake faint) */
		royale_beam.damage_per_point = (1.0f - ROYALE_BEAM_DAMAGE_SCALE) * damage->damage_minimum +
			ROYALE_BEAM_DAMAGE_SCALE * 0.5f * (damage->damage_lower_bound + damage->damage_upper_bound);
		platform_log("royale beam: damage %s (%.2f..%.2f, minimum %.2f)", tag_get_name(royale_beam.damage),
			damage->damage_lower_bound, damage->damage_upper_bound, damage->damage_minimum);
	}
	for (index = 0; index < ROYALE_BEAM_COLUMNS; index++)
		royale_beam.column_ground[index] = 0.0f;
	royale_beam.running = TRUE;
	royale_beam_status_at(royale_beam.stages, royale_beam.stage_count, royale_beam.ticks_per_second, 0,
		&royale_beam.status);
	platform_log("royale beam: from the %s, %d stages, bounds %.0f %.0f .. %.0f %.0f; contrail %s, fire %s, blast %s, hum %s",
		side_names[royale_beam.side], royale_beam.stage_count,
		royale_beam.bounds.minimum.x, royale_beam.bounds.minimum.y,
		royale_beam.bounds.maximum.x, royale_beam.bounds.maximum.y,
		royale_beam.contrail != NONE ? tag_get_name(royale_beam.contrail) : "none",
		royale_beam.fire_effect != NONE ? tag_get_name(royale_beam.fire_effect) : "none",
		royale_beam.blast_effect != NONE ? tag_get_name(royale_beam.blast_effect) :
			royale_beam.blast_particles != NONE ? tag_get_name(royale_beam.blast_particles) : "none",
		royale_beam.hum != NONE ? tag_get_name(royale_beam.hum) : "none");
}

/* ---- the queries (royale.h's, the bots') */

static boolean royale_beam_sweeping(
	void)
{
	return royale_beam.running && royale_beam.play_start != NONE;
}

short royale_beam_side(void)
{
	return royale_beam.running ? royale_beam.side : NONE;
}

real royale_beam_front(void)
{
	return royale_beam_sweeping() ? royale_beam.status.front : 0.0f;
}

real royale_beam_target(void)
{
	return royale_beam_sweeping() ? royale_beam.status.target : 0.0f;
}

long royale_beam_ticks_to_move(void)
{
	return royale_beam_sweeping() ? royale_beam.status.ticks_to_move : 0;
}

real royale_beam_depth(
	real_point3d const *point)
{
	if (!royale_beam_sweeping())
		return -1.0e9f;
	return royale_beam_depth_at(&royale_beam.bounds, royale_beam.side, royale_beam.status.front, point->x, point->y);
}

/* ---- the front's geometry */

/* the front line's two ends (across the map, at the front) and the
direction toward safety */
static void royale_beam_front_line(
	real front,
	real_point2d *a,
	real_point2d *b,
	real_vector2d *safe)
{
	struct royale_bounds const *bounds = &royale_beam.bounds;
	real length;

	switch (royale_beam.side)
	{
	case ROYALE_SIDE_NORTH:
		length = bounds->maximum.y - bounds->minimum.y;
		a->x = bounds->minimum.x; b->x = bounds->maximum.x;
		a->y = b->y = bounds->maximum.y - front * length;
		safe->i = 0.0f; safe->j = -1.0f;
		break;
	case ROYALE_SIDE_EAST:
		length = bounds->maximum.x - bounds->minimum.x;
		a->y = bounds->minimum.y; b->y = bounds->maximum.y;
		a->x = b->x = bounds->maximum.x - front * length;
		safe->i = -1.0f; safe->j = 0.0f;
		break;
	case ROYALE_SIDE_SOUTH:
		length = bounds->maximum.y - bounds->minimum.y;
		a->x = bounds->minimum.x; b->x = bounds->maximum.x;
		a->y = b->y = bounds->minimum.y + front * length;
		safe->i = 0.0f; safe->j = 1.0f;
		break;
	default:
		length = bounds->maximum.x - bounds->minimum.x;
		a->y = bounds->minimum.y; b->y = bounds->maximum.y;
		a->x = b->x = bounds->minimum.x + front * length;
		safe->i = 1.0f; safe->j = 0.0f;
		break;
	}
}

/* the ground under a point (the structure's, from high above), or the
fallback's height */
static real royale_beam_ground(
	real x,
	real y,
	real fallback)
{
	struct collision_result collision;
	real_point3d top;
	real_vector3d down;
	unsigned long flags = FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit);

	top.x = x;
	top.y = y;
	top.z = royale_beam.bounds.maximum.z > royale_beam.bounds.minimum.z ?
		royale_beam.bounds.maximum.z + 50.0f : 400.0f;
	down.i = 0.0f;
	down.j = 0.0f;
	down.k = -(top.z - (royale_beam.bounds.maximum.z > royale_beam.bounds.minimum.z ?
		royale_beam.bounds.minimum.z - 50.0f : -200.0f));
	if (collision_test_vector(flags, &top, &down, NONE, &collision))
		return collision.point.z;
	return fallback;
}

/* where a column stands along the front (0..1 across), swaying */
static real royale_beam_column_across(
	short column,
	real seconds)
{
	real base = ((real)column + 0.5f) / (real)ROYALE_BEAM_COLUMNS;
	real sway = 0.35f / (real)ROYALE_BEAM_COLUMNS;

	return base + sway * sinf(seconds * (0.21f + 0.04f * (real)column) + (real)column * 1.7f);
}

/* ---- the tick (simulation, and each machine's sounds and effects) */

static unsigned long royale_beam_next_random(
	void)
{
	royale_beam.effect_seed = royale_beam.effect_seed * 1664525UL + 1013904223UL;
	return royale_beam.effect_seed >> 8;
}

static void royale_beam_spawn_effect(
	long definition_index,
	real_point3d const *point)
{
	/* (the markers damage.c gives an effect: the surface's normal, up here,
	and the gravity's way) */
	static char const *const marker_names[2] = { "normal", "gravity" };
	real_point3d marker_points[2];
	real_vector3d marker_forwards[2];

	if (definition_index == NONE)
		return;
	marker_points[0] = marker_points[1] = *point;
	marker_forwards[0] = *global_up3d;
	marker_forwards[1] = *global_down3d;
	effect_new_unattached_from_markers(definition_index, NONE, global_zero_vector3d,
		2, (char const **)marker_names, marker_points, marker_forwards,
		1.0f, 0.0f, NULL, NULL, FALSE);
}

/* the host's: the damage behind the front, a tick's share of the stage's */
static void royale_beam_damage_tick(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	real per_tick;

	if (network_game_distributed_client() || royale_beam.damage == NONE ||
		royale_beam.status.damage_per_second <= 0.0f)
	{
		return;
	}
	per_tick = royale_beam.status.damage_per_second / royale_beam.ticks_per_second;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		enum royale_player_state state = royale_player_state(iterator.datum_index);
		real_point3d position;
		struct damage_data damage;

		if (player->unit_index == NONE || !unit_try_and_get(player->unit_index))
			continue;
		if (state != ROYALE_PLAYER_ALIVE && state != ROYALE_PLAYER_DOWNED)
			continue;
		object_get_origin(player->unit_index, &position);
		if (royale_beam_depth(&position) <= 0.0f)
			continue;
		damage_data_new(&damage, royale_beam.damage);
		damage.origin = position;
		damage.epicenter = position;
		damage.direction = *global_down3d;
		damage.scale = ROYALE_BEAM_DAMAGE_SCALE;
		/* (the tick's share of the unit's whole health, in the damage tag's
		points) */
		damage.multiplier = royale_beam.damage_per_point > 0.0f ?
			per_tick * (object_get_maximum_body_vitality(player->unit_index, TRUE) +
				object_get_maximum_shield_vitality(player->unit_index, TRUE)) / royale_beam.damage_per_point :
			0.0f;
		scenario_location_from_point(&damage.location, &damage.epicenter);
		object_cause_damage(&damage, player->unit_index, NONE, NONE, NONE, NULL);
	}
}

/* the nearest a local player's camera (their unit) is to the front, in
world units, behind it positive; and that player's place */
static boolean royale_beam_local_listener(
	real_point3d *position)
{
	short local_player_index = local_player_get_next(NONE);
	long player_index;
	struct player_datum *player;

	if (local_player_index == NONE)
		return FALSE;
	player_index = local_player_get_player_index(local_player_index);
	player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	if (!player || player->unit_index == NONE || !object_header_try_and_get(player->unit_index))
		return FALSE;
	object_get_origin(player->unit_index, position);
	return TRUE;
}

/* each machine's: the hum's volume, and fire and blasts along the front
near its players */
static void royale_beam_local_tick(
	void)
{
	real_point3d listener;
	real_point2d a, b;
	real_vector2d safe;
	real seconds = (real)royale_beam.ticks / royale_beam.ticks_per_second;
	long fire_period = (long)(royale_beam.ticks_per_second * 0.25f);
	long blast_period = (long)(royale_beam.ticks_per_second * 0.6f);
	boolean has_listener = royale_beam_local_listener(&listener);

	/* the hum: started once the sweep is on (its loudness by the distance
	is royale_fx.c's: the deep rumble, louder as the front nears) */
	if (royale_beam.hum != NONE && !royale_beam.hum_playing)
	{
		scripted_looping_sound_start(royale_beam.hum, NONE, 0.0f);
		royale_beam.hum_playing = TRUE;
	}
	if (!has_listener || royale_beam.status.front <= 0.0f)
		return;
	if (fabsf(royale_beam_depth(&listener)) > 200.0f)
		return;
	royale_beam_front_line(royale_beam.status.front, &a, &b, &safe);
	if (fire_period < 1)
		fire_period = 1;
	if (blast_period < 1)
		blast_period = 1;
	/* fire along the seam, within 80 units of the player across the front */
	if (royale_beam.fire_effect != NONE && royale_beam.ticks % fire_period == 0)
	{
		real_point3d point;
		real offset = ((real)(royale_beam_next_random() % 1000) / 1000.0f - 0.5f) * 160.0f;

		if (a.x == b.x)
		{
			point.x = a.x + safe.i * -2.0f;
			point.y = PIN(listener.y + offset, MIN(a.y, b.y), MAX(a.y, b.y));
		}
		else
		{
			point.x = PIN(listener.x + offset, MIN(a.x, b.x), MAX(a.x, b.x));
			point.y = a.y + safe.j * -2.0f;
		}
		point.z = royale_beam_ground(point.x, point.y, listener.z);
		royale_beam_spawn_effect(royale_beam.fire_effect, &point);
	}
	/* blasts at the columns' feet */
	if ((royale_beam.blast_effect != NONE || royale_beam.blast_particles != NONE) &&
		royale_beam.ticks % blast_period == 0)
	{
		short column = (short)(royale_beam_next_random() % ROYALE_BEAM_COLUMNS);
		real across = royale_beam_column_across(column, seconds);
		real_point3d point;

		point.x = a.x + (b.x - a.x) * across;
		point.y = a.y + (b.y - a.y) * across;
		point.z = royale_beam_ground(point.x, point.y, listener.z) + 0.5f;
		if (royale_beam.blast_effect != NONE)
		{
			royale_beam_spawn_effect(royale_beam.blast_effect, &point);
		}
		else
		{
			/* (orange-white, three times the size) */
			real_argb_color color = { 1.0f, 1.0f, 0.62f, 0.25f };

			particle_system_new_unattached(royale_beam.blast_particles, &point, global_zero_vector3d, &color, 3.0f);
		}
		if (royale_beam.blast_sound != NONE)
		{
			struct sound_location location;

			location.position = point;
			location.forward = *global_up3d;
			location.translational_velocity = *global_zero_vector3d;
			scenario_location_from_point(&location.game_location, &point);
			unattached_impulse_sound_new(royale_beam.blast_sound, &location, 1.0f);
		}
	}
}

void royale_beam_tick(
	void)
{
	enum royale_phase phase;

	if (!royale_beam.running)
		return;
	phase = royale_phase();
	if (phase != ROYALE_PHASE_PLAY && phase != ROYALE_PHASE_ENDGAME)
	{
		if (royale_beam.play_start == NONE)
			return;
		/* (RESULTS: the beam stays where it was) */
		return;
	}
	/* (from the match clock every machine keeps alike, each tick: PLAY's
	start is the rules', also when ENDGAME came at once; a client that
	joined late, or whose clock the host corrected, is right at once) */
	royale_beam.play_start = royale_play_start_time();
	if (royale_beam.play_start == NONE)
		return;
	royale_beam.ticks = MAX(royale_match_time() - royale_beam.play_start, 0);
	{
		short stage = royale_beam.status.stage;
		boolean moving = royale_beam.status.moving;

		royale_beam_status_at(royale_beam.stages, royale_beam.stage_count, royale_beam.ticks_per_second,
			royale_beam.ticks, &royale_beam.status);
		/* (every machine's, alike: a host and client's logs compare) */
		if (stage != royale_beam.status.stage || moving != royale_beam.status.moving)
		{
			platform_log("royale beam: stage %d %s at match tick %ld (front %.3f)", royale_beam.status.stage,
				royale_beam.status.moving ? "moving" : "holding", royale_match_time(), royale_beam.status.front);
		}
	}
	royale_beam_damage_tick();
	royale_beam_local_tick();
}

/* ---- the world's drawing */

struct royale_beam_vertex
{
	real_point3d point;
	real_point2d texture;
	pixel32 color;
};

typedef char verify_royale_beam_vertex_size[sizeof(struct royale_beam_vertex) == 0x18 ? 1 : -1];

/* a strip of quads (pairs of vertices) through the contrail's material */
struct royale_beam_strip
{
	struct royale_beam_vertex vertices[2 * ROYALE_BEAM_SEAM_POINTS];
	short pairs;
	real_point3d centroid;
};

static pixel32 royale_beam_pixel(
	real red,
	real green,
	real blue,
	real alpha)
{
	real_argb_color color;

	/* (additive materials take no alpha: their colors are faded too) */
	color.alpha = PIN(alpha, 0.0f, 1.0f);
	color.red = PIN(red * color.alpha, 0.0f, 1.0f);
	color.green = PIN(green * color.alpha, 0.0f, 1.0f);
	color.blue = PIN(blue * color.alpha, 0.0f, 1.0f);
	return real_argb_color_to_pixel32(&color);
}

static void royale_beam_strip_add(
	struct royale_beam_strip *strip,
	real_point3d const *p0,
	real_point3d const *p1,
	real u,
	pixel32 color0,
	pixel32 color1)
{
	struct royale_beam_vertex *vertices;

	if (strip->pairs >= ROYALE_BEAM_SEAM_POINTS)
		return;
	vertices = &strip->vertices[2 * strip->pairs];
	vertices[0].point = *p0;
	vertices[0].texture.x = u;
	vertices[0].texture.y = 1.0f;
	vertices[0].color = color0;
	vertices[1].point = *p1;
	vertices[1].texture.x = u;
	vertices[1].texture.y = 0.0f;
	vertices[1].color = color1;
	strip->centroid.x += 0.5f * (p0->x + p1->x);
	strip->centroid.y += 0.5f * (p0->y + p1->y);
	strip->centroid.z += 0.5f * (p0->z + p1->z);
	strip->pairs++;
}

static void royale_beam_strip_draw(
	struct royale_beam_strip *strip)
{
	struct contrail_definition *definition;
	struct bitmap_data *bitmap;
	short segments = (short)(strip->pairs - 1);
	short triangle_count = (short)(2 * segments);
	long triangle_buffer;
	long vertex_buffer;

	if (strip->pairs < 2 || royale_beam.contrail == NONE)
		return;
	definition = contrail_definition_get(royale_beam.contrail);
	bitmap = bitmap_group_get_bitmap_from_sequence(definition->bitmap.index,
		definition->first_sequence_index, 0);
	if (!bitmap)
		return;
	rasterizer_globals.current_lock_operation = _rasterizer_lock_contrail;
	if (_texture_cache_bitmap_get_hardware_format(bitmap, FALSE, TRUE))
	{
		triangle_buffer = rasterizer_dynamic_triangles_new(triangle_count);
		vertex_buffer = rasterizer_dynamic_vertices_new(6, 2 * strip->pairs);
		if (triangle_buffer != NONE && vertex_buffer != NONE)
		{
			short *triangles = rasterizer_dynamic_triangles_lock(triangle_buffer);
			struct royale_beam_vertex *vertices = rasterizer_dynamic_vertices_lock(vertex_buffer);
			short segment;

			if (triangles && vertices)
			{
				csmemcpy(vertices, strip->vertices, sizeof(struct royale_beam_vertex) * 2 * strip->pairs);
				for (segment = 0; segment < segments; segment++)
				{
					triangles[0] = (short)(2 * segment);
					triangles[1] = (short)(2 * segment + 1);
					triangles[2] = (short)(2 * segment + 2);
					triangles[3] = (short)(2 * segment + 2);
					triangles[4] = (short)(2 * segment + 1);
					triangles[5] = (short)(2 * segment + 3);
					triangles += 6;
				}
			}
			rasterizer_dynamic_triangles_unlock(triangle_buffer);
			rasterizer_dynamic_vertices_unlock(vertex_buffer);
			strip->centroid.x /= strip->pairs;
			strip->centroid.y /= strip->pairs;
			strip->centroid.z /= strip->pairs;
			rasterizer_dynamic_unlit_geometry_draw((struct shader const *)&definition->shader, bitmap, 0,
				triangle_buffer, vertex_buffer, triangle_count, &strip->centroid, 0);
		}
		if (triangle_buffer != NONE)
			rasterizer_dynamic_triangles_delete(triangle_buffer);
		if (vertex_buffer != NONE)
			rasterizer_dynamic_vertices_delete(vertex_buffer);
	}
	rasterizer_globals.current_lock_operation = _rasterizer_lock_none;
}

/* a column of light from the ground to the sky at a point, facing the
camera, as several strips: a wide, dim orange glow and a narrow white-hot
core, the material's texture flowing down */
static void royale_beam_draw_column(
	real x,
	real y,
	real ground,
	real seconds,
	real intensity)
{
	static struct royale_beam_strip strip;
	real_vector2d to_camera;
	real_vector2d across;
	real length;
	short layer;
	real const top = ground + 900.0f;

	to_camera.i = render.camera.position.x - x;
	to_camera.j = render.camera.position.y - y;
	length = sqrtf(to_camera.i * to_camera.i + to_camera.j * to_camera.j);
	if (length < 0.01f)
		return;
	across.i = -to_camera.j / length;
	across.j = to_camera.i / length;
	for (layer = 0; layer < 3; layer++)
	{
		/* (glow, body, core) */
		static real const widths[3] = { 46.0f, 18.0f, 5.0f };
		static real const reds[3] = { 1.0f, 1.0f, 1.0f };
		static real const greens[3] = { 0.30f, 0.55f, 0.92f };
		static real const blues[3] = { 0.05f, 0.18f, 0.75f };
		static real const alphas[3] = { 0.45f, 0.8f, 1.0f };
		short step;
		short const steps = 12;
		real flicker = 0.85f + 0.15f * sinf(seconds * 23.0f + (real)layer * 2.1f + x * 0.01f);

		csmemset(&strip, 0, sizeof(strip));
		for (step = 0; step <= steps; step++)
		{
			/* (closer together near the ground, where the flare is) */
			real t = (real)step / (real)steps;
			real height = t * t;
			real z = ground - 2.0f + (top - ground) * height;
			/* flared at the foot, a little narrower up high */
			real width = widths[layer] * (1.0f + 1.6f * expf(-t * 14.0f)) * (1.0f - 0.35f * t);
			real alpha = alphas[layer] * intensity * flicker * (1.0f - t * t * 0.85f);
			real_point3d p0, p1;
			pixel32 color = royale_beam_pixel(reds[layer], greens[layer], blues[layer], alpha);

			p0.x = x - across.i * width * 0.5f;
			p0.y = y - across.j * width * 0.5f;
			p0.z = z;
			p1.x = x + across.i * width * 0.5f;
			p1.y = y + across.j * width * 0.5f;
			p1.z = z;
			royale_beam_strip_add(&strip, &p0, &p1, height * 6.0f + seconds * 1.5f, color, color);
		}
		royale_beam_strip_draw(&strip);
	}
}

void royale_beam_render(
	void)
{
	static struct royale_beam_strip strip;
	real_point2d a, b;
	real_vector2d safe;
	real seconds;
	real line_length;
	real camera_fallback = render.camera.position.z - 20.0f;
	short column;
	short point;

	/* (Royale's alone: the render calls it in every game) */
	if (!royale_active() || !royale_beam_sweeping() || royale_beam.contrail == NONE ||
		royale_beam.status.front <= 0.0f)
	{
		return;
	}
	seconds = (real)royale_beam.ticks / royale_beam.ticks_per_second;
	royale_beam_front_line(royale_beam.status.front, &a, &b, &safe);
	line_length = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
	if (line_length < 1.0f)
		return;

	/* the columns */
	for (column = 0; column < ROYALE_BEAM_COLUMNS; column++)
	{
		real across = royale_beam_column_across(column, seconds);
		real x = a.x + (b.x - a.x) * across;
		real y = a.y + (b.y - a.y) * across;
		real ground = royale_beam_ground(x, y, camera_fallback);

		royale_beam_draw_column(x, y, ground, seconds, royale_beam.status.moving ? 1.0f : 0.8f);
	}

	/* the seam and the haze: the front's line on the ground, a bright glow
	along it, and an orange curtain rising over the glassed side */
	{
		real grounds[ROYALE_BEAM_SEAM_POINTS];
		short const points = ROYALE_BEAM_SEAM_POINTS;

		for (point = 0; point < points; point++)
		{
			real t = (real)point / (real)(points - 1);

			grounds[point] = royale_beam_ground(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, camera_fallback);
		}
		/* the curtain: from the ground up, fading */
		csmemset(&strip, 0, sizeof(strip));
		for (point = 0; point < points; point++)
		{
			real t = (real)point / (real)(points - 1);
			real_point3d p0, p1;
			real pulse = 0.8f + 0.2f * sinf(seconds * 2.0f + t * 19.0f);

			p0.x = p1.x = a.x + (b.x - a.x) * t - safe.i * 1.0f;
			p0.y = p1.y = a.y + (b.y - a.y) * t - safe.j * 1.0f;
			p0.z = grounds[point] - 1.0f;
			p1.z = grounds[point] + 140.0f;
			royale_beam_strip_add(&strip, &p0, &p1, t * 8.0f + seconds * 0.4f,
				royale_beam_pixel(1.0f, 0.38f, 0.08f, 0.55f * pulse), royale_beam_pixel(1.0f, 0.2f, 0.05f, 0.0f));
		}
		royale_beam_strip_draw(&strip);
		/* the seam: a bright band lying on the ground across the front */
		csmemset(&strip, 0, sizeof(strip));
		for (point = 0; point < points; point++)
		{
			real t = (real)point / (real)(points - 1);
			real_point3d p0, p1;
			real pulse = 0.75f + 0.25f * sinf(seconds * 5.0f + t * 41.0f);
			pixel32 color = royale_beam_pixel(1.0f, 0.75f, 0.35f, pulse);

			p0.x = a.x + (b.x - a.x) * t - safe.i * 3.0f;
			p0.y = a.y + (b.y - a.y) * t - safe.j * 3.0f;
			p0.z = grounds[point] + 0.4f;
			p1.x = a.x + (b.x - a.x) * t + safe.i * 3.0f;
			p1.y = a.y + (b.y - a.y) * t + safe.j * 3.0f;
			p1.z = grounds[point] + 0.4f;
			royale_beam_strip_add(&strip, &p0, &p1, t * 30.0f - seconds * 2.0f, color, color);
		}
		royale_beam_strip_draw(&strip);
		/* and a low wall of light standing on it */
		csmemset(&strip, 0, sizeof(strip));
		for (point = 0; point < points; point++)
		{
			real t = (real)point / (real)(points - 1);
			real_point3d p0, p1;

			p0.x = p1.x = a.x + (b.x - a.x) * t;
			p0.y = p1.y = a.y + (b.y - a.y) * t;
			p0.z = grounds[point] - 0.5f;
			p1.z = grounds[point] + 9.0f;
			royale_beam_strip_add(&strip, &p0, &p1, t * 20.0f + seconds * 3.0f,
				royale_beam_pixel(1.0f, 0.85f, 0.5f, 1.0f), royale_beam_pixel(1.0f, 0.35f, 0.05f, 0.0f));
		}
		royale_beam_strip_draw(&strip);
	}
}

/* ---- the HUD */

static pixel32 royale_beam_hud_pixel(
	real red,
	real green,
	real blue,
	real alpha)
{
	return ((pixel32)(PIN(alpha, 0.0f, 1.0f) * 255.0f + 0.5f) << 24) |
		((pixel32)(PIN(red, 0.0f, 1.0f) * 255.0f + 0.5f) << 16) |
		((pixel32)(PIN(green, 0.0f, 1.0f) * 255.0f + 0.5f) << 8) |
		(pixel32)(PIN(blue, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void royale_beam_draw(
	void)
{
	rectangle2d bounds;
	rectangle2d screen;
	real_point3d eye;
	real depth;
	real seconds;
	short width, height;

	if (!royale_beam_sweeping())
		return;
	seconds = (real)royale_beam.ticks / royale_beam.ticks_per_second;
	/* the warnings are for a player standing in the world (alive or downed),
	from where their unit is; the countdown for everyone */
	{
		long player_index = local_player_get_player_index(render.local_player_index);
		struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
		enum royale_player_state state = player_index != NONE ? royale_player_state(player_index) : ROYALE_PLAYER_NONE;

		depth = -1.0e9f;
		if (player && player->unit_index != NONE && object_header_try_and_get(player->unit_index) &&
			(state == ROYALE_PLAYER_ALIVE || state == ROYALE_PLAYER_DOWNED))
		{
			object_get_origin(player->unit_index, &eye);
			depth = royale_beam_depth(&eye);
		}
	}
	/* (in the view's own coordinates: a split screen's lower window's too) */
	bounds = render.camera.window_bounds;
	offset_rectangle2d(&bounds, (short)-render.camera.viewport_bounds.x0, (short)-render.camera.viewport_bounds.y0);
	width = (short)(bounds.x1 - bounds.x0);
	height = (short)(bounds.y1 - bounds.y0);
	/* (the HUD's canvas is drawn inset from the window's edges, by about
	7.4% on each side: the screen's edge is that far out of its bounds) */
	screen.x0 = (short)(bounds.x0 - width * ROYALE_BEAM_HUD_EDGE);
	screen.x1 = (short)(bounds.x1 + width * ROYALE_BEAM_HUD_EDGE);
	screen.y0 = (short)(bounds.y0 - height * ROYALE_BEAM_HUD_EDGE);
	screen.y1 = (short)(bounds.y1 + height * ROYALE_BEAM_HUD_EDGE);

	/* behind the front: the world's light orange-red, the screen's edges
	burning; near it, the edges only, fainter */
	if (depth > -40.0f)
	{
		real pulse = 0.5f + 0.5f * sinf(seconds * 6.0f);
		real strength = depth > 0.0f ? 1.0f : 1.0f + depth / 40.0f;
		short band;
		short const bands = 8;
		short const thickness = (short)MAX(4, height / 50);

		if (depth > 0.0f)
		{
			rectangle2d whole = screen;

			draw_quad(&whole, royale_beam_hud_pixel(1.0f, 0.32f, 0.06f, 0.11f + 0.05f * pulse));
		}
		for (band = 0; band < bands; band++)
		{
			real alpha = strength * (0.5f + 0.25f * pulse) * (1.0f - (real)band / (real)bands);
			pixel32 color = royale_beam_hud_pixel(1.0f, 0.18f + 0.02f * band, 0.04f, alpha * 0.55f);
			rectangle2d edge;
			short inset = (short)(band * thickness);

			edge.x0 = screen.x0; edge.x1 = screen.x1;
			edge.y0 = (short)(screen.y0 + inset); edge.y1 = (short)(edge.y0 + thickness);
			draw_quad(&edge, color);
			edge.y1 = (short)(screen.y1 - inset); edge.y0 = (short)(edge.y1 - thickness);
			draw_quad(&edge, color);
			edge.y0 = (short)(screen.y0 + (band + 1) * thickness);
			edge.y1 = (short)(screen.y1 - (band + 1) * thickness);
			edge.x0 = (short)(screen.x0 + inset); edge.x1 = (short)(edge.x0 + thickness);
			draw_quad(&edge, color);
			edge.x1 = (short)(screen.x1 - inset); edge.x0 = (short)(edge.x1 - thickness);
			draw_quad(&edge, color);
		}
	}

	/* (the countdown, the warning and the way to safety are the HUD's:
	royale_hud.c, from royale_beam_hud_state) */
}

boolean royale_beam_hud_state(
	real_point3d const *point,
	struct royale_beam_hud_state *state)
{
	real_point2d a, b;

	csmemset(state, 0, sizeof(*state));
	if (!royale_beam_sweeping())
		return FALSE;
	state->moving = royale_beam.status.moving;
	state->holding = !royale_beam.status.moving && royale_beam.status.stage < royale_beam.stage_count;
	state->ticks_to_move = royale_beam.status.ticks_to_move;
	state->ticks_per_second = royale_beam.ticks_per_second;
	state->depth = -1.0e9f;
	state->target_depth = -1.0e9f;
	if (point)
	{
		state->depth = royale_beam_depth(point);
		state->target_depth = royale_beam_depth_at(&royale_beam.bounds, royale_beam.side, royale_beam.status.target,
			point->x, point->y);
	}
	royale_beam_front_line(royale_beam.status.front, &a, &b, &state->safe);
	return TRUE;
}

/* (royale.h) not running, no play start, none of the last map's tags */
void royale_beam_reset(
	void)
{
	csmemset(&royale_beam, 0, sizeof(royale_beam));
	royale_beam.play_start = NONE;
	royale_beam.contrail = NONE;
	royale_beam.fire_effect = NONE;
	royale_beam.blast_effect = NONE;
	royale_beam.blast_particles = NONE;
	royale_beam.blast_sound = NONE;
	royale_beam.hum = NONE;
	royale_beam.damage = NONE;
}
