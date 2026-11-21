"""Exercise the production dispatcher up to its handlers across world boundaries, through its one world gate
(distributed_world_gate): the epoch, the sender's clock, staleness by kind or by thing."""
import re
from pathlib import Path

import pytest
from harness import enum_with, function, structure
from test_hud_button_style import run_c

ROOT = Path(__file__).resolve().parents[1]


def generated(checkpoint_reset=None):
    source = (ROOT / "port/linux/game/network_distributed.c").read_text()
    header = (ROOT / "port/linux/game/network_distributed.h").read_text()
    enum_start = header.index("enum\n{")
    kinds = header[enum_start:header.index("\n};", enum_start) + 3]
    dispatch = function(source, "network_distributed_handle_message")
    # Preserve all real framing, batch recursion, source, epoch and stale checks.
    # Only the final gameplay handlers are replaced by observable deliveries.
    dispatch = dispatch[:dispatch.rindex("\tswitch (header.type)")] + \
        "\tif (world_kind(header.type)) { delivered[header.type]++; return; }\n" \
        "\tdelivered[header.type]++;delivered_count[header.type]=header.count;\n" \
        "\tif (header.count) memcpy(&delivered_key[header.type],entries,4);\n}\n"
    prefix = r'''
#include <assert.h>
#include <string.h>
#include <limits.h>
typedef unsigned char byte;
typedef unsigned short word, message_header;
typedef int boolean;
typedef float real;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define csmemcpy memcpy
#define csmemset memset
#define HALO_PORT_MAXIMUM_NETWORK_MACHINES 4
#define MAXIMUM_SENDERS 5
#define HOST_SENDER 4
#define DATAGRAM_MAXIMUM_SIZE 1200
#define DISTRIBUTED_TIME_WINDOW_TICKS 450
#define DISTRIBUTED_TICK_LEAD_TICKS 30
#define MAXIMUM_MESSAGE_SIZE 0x400
#define NUMBEROF(a) ((long)(sizeof(a)/sizeof((a)[0])))
#define csmemmove memmove
#define DISTRIBUTED_UNIT_STATE_MINIMUM_SIZE 1
#define DISTRIBUTED_RELAYED_ACTION_MINIMUM_SIZE 1
#define DISTRIBUTED_FORWARDED_INPUT_MINIMUM_SIZE 1
#define _game_connection_network_client 1
#define _game_connection_network_server 2
#define _error_silent 0
struct distributed_player_statistics {byte data;};
struct distributed_player_ping {byte data;};
struct distributed_structure_bsp {byte data;};
struct distributed_pickup {byte data;};
struct distributed_player_input {byte data;};
struct distributed_player_held {byte data;};
struct distributed_substep_request {byte data;};
struct distributed_client_identity {byte data;};
struct distributed_client_build {byte data;};
static int connection=1, unreliable, in_game=1, mcc=1, resets, clock_notes;
static int delivered[256];
/* the entries each delivery handed on, and the first's key (a keyed kind's: 4 bytes) */
static int delivered_count[256];
static long delivered_key[256];
static long now_tick=100;
static long game_time_get(void){return now_tick;}
static int distributed_handling_batch, distributed_handling_stream_message;
static unsigned long distributed_checkpoint_epoch, distributed_host_epoch;
static boolean distributed_checkpoint_synchronized;
static boolean distributed_identity_sent=1, distributed_build_sent=1;
static long distributed_host_time=NONE;
static unsigned long distributed_host_time_received;
static struct {int received,sent;} distributed_statistics;
static struct {unsigned long heard;} distributed_round_trips[4];
static int game_connection(void){return connection;}
static int game_in_progress(void){return in_game;}
static int mcc_cache_tags_loaded(void){return mcc;}
static int network_connection_last_read_was_unreliable(void){return unreliable;}
static unsigned long system_milliseconds(void){return 1000;}
static void distributed_restore_checkpoint(void){resets++;}
static void network_objects_discard_client_effects(void){}
static void network_damage_new_game(void){}
static void distributed_note_client_clock(long m,long t){(void)m;(void)t;clock_notes++;}
static int distributed_machine_clock_fast(long m){(void)m;return 0;}
static void error(int level,char const *fmt,...){(void)level;(void)fmt;}
/* (a GOAT H1 Standard game's messages that only some machines are sent:
   network_standard.c's; every one goes to everyone in a Classic game) */
static boolean network_standard_message_goes_to(long machine,byte type){(void)machine;(void)type;return TRUE;}
static void network_standard_message_heard(byte type){(void)type;}
static int world_kind(byte type);
/* (an OpenH1 host's packed objects' states: none read here, network_objects.c's) */
#define NETWORK_OBJECT_STATE_SIZE 44
static word network_objects_unpack_states(void const *m,word s,void *u,word mx){(void)m;(void)s;(void)u;(void)mx;return 0;}
#define DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE 128
static word distributed_unit_differences_unjoin(void const *m,word s,void *u,word mx){(void)m;(void)s;(void)u;(void)mx;return 0;}
#define DISTRIBUTED_RELAYED_ACTION_MAXIMUM_SIZE 20
static word distributed_relayed_actions_unpack(void const *m,word s,void *u,word mx){(void)m;(void)s;(void)u;(void)mx;return 0;}
'''
    sizes = sorted(set(re.findall(r"\b(\w+_entry_size|\w+_minimum_size)\(", dispatch)) - {"network_objects_entry_size"})
    prefix += "\n".join(f"#define {name}(...) 1" for name in sizes) + "\n"
    # (the keyed kinds' entries begin with the thing's key: a long, as the inventories' unit and the objects')
    prefix += "#define network_objects_entry_size(type) ((type)==_distributed_message_inventories || " \
              "(type)==_distributed_message_object_states ? 4 : 1)\n"
    # the other modules' messages (chat, voice, map sharing...), each claimed
    # by its own module: none of them here; OpenH1's own of the game world
    # (sub-steps, hijacks, the clock, pings, the host's team moves), seen
    # delivered as the dispatcher's own are, wherever it hands them on
    world = {"network_substeps": ("player_substeps", "relayed_substeps", "player_live_controls",
                                  "forwarded_live_controls"),
             "network_standard": ("opence_players",), "network_host_controls": ("host_team", "host_teleport"),
             "network_hijack": ("vehicle_hijack",), "network_clock": ("clock_phase",),
             "network_markers": ("marker_up", "marker_down")}
    hooks = sorted(set(re.findall(r"\b(\w+_handles_(?:lobby_)?message)\(", dispatch)))
    for name in hooks:
        module = name[:-len("_handles_message")]
        if module in world:
            test = " || ".join(f"t==_distributed_message_{kind}" for kind in world[module])
            prefix += f"#define {name}(m,s) ((s)>=12 && ((t=((byte const *)(m))[2]),({test})))\n"
        else:
            prefix += f"#define {name}(...) 0\n"
    prefix += "static byte t;\n"
    handlers = sorted(set(re.findall(r"\b(\w+_handle_(?:lobby_)?message)\(", dispatch)) -
                      {"network_distributed_handle_message"})
    for name in handlers:
        if name[:-len("_handle_message")] in world:
            prefix += f"#define {name}(machine,m,...) (delivered[((byte const *)(m))[2]]++)\n"
        else:
            prefix += f"#define {name}(...) ((void)0)\n"
    if "distributed_keyed_reset" in source:
        prefix += "#define KEYED 1\n"
    code = prefix + kinds + structure(header, "distributed_message_header")
    compact = re.search(r"#define COMPACT_BATCH_HEADER_SIZE [^\n]*\n", source)
    if compact:
        code += "\n" + compact.group(0) + "void network_distributed_handle_message(long, word const *, word);\n"
    code += r'''
static long distributed_sender_offsets[MAXIMUM_SENDERS];
static long distributed_sender_times[MAXIMUM_SENDERS];
static long distributed_received_times[MAXIMUM_SENDERS][NUMBER_OF_DISTRIBUTED_MESSAGES];
static int distributed_tick_stuck(long last,long tick,long now)
{return last>now+DISTRIBUTED_TICK_LEAD_TICKS && tick<=now+DISTRIBUTED_TICK_LEAD_TICKS;}
'''
    # (the keyed kinds' table and each thing's latest: distributed_keyed_fresh)
    keyed = re.search(r"enum\n\{\n\tKEYED_FRESHNESS_SLOTS.*?\} distributed_keyed_times\[[^;]*;", source, re.S)
    if keyed:
        code += keyed.group(0) + "\n" + function(source, "distributed_keyed_reset")
    code += checkpoint_reset or "static void distributed_reset_checkpoint_state(boolean receive){(void)receive;}\n"
    for name in ("distributed_advance_checkpoint", "distributed_message_epoch_valid",
                 "distributed_sender_clock", "distributed_message_time_bounded", "distributed_keyed_kind",
                 "distributed_keyed_fresh", "distributed_message_stale", "distributed_world_side_kind",
                 "distributed_world_gate", "distributed_compact_batch_handle"):
        try:
            code += function(source, name)
        except LookupError:
            pass  # (an older dispatcher's: no world gate of its own)
    code += dispatch
    code += r'''
static void send(long machine,byte type,unsigned long epoch,long time,byte count,word size){
 union {word alignment;byte bytes[128];} message;
 struct distributed_message_header h;
 assert(size<=sizeof(message));memset(&message,0,sizeof(message));memset(&h,0,sizeof(h));
 h.type=type;h.count=count;h.checkpoint_epoch=epoch;h.game_time=time;
 memcpy(message.bytes,&h,sizeof(h));network_distributed_handle_message(machine,(word *)message.bytes,size);
}
static void batch(byte type,unsigned long epoch,long time){
 union {word alignment;byte bytes[128];} message;
 struct distributed_message_header outer,inner;word length;
 memset(&message,0,sizeof(message));memset(&outer,0,sizeof(outer));memset(&inner,0,sizeof(inner));
 outer.type=_distributed_message_batch;outer.checkpoint_epoch=epoch;outer.game_time=time;
 inner.type=type;inner.checkpoint_epoch=epoch;inner.game_time=time;
 length=sizeof(inner)-sizeof(message_header);
 memcpy(message.bytes,&outer,sizeof(outer));memcpy(message.bytes+sizeof(outer),&length,sizeof(length));
 memcpy(message.bytes+sizeof(outer)+sizeof(length),(byte *)&inner+sizeof(message_header),length);
 network_distributed_handle_message(NONE,(word *)message.bytes,sizeof(outer)+sizeof(length)+length);
}
/* (a compact batch's: its world's low byte, its tick's low 16 bits) */
static void compact(byte type,unsigned long epoch,long time){
 byte message[16];memset(message,0,sizeof(message));
 message[2]=_distributed_message_compact_batch;message[3]=(byte)epoch;message[4]=(byte)time;message[5]=(byte)(time>>8);
 message[6]=0;message[7]=type;message[8]=0;
 unreliable=1;network_distributed_handle_message(NONE,(word *)message,9);unreliable=0;
}
static void setup(void){
 memset(distributed_sender_times,255,sizeof(distributed_sender_times));
 memset(distributed_received_times,255,sizeof(distributed_received_times));
#ifdef KEYED
 distributed_keyed_reset();
#endif
}
/* a message of a keyed kind (4-byte entries): its things' keys */
static void send_keyed(long machine,byte type,unsigned long epoch,long time,int count,long const *keys){
 union {word alignment;byte bytes[128];} message;
 struct distributed_message_header h;int i;
 memset(&message,0,sizeof(message));memset(&h,0,sizeof(h));
 h.type=type;h.count=(byte)count;h.checkpoint_epoch=epoch;h.game_time=time;
 memcpy(message.bytes,&h,sizeof(h));
 for(i=0;i<count;i++)memcpy(message.bytes+sizeof(h)+4*i,&keys[i],4);
 network_distributed_handle_message(machine,(word *)message.bytes,(word)(sizeof(h)+4*count));
}
static word sized(byte type){return 12+(type==_distributed_message_inventories||type==_distributed_message_object_states?4:1);}
static int world_kind(byte type){
 switch(type){
 case _distributed_message_player_substeps:case _distributed_message_relayed_substeps:
 case _distributed_message_player_live_controls:case _distributed_message_forwarded_live_controls:
 case _distributed_message_opence_players:case _distributed_message_host_team:case _distributed_message_host_teleport:
 case _distributed_message_vehicle_hijack:case _distributed_message_clock_phase:
 case _distributed_message_marker_up:case _distributed_message_marker_down:return 1;}
 return 0;
}
'''
    return code


