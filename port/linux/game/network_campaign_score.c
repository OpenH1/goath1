/*
NETWORK_CAMPAIGN_SCORE.C

The co-op host's campaign score (campaign_score.c) to its OpenH1 clients
(_distributed_message_campaign_score, reliably, over the host's stream).
The host keeps the score; a client shows the host's, and no score at all
without its word (a Legacy game's host, OpenCE's, sends none; nor does any
host to an OpenCE machine).

Each message (struct distributed_campaign_score_header, then its player
records, then its events):

- the header: the format (a client takes its own alone), the flags (the
  host's option on; the level won, on the last message of that word), the
  game's player count, the difficulty's multiplier in hundredths, the team's
  score, and how many records and events follow;
- each player record (struct distributed_campaign_score_player): the
  player's slot (their absolute index in the players' list, the same on
  every machine) and all their tallies, the counts as words;
- each event (struct distributed_campaign_score_event): the player, the
  kind, the victim's class, the points and the host's tick.

The host sends at most each 8 ticks, and only what changed since: the
players whose tallies changed and the events since. Every 10 seconds, to a
client it has not yet sent this map's word, and after a revert, it sends
every player. A word longer than one message (16 players, 32 events) goes
as more, in order. The worst a send takes: 16 + 56 a player + 12 an event,
with the distributed message header's 12 a message (a revert or a refresh
of 128 players: 8 messages, 7.4 KB, once in 8 ticks at most; a co-op game of
4 players' kills: one message of about 300 bytes).
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "networking/network_game_globals.h"
#include "campaign_score.h"
#include "network_distributed.h"
#include "network_standard.h"
#include "network_campaign_score.h"

/* ---------- constants */

enum
{
	MAXIMUM_CAMPAIGN_SCORE_MACHINES = HALO_PORT_MAXIMUM_NETWORK_MACHINES,
};

/* ---------- structures */

/* on the wire: each message's own header */
struct distributed_campaign_score_header
{
	byte format;
	byte flags;
	byte record_count;
	byte event_count;
	short player_count;
	short multiplier_hundredths;
	long team_score;
};

/* ... a player's record */
struct distributed_campaign_score_player
{
	byte slot;
	byte pad;
	word kills;
	word headshots;
	word melee_kills;
	word sticks;
	word deaths;
	word betrayals;
	word best_streak;
	word current_streak;
	word best_multi;
	long score;
	word kills_by_class[16];
};

/* ... an event */
struct distributed_campaign_score_event
{
	byte player;
	byte kind;
	byte victim_class;
	byte pad;
	long points;
	long tick;
};

typedef char distributed_campaign_score_header_size_assert[
	sizeof(struct distributed_campaign_score_header) == 12 ? 1 : -1];
typedef char distributed_campaign_score_player_size_assert[
	sizeof(struct distributed_campaign_score_player) == 56 ? 1 : -1];
typedef char distributed_campaign_score_event_size_assert[
	sizeof(struct distributed_campaign_score_event) == 12 ? 1 : -1];
typedef char distributed_campaign_score_classes_assert[
	NUMBEROF(((struct campaign_score_player *)0)->kills_by_class) ==
	NUMBEROF(((struct distributed_campaign_score_player *)0)->kills_by_class) ? 1 : -1];

/* the largest message's body */
enum
{
	NETWORK_CAMPAIGN_SCORE_MAXIMUM_BODY = sizeof(struct distributed_campaign_score_header) +
		NETWORK_CAMPAIGN_SCORE_PLAYERS_PER_MESSAGE * sizeof(struct distributed_campaign_score_player) +
		NETWORK_CAMPAIGN_SCORE_EVENTS_PER_MESSAGE * sizeof(struct distributed_campaign_score_event),
};

/* ---------- globals */

/* the machines the host has sent this map's word to */
static struct
{
	long machines[MAXIMUM_CAMPAIGN_SCORE_MACHINES];
	short count;
} network_campaign_score_sent;

/* ---------- private code: the wire */

static word network_campaign_score_word(
	long value)
{
	return (word)PIN(value, 0, 0xFFFF);
}

word network_campaign_score_pack(
	byte *buffer,
	word size,
	struct network_campaign_score_contents const *message)
{
	struct distributed_campaign_score_header header;
	word length;
	short index;

