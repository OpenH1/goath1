/* An effect on a first-person weapon (effects/effects.c): the game state the
ticks keep (the effects and their locations) is the same whether the
renderer drew the weapon or not; the first-person locations, the frames',
are where a drawn weapon's particles go, and only while it is drawn; they
are outside the game state, and a game state written over leaves none of
the state's before them walked and a shown weapon's back. */
#include "harness.h"

typedef unsigned short word;
typedef struct { real red, green, blue; } real_rgb_color;
typedef struct { real scale; real_vector3d forward, left, up; real_point3d position; } real_matrix4x3;
struct location { long leaf_index; short cluster_index; short bsp_index; };
struct tag_block { long count; void *elements; };
struct object_marker { short node_index; real_matrix4x3 node_matrix; real_matrix4x3 matrix; };
struct object_datum { long definition_index; };

#define NUMBEROF(a) ((long)(sizeof(a) / sizeof((a)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->elements)[index])
#define csmemset memset
#define match_vassert(file, line, condition, message) ((void)0)
#define csprintf(buffer, ...) ""
#define error(priority, ...) ((void)0)
#define _error_silent 0
#define NUMBER_OF_OBJECT_CHANGE_COLORS 4
static char temporary[256];

#include "config.inc"
#define effect_get(index) ((struct effect_datum *)datum_get(effect_data, (index)))
#define effect_try_and_get(index) ((struct effect_datum *)datum_try_and_get(effect_data, (index)))
#define effect_location_get(index) ((struct effect_location_datum *)datum_get(effect_location_data, (index)))

/* data arrays: harness.h's, with what effects.c also asks of them */
static void *datum_try_and_get(struct data_array *array, long datum_index)
{
	struct datum_header *header;

	if (datum_index == NONE || DATUM_INDEX_TO_ABSOLUTE_INDEX(datum_index) >= array->maximum_count)
		return NULL;
	header = datum_get(array, datum_index);
	return header->identifier && header->identifier == (short)(datum_index >> 16) ? header : NULL;
}

static long data_next_index(struct data_array *array, long datum_index)
{
	long index;

	for (index = datum_index == NONE ? 0 : DATUM_INDEX_TO_ABSOLUTE_INDEX(datum_index) + 1;
		index < array->maximum_count; index++)
	{
		struct datum_header *header = (struct datum_header *)(array->data + index * array->size);

		if (header->identifier)
			return ((long)(unsigned short)header->identifier << 16) | index;
	}
	return NONE;
}

static void data_make_valid(struct data_array *array)
{
	memset(array->data, 0, (size_t)(array->maximum_count * array->size));
	array->count = 0;
	array->next_identifier = 0x1000;
}

static void data_delete_all(struct data_array *array)
{
	long index;

	for (index = 0; index < array->maximum_count; index++)
		((struct datum_header *)(array->data + index * array->size))->identifier = 0;
	array->count = 0;
}

static struct data_array *effect_data, *effect_location_data;
static struct effect_first_person *effect_first_persons;
static struct data_array *effect_first_person_location_data;
/* (the particles' own effects, outside the game state: effects.c) */
static struct data_array *effect_particle_data, *effect_particle_location_data;
static boolean effects_particles_active;

/* one effect definition, with two locations, each at one marker */
static struct effect_location_definition locations[2] = { { "primary" }, { "secondary" } };
static struct effect_definition { long flags; struct tag_block locations, events; } the_definition =
	{ 0, { 2, locations }, { 1, NULL } };
#define effect_definition_get(index) (&the_definition)
static boolean game_in_editor(void) { return FALSE; }
static char const *tag_get_name(long index) { return "effect"; }
static char const *tag_name_strip_path(char const *name) { return name; }
static struct object_datum *object_try_and_get(long index) { return NULL; }
static boolean effects_corpse_nonviolent = FALSE, debug_effects_nonviolent = FALSE;
static real_rgb_color const white = { 1.0f, 1.0f, 1.0f };
static real_rgb_color const *const global_real_rgb_white = &white;
static short local_player_count(void) { return 1; }