@pytest.mark.parametrize("case", ["barrier", "late_packets", "ready", "batch", "clock", "rollover",
                                  "first_datagram", "stream_snapshot", "keyed_reorder", "world_epoch", "legit_window",
                                  "compact_batch"])
def test_epoch_dispatch(tmp_path, case):
    cases = {
        "barrier": r'''
 int i;unsigned long epoch;
 for(i=0;i<sizeof(struct distributed_message_header);i++)
  send(NONE,_distributed_message_checkpoint,1,100,0,i);
 assert(!resets);
 unreliable=1;send(NONE,_distributed_message_checkpoint,1,100,0,12);assert(!resets);unreliable=0;
 send(NONE,_distributed_message_checkpoint,0,100,0,12);
 send(NONE,_distributed_message_checkpoint,1,100,1,12);
 send(NONE,_distributed_message_checkpoint,1,100,0,13);
 assert(!resets);
 connection=2;distributed_handling_stream_message=1;
 send(0,_distributed_message_checkpoint,1,100,0,12);assert(!resets);
 connection=1;distributed_handling_stream_message=0;
 send(NONE,_distributed_message_checkpoint,1,100,0,12);
 assert(resets==1 && distributed_checkpoint_epoch==1);
 assert(!distributed_identity_sent && !distributed_build_sent);
 send(NONE,_distributed_message_checkpoint,1,101,0,12);assert(resets==1);
 send(NONE,_distributed_message_checkpoint,2,101,0,12);assert(resets==2);
 for(epoch=0;epoch<3;epoch++)send(NONE,_distributed_message_checkpoint,epoch,102,0,12);
 assert(resets==2 && distributed_checkpoint_epoch==2);
''',
        "late_packets": r'''
 int i;byte types[]={_distributed_message_damage_events,_distributed_message_object_changes,
 _distributed_message_unit_states,_distributed_message_inventories,_distributed_message_object_states,
 _distributed_message_actor_states,_distributed_message_damage_animations,_distributed_message_coop_events,
 _distributed_message_mcc_grenades,_distributed_message_mcc_object_names};
 send(NONE,_distributed_message_checkpoint,1,100,0,12);
 send(NONE,_distributed_message_objects_synchronized,1,100,0,12);
 for(i=0;i<sizeof(types);i++)send(NONE,types[i],1,100,1,sized(types[i]));
 send(NONE,_distributed_message_checkpoint,2,100,0,12); /* Same host tick, new world. */
 send(NONE,_distributed_message_objects_synchronized,2,100,0,12);
 for(i=0;i<sizeof(types);i++){
  assert(delivered[types[i]]==1);
  send(NONE,types[i],1,101,1,sized(types[i])); /* Old world, newer timestamp. */
  unreliable=1;send(NONE,types[i],1,100,1,sized(types[i]));unreliable=0;
  assert(delivered[types[i]]==1);
  send(NONE,types[i],2,101,1,sized(types[i]));assert(delivered[types[i]]==2);
 }
''',
        "ready": r'''
 connection=2;distributed_checkpoint_epoch=4;
 send(0,_distributed_message_client_ready,0,100,0,12);assert(!delivered[_distributed_message_client_ready]);
 distributed_handling_stream_message=1;
 send(0,_distributed_message_client_ready,0,100,0,12);
 send(0,_distributed_message_client_ready,1,100,1,12);
 assert(delivered[_distributed_message_client_ready]==2 && !resets);
 assert(!clock_notes && distributed_sender_offsets[0]==0);
 send(0,_distributed_message_client_ready,4,100,2,12);
 send(0,_distributed_message_client_ready,4,100,0,13);
 send(4,_distributed_message_client_ready,4,100,0,12);
 assert(delivered[_distributed_message_client_ready]==2);
 send(0,_distributed_message_hit_reports,3,101,1,13);
 send(0,_distributed_message_player_inputs,3,101,1,13);
 assert(!delivered[_distributed_message_hit_reports] && !delivered[_distributed_message_player_inputs]);
 send(0,_distributed_message_hit_reports,4,101,1,13);assert(delivered[_distributed_message_hit_reports]==1);
''',
        "batch": r'''
 batch(_distributed_message_checkpoint,2,100);assert(!resets);
 batch(_distributed_message_damage_events,2,100);assert(!delivered[_distributed_message_damage_events]);
 send(NONE,_distributed_message_checkpoint,2,100,0,12);
 send(NONE,_distributed_message_object_changes,2,100,1,13);
 assert(delivered[_distributed_message_object_changes]==1);
 unreliable=1;send(NONE,_distributed_message_inventories,2,100,1,16);unreliable=0;
 assert(!delivered[_distributed_message_inventories]);
 send(NONE,_distributed_message_inventories,2,100,1,16);
 assert(delivered[_distributed_message_inventories]==1);
 batch(_distributed_message_damage_events,1,101);assert(!delivered[_distributed_message_damage_events]);
 batch(_distributed_message_damage_events,2,101);assert(!delivered[_distributed_message_damage_events]);
 batch(_distributed_message_objects_synchronized,2,101);assert(!distributed_checkpoint_synchronized);
 unreliable=1;send(NONE,_distributed_message_objects_synchronized,2,101,0,12);
 assert(!distributed_checkpoint_synchronized);unreliable=0;
 send(NONE,_distributed_message_objects_synchronized,2,101,1,12);
 send(NONE,_distributed_message_objects_synchronized,2,101,0,13);assert(!distributed_checkpoint_synchronized);
 send(NONE,_distributed_message_objects_synchronized,2,101,0,12);
 unreliable=1;send(NONE,_distributed_message_object_changes,2,101,1,13);unreliable=0;
 assert(delivered[_distributed_message_object_changes]==1);
 batch(_distributed_message_damage_events,2,101);assert(delivered[_distributed_message_damage_events]==1);
 batch(_distributed_message_checkpoint,3,102);assert(resets==1 && distributed_checkpoint_epoch==2);
''',
        "compact_batch": r'''
 compact(_distributed_message_checkpoint,2,100);assert(!resets);
 compact(_distributed_message_damage_events,2,100);assert(!delivered[_distributed_message_damage_events]);
 send(NONE,_distributed_message_checkpoint,2,100,0,12);
 send(NONE,_distributed_message_objects_synchronized,2,100,0,12);
 compact(_distributed_message_damage_events,1,101);assert(!delivered[_distributed_message_damage_events]);
 compact(_distributed_message_damage_events,2,101);assert(delivered[_distributed_message_damage_events]==1);
 compact(_distributed_message_damage_events,3,102);assert(delivered[_distributed_message_damage_events]==1);
 compact(_distributed_message_objects_synchronized,2,102);
 compact(_distributed_message_checkpoint,3,102);assert(resets==1 && distributed_checkpoint_epoch==2);
 /* (a tick past the window, by its low 16 bits too) */
 compact(_distributed_message_damage_events,2,100+65536+1000);assert(delivered[_distributed_message_damage_events]==1);
 compact(_distributed_message_damage_events,2,103);assert(delivered[_distributed_message_damage_events]==2);
''',
        "clock": r'''
 send(NONE,_distributed_message_damage_events,3,LONG_MAX,0,12);
 assert(distributed_host_time==NONE && distributed_sender_offsets[HOST_SENDER]==0);
 send(NONE,_distributed_message_checkpoint,2,100,0,12);
 send(NONE,_distributed_message_objects_synchronized,2,100,0,12);
 send(NONE,_distributed_message_unit_states,2,100,0,12);
 send(NONE,_distributed_message_unit_states,1,LONG_MAX,0,12);
 assert(distributed_host_time==100 && distributed_sender_offsets[HOST_SENDER]==0);
 unreliable=1;send(NONE,_distributed_message_unit_states,2,101,0,12);
 assert(delivered[_distributed_message_unit_states]==2);
 send(NONE,_distributed_message_unit_states,2,100,0,12);unreliable=0;
 assert(delivered[_distributed_message_unit_states]==2); /* Ordinary stale checks retained. */
''',
        # after a reset (a client machine ready: its anchor cleared), a datagram stamped far off comes
        # first: it does not set the sender's clock, and the sender's real ones after it are taken; the sender's own
        # stream still moves it
        "first_datagram": r'''
 connection=2;distributed_checkpoint_epoch=2;distributed_handling_stream_message=0;
 send(0,_distributed_message_player_inputs,2,100+5000,0,12);
 send(0,_distributed_message_player_inputs,2,101,0,12);
 send(0,_distributed_message_player_inputs,2,102,0,12);
 assert(delivered[_distributed_message_player_inputs]==2);
 /* (a client: the host's, the same) */
 connection=1;distributed_checkpoint_epoch=0;
 send(NONE,_distributed_message_checkpoint,2,100,0,12);
 send(NONE,_distributed_message_objects_synchronized,2,100,0,12);
 unreliable=1;
 send(NONE,_distributed_message_unit_states,2,100+5000,0,12);
 assert(!delivered[_distributed_message_unit_states]);
 send(NONE,_distributed_message_unit_states,2,101,0,12);
 send(NONE,_distributed_message_unit_states,2,102,0,12);
 assert(delivered[_distributed_message_unit_states]==2 && distributed_host_time==102);
 /* (the sender's own stream, far off: its clock moved, its datagrams taken there) */
 unreliable=0;
 send(NONE,_distributed_message_unit_states,2,100+5000,0,12);
 assert(delivered[_distributed_message_unit_states]==3 && distributed_sender_offsets[HOST_SENDER]==5000);
 unreliable=1;send(NONE,_distributed_message_unit_states,2,100+5001,0,12);
 send(NONE,_distributed_message_unit_states,2,103,0,12);
 assert(delivered[_distributed_message_unit_states]==4);
''',
        # the ordered inventory snapshot after a revert or a join, overtaken by a newer datagram of one
        # unit: the snapshot's other units taken, the one it overtook left out
        "stream_snapshot": r'''
 long a=0x10001,b=0x20002,both[2];both[0]=a;both[1]=b;
 send(NONE,_distributed_message_checkpoint,2,100,0,12);
 send(NONE,_distributed_message_objects_synchronized,2,100,0,12);
 unreliable=1;send_keyed(NONE,_distributed_message_inventories,2,105,1,&a);unreliable=0;
 assert(delivered[_distributed_message_inventories]==1);
 send_keyed(NONE,_distributed_message_inventories,2,103,2,both);
 assert(delivered[_distributed_message_inventories]==2);
 assert(delivered_count[_distributed_message_inventories]==1 && delivered_key[_distributed_message_inventories]==b);
 /* (an older datagram of a unit after the snapshot: stale) */
 unreliable=1;send_keyed(NONE,_distributed_message_inventories,2,102,1,&b);
 assert(delivered[_distributed_message_inventories]==2);
''',
        # a partial inventory datagram reordered behind a newer one of another unit: taken; one of the
        # same unit: stale, as before
        "keyed_reorder": r'''
 long a=0x10001,b=0x20002,c=7;
 send(NONE,_distributed_message_checkpoint,2,100,0,12);
 send(NONE,_distributed_message_objects_synchronized,2,100,0,12);
 unreliable=1;
 send_keyed(NONE,_distributed_message_inventories,2,110,1,&a);
 send_keyed(NONE,_distributed_message_inventories,2,108,1,&b);
 assert(delivered[_distributed_message_inventories]==2 && delivered_key[_distributed_message_inventories]==b);
 send_keyed(NONE,_distributed_message_inventories,2,109,1,&a);
 assert(delivered[_distributed_message_inventories]==2);
 /* (a tick's several messages alike: taken) */
 send_keyed(NONE,_distributed_message_inventories,2,110,1,&a);
 assert(delivered[_distributed_message_inventories]==3);
 /* (the objects' states alike, by object) */
 send_keyed(NONE,_distributed_message_object_states,2,110,1,&a);
 send_keyed(NONE,_distributed_message_object_states,2,109,1,&c);
 send_keyed(NONE,_distributed_message_object_states,2,109,1,&a);
 assert(delivered[_distributed_message_object_states]==2);
 /* (a whole-state kind's: by the kind, as before) */
 send(NONE,_distributed_message_actor_states,2,110,1,13);
 send(NONE,_distributed_message_actor_states,2,109,1,13);
 assert(delivered[_distributed_message_actor_states]==1);
''',
        # OpenH1's own kinds of the world handled in their own files pass the same gate: another world's
        # (wrong epoch) not handed on, the current world's are; during a snapshot only the clock's and team moves
        "world_epoch": r'''
 int i;byte host_kinds[]={_distributed_message_relayed_substeps,_distributed_message_forwarded_live_controls,
 _distributed_message_vehicle_hijack,_distributed_message_marker_down,_distributed_message_host_teleport,
 _distributed_message_opence_players,_distributed_message_clock_phase,_distributed_message_host_team};
 byte client_kinds[]={_distributed_message_player_substeps,_distributed_message_player_live_controls,
 _distributed_message_marker_up};
 send(NONE,_distributed_message_checkpoint,2,100,0,12);
 send(NONE,_distributed_message_objects_synchronized,2,100,0,12);
 for(i=0;i<sizeof(host_kinds);i++){
  send(NONE,host_kinds[i],1,101,1,13);
  assert(!delivered[host_kinds[i]]);
  send(NONE,host_kinds[i],2,101,1,13);
  assert(delivered[host_kinds[i]]==1);
 }
 /* (a far-stamped datagram of them: dropped as the rest are) */
 unreliable=1;send(NONE,_distributed_message_relayed_substeps,2,100+5000,1,13);unreliable=0;
 assert(delivered[_distributed_message_relayed_substeps]==1);
 /* (a new world's snapshot under way: world kinds wait for its end) */
 send(NONE,_distributed_message_checkpoint,3,102,0,12);
 for(i=0;i<sizeof(host_kinds);i++)send(NONE,host_kinds[i],3,102,1,13);
 assert(delivered[_distributed_message_relayed_substeps]==1 && delivered[_distributed_message_vehicle_hijack]==1);
 assert(delivered[_distributed_message_clock_phase]==2 && delivered[_distributed_message_host_team]==2);
 /* (the host: a client machine's, of its world alone) */
 connection=2;distributed_checkpoint_epoch=3;distributed_handling_stream_message=1;
 for(i=0;i<sizeof(client_kinds);i++){
  send(0,client_kinds[i],2,102,1,13);
  assert(!delivered[client_kinds[i]]);
  send(0,client_kinds[i],3,102,1,13);
  assert(delivered[client_kinds[i]]==1);
 }
 /* (a host's kind from a client: not its) */
 send(0,_distributed_message_vehicle_hijack,3,102,1,13);
 assert(delivered[_distributed_message_vehicle_hijack]==1);
''',
        # (negative control) legitimate traffic: a client's clock leading the host's by its trip, trailing it by a
        # high ping or a second's hitch, the host's ticks in order, a stream message queued behind others
        "legit_window": r'''
 now_tick=1000;
 send(NONE,_distributed_message_checkpoint,2,1000,0,12);
 send(NONE,_distributed_message_objects_synchronized,2,1000,0,12);
 unreliable=1;
 send(NONE,_distributed_message_unit_states,2,1000-12,0,12);
 send(NONE,_distributed_message_unit_states,2,1000-11,0,12);
 now_tick=1030;
 send(NONE,_distributed_message_unit_states,2,1000+30,0,12);
 now_tick=1040;
 send(NONE,_distributed_message_unit_states,2,1040+60,0,12);
 send(NONE,_distributed_message_unit_states,2,1040+400,0,12);
 assert(delivered[_distributed_message_unit_states]==5);
 connection=2;distributed_handling_stream_message=0;
 now_tick=2000;
 send(0,_distributed_message_player_inputs,2,2000+12,0,12);
 send(0,_distributed_message_player_inputs,2,2000+13,0,12);
 send(0,_distributed_message_player_inputs,2,2000-60,0,12);
 assert(delivered[_distributed_message_player_inputs]==2);
''',
        "rollover": r'''
 distributed_host_epoch=ULONG_MAX-1;distributed_advance_checkpoint();assert(distributed_checkpoint_epoch==ULONG_MAX);
 distributed_advance_checkpoint();assert(distributed_checkpoint_epoch==1);
 distributed_checkpoint_epoch=ULONG_MAX;
 send(NONE,_distributed_message_checkpoint,1,100,0,12);assert(resets==1 && distributed_checkpoint_epoch==1);
 send(NONE,_distributed_message_checkpoint,ULONG_MAX,101,0,12);assert(resets==1);
''',
    }
    run_c(tmp_path, generated() + "\nint main(void){setup();\n" + cases[case] + "\nreturn 0;}\n")


