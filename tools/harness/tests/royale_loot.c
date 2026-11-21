/*
ROYALE_LOOT.C (test)

The real loot plan of Royale (port/linux/game/royale_loot.c): each loot
spot's kind drawn by its tier's rarity weights among the kinds the map has,
the supply pods' spots, times and contents, all from the match's seed with
the game's seeded random (seed_random_range, random_math.c). The same seed
must give the same plan (every machine draws its own); another seed another.
test_royale_loot.py takes the code (generated.inc).
*/

#include "harness.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define csmemset memset

#include "generated.inc"

/* b30's kinds: no shotgun, sniper rifle or fuel rod gun */
static unsigned long const b30 = (1UL << NUMBER_OF_ROYALE_LOOT_KINDS) - 1 &
	~(1UL << ROYALE_LOOT_SHOTGUN) & ~(1UL << ROYALE_LOOT_SNIPER_RIFLE) & ~(1UL << ROYALE_LOOT_FUEL_ROD);
static unsigned long const every = (1UL << NUMBER_OF_ROYALE_LOOT_KINDS) - 1;

static short tiers[ROYALE_MAXIMUM_LOOT_SPOTS];
static struct royale_loot_plan first, second;

static void tiers_all(short tier)
{
	long index;

	for (index = 0; index < ROYALE_MAXIMUM_LOOT_SPOTS; index++)
		tiers[index] = tier;
}

static void tiers_mixed(void)
{
	long index;

	for (index = 0; index < ROYALE_MAXIMUM_LOOT_SPOTS; index++)
		tiers[index] = (short)(index % NUMBER_OF_ROYALE_LOOT_TIERS);
}