/* the renderer's: whether it drew the weapon (first_person_weapon_draw),
the first-person weapons' (the game state's, as a frame left them) */
#define WEAPON 0x12340005
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 4
static struct first_person_weapon { boolean visible; long weapon_index; }
	first_person_weapon_slots[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS] = { { FALSE, WEAPON }, { FALSE, NONE }, { FALSE, NONE },
	{ FALSE, NONE } };
static struct first_person_weapon *first_person_weapons = first_person_weapon_slots;
#define first_person_weapon_get(index) (&first_person_weapons[index])
#define weapon_drawn (first_person_weapon_slots[0].visible)
static long dead_spectate_eyes_unit(short local_player_index) { return NONE; }
static void effects_start_on_first_person_weapon(short local_player_index, long object_index);
void first_person_weapons_port_effects_restart(void);
static long first_person_weapon_get_local_index(long weapon_index)
{
	return weapon_drawn && weapon_index == WEAPON ? 0 : NONE;
}

static short markers_of(short first_node, struct object_marker *markers, short maximum)
{
	CHECK(maximum >= 1, "no room for a marker");
	memset(markers, 0, sizeof(*markers));
	markers[0].node_index = first_node;
	markers[0].node_matrix.position.x = first_node;
	return 1;
}
static short object_get_marker_by_name(long object, char const *name, struct object_marker *markers, short maximum)
{
	return markers_of(name[0] == 'p' ? 1 : 2, markers, maximum);
}
static short first_person_weapon_get_marker_by_name(long object, char const *name, struct object_marker *markers,
	short maximum)
{
	CHECK(weapon_drawn, "the first-person weapon's markers asked for while it is not drawn");
	return markers_of(name[0] == 'p' ? 3 : 4, markers, maximum);
}

struct effect_datum;
static void effect_set_event(long effect_index, short event_index) { }
static void effect_update(long effect_index, real dt) { }
static void impulse_effect_initialize(struct effect_datum *effect, real scale_a, real scale_b,
	real_rgb_color const *color, struct effect_vector_field const *impulse_field) { }
static boolean effects_object_is_corpse(long object_index) { return FALSE; }
static void effect_build_locations(struct effect_datum *effect, short (*get_markers_by_name)(long object_index,
	char const *name, struct object_marker *markers, short maximum_marker_count));
static long effect_build_location(struct effect_datum *effect, struct object_marker const *marker,
	short location_index, boolean first_person_weapon);
static struct effect_location_datum *effect_location_get_next_instance(struct effect_datum const *effect,
	long *location_datum_index, short camera_mode);

#include "under_test.inc"

/* the bytes of a data array the ticks keep, as the demo's hash takes them */
static unsigned long array_hash(struct data_array const *array)
{
	unsigned long hash = 2166136261UL;
	long index;

	hash = (hash ^ (unsigned long)array->count) * 16777619UL;
	hash = (hash ^ (unsigned short)array->next_identifier) * 16777619UL;
	for (index = 0; index < array->maximum_count * array->size; index++)
		hash = (hash ^ array->data[index]) * 16777619UL;
	return hash;
}

static void map_begin(void)
{
	effects_initialize_for_new_map();
}

/* a muzzle's flash and a looping effect on the weapon, the renderer
hiding and showing it (as a weapon switch does) between them, one effect
deleted, another made: the hashes of what the ticks keep */
static void play(boolean drawn, unsigned long *hashes)
{
	long looping, flash;

	map_begin();
	weapon_drawn = drawn;
	looping = effect_new_looping(7, WEAPON, NONE, NONE, NONE);
	flash = effect_new_from_object(7, WEAPON, WEAPON, NONE, 0.0f, 0.0f, NULL, NULL);
	CHECK(looping != NONE && flash != NONE, "no effects made");
	if (drawn)
	{
		weapon_drawn = FALSE;
		effects_stop_on_first_person_weapon(0);
		weapon_drawn = TRUE;
		effects_start_on_first_person_weapon(0, WEAPON);
	}
	effect_delete(flash);
	CHECK(effect_new_from_object(7, WEAPON, WEAPON, NONE, 0.0f, 0.0f, NULL, NULL) != NONE, "no effect made");
	hashes[0] = array_hash(effect_data);
	hashes[1] = array_hash(effect_location_data);
}

