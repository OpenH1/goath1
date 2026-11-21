/* The host's relayed input to an OpenH1 client packed
(_distributed_message_relayed_actions_packed: distributed_relayed_action_add,
distributed_relayed_action_pack): each entry without its parts at their
usual values, its player a bit where it is the one after the entry before's
in its message. The client makes each message back into the message of the
entries whole (distributed_relayed_actions_unpack): every byte as the whole
message (_distributed_message_relayed_actions) would have been, a message
split where it fills (its first entry's player whole), and a message cut
short makes none. The batch takes messages of ROOM bytes past the header. */
#include "harness.h"
typedef unsigned short word;
typedef word message_header;
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
#define csmemset memset
#define csmemcpy memcpy
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
enum { DATAGRAM_MAXIMUM_SIZE = 1200, MAXIMUM_PLAYERS = 16, MAXIMUM_MESSAGES = 64 };
#include "config.inc"

/* the batch: the messages sent, each its bytes */
static word room = DATAGRAM_MAXIMUM_SIZE;
static byte messages[MAXIMUM_MESSAGES][sizeof(struct distributed_message_header) + DATAGRAM_MAXIMUM_SIZE];
static word message_sizes[MAXIMUM_MESSAGES];
static short message_count;

static word distributed_batch_room(short sender, word minimum) { (void)sender; (void)minimum; return room; }
static boolean distributed_fill_header(void *message, byte type, short count, word size)
{
	struct distributed_message_header *header = (struct distributed_message_header *)message;

	header->header = size;
	header->type = type;
	header->count = (byte)count;
	header->game_time = 1234;
	header->checkpoint_epoch = 7;
	return TRUE;
}
static void distributed_batch_add(short sender, void const *message, word size, word entry_size)
{
	(void)sender;
	(void)entry_size;
	CHECK(message_count < MAXIMUM_MESSAGES, "too many messages");
	memcpy(messages[message_count], message, size);
	message_sizes[message_count++] = size;
}
#include "under_test.inc"

static struct distributed_packer packer;
static long update_number = 0x01020304;

/* the players' actions: a bot's (no choice, trigger or buttons), and every
part used */
static struct distributed_relayed_action bot(byte player_index, short yaw)
{
	struct distributed_relayed_action action;
	short history;

	memset(&action, 0, sizeof(action));
	action.player_index = player_index;
	action.desired_weapon_index = NONE;
	action.desired_grenade_index = NONE;
	action.desired_zoom_level = NONE;
	action.yaw = yaw;
	action.pitch = (short)-yaw / 3;
	action.throttle_i = 127;
	action.throttle_j = -40;
	for (history = 0; history < DISTRIBUTED_INPUT_HISTORY; history++)
		action.control_flags[history] = 0;
	return action;
}

static struct distributed_relayed_action busy(byte player_index)
{
	struct distributed_relayed_action action = bot(player_index, -32768);

	action.desired_weapon_index = 1;
	action.desired_grenade_index = 0;
	action.desired_zoom_level = 2;
	action.throttle_i = 0;
	action.throttle_j = 0;
	action.primary_trigger = 255;
	action.control_flags[0] = 0x8001;
	action.control_flags[1] = 0x8001;
	action.control_flags[2] = 0x0040;
	action.control_flags[3] = 0;
	return action;
}

/* the actions sent, packed or whole, as the messages the batch took */
static short send(struct distributed_relayed_action const *actions, short count, boolean packed)
{
	short before = NONE;
	short index;

	message_count = 0;
	distributed_packer_begin(&packer, 0, packed ? _distributed_message_relayed_actions_packed :
		_distributed_message_relayed_actions, &update_number, sizeof(update_number));
	for (index = 0; index < count; index++)
		distributed_relayed_action_add(&packer, &actions[index], packed, &before);
	distributed_packer_flush(&packer);
	return message_count;
}