	if (!VALID_INDEX(message->record_count, NETWORK_CAMPAIGN_SCORE_PLAYERS_PER_MESSAGE + 1) ||
		!VALID_INDEX(message->event_count, NETWORK_CAMPAIGN_SCORE_EVENTS_PER_MESSAGE + 1))
	{
		return 0;
	}
	length = (word)(sizeof(header) + message->record_count * sizeof(struct distributed_campaign_score_player) +
		message->event_count * sizeof(struct distributed_campaign_score_event));
	if (length > size)
		return 0;
	csmemset(&header, 0, sizeof(header));
	header.format = NETWORK_CAMPAIGN_SCORE_FORMAT;
	header.flags = (byte)((message->enabled ? FLAG(_network_campaign_score_enabled_bit) : 0) |
		(message->level_end ? FLAG(_network_campaign_score_level_end_bit) : 0));
	header.record_count = (byte)message->record_count;
	header.event_count = (byte)message->event_count;
	header.player_count = message->player_count;
	header.multiplier_hundredths = (short)PIN((long)(message->difficulty_multiplier * 100.0f + 0.5f), 0, 10000);
	header.team_score = message->team_score;
	csmemcpy(buffer, &header, sizeof(header));
	length = sizeof(header);
	for (index = 0; index < message->record_count; index++)
	{
		struct campaign_score_player const *player = &message->players[index];
		struct distributed_campaign_score_player record;
		short class_index;

		csmemset(&record, 0, sizeof(record));
		record.slot = (byte)message->slots[index];
		record.kills = network_campaign_score_word(player->kills);
		record.headshots = network_campaign_score_word(player->headshots);
		record.melee_kills = network_campaign_score_word(player->melee_kills);
		record.sticks = network_campaign_score_word(player->sticks);
		record.deaths = network_campaign_score_word(player->deaths);
		record.betrayals = network_campaign_score_word(player->betrayals);
		record.best_streak = network_campaign_score_word(player->best_streak);
		record.current_streak = network_campaign_score_word(player->current_streak);
		record.best_multi = network_campaign_score_word(player->best_multi);
		record.score = player->score;
		for (class_index = 0; class_index < NUMBEROF(record.kills_by_class); class_index++)
			record.kills_by_class[class_index] = network_campaign_score_word(player->kills_by_class[class_index]);
		csmemcpy(buffer + length, &record, sizeof(record));
		length += sizeof(record);
	}
	for (index = 0; index < message->event_count; index++)
	{
		struct campaign_score_event const *event = &message->events[index];
		struct distributed_campaign_score_event record;

		csmemset(&record, 0, sizeof(record));
		record.player = (byte)event->player;
		record.kind = (byte)event->kind;
		record.victim_class = (byte)PIN(event->victim_class, 0, 255);
		record.points = event->points;
		record.tick = event->tick;
		csmemcpy(buffer + length, &record, sizeof(record));
		length += sizeof(record);
	}

	return length;
}

boolean network_campaign_score_unpack(
	byte const *buffer,
	word size,
	struct network_campaign_score_contents *message)
{
	struct distributed_campaign_score_header header;
	word offset;
	short index;

	if (size < sizeof(header))
		return FALSE;
	csmemcpy(&header, buffer, sizeof(header));
	if (header.format != NETWORK_CAMPAIGN_SCORE_FORMAT ||
		header.record_count > NETWORK_CAMPAIGN_SCORE_PLAYERS_PER_MESSAGE ||
		header.event_count > NETWORK_CAMPAIGN_SCORE_EVENTS_PER_MESSAGE ||
		!VALID_INDEX(header.player_count, CAMPAIGN_SCORE_MAX_PLAYERS + 1) ||
		size != sizeof(header) + header.record_count * sizeof(struct distributed_campaign_score_player) +
			header.event_count * sizeof(struct distributed_campaign_score_event))
	{
		return FALSE;
	}
	csmemset(message, 0, sizeof(*message));
	message->enabled = TEST_FLAG(header.flags, _network_campaign_score_enabled_bit) != 0;
	message->level_end = TEST_FLAG(header.flags, _network_campaign_score_level_end_bit) != 0;
	message->player_count = header.player_count;
	message->team_score = header.team_score;
	message->difficulty_multiplier = (real)header.multiplier_hundredths / 100.0f;
	message->record_count = header.record_count;
	message->event_count = header.event_count;
	offset = sizeof(header);
	for (index = 0; index < header.record_count; index++)
	{
		struct campaign_score_player *player = &message->players[index];
		struct distributed_campaign_score_player record;
		short class_index;

		csmemcpy(&record, buffer + offset, sizeof(record));
		offset += sizeof(record);
		if (!VALID_INDEX(record.slot, CAMPAIGN_SCORE_MAX_PLAYERS))
			return FALSE;
		message->slots[index] = record.slot;
		player->kills = record.kills;
		player->headshots = record.headshots;
		player->melee_kills = record.melee_kills;
		player->sticks = record.sticks;
		player->deaths = record.deaths;
		player->betrayals = record.betrayals;
		player->best_streak = record.best_streak;
		player->current_streak = record.current_streak;
		player->best_multi = record.best_multi;
		player->score = record.score;
		for (class_index = 0; class_index < NUMBEROF(record.kills_by_class); class_index++)
			player->kills_by_class[class_index] = record.kills_by_class[class_index];
	}
	for (index = 0; index < header.event_count; index++)
	{
		struct campaign_score_event *event = &message->events[index];
		struct distributed_campaign_score_event record;

		csmemcpy(&record, buffer + offset, sizeof(record));
		offset += sizeof(record);
		if (!VALID_INDEX(record.player, CAMPAIGN_SCORE_MAX_PLAYERS) ||
			record.kind >= NUMBER_OF_CAMPAIGN_SCORE_EVENT_KINDS)
		{
			return FALSE;
		}
		event->player = record.player;
		event->kind = record.kind;
		event->victim_class = record.victim_class;
		event->points = record.points;
		event->tick = record.tick;
	}

