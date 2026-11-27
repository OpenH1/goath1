/* Campaign scoring's word from the co-op host (network_campaign_score.c): a
message packed and read back whole, its bytes as the wire has them, and what
a client refuses (another format, a wrong size, a player or an event kind out
of range). */
#include "harness.h"
typedef unsigned short word;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#define csmemset memset
#define csmemcpy memcpy
#include "types.inc"
#include "under_test.inc"

static struct network_campaign_score_contents sample(void)
{
	struct network_campaign_score_contents contents;
	short index;

	memset(&contents, 0, sizeof(contents));
	contents.enabled = TRUE;
	contents.level_end = TRUE;
	contents.player_count = 4;
	contents.team_score = 123456;
	contents.difficulty_multiplier = 1.5f;
	contents.record_count = 2;
	contents.slots[0] = 0;
	contents.slots[1] = 3;
	contents.players[0].score = 4321;
	contents.players[0].kills = 40;
	contents.players[0].headshots = 7;
	contents.players[0].best_multi = 3;
	contents.players[0].kills_by_class[3] = 12;
	contents.players[1].score = 99;
	contents.players[1].deaths = 2;
	contents.players[1].betrayals = 1;
	contents.players[1].best_streak = 5;
	contents.players[1].current_streak = 1;
	contents.players[1].melee_kills = 4;
	contents.players[1].sticks = 2;
	contents.players[1].kills_by_class[15] = 1;
	contents.event_count = 3;
	for (index = 0; index < 3; index++)
	{
		contents.events[index].player = (short)(index ? 3 : 0);
		contents.events[index].kind = (short)(_campaign_score_event_kill + index);
		contents.events[index].points = index == 2 ? -25 : 75 + index;
		contents.events[index].tick = 1000 + index;
		contents.events[index].victim_class = 4;
	}
	return contents;
}