/* each packed message made back: the same bytes as the whole message of
the same entries, past the header's type and size, which the whole would
have of its own */
static void check_same(struct distributed_relayed_action const *actions, short count, word packed_bytes_most)
{
	static byte whole[MAXIMUM_MESSAGES][sizeof(struct distributed_message_header) + DATAGRAM_MAXIMUM_SIZE];
	static word whole_sizes[MAXIMUM_MESSAGES];
	byte unpacked[sizeof(struct distributed_message_header) + 4 + 255 * DISTRIBUTED_RELAYED_ACTION_MAXIMUM_SIZE];
	short whole_count, packed_count, index, offset = 0;
	long packed_total = 0, whole_total = 0;

	whole_count = send(actions, count, FALSE);
	memcpy(whole, messages, sizeof(whole));
	memcpy(whole_sizes, message_sizes, sizeof(whole_sizes));
	packed_count = send(actions, count, TRUE);
	CHECK(packed_count >= 1 && packed_count <= whole_count, "%d packed messages, %d whole", packed_count, whole_count);
	for (index = 0; index < packed_count; index++)
	{
		struct distributed_message_header const *header = (struct distributed_message_header const *)messages[index];
		word size = distributed_relayed_actions_unpack(messages[index], message_sizes[index], unpacked,
			(word)sizeof(unpacked));
		struct distributed_message_header made;
		short entry;
		byte const *cursor;

		CHECK(header->type == _distributed_message_relayed_actions_packed, "a packed message's type %d", header->type);
		CHECK(size, "message %d made back", index);
		memcpy(&made, unpacked, sizeof(made));
		CHECK(made.type == _distributed_message_relayed_actions && made.count == header->count &&
			made.game_time == header->game_time && made.checkpoint_epoch == header->checkpoint_epoch,
			"message %d's header as it came, of the whole kind", index);
		/* (its entries, as the whole message of them only writes them) */
		cursor = unpacked + sizeof(made);
		CHECK(!memcmp(cursor, &update_number, sizeof(update_number)), "the update number");
		cursor += sizeof(update_number);
		for (entry = 0; entry < header->count; entry++)
		{
			byte written[DISTRIBUTED_RELAYED_ACTION_MAXIMUM_SIZE];
			word written_size = distributed_relayed_action_write(&actions[offset + entry], written);

			CHECK(!memcmp(cursor, written, written_size), "message %d entry %d: its bytes as the whole one's",
				index, entry);
			cursor += written_size;
		}
		CHECK(cursor == unpacked + size, "message %d: %ld bytes made, %u", index, (long)(cursor - unpacked), size);
		offset += header->count;
		packed_total += message_sizes[index];
		/* (cut short: none) */
		CHECK(!distributed_relayed_actions_unpack(messages[index], (word)(message_sizes[index] - 1), unpacked,
			(word)sizeof(unpacked)), "message %d cut short made none", index);
	}
	CHECK(offset == count, "%d entries sent of %d", offset, count);
	/* (and the whole messages, when every entry fits one, the same entries:
	the client takes them as it took those) */
	for (index = 0; index < whole_count; index++)
		whole_total += whole_sizes[index];
	CHECK(packed_total <= packed_bytes_most, "%ld bytes packed (whole %ld), at most %u", packed_total, whole_total,
		packed_bytes_most);
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	struct distributed_relayed_action actions[255];
	short index;
	word header = (word)(sizeof(struct distributed_message_header) + sizeof(update_number));

	if (!strcmp(name, "bots"))
	{
		/* (13 bots in a row: 8 bytes an entry, the first's player whole) */
		for (index = 0; index < 13; index++)
			actions[index] = bot((byte)index, (short)(index * 2500 - 15000));
		check_same(actions, 13, (word)(header + 13 * 7 + 1));
		return 0;
	}
	if (!strcmp(name, "mixed"))
	{
		/* (players apart, every part used, a stick at none, the first player 0) */
		actions[0] = busy(0);
		actions[1] = bot(3, 77);
		actions[1].throttle_i = actions[1].throttle_j = 0;
		actions[2] = busy(4);
		actions[2].desired_weapon_index = NONE;
		actions[2].desired_grenade_index = NONE;
		actions[3] = bot(5, 32767);
		actions[3].control_flags[3] = 0xFFFF;
		actions[4] = bot(15, -1);
		actions[4].desired_zoom_level = 0;
		check_same(actions, 5, (word)(header + 5 * DISTRIBUTED_RELAYED_ACTION_MAXIMUM_SIZE));
		return 0;
	}
	if (!strcmp(name, "split"))
	{
		/* (more than a message holds: split where it fills, each message's
		first entry with its player whole) */
		room = 4 + 5 * 8;
		for (index = 0; index < 40; index++)
			actions[index] = bot((byte)index, (short)index);
		check_same(actions, 40, (word)(40 * 8 + 8 * header + 8));
		CHECK(message_count >= 7, "%d messages", message_count);
		room = DATAGRAM_MAXIMUM_SIZE;
		return 0;
	}
	if (!strcmp(name, "first-unknown"))
	{
		/* (a message whose first entry says its player is the next one:
		none, there being no entry before) */
		byte message[64];
		byte unpacked[256];
		struct distributed_message_header *made = (struct distributed_message_header *)message;
		word size;

		actions[0] = bot(1, 10);
		send(actions, 1, TRUE);
		memcpy(message, messages[0], message_sizes[0]);
		size = message_sizes[0];
		CHECK(distributed_relayed_actions_unpack(message, size, unpacked, sizeof(unpacked)), "as sent, made back");
		/* (its player byte taken out, its bit set) */
		{
			byte *entry = message + sizeof(*made) + sizeof(update_number);

			entry[0] |= FLAG(_relayed_packed_next_player_bit);
			memmove(entry + 1, entry + 2, size - (word)(entry + 2 - message));
			size--;
		}
		CHECK(!distributed_relayed_actions_unpack(message, size, unpacked, sizeof(unpacked)),
			"a first entry after none made none");
		return 0;
	}
	fprintf(stderr, "no case %s\n", name);
	return 2;
}
