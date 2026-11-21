/* Co-op one-shot events (network_coop.c's event_new, host_send_events and
network_coop_handle_events) over a fake network that loses datagrams in a
burst and delays the reliable stream. An event lost on every datagram still
arrives, each is applied once and in order, and one that only happens at a
moment is not applied once it comes too late, while one that sets a state
always is. */
#include "harness.h"

typedef unsigned short word;
typedef unsigned short message_header;
#define MIN(a,b) ((a)<(b)?(a):(b))
#define csmemset memset
#define csmemcpy memcpy
#define TICKS_PER_SECOND 30
/* (a few to a message, so a tick's events take several) */
#define RELIABLE_ENTRIES(type) ((short)5)
enum {_error_silent};
static int diagnostics;
#define error(...) (++diagnostics)
enum {_distributed_to_clients,_distributed_to_clients_reliably,_distributed_to_host,_distributed_to_host_reliably};
enum {_distributed_message_coop_events=1};
struct distributed_message_header {message_header header;byte type,count;long game_time;unsigned long checkpoint_epoch;};
#include "types.inc"

/* the host's clock, and which role the one coop_events plays */
static long tick=100;
static boolean hosting=TRUE;
static boolean coop_host(void){return hosting;}
static boolean coop_client(void){return !hosting;}
static boolean distributed_real_valid(real value){return value==value;}

/* the network: the host's messages, each with its kind of delivery and when it went */
enum {MAXIMUM_WIRE=64};
static struct {short destination;long host_time;short count;struct distributed_coop_events_message message;} wire[MAXIMUM_WIRE];
static int wire_count,wire_next;
static void distributed_send(void *message,byte type,short count,word size,short destination)
{
 CHECK(type==_distributed_message_coop_events&&wire_count<MAXIMUM_WIRE,"unexpected message");
 CHECK(size==sizeof(struct distributed_message_header)+count*sizeof(struct distributed_coop_event),"size %u",size);
 CHECK(count>0&&count<=RELIABLE_ENTRIES(struct distributed_coop_event),"%d events in one message",count);
 wire[wire_count].destination=destination;wire[wire_count].host_time=tick;wire[wire_count].count=count;
 memcpy(&wire[wire_count].message,message,size);wire_count++;
}

/* the client: the newest host tick it has heard, and the events it applied */
static long latest_host_time=NONE;
static long distributed_latest_host_time(void){return latest_host_time;}
static struct {byte kind,type;word number;} applied[256];
static int applied_count;
static void client_apply_event(struct distributed_coop_event const *event)
{
 CHECK(applied_count<256,"too many applied");
 applied[applied_count].kind=event->kind;applied[applied_count].type=event->type;applied[applied_count].number=event->number;
 applied_count++;
}

#include "under_test.inc"

/* coop_events is the host's or the client's: each keeps its own */
static unsigned char host_events[sizeof(coop_events)],client_events[sizeof(coop_events)];
static void as_client(void){memcpy(host_events,&coop_events,sizeof(coop_events));memcpy(&coop_events,client_events,sizeof(coop_events));hosting=FALSE;}
static void as_host(void){memcpy(client_events,&coop_events,sizeof(coop_events));memcpy(&coop_events,host_events,sizeof(coop_events));hosting=TRUE;}

/* the host's tick ends: its events go */
static void host_tick_end(void){host_send_events();tick++;}
/* the network delivers what went before `before` (a datagram unless lost),
the client having heard the host's tick `heard` */
static void deliver(long before,boolean datagrams_lost,long heard)
{
 as_client();
 latest_host_time=heard;
 for(;wire_next<wire_count&&wire[wire_next].host_time<before;wire_next++){
  if(wire[wire_next].destination==_distributed_to_clients&&datagrams_lost)continue;
  network_coop_handle_events(wire[wire_next].message.events,wire[wire_next].count,wire[wire_next].host_time);
 }
 as_host();
}
static struct distributed_coop_event *queue(byte kind,byte type)
{
 struct distributed_coop_event *event=event_new(kind);
 CHECK(event!=NULL,"event_new refused");
 event->type=type;
 return event;
}
static boolean was_applied(byte kind,byte type)
{
 int index;
 for(index=0;index<applied_count;index++)if(applied[index].kind==kind&&applied[index].type==type)return TRUE;
 return FALSE;
}
static void check_order(void)
{
 int index;
 for(index=1;index<applied_count;index++)
  CHECK((short)(applied[index].number-applied[index-1].number)>0,"applied out of order or twice at %d",index);
}

