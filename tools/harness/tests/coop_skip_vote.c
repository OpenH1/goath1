/* The co-op cutscene skip vote (network_coop.c's host_count_skip_votes and
its answer, network_coop_skip_answered) with the host's reverts that carry it
out (main.c's main_skip_cinematic_private, main_coop_host_revert and the
failure reverts' limit, main_coop_host_failure_revert). Every outcome of a
passed vote, a refusal included, leaves a vote that can be held again; a skip
is no failure, and the failures' limit is the map's own. */
#include "harness.h"

#define NUMBEROF(a) (sizeof(a)/sizeof((a)[0]))
#define TICKS_PER_SECOND 30
#define HALO_PORT_MAXIMUM_NETWORK_MACHINES 4
enum {_error_silent,_error_log};
static int diagnostics;
#define error(...) (++diagnostics)
enum {_game_connection_local,_game_connection_network_client,_game_connection_network_server};
#include "types.inc"

/* the host's world: its clock, one client machine (1), the cutscene */
static long tick=1000;
static boolean cinematic=TRUE,skippable=TRUE,saving,saved_valid=TRUE;
static int reverts,reverted_notes;
static struct {boolean revert_map,skip_cinematic;} main_globals;
static long game_time_get(void){return tick;}
static boolean cinematic_in_progress(void){return cinematic;}
static boolean cinematic_can_be_skipped(void){return skippable;}
static boolean main_saving_map(void){return saving;}
static boolean game_state_port_saved_game_valid(void){return saved_valid;}
static void game_state_revert(void){reverts++;}
static void network_coop_reverted(long now){(void)now;reverted_notes++;}
static void ui_widgets_disable_pause_game(long ticks){(void)ticks;}
static short game_connection(void){return _game_connection_network_server;}
static boolean coop_host(void){return TRUE;}
static boolean main_coop_host(void){return TRUE;}
static short local_player_get_next(short index){return index==NONE?0:NONE;}
static short distributed_client_machines(long *machines,short maximum){(void)maximum;machines[0]=1;return 1;}
static void main_skip_cinematic(void){main_globals.skip_cinematic=TRUE;}
void network_coop_skip_answered(boolean skipped);

#include "under_test.inc"

/* both machines vote this tick */
static void vote(void)
{
 struct distributed_coop_skip_vote client={1};
 network_coop_handle_skip_vote(1,&client);
 skip_vote.voted=TRUE;
}
/* the host's tick, then its frame's skip if one was asked for */
static void tick_and_frame(void)
{
 host_count_skip_votes();
 if(main_globals.skip_cinematic)main_skip_cinematic_private();
 tick++;
}
/* the tick the vote is open again on (within a minute), else NONE */
static long tick_voting(void)
{
 long start=tick;
 while(tick-start<60*TICKS_PER_SECOND){
  long now=tick;
  tick_and_frame();
  if(skip_vote.state==_skip_vote_voting){CHECK(skip_vote.offered,"voting but not offered");return now;}
 }
 return NONE;
}