def test_outgoing_snapshot_boundary(tmp_path):
    source = (ROOT / "port/linux/game/network_distributed.c").read_text()
    header = (ROOT / "port/linux/game/network_distributed.h").read_text()
    code = generated() + enum_with(header, "_distributed_to_clients")
    code += structure(source, "distributed_packer") + r'''
static int wire_calls;
static void build_message_header(message_header *h,word size,int type,int version){*h=size;(void)type;(void)version;}
static short distributed_client_machines(long *ids,short n){assert(n==4);ids[0]=1;return 1;}
static void distributed_batch_add(short sender,void const *message,word size,word entry){
 const struct distributed_message_header *h=message;
 (void)sender;(void)entry;assert(size>=12 && h->checkpoint_epoch==distributed_checkpoint_epoch);wire_calls++;
}
#define network_distributed_server_send_to_all_reliably(m,s) distributed_batch_add(0,m,s,0)
#define network_distributed_client_send_reliably(m,s) distributed_batch_add(HOST_SENDER,m,s,0)
#define network_distributed_server_send_to_machine_reliably(i,m,s) distributed_batch_add(i,m,s,0)
'''
    for name in ("distributed_fill_header", "distributed_entry_size", "distributed_send",
                 "distributed_send_to_machine", "distributed_send_to_machine_reliably",
                 "distributed_packer_flush"):
        code += function(source, name)
    code += r'''
int main(void){
 struct distributed_message_header message;
 struct distributed_packer packer;
 setup();memset(&message,0,sizeof(message));memset(&packer,0,sizeof(packer));
 distributed_checkpoint_epoch=2;
 distributed_send(&message,_distributed_message_hit_reports,0,sizeof(message),_distributed_to_host_reliably);
 distributed_send(&message,_distributed_message_player_inputs,0,sizeof(message),_distributed_to_host);
 assert(!wire_calls && !distributed_statistics.sent && !message.checkpoint_epoch);
 packer.sender=HOST_SENDER;packer.type=_distributed_message_player_prediction;
 packer.count=1;packer.prefix_size=4;packer.size=8;
 distributed_packer_flush(&packer);
 assert(!wire_calls && !packer.count && packer.size==4);
 distributed_send(&message,_distributed_message_client_ready,0,sizeof(message),_distributed_to_host_reliably);
 assert(wire_calls==1 && message.checkpoint_epoch==2);
 distributed_identity_sent=distributed_build_sent=TRUE;
 send(NONE,_distributed_message_objects_synchronized,2,100,0,12);
 assert(distributed_checkpoint_synchronized && !distributed_identity_sent && !distributed_build_sent);
 distributed_send(&message,_distributed_message_hit_reports,0,sizeof(message),_distributed_to_host_reliably);
 packer.count=1;packer.size=8;distributed_packer_flush(&packer);
 assert(wire_calls==3 && !packer.count && packer.size==4);
 connection=2;distributed_checkpoint_synchronized=FALSE;
 distributed_send_to_machine(1,&message,_distributed_message_unit_states,0,sizeof(message));
 distributed_send_to_machine_reliably(1,&message,_distributed_message_object_changes,0,sizeof(message));
 distributed_send(&message,_distributed_message_damage_events,0,sizeof(message),_distributed_to_clients);
 assert(wire_calls==6);
 connection=1;in_game=0;
 distributed_send(&message,_distributed_message_chat_up,0,sizeof(message),_distributed_to_host_reliably);
 assert(wire_calls==7);return 0;
}
'''
    run_c(tmp_path, code)