int main(int argc,char **argv)
{
 char const *case_name=argc>1?argv[1]:"";

 memset(&coop_events,0,sizeof(coop_events));memset(client_events,0,sizeof(client_events));
 CASE("loss-burst") {
  /* a line of dialogue and a nav point, as a 100 ms (and longer) loss burst
  takes every datagram: they come over the stream, a few ticks late */
  queue(_coop_event_sound,_coop_sound_impulse);queue(_coop_event_nav_point,0);
  host_tick_end();host_tick_end();host_tick_end();host_tick_end();
  CHECK(!coop_events.count,"the host kept %d events after sending them",coop_events.count);
  deliver(tick,TRUE,tick+3);
  CHECK(applied_count==2&&applied[0].kind==_coop_event_sound&&applied[1].kind==_coop_event_nav_point,
   "events lost with every datagram (%d applied)",applied_count);
 } else CASE("late") {
  /* ten seconds late (a long stall): the moments are gone, the states apply, in order */
  queue(_coop_event_sound,_coop_sound_impulse);
  queue(_coop_event_title,0);
  queue(_coop_event_hud,_coop_hud_help_text);
  queue(_coop_event_hud,_coop_hud_checkpoint);
  queue(_coop_event_effect,0);
  queue(_coop_event_player_effect,_coop_player_effect_start);
  queue(_coop_event_hud,_coop_hud_objective);
  queue(_coop_event_nav_point,0);
  queue(_coop_event_unit_animation,0);
  queue(_coop_event_attach,0);
  queue(_coop_event_sound,_coop_sound_looping_start);
  queue(_coop_event_player_effect,_coop_player_effect_stop);
  queue(_coop_event_reverted,0);
  host_tick_end();
  deliver(tick,TRUE,tick+10*TICKS_PER_SECOND);
  CHECK(!was_applied(_coop_event_sound,_coop_sound_impulse),"a line of dialogue played 10 s late");
  CHECK(!was_applied(_coop_event_title,0)&&!was_applied(_coop_event_hud,_coop_hud_help_text)&&
   !was_applied(_coop_event_hud,_coop_hud_checkpoint),"text shown 10 s late");
  CHECK(!was_applied(_coop_event_effect,0)&&!was_applied(_coop_event_player_effect,_coop_player_effect_start),
   "an effect started 10 s late");
  CHECK(applied_count==7,"%d of the 7 states applied",applied_count);
  check_order();
  /* the next, on time, applies; the late ones are not applied again */
  wire_next=0;queue(_coop_event_sound,_coop_sound_impulse);host_tick_end();
  deliver(tick,TRUE,tick);
  CHECK(applied_count==8&&applied[7].kind==_coop_event_sound,"the next event after late ones (%d)",applied_count);
  check_order();
 } else CASE("bound") {
  /* dialogue at its bound applies, a tick past it does not */
  queue(_coop_event_sound,_coop_sound_impulse);host_tick_end();
  deliver(tick,FALSE,tick-1+EVENT_LATE_SOUND_TICKS);
  CHECK(applied_count==1,"dialogue at its bound not applied");
  queue(_coop_event_sound,_coop_sound_impulse);host_tick_end();
  deliver(tick,FALSE,tick+EVENT_LATE_SOUND_TICKS);
  CHECK(applied_count==1,"dialogue past its bound applied");
 } else CASE("chunks") {
  /* more events in a tick than a message holds: all go, in order */
  int index;
  for(index=0;index<13;index++)queue(_coop_event_nav_point,0);
  host_tick_end();
  CHECK(wire_count==3&&!coop_events.count,"13 events in %d messages, %d kept",wire_count,coop_events.count);
  deliver(tick,TRUE,tick);
  CHECK(applied_count==13,"%d of 13 applied",applied_count);
  check_order();
 } else CASE("old-host") {
  /* an older host sends each event in three ticks' datagrams: each applied once */
  int copy;
  queue(_coop_event_nav_point,0);queue(_coop_event_sound,_coop_sound_impulse);
  as_client();
  for(copy=0;copy<3;copy++){
   latest_host_time=tick+copy;
   network_coop_handle_events(((struct distributed_coop_event *)host_events),2,tick+copy);
  }
  as_host();
  CHECK(applied_count==2,"an older host's resends applied %d times",applied_count);
 } else return 2;
 return 0;
}
