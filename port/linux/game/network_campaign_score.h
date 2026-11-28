/*
NETWORK_CAMPAIGN_SCORE.H

The co-op host's campaign score to its OpenH1 clients
(network_campaign_score.c).
*/

#ifndef NETWORK_CAMPAIGN_SCORE_H
#define NETWORK_CAMPAIGN_SCORE_H

/* ---------- constants */

enum
{
	/* a machine takes its own format alone */
	NETWORK_CAMPAIGN_SCORE_FORMAT = 1,
	/* the most of each one message carries (a longer word goes as more
	messages, in order) */
	NETWORK_CAMPAIGN_SCORE_PLAYERS_PER_MESSAGE = 16,
	NETWORK_CAMPAIGN_SCORE_EVENTS_PER_MESSAGE = 32,
};

/* the header's flags */
enum
{
	_network_campaign_score_enabled_bit = 0,
	/* the level won: the last message of the level's word */
	_network_campaign_score_level_end_bit,
};

/* ---------- structures */

/* a message as read (network_campaign_score_unpack) */
struct network_campaign_score_contents
{
	boolean enabled;
	boolean level_end;
	short player_count;
	long team_score;
	real difficulty_multiplier;
	short record_count;
	short event_count;
	short slots[NETWORK_CAMPAIGN_SCORE_PLAYERS_PER_MESSAGE];
	struct campaign_score_player players[NETWORK_CAMPAIGN_SCORE_PLAYERS_PER_MESSAGE];
	struct campaign_score_event events[NETWORK_CAMPAIGN_SCORE_EVENTS_PER_MESSAGE];
};

/* ---------- prototypes/NETWORK_CAMPAIGN_SCORE.C */

/* campaign_score.c: a map began (no client has had its word) */
void network_campaign_score_new_map(void);
/* campaign_score.c, the co-op host: its word to every OpenH1 client,
reliably: the state's header, the players changed (dirty; all of them with
whole, and to a client that had none of this map's), the events of the
ring (ring_size of them) from first to end, the sequence past the last;
level_end on the last message */
void network_campaign_score_send(struct campaign_score_state const *state, boolean const *dirty, boolean whole,
	boolean level_end, struct campaign_score_event const *ring, long ring_size, long first, long end);
/* the wire's: a message's body (after its distributed header) packed into
buffer (its size), its length returned (0: too small); and read back
(FALSE: not one of this format, or of a wrong size) */
word network_campaign_score_pack(byte *buffer, word size, struct network_campaign_score_contents const *message);
boolean network_campaign_score_unpack(byte const *buffer, word size, struct network_campaign_score_contents *message);
/* a client: the host's message (network_distributed.c) */
void network_campaign_score_handle_message(long machine_index, word const *message, word size, boolean from_stream);

#endif