static short rarity_of(byte kind)
{
	return kind == ROYALE_LOOT_KIND_NONE ? NONE : royale_loot_kinds[kind].rarity;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	unsigned long seed;
	long index;

	CASE("determinism")
	{
		/* the same seed: the same plan, spot for spot, pod for pod */
		tiers_mixed();
		for (seed = 1; seed < 50; seed++)
		{
			royale_loot_plan_make(seed * 2654435761UL, tiers, 300, 12, b30, &first);
			royale_loot_plan_make(seed * 2654435761UL, tiers, 300, 12, b30, &second);
			CHECK(!memcmp(&first, &second, sizeof(first)), "seed %lu: two plans differ", seed);
		}
		/* another seed: another plan */
		royale_loot_plan_make(1234, tiers, 300, 12, b30, &first);
		royale_loot_plan_make(1235, tiers, 300, 12, b30, &second);
		CHECK(memcmp(first.spot_kinds, second.spot_kinds, sizeof(first.spot_kinds)),
			"seeds 1234 and 1235 give the same ground loot");
		return 0;
	}
	CASE("tiers")
	{
		/* the rarities fall near their tier's weights over many spots */
		long counts[NUMBER_OF_ROYALE_LOOT_TIERS][1 + NUMBER_OF_ROYALE_LOOT_RARITIES];
		long total = 0;
		short tier;
		short rarity;

		memset(counts, 0, sizeof(counts));
		for (tier = 0; tier < NUMBER_OF_ROYALE_LOOT_TIERS; tier++)
		{
			tiers_all(tier);
			for (seed = 0; seed < 40; seed++)
			{
				royale_loot_plan_make(seed, tiers, ROYALE_MAXIMUM_LOOT_SPOTS, 0, every, &first);
				for (index = 0; index < first.spot_count; index++)
					counts[tier][1 + rarity_of(first.spot_kinds[index])]++;
			}
		}
		for (rarity = 0; rarity <= NUMBER_OF_ROYALE_LOOT_RARITIES; rarity++)
			total += royale_loot_tier_weights[0][rarity];
		for (tier = 0; tier < NUMBER_OF_ROYALE_LOOT_TIERS; tier++)
		{
			long weight_total = 0;

			for (rarity = 0; rarity <= NUMBER_OF_ROYALE_LOOT_RARITIES; rarity++)
				weight_total += royale_loot_tier_weights[tier][rarity];
			for (rarity = 0; rarity <= NUMBER_OF_ROYALE_LOOT_RARITIES; rarity++)
			{
				real expected = (real)royale_loot_tier_weights[tier][rarity] / (real)weight_total;
				real seen = (real)counts[tier][rarity] / (real)(40 * ROYALE_MAXIMUM_LOOT_SPOTS);

				CHECK(seen > expected - 0.02f && seen < expected + 0.02f,
					"tier %d, rarity %d: %.3f of the spots, %.3f expected", tier, rarity - 1, seen, expected);
				if (!royale_loot_tier_weights[tier][rarity])
					CHECK(!counts[tier][rarity], "tier %d drew rarity %d, weighted 0", tier, rarity - 1);
			}
		}
		/* a common spot is mostly common loot; a legendary one never empty nor common */
		CHECK(counts[ROYALE_LOOT_COMMON][1 + ROYALE_LOOT_RARITY_COMMON] > counts[ROYALE_LOOT_COMMON][1 + ROYALE_LOOT_RARITY_RARE] * 5,
			"common spots are not mostly common loot");
		CHECK(!counts[ROYALE_LOOT_LEGENDARY][0] && !counts[ROYALE_LOOT_LEGENDARY][1 + ROYALE_LOOT_RARITY_COMMON],
			"a legendary spot was empty or common");
		(void)total;
		return 0;
	}
	CASE("available")
	{
		/* only the kinds the map has; a rare roll on a map without rare
		weapons becomes common loot */
		unsigned long no_rare = b30 & ~(1UL << ROYALE_LOOT_ROCKET_LAUNCHER);
		long rockets = 0;

		tiers_all(ROYALE_LOOT_LEGENDARY);
		for (seed = 0; seed < 20; seed++)
		{
			royale_loot_plan_make(seed, tiers, ROYALE_MAXIMUM_LOOT_SPOTS, 8, b30, &first);
			for (index = 0; index < first.spot_count; index++)
			{
				byte kind = first.spot_kinds[index];

				CHECK(kind != ROYALE_LOOT_KIND_NONE && (b30 & (1UL << kind)), "spot %ld: kind %d not on the map", index, kind);
				rockets += kind == ROYALE_LOOT_ROCKET_LAUNCHER;
			}
			royale_loot_plan_make(seed, tiers, ROYALE_MAXIMUM_LOOT_SPOTS, 8, no_rare, &first);
			for (index = 0; index < first.spot_count; index++)
				CHECK(rarity_of(first.spot_kinds[index]) != ROYALE_LOOT_RARITY_RARE, "a rare kind on a map without");
		}
		CHECK(rockets > 0, "b30's rare loot never drew its rocket launcher");
		return 0;
	}
	CASE("pods")
	{
		/* a few pods on distinct spots, in order, each full of loot */
		tiers_all(ROYALE_LOOT_COMMON);
		for (seed = 0; seed < 200; seed++)
		{
			short pod;

			royale_loot_plan_make(seed, tiers, 10, 5, b30, &first);
			CHECK(first.pod_count == ROYALE_LOOT_POD_COUNT, "%d pods", first.pod_count);
			for (pod = 0; pod < first.pod_count; pod++)
			{
				short other;
				short item;

				CHECK(first.pods[pod].spot >= 0 && first.pods[pod].spot < 5, "pod %d on spot %d", pod, first.pods[pod].spot);
				for (other = 0; other < pod; other++)
				{
					CHECK(first.pods[other].spot != first.pods[pod].spot, "seed %lu: pods %d and %d share spot %d", seed,
						other, pod, first.pods[pod].spot);
					CHECK(first.pods[other].drop_second < first.pods[pod].drop_second, "pods out of order");
				}
				for (item = 0; item < ROYALE_LOOT_POD_ITEMS; item++)
					CHECK(rarity_of(first.pods[pod].kinds[item]) >= ROYALE_LOOT_RARITY_RARE, "a pod holds common loot or none");
			}
		}
		royale_loot_plan_make(1, tiers, 10, 2, b30, &first);
		CHECK(first.pod_count == 2, "two pod spots, %d pods", first.pod_count);
		royale_loot_plan_make(1, tiers, 10, 0, b30, &first);
		CHECK(first.pod_count == 0, "no pod spots, %d pods", first.pod_count);
		return 0;
	}
	CASE("stable")
	{
		/* a spot's tier changes only its own loot: each spot takes as much
		of the seed */
		/* (the first spots common, some of them empty, then legendary,
		never empty) */
		tiers_mixed();
		for (index = 0; index < 20; index++)
			tiers[index] = ROYALE_LOOT_COMMON;
		royale_loot_plan_make(99, tiers, 200, 4, b30, &first);
		for (index = 0; index < 20; index++)
			CHECK(index < 19 || memchr(first.spot_kinds, ROYALE_LOOT_KIND_NONE, 20), "no empty spot to test with");
		for (index = 0; index < 20; index++)
			tiers[index] = ROYALE_LOOT_LEGENDARY;
		royale_loot_plan_make(99, tiers, 200, 4, b30, &second);
		CHECK(!memcmp(first.spot_kinds + 20, second.spot_kinds + 20, 180), "a tier changed shifts later spots' loot");
		CHECK(!memcmp(first.pods, second.pods, sizeof(first.pods)), "a tier changed shifts the pods");
		return 0;
	}
	/* a pod falls at its time into PLAY from the match clock, or at
	ENDGAME's start if that comes first (also straight from the drop) */
	CASE("pod-clock")
	{
		CHECK(royale_loot_pod_fall_start(NONE, NONE, 100) == NONE, "a pod before PLAY");
		CHECK(royale_loot_pod_fall_start(500, NONE, 100) == 600, "in PLAY: %ld", royale_loot_pod_fall_start(500, NONE, 100));
		CHECK(royale_loot_pod_fall_start(500, 550, 100) == 550, "ENDGAME first: %ld",
			royale_loot_pod_fall_start(500, 550, 100));
		CHECK(royale_loot_pod_fall_start(500, 700, 100) == 600, "ENDGAME after it fell: %ld",
			royale_loot_pod_fall_start(500, 700, 100));
		CHECK(royale_loot_pod_fall_start(500, 500, 100) == 500, "straight to ENDGAME: %ld",
			royale_loot_pod_fall_start(500, 500, 100));
		return 0;
	}
	/* the start kit: the lobby's and the drop's units, again after a seat's
	drop; never over a gulag fighter's, a gulag winner's or a redeployed
	one's weapons */
	CASE("kit")
	{
		CHECK(royale_loot_kit_action(ROYALE_PHASE_LOBBY, ROYALE_PLAYER_IN_PELICAN, FALSE, TRUE) == ROYALE_LOOT_KIT_GIVE,
			"no kit in the lobby");
		CHECK(royale_loot_kit_action(ROYALE_PHASE_LOBBY, ROYALE_PLAYER_IN_PELICAN, TRUE, TRUE) == ROYALE_LOOT_KIT_LEAVE,
			"a kitted unit kitted again");
		CHECK(royale_loot_kit_action(ROYALE_PHASE_INSERTION, ROYALE_PLAYER_FALLING, TRUE, FALSE) == ROYALE_LOOT_KIT_GIVE,
			"a rider out of the seat with nothing not kitted again");
		CHECK(royale_loot_kit_action(ROYALE_PHASE_PLAY, ROYALE_PLAYER_GULAG, FALSE, TRUE) == ROYALE_LOOT_KIT_KEEP,
			"a gulag fighter's unit not taken as their own");
		/* (the winner: the same unit, ALIVE then FALLING on the redeploy) */
		CHECK(royale_loot_kit_action(ROYALE_PHASE_PLAY, ROYALE_PLAYER_ALIVE, TRUE, TRUE) == ROYALE_LOOT_KIT_LEAVE,
			"a gulag winner's weapons taken");
		CHECK(royale_loot_kit_action(ROYALE_PHASE_PLAY, ROYALE_PLAYER_FALLING, TRUE, TRUE) == ROYALE_LOOT_KIT_LEAVE,
			"a redeployed player's weapons taken");
		CHECK(royale_loot_kit_action(ROYALE_PHASE_INSERTION, ROYALE_PLAYER_ALIVE, TRUE, TRUE) == ROYALE_LOOT_KIT_LEAVE,
			"a gulag winner of the drop's time disarmed");
		/* (a unit come after the drop, whatever it holds, is left so) */
		CHECK(royale_loot_kit_action(ROYALE_PHASE_PLAY, ROYALE_PLAYER_ALIVE, FALSE, TRUE) == ROYALE_LOOT_KIT_KEEP,
			"a unit of PLAY disarmed");
		return 0;
	}
	/* the sweep deletes only the map's own: never loot's, nor a weapon a unit
	held (a real drop); a loose start kit pistol goes */
	CASE("sweep")
	{
		long placed[] = { 11, 12 };
		long held[] = { 21, 22, 31 };
		long kits[] = { 31 };

		CHECK(royale_loot_purge_spares(11, placed, 2, held, 3, kits, 1), "loot's swept");
		CHECK(royale_loot_purge_spares(22, placed, 2, held, 3, kits, 1), "a real drop swept");
		CHECK(!royale_loot_purge_spares(40, placed, 2, held, 3, kits, 1), "the map's own spared");
		CHECK(!royale_loot_purge_spares(31, placed, 2, held, 3, kits, 1), "a loose kit pistol spared");
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