/* the locations a particle of the camera mode is placed at: how many, and
how many of them on the first-person weapon */
static int instances(long effect_index, short camera_mode, int *first_person)
{
	struct effect_datum *effect = effect_get(effect_index);
	int count = 0;
	short location_index;

	*first_person = 0;
	for (location_index = 0; location_index < 2; location_index++)
	{
		long location_datum_index = effect_location_first_instance(effect, location_index, camera_mode);
		struct effect_location_datum *instance;

		while ((instance = effect_location_get_next_instance(effect, &location_datum_index, camera_mode)) != NULL)
		{
			count++;
			if (instance->node_designator != NONE && TEST_FLAG(instance->node_designator, _effect_location_first_person_bit))
				*first_person += 1;
		}
	}
	return count;
}

/* the game state (the effects, their locations, the first-person weapons) as
a saved game holds it, and written back over as game_state.c does, which
then has the effects put right what is outside it */
struct saved_state
{
	struct data_array effects, locations;
	unsigned char *effect_bytes, *location_bytes;
	struct first_person_weapon weapons[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
};

static void state_save(struct saved_state *state)
{
	size_t effect_size = (size_t)(effect_data->maximum_count * effect_data->size);
	size_t location_size = (size_t)(effect_location_data->maximum_count * effect_location_data->size);

	state->effects = *effect_data;
	state->locations = *effect_location_data;
	state->effect_bytes = malloc(effect_size);
	state->location_bytes = malloc(location_size);
	memcpy(state->effect_bytes, effect_data->data, effect_size);
	memcpy(state->location_bytes, effect_location_data->data, location_size);
	memcpy(state->weapons, first_person_weapon_slots, sizeof(state->weapons));
}

static void state_load(struct saved_state const *state)
{
	unsigned char *effect_bytes = effect_data->data, *location_bytes = effect_location_data->data;

	*effect_data = state->effects;
	*effect_location_data = state->locations;
	effect_data->data = effect_bytes;
	effect_location_data->data = location_bytes;
	memcpy(effect_bytes, state->effect_bytes, (size_t)(effect_data->maximum_count * effect_data->size));
	memcpy(location_bytes, state->location_bytes,
		(size_t)(effect_location_data->maximum_count * effect_location_data->size));
	memcpy(first_person_weapon_slots, state->weapons, sizeof(state->weapons));
	effects_port_game_state_rewritten();
}

int main(int argc, char **argv)
{
	const char *case_name = argv[1];

	effect_data = game_state_data_new("effect", HALO_PORT_MAXIMUM_EFFECTS, sizeof(struct effect_datum));
	effect_location_data = game_state_data_new("effect location", HALO_PORT_MAXIMUM_EFFECT_LOCATIONS,
		sizeof(struct effect_location_datum));
	effect_first_persons = calloc(HALO_PORT_MAXIMUM_EFFECTS, sizeof(*effect_first_persons));
	effect_first_person_location_data = game_state_data_new("effect first person location",
		MAXIMUM_EFFECT_FIRST_PERSON_LOCATIONS, sizeof(struct effect_location_datum));
	effect_particle_data = game_state_data_new("particle effect", HALO_PORT_MAXIMUM_EFFECTS, sizeof(struct effect_datum));
	effect_particle_location_data = game_state_data_new("particle effect location", HALO_PORT_MAXIMUM_EFFECT_LOCATIONS,
		sizeof(struct effect_location_datum));

	CASE("drawn-or-not")
	{
		unsigned long hidden[2], drawn[2];

		play(FALSE, hidden);
		play(TRUE, drawn);
		CHECK(hidden[0] == drawn[0], "the effects differ with the weapon drawn: %08lx, %08lx", hidden[0], drawn[0]);
		CHECK(hidden[1] == drawn[1], "the effects' locations differ with the weapon drawn: %08lx, %08lx",
			hidden[1], drawn[1]);
	}
	CASE("first-person")
	{
		/* drawn: a first-person particle goes at the weapon's markers, a
		third-person one at the object's; hidden, the first-person ones go
		nowhere and the effect has no player */
		long looping;
		int first_person;

		map_begin();
		weapon_drawn = TRUE;
		looping = effect_new_looping(7, WEAPON, NONE, NONE, NONE);
		CHECK(instances(looping, _effect_camera_mode_first_person_only, &first_person) == 2 && first_person == 2,
			"first person: %d of the weapon's markers", first_person);
		CHECK(instances(looping, _effect_camera_mode_both, &first_person) == 2 && first_person == 2,
			"both, one local player: %d of the weapon's markers", first_person);
		CHECK(instances(looping, _effect_camera_mode_third_person_only, &first_person) == 2 && first_person == 0,
			"third person: %d of the weapon's markers", first_person);
		CHECK(instances(looping, _effect_camera_mode_independent_of_camera_mode, &first_person) == 2 &&
			first_person == 0, "the parts: %d of the weapon's markers", first_person);
		CHECK(effect_local_player_index(effect_get(looping)) == 0, "the effect has no player");
		CHECK(effect_get(looping)->local_player_index == NONE, "the effect's own player is the frames'");
		weapon_drawn = FALSE;
		effects_stop_on_first_person_weapon(0);
		CHECK(instances(looping, _effect_camera_mode_first_person_only, &first_person) == 0,
			"hidden: first-person particles still placed");
		CHECK(instances(looping, _effect_camera_mode_both, &first_person) == 2 && first_person == 0,
			"hidden: both not at the object");
		CHECK(effect_local_player_index(effect_get(looping)) == NONE, "hidden: the effect keeps its player");
		CHECK(effect_first_person_location_data->count == 0, "hidden: %ld first-person locations left",
			(long)effect_first_person_location_data->count);
		weapon_drawn = TRUE;
		effects_start_on_first_person_weapon(0, WEAPON);
		CHECK(instances(looping, _effect_camera_mode_first_person_only, &first_person) == 2 && first_person == 2,
			"shown again: %d of the weapon's markers", first_person);
		effect_delete(looping);
		CHECK(effect_first_person_location_data->count == 0, "deleted: %ld first-person locations left",
			(long)effect_first_person_location_data->count);
	}
	CASE("rewritten")
	{
		/* the game state saved with the weapon shown, and again hidden; the
		renderer then showing it, which takes the effect's slot again under
		the same identifier the saved states' effect has; each state loaded
		over that */
		struct saved_state shown, hidden;
		long looping;
		int first_person;

		map_begin();
		weapon_drawn = TRUE;
		looping = effect_new_looping(7, WEAPON, NONE, NONE, NONE);
		state_save(&shown);
		weapon_drawn = FALSE;
		effects_stop_on_first_person_weapon(0);
		state_save(&hidden);
		weapon_drawn = TRUE;
		effects_start_on_first_person_weapon(0, WEAPON);
		CHECK(effect_first_person_location_data->count == 2, "shown: %ld first-person locations",
			(long)effect_first_person_location_data->count);

		/* hidden in the state loaded: the slot of the state before (its
		identifier the loaded effect's) is not the effect's */
		state_load(&hidden);
		CHECK(effect_local_player_index(effect_get(looping)) == NONE,
			"loaded hidden: the effect has the player of the state before");
		CHECK(instances(looping, _effect_camera_mode_first_person_only, &first_person) == 0,
			"loaded hidden: %d first-person locations of the state before walked", first_person);
		CHECK(effect_first_person_location_data->count == 0, "loaded hidden: %ld first-person locations left",
			(long)effect_first_person_location_data->count);
		/* (its next frame shows it, as the renderer does) */
		weapon_drawn = TRUE;
		effects_start_on_first_person_weapon(0, WEAPON);
		CHECK(instances(looping, _effect_camera_mode_first_person_only, &first_person) == 2 && first_person == 2,
			"loaded hidden, shown: %d of the weapon's markers", first_person);

		/* shown in the state loaded: the weapon's own state says it is
		shown, so no frame starts it; its locations are back all the same,
		and only they */
		state_load(&shown);
		CHECK(effect_local_player_index(effect_get(looping)) == 0, "loaded shown: the effect has no player");
		CHECK(instances(looping, _effect_camera_mode_first_person_only, &first_person) == 2 && first_person == 2,
			"loaded shown: %d of the weapon's markers", first_person);
		CHECK(effect_first_person_location_data->count == 2, "loaded shown: %ld first-person locations",
			(long)effect_first_person_location_data->count);
		effect_delete(looping);
		CHECK(effect_first_person_location_data->count == 0, "deleted: %ld first-person locations left",
			(long)effect_first_person_location_data->count);
	}
	return 0;
}