int main(int argc,char **argv)
{
 char const *case_name=argc>1?argv[1]:"";
 long answered;

 skip_vote_reset();
 main_coop_failure_reverts_reset();
 host_count_skip_votes();
 CHECK(skip_vote.offered&&skip_vote.state==_skip_vote_voting,"a skippable cutscene with its save not offered");
 CASE("refused") {
  /* passed, but the revert is refused (the save went away by the frame) */
  vote();host_count_skip_votes();
  CHECK(skip_vote.state==_skip_vote_requested&&main_globals.skip_cinematic,"2 of 2 votes did not pass");
  saved_valid=FALSE;main_skip_cinematic_private();answered=tick;tick++;
  CHECK(!reverts&&skip_vote.state==_skip_vote_answered,"a refused skip left the vote requested (state %d)",skip_vote.state);
  CHECK(!skip_vote.votes&&!skip_vote.voted,"a refused skip kept its votes");
  /* while there is nothing to revert to, nothing is offered */
  tick_and_frame();CHECK(!skip_vote.offered,"offered with no saved state");
  saved_valid=TRUE;
  /* offered again once the cooldown from the answer is over, voted and skipped */
  {
   long opened=tick_voting();
   CHECK(opened==answered+SKIP_COOLDOWN_TICKS,"after a refusal the vote opened at %ld, not %ld",opened,
    answered+SKIP_COOLDOWN_TICKS);
  }
  vote();tick_and_frame();
  CHECK(reverts==1&&skip_vote.state==_skip_vote_answered,"the vote after a refusal did not skip");
 } else CASE("unanswered") {
  /* passed, and main.c never ran the skip (its request cleared elsewhere) */
  vote();host_count_skip_votes();answered=tick;tick++;main_globals.skip_cinematic=FALSE;
  CHECK(skip_vote.state==_skip_vote_requested,"2 of 2 votes did not pass");
  {
   long opened=tick_voting();
   CHECK(opened!=NONE,"a skip never answered froze the vote");
   CHECK(opened<=answered+SKIP_ANSWER_TICKS+1+SKIP_COOLDOWN_TICKS,"the vote reopened late (%ld)",opened-answered);
  }
  vote();tick_and_frame();CHECK(reverts==1,"the vote after an unanswered skip did not skip");
 } else CASE("skips-not-failures") {
  /* three skips in a minute all skip, and the failure after them still reverts */
  int skip;
  for(skip=0;skip<3;skip++){
   CHECK(tick_voting()!=NONE,"skip %d not offered",skip+1);
   vote();tick_and_frame();CHECK(reverts==skip+1,"skip %d refused",skip+1);
  }
  CHECK(main_coop_host_failure_revert()&&reverts==4,"two skips made a mission failure respawn instead of revert");
 } else CASE("failures-then-skip") {
  /* two failures, then a skip, within a minute: the skip still skips */
  CHECK(main_coop_host_failure_revert()&&main_coop_host_failure_revert()&&reverts==2,"failure reverts refused");
  tick+=TICKS_PER_SECOND;vote();tick_and_frame();
  CHECK(reverts==3&&skip_vote.state==_skip_vote_answered,"two failures refused the skip after them");
 } else CASE("failure-loop") {
  /* a failure that holds: the third within a minute respawns; the next reverts */
  CHECK(main_coop_host_failure_revert()&&main_coop_host_failure_revert(),"failure reverts refused");
  tick+=MAIN_COOP_FAILURE_REVERT_WINDOW_TICKS-1;
  CHECK(!main_coop_host_failure_revert()&&reverts==2,"a third failure revert in a minute reverted");
  CHECK(main_coop_host_failure_revert()&&reverts==3,"the failure revert after a refused one refused");
  /* a minute apart, failures keep reverting */
  tick+=MAIN_COOP_FAILURE_REVERT_WINDOW_TICKS;CHECK(main_coop_host_failure_revert(),"refused a minute later");
  tick+=MAIN_COOP_FAILURE_REVERT_WINDOW_TICKS;CHECK(main_coop_host_failure_revert(),"refused a minute later");
  /* with nothing saved, no revert is counted */
  saved_valid=FALSE;tick+=10;CHECK(!main_coop_host_failure_revert()&&reverts==5,"reverted with no saved state");
 } else CASE("failure-new-map") {
  /* two failures, then the next map's first failure within a minute */
  CHECK(main_coop_host_failure_revert()&&main_coop_host_failure_revert(),"failure reverts refused");
  tick+=TICKS_PER_SECOND;main_coop_failure_reverts_reset();
  CHECK(main_coop_host_failure_revert()&&reverts==3,"the last map's failures refused the new map's revert");
  /* a clock that went back (a new game) never counts the old game's */
  main_coop_host_failure_revert();tick=10;
  CHECK(main_coop_host_failure_revert()&&reverts==5,"another game's failures counted");
 } else return 2;
 return 0;
}