	return TRUE;
}

/* ---------- private code: the host */

static boolean network_campaign_score_had(
	long machine_index)
{
	short index;

	for (index = 0; index < network_campaign_score_sent.count; index++)
	{
		if (network_campaign_score_sent.machines[index] == machine_index)
			return TRUE;
	}
	return FALSE;
}

/* one machine's word: the players it is to have (all of them, or the
changed), the events, in as many messages as they take */
static void network_campaign_score_send_to(
	long machine_index,
	struct campaign_score_state const *state,
	boolean const *dirty,
	boolean whole,
	boolean level_end,
	struct campaign_score_event const *ring,
	long ring_size,
	long first,
	long end)
{
	struct
	{
		struct distributed_message_header header;
		byte body[NETWORK_CAMPAIGN_SCORE_MAXIMUM_BODY];
	} message;
	static struct network_campaign_score_contents contents;
	short slot = 0;
	long event = first;
	boolean sent_one = FALSE;

	while (!sent_one || slot < state->player_count || event < end)
	{
		word length;

		csmemset(&contents, 0, sizeof(contents));
		contents.enabled = state->enabled;
		contents.player_count = state->player_count;
		contents.team_score = state->team_score;
		contents.difficulty_multiplier = state->difficulty_multiplier;
		for (; slot < state->player_count && contents.record_count < NETWORK_CAMPAIGN_SCORE_PLAYERS_PER_MESSAGE;
			slot++)
		{
			if (whole || dirty[slot])
			{
				contents.slots[contents.record_count] = slot;
				contents.players[contents.record_count] = state->players[slot];
				contents.record_count++;
			}
		}
		for (; event < end && contents.event_count < NETWORK_CAMPAIGN_SCORE_EVENTS_PER_MESSAGE; event++)
			contents.events[contents.event_count++] = ring[event % ring_size];
		contents.level_end = level_end && slot >= state->player_count && event >= end;
		/* (nothing more to say but the header, said once) */
		if (sent_one && !contents.record_count && !contents.event_count && !contents.level_end)
			break;
		csmemset(&message.header, 0, sizeof(message.header));
		length = network_campaign_score_pack(message.body, sizeof(message.body), &contents);
		if (!length)
			break;
		distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_campaign_score, 1,
			(word)(sizeof(message.header) + length));
		sent_one = TRUE;
	}
}

/* ---------- public code */

void network_campaign_score_new_map(
	void)
{
	network_campaign_score_sent.count = 0;
}

void network_campaign_score_send(
	struct campaign_score_state const *state,
	boolean const *dirty,
	boolean whole,
	boolean level_end,
	struct campaign_score_event const *ring,
	long ring_size,
	long first,
	long end)
{
	long machines[MAXIMUM_CAMPAIGN_SCORE_MACHINES];
	short count;
	short index;
	boolean changed = whole || level_end || first < end;

	if (game_connection() != _game_connection_network_server || !network_standard_hosting() || !game_in_progress())
		return;
	for (index = 0; !changed && index < state->player_count; index++)
		changed = dirty[index];
	count = distributed_client_machines(machines, MAXIMUM_CAMPAIGN_SCORE_MACHINES);
	for (index = 0; index < count; index++)
	{
		boolean had = network_campaign_score_had(machines[index]);

		if (!network_standard_machine_is_openh1(machines[index]) || (had && !changed))
			continue;
		network_campaign_score_send_to(machines[index], state, dirty, whole || !had, level_end, ring, ring_size,
			first, end);
		if (!had && network_campaign_score_sent.count < MAXIMUM_CAMPAIGN_SCORE_MACHINES)
			network_campaign_score_sent.machines[network_campaign_score_sent.count++] = machines[index];
	}
}

/* (the host's alone, over its stream, whole, in an OpenH1 game under way) */
void network_campaign_score_handle_message(
	long machine_index,
	word const *message,
	word size,
	boolean from_stream)
{
	static struct network_campaign_score_contents contents;
	struct distributed_message_header header;
	short index;

	if (machine_index != NONE || !from_stream || game_connection() != _game_connection_network_client ||
		!network_standard_agreed_rate() || !game_in_progress() || size < sizeof(header))
	{
		return;
	}
	csmemcpy(&header, message, sizeof(header));
	if (header.count != 1 ||
		!network_campaign_score_unpack((byte const *)message + sizeof(header), (word)(size - sizeof(header)),
			&contents))
	{
		return;
	}
	campaign_score_heard_state(contents.enabled, contents.team_score, contents.player_count,
		contents.difficulty_multiplier);
	for (index = 0; index < contents.record_count; index++)
		campaign_score_heard_player(contents.slots[index], &contents.players[index]);
	for (index = 0; index < contents.event_count; index++)
		campaign_score_heard_event(&contents.events[index]);
	if (contents.level_end)
		campaign_score_heard_level_end();
}