def test_snapshot_completion_retires_predictions_but_keeps_receive_history(tmp_path):
    source = (ROOT / "port/linux/game/network_distributed.c").read_text()
    # Bounded fake storage for the real reset and dispatcher; the separate
    # lifecycle fixture owns actual object/inventory and damage-queue behavior.
    storage = r'''
#define MAXIMUM_TRACKED_PLAYERS 2
#define MAXIMUM_LOCAL_PLAYERS 2
#define OWN_POSITION_TICKS 4
static long distributed_last_sent_time;
static struct {boolean valid;long dead_since;} distributed_deaths[2];
static int distributed_seat_disagreements[2];
static struct {boolean valid;long taken_air_unit_index,taken_air_vehicle;} distributed_accepted[2];
static struct {boolean valid;long taken_host_time;} distributed_predictions[2];
static struct {long unit_index;} distributed_host_speeds[2];
static int distributed_input_facings[2],distributed_seen[2],distributed_viewers[2];
static int distributed_sends[2],distributed_host_states[2],distributed_host_actions[2];
static int distributed_sent_statistics[2],distributed_statistics_cursor,distributed_statistics_due;
static int distributed_statistics_openh1_cursor;
static long distributed_statistics_repeat_times[2];
static long distributed_game_state_checksum,distributed_game_state_time,distributed_host_update_number;
static long distributed_sent_structure_bsp_index,distributed_pickup_count;
static struct {boolean valid;} distributed_machines;
static struct {long flags,unit_index,vehicle_index,seat_index;} distributed_sent_units[2];
static long distributed_player_machines[2];
static struct {long taken_time[2],taken_host_time[2],input_heard_time[2],input_time[2],ran_time[2],
 last_input_time[2];} distributed_divergence;
/* (our netcode's: the forwarded inputs, prediction history, host takes, and
   the GOAT H1 Standard protocol's difference and sub-step bases) */
#define REMOTE_POSITION_TICKS 2
#define MAXIMUM_SIM_SUBSTEPS 4
#define SUBSTEP_BASE_TICKS 2
#define UNIT_BASE_TICKS 2
#define UNIT_SENT_TICKS 2
static int distributed_predictions_heard[2],distributed_predictions_earlier[2];
static int distributed_forwards[2],distributed_forward_periods[2],distributed_far_live[2],distributed_relayed_sent[2];
static int distributed_forward_immediate[2],distributed_far_owed[2],distributed_budget_balances[2],distributed_budget_stretches[2];
static long distributed_forwarded_ticks[2],distributed_forwarded_times[2],distributed_host_run_times[2];
static struct {long last_run[2];int count;} distributed_host_takes;
static struct {long key,unit_index;} distributed_remote_positions[2][REMOTE_POSITION_TICKS],
 distributed_remote_substep_positions[2][REMOTE_POSITION_TICKS][MAXIMUM_SIM_SUBSTEPS-1];
static struct {long time,substep_index;} distributed_substep_states_taken[2];
static struct {long time;} distributed_taken_substep_bases[2][SUBSTEP_BASE_TICKS];
static long distributed_tick_state_times[2][UNIT_BASE_TICKS],distributed_unit_base_times[2][UNIT_BASE_TICKS];
static long distributed_unit_taken_time,distributed_unit_missing_time;
static boolean distributed_unit_bases_missing;
static struct {long time;} distributed_sent_substep_bases[HALO_PORT_MAXIMUM_NETWORK_MACHINES][2];
static struct {long time;} distributed_substep_sents[HALO_PORT_MAXIMUM_NETWORK_MACHINES][2];
/* (the sub-step states a client asks for: the host's windows, the client's wants) */
static long distributed_substep_requested_until[HALO_PORT_MAXIMUM_NETWORK_MACHINES][2];
static struct {long last_key,asked_time;boolean wrong;} distributed_substep_wants[2];
/* (the game type's state's changes, and the held inputs and predictions) */
static long distributed_game_state_sent_size;
static int distributed_game_state_machines[2],distributed_inputs_held_sent[2],distributed_predictions_held_sent[2];
static int distributed_held_inputs[2],distributed_held_predictions[2];
static long distributed_unit_sent_times[HALO_PORT_MAXIMUM_NETWORK_MACHINES][2][UNIT_SENT_TICKS];
static long distributed_unit_acked_times[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
static struct {long size;} distributed_batches[MAXIMUM_SENDERS];
static struct {long time,unit_index;} distributed_own_positions[2][4];
static int provisional_inputs;
static void update_queues_distributed_reset(void){provisional_inputs=0;}
'''
    code = generated(storage + function(source, "distributed_reset_checkpoint_state"))
    code += r'''
static void provisional(void){
 int i,j;
 provisional_inputs=3;distributed_last_sent_time=111;distributed_pickup_count=3;
 distributed_machines.valid=TRUE;
 for(i=0;i<2;i++){
  distributed_predictions[i].valid=TRUE;distributed_predictions[i].taken_host_time=111;
  distributed_host_states[i]=9;distributed_accepted[i].valid=TRUE;distributed_sends[i]=9;
  distributed_accepted[i].taken_air_unit_index=77;distributed_accepted[i].taken_air_vehicle=88;
  distributed_divergence.input_time[i]=111;distributed_sent_units[i].unit_index=77;
  distributed_substep_states_taken[i].time=111;distributed_unit_base_times[i][0]=111;
  for(j=0;j<4;j++){distributed_own_positions[i][j].time=111;distributed_own_positions[i][j].unit_index=77;}
 }
 for(i=0;i<MAXIMUM_SENDERS;i++)distributed_batches[i].size=17;
}
int main(void){
 int i,j;long received,sender;
 setup();send(NONE,_distributed_message_checkpoint,2,100,0,12);
 send(NONE,_distributed_message_inventories,2,100,1,16);
 assert(delivered[_distributed_message_inventories]==1);
 received=distributed_keyed_times[0][0].time;
 sender=distributed_sender_offsets[HOST_SENDER];assert(received==100);
 provisional();
 send(NONE,_distributed_message_objects_synchronized,2,100,1,12);
 assert(!distributed_checkpoint_synchronized && provisional_inputs==3);
 send(NONE,_distributed_message_objects_synchronized,2,100,0,12);
 assert(distributed_checkpoint_synchronized && !provisional_inputs && !distributed_pickup_count);
 assert(distributed_last_sent_time==NONE && !distributed_machines.valid);
 for(i=0;i<2;i++){
  assert(!distributed_predictions[i].valid && distributed_predictions[i].taken_host_time==NONE);
  assert(!distributed_host_states[i] && !distributed_accepted[i].valid && !distributed_sends[i]);
  assert(distributed_accepted[i].taken_air_unit_index==NONE && distributed_accepted[i].taken_air_vehicle==NONE);
  assert(distributed_divergence.input_time[i]==NONE && distributed_sent_units[i].unit_index==NONE);
  assert(distributed_substep_states_taken[i].time==NONE && distributed_unit_base_times[i][0]==NONE);
  for(j=0;j<4;j++)assert(distributed_own_positions[i][j].time==NONE && distributed_own_positions[i][j].unit_index==NONE);
 }
 for(i=0;i<MAXIMUM_SENDERS;i++)assert(!distributed_batches[i].size);
 assert(distributed_keyed_times[0][0].time==received);
 assert(distributed_sender_offsets[HOST_SENDER]==sender);
 unreliable=1;send(NONE,_distributed_message_inventories,2,99,1,16);unreliable=0;
 assert(delivered[_distributed_message_inventories]==1); /* Snapshot remains newer. */
 send(NONE,_distributed_message_inventories,2,101,1,16);
 assert(delivered[_distributed_message_inventories]==2);
 provisional();send(NONE,_distributed_message_objects_synchronized,2,101,0,12);
 assert(provisional_inputs==3 && distributed_own_positions[0][0].time==111);
 assert(distributed_batches[HOST_SENDER].size==17); /* Duplicate marker preserves current work. */
 distributed_reset_checkpoint_state(TRUE);
 assert(distributed_keyed_times[0][0].time==NONE);
 for(i=0;i<MAXIMUM_SENDERS;i++){
  assert(distributed_sender_offsets[i]==0);
  for(j=0;j<NUMBER_OF_DISTRIBUTED_MESSAGES;j++)assert(distributed_received_times[i][j]==NONE);
 }
 return 0;
}
'''
    run_c(tmp_path, code)