static void expect_same(struct network_campaign_score_contents const *a, struct network_campaign_score_contents const *b)
{
	short index;

	CHECK(a->enabled == b->enabled && a->level_end == b->level_end, "the flags");
	CHECK(a->player_count == b->player_count && a->team_score == b->team_score, "the header's numbers");
	CHECK(a->difficulty_multiplier > b->difficulty_multiplier - 0.001f &&
		a->difficulty_multiplier < b->difficulty_multiplier + 0.001f, "the multiplier: %f, not %f",
		a->difficulty_multiplier, b->difficulty_multiplier);
	CHECK(a->record_count == b->record_count && a->event_count == b->event_count, "the counts");
	for (index = 0; index < a->record_count; index++)
	{
		CHECK(a->slots[index] == b->slots[index], "record %d's slot", index);
		CHECK(!memcmp(&a->players[index], &b->players[index], sizeof(a->players[index])), "record %d's tallies",
			index);
	}
	for (index = 0; index < a->event_count; index++)
	{
		CHECK(!memcmp(&a->events[index], &b->events[index], sizeof(a->events[index])), "event %d", index);
	}
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	static byte buffer[4096];
	static struct network_campaign_score_contents read;
	struct network_campaign_score_contents contents = sample();
	word length = network_campaign_score_pack(buffer, sizeof(buffer), &contents);

	CASE("round-trip")
	{
		CHECK(length == 12 + 2 * 56 + 3 * 12, "the length: %u", length);
		CHECK(network_campaign_score_unpack(buffer, length, &read), "read back");
		expect_same(&read, &contents);
		return 0;
	}
	CASE("layout")
	{
		/* the header: format 1, flags enabled and the level's end, 2 records, 3
		events, 4 players, 150 hundredths, the team's score; the first record's
		slot, kills and score; the first event */
		static byte const header[12] = { 1, 3, 2, 3, 4, 0, 150, 0, 0x40, 0xE2, 0x01, 0x00 };

		CHECK(!memcmp(buffer, header, sizeof(header)), "the header's bytes");
		CHECK(buffer[12] == 0 && buffer[14] == 40 && buffer[15] == 0, "the first record's slot and kills");
		CHECK(buffer[12 + 20] == 0xE1 && buffer[12 + 21] == 0x10, "the first record's score (4321) at 20");
		CHECK(buffer[12 + 24 + 2 * 3] == 12, "its kills of class 3");
		CHECK(buffer[12 + 2 * 56 + 1] == _campaign_score_event_kill && buffer[12 + 2 * 56 + 2] == 4 &&
			buffer[12 + 2 * 56 + 4] == 75 && buffer[12 + 2 * 56 + 8] == 0xE8, "the first event");
		return 0;
	}
	CASE("refused")
	{
		byte copy[4096];

		CHECK(!network_campaign_score_unpack(buffer, (word)(length - 1), &read), "a byte short");
		CHECK(!network_campaign_score_unpack(buffer, (word)(length + 12), &read), "an event too many");
		CHECK(!network_campaign_score_unpack(buffer, 11, &read), "shorter than the header");
		memcpy(copy, buffer, length);
		copy[0] = 2;
		CHECK(!network_campaign_score_unpack(copy, length, &read), "another format");
		memcpy(copy, buffer, length);
		copy[12] = 200;
		CHECK(!network_campaign_score_unpack(copy, length, &read), "a slot past the players");
		memcpy(copy, buffer, length);
		copy[12 + 2 * 56 + 1] = NUMBER_OF_CAMPAIGN_SCORE_EVENT_KINDS;
		CHECK(!network_campaign_score_unpack(copy, length, &read), "an event of no kind");
		memcpy(copy, buffer, length);
		copy[2] = NETWORK_CAMPAIGN_SCORE_PLAYERS_PER_MESSAGE + 1;
		CHECK(!network_campaign_score_unpack(copy, length, &read), "too many records");
		contents.record_count = NETWORK_CAMPAIGN_SCORE_PLAYERS_PER_MESSAGE + 1;
		CHECK(!network_campaign_score_pack(buffer, sizeof(buffer), &contents), "too many to pack");
		contents = sample();
		CHECK(!network_campaign_score_pack(buffer, 20, &contents), "a buffer too small");
		return 0;
	}
	CASE("clamped")
	{
		contents.players[0].kills = 70000;
		contents.players[0].deaths = -3;
		length = network_campaign_score_pack(buffer, sizeof(buffer), &contents);
		CHECK(network_campaign_score_unpack(buffer, length, &read), "read back");
		CHECK(read.players[0].kills == 0xFFFF && read.players[0].deaths == 0, "a count kept to a word: %ld, %ld",
			read.players[0].kills, read.players[0].deaths);
		return 0;
	}
	CASE("new-kinds")
	{
		/* an assassination and a driver's assist: their kinds' numbers on the
		wire (after the flawless bonus's 14), read back */
		contents.events[0].kind = _campaign_score_event_assassination;
		contents.events[1].kind = _campaign_score_event_driver_assist;
		length = network_campaign_score_pack(buffer, sizeof(buffer), &contents);
		CHECK(buffer[12 + 2 * 56 + 1] == 15 && buffer[12 + 2 * 56 + 12 + 1] == 16, "the kinds' bytes: %u, %u",
			buffer[12 + 2 * 56 + 1], buffer[12 + 2 * 56 + 12 + 1]);
		CHECK(network_campaign_score_unpack(buffer, length, &read), "read back");
		expect_same(&read, &contents);
		return 0;
	}
	CASE("full")
	{
		short index;

		contents.record_count = NETWORK_CAMPAIGN_SCORE_PLAYERS_PER_MESSAGE;
		contents.event_count = NETWORK_CAMPAIGN_SCORE_EVENTS_PER_MESSAGE;
		for (index = 0; index < contents.record_count; index++)
		{
			contents.slots[index] = (short)(CAMPAIGN_SCORE_MAX_PLAYERS - 1 - index);
			contents.players[index].score = 1000 * index;
		}
		for (index = 0; index < contents.event_count; index++)
		{
			contents.events[index].player = (short)(CAMPAIGN_SCORE_MAX_PLAYERS - 1);
			contents.events[index].kind = _campaign_score_event_level_bonus;
			contents.events[index].tick = index;
		}
		contents.player_count = CAMPAIGN_SCORE_MAX_PLAYERS;
		length = network_campaign_score_pack(buffer, sizeof(buffer), &contents);
		CHECK(length == 12 + 16 * 56 + 32 * 12, "the largest message: %u bytes", length);
		CHECK(network_campaign_score_unpack(buffer, length, &read), "read back");
		expect_same(&read, &contents);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
