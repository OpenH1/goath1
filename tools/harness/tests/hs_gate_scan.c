/* hs_runtime.c's gate scan over scripts built here, node by node (test_hs_gate_scan.py) */
#include "harness.h"

typedef unsigned short word;
#define NUMBEROF(a) (sizeof(a) / sizeof((a)[0]))
#define csstrncmp strncmp
#define csstrlen strlen
#include "types.inc"

/* the functions: the language's own (hs.h's), then the game's, by name */
enum { AI_RETREAT = 100, DEVICE_SET_POSITION, GAME_SAVE, OBJECT_TELEPORT, AI_LIVING_COUNT, VOLUME_TEST_ALL,
	PLAYERS, SOUND_IMPULSE_START, PRINT, UNNAMED, FUNCTION_COUNT };
struct hs_function_definition { char const *name; };
static struct hs_function_definition functions[FUNCTION_COUNT];
static long const hs_function_table_count = FUNCTION_COUNT;
static struct hs_function_definition *mcc_script_function(short index) { return NULL; }
static struct hs_function_definition *hs_function_get(short index)
{
	CHECK(index >= 0 && index < FUNCTION_COUNT, "function %d past the table", index);
	return &functions[index];
}

static struct hs_syntax_node nodes[8192];
static long node_count;
static struct hs_syntax_node *syntax_get(long index)
{
	CHECK(index >= 0 && index < node_count, "node %ld", index);
	return &nodes[index];
}
#define hs_syntax_get syntax_get

static struct hs_script scripts[4];
static struct scenario { struct { long count; } hs_scripts; } scenario = { { 0 } };
static struct scenario *global_scenario_get(void) { return &scenario; }
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&scripts[(index)])

static struct hs_runtime_globals hs_runtime_globals;
static struct hs_thread_datum thread;
static struct hs_thread_datum *hs_thread_get(long index) { return &thread; }
static boolean waiting = TRUE;
static boolean hs_runtime_waiting_on_call(void) { return waiting; }

#include "scan.inc"

static long node(short flags)
{
	long index = node_count++;

	CHECK(index < (long)NUMBEROF(nodes), "nodes");
	memset(&nodes[index], 0, sizeof(nodes[index]));
	nodes[index].flags = flags;
	nodes[index].next_node_index = NONE;
	return index;
}

static long primitive(void) { return node(FLAG(_hs_syntax_node_primitive_bit)); }

/* a call: its name, then its arguments (NONE ends them) */
static long call_list(short function_index, long const *arguments)
{
	long call = node(0), name = primitive(), last = name;

	nodes[call].function_index = function_index;
	nodes[call].data = name;
	for (; *arguments != NONE; arguments++)
	{
		nodes[last].next_node_index = *arguments;
		last = *arguments;
	}
	return call;
}
#define CALL(function, ...) call_list((function), (long const []){ __VA_ARGS__, NONE })
#define CALL0(function) call_list((function), (long const []){ NONE })

static long script_call(short script_index)
{
	long call = node(FLAG(_hs_syntax_node_script_bit)), name = primitive();

	nodes[call].script_index = script_index;
	nodes[call].data = name;
	return call;
}

static long all_players_inside(void) { return CALL(VOLUME_TEST_ALL, primitive(), CALL0(PLAYERS)); }

static struct hs_stack_frame frames[3];

/* the thread waits in the script's begin (`block`) on its statement `wait`
(a sleep_until of every player in a volume); the block is the script's
whole unless nested */
static void wait_in(long block, long wait, short script_type, boolean nested)
{
	static struct hs_stack_frame outer;

	scripts[0].script_type = script_type;
	scripts[0].root_expression_index = block;
	scenario.hs_scripts.count = scenario.hs_scripts.count > 1 ? scenario.hs_scripts.count : 1;
	frames[0].expression_index = block;
	frames[0].previous = nested ? &outer : NULL;
	frames[1].expression_index = wait;
	frames[1].previous = &frames[0];
	frames[2].expression_index = nodes[nodes[wait].data].next_node_index;
	frames[2].previous = &frames[1];
	thread.stack = &frames[2];
	thread.type = _hs_thread_type_script;
	thread.script_index = 0;
	hs_runtime_globals.executing_thread_index = 0;
}

/* a script: (begin (sleep_until <every player in a volume>) <after...>), waiting on that */
static boolean gate_with(short script_type, boolean nested, long const *after)
{
	long wait = CALL(_hs_function_sleep_until, all_players_inside());
	long statements[64] = { wait };
	short count = 1;
	long block;

	for (; *after != NONE; after++)
		statements[count++] = *after;
	statements[count] = NONE;
	block = call_list(_hs_function_begin, statements);
	wait_in(block, wait, script_type, nested);
	return hs_runtime_waiting_call_is_a_gate();
}
#define GATE(...) gate_with(_hs_script_dormant, FALSE, (long const []){ __VA_ARGS__, NONE })
#define GATE_ENDING() gate_with(_hs_script_dormant, FALSE, (long const []){ NONE })

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	functions[AI_RETREAT].name = "ai_retreat";
	functions[DEVICE_SET_POSITION].name = "device_set_position";
	functions[GAME_SAVE].name = "game_save";
	functions[OBJECT_TELEPORT].name = "object_teleport";
	functions[AI_LIVING_COUNT].name = "ai_living_count";
	functions[VOLUME_TEST_ALL].name = "volume_test_objects_all";
	functions[PLAYERS].name = "players";
	functions[SOUND_IMPULSE_START].name = "sound_impulse_start";
	functions[PRINT].name = "print";
	if (!strcmp(name, "ai-only"))
	{
		/* a30's mission_first_retreat: the Marines fall back, then the next wave's wait */
		CHECK(!GATE(CALL(AI_RETREAT, primitive()), CALL(_hs_function_sleep_until, primitive())),
			"a wait that only sends the AI back is a gate");
		CHECK(!GATE(CALL(SOUND_IMPULSE_START, primitive()), CALL(_hs_function_set, primitive(), primitive()),
			CALL(_hs_function_sleep_until, primitive()), CALL(DEVICE_SET_POSITION, primitive(), primitive())),
			"a door past the next wait made this one a gate");
	}
	else if (!strcmp(name, "a30-last-wave"))
	{
		CHECK(!GATE(CALL(AI_RETREAT, primitive())), "the end of a dormant script that only sends the AI back is a gate");
	}
	else if (!strcmp(name, "door"))
	{
		/* a10's tunnel door: a save, then the door closes */
		CHECK(GATE(CALL0(GAME_SAVE), CALL(DEVICE_SET_POSITION, primitive(), primitive())), "a closing door is not a gate");
	}
	else if (!strcmp(name, "sleep-then-door"))
	{
		CHECK(GATE(CALL(AI_RETREAT, primitive()), CALL(_hs_function_sleep, primitive()),
			CALL(DEVICE_SET_POSITION, primitive(), primitive())), "a door after a timed sleep is not a gate");
	}
	else if (!strcmp(name, "arguments"))
	{
		CHECK(!GATE(CALL(_hs_function_set, primitive(), CALL(AI_LIVING_COUNT, primitive())),
			CALL(_hs_function_if, primitive(), CALL(PRINT, primitive()))), "harmless arguments made a gate");
		CHECK(GATE(CALL(_hs_function_if, primitive(), CALL(OBJECT_TELEPORT, primitive(), primitive()))),
			"a teleport inside an if is not a gate");
	}
	else if (!strcmp(name, "static-script"))
	{
		/* d20's and c20's certain_save: (begin (set save_now true)) */
		scenario.hs_scripts.count = 2;
		scripts[1].root_expression_index = CALL(_hs_function_begin, CALL(_hs_function_set, primitive(), primitive()));
		CHECK(!GATE(script_call(1), CALL(AI_RETREAT, primitive())), "a script that sets a variable made a gate");
		scripts[1].root_expression_index = CALL(_hs_function_begin, CALL(DEVICE_SET_POSITION, primitive(), primitive()));
		CHECK(GATE(script_call(1)), "a script that moves a door is not a gate");
		CHECK(GATE(script_call(3)), "a script past the table is not a gate");
	}
	else if (!strcmp(name, "continuous"))
	{
		/* a continuous script begins again: its first statements count, up to its first wait */
		long door = CALL(DEVICE_SET_POSITION, primitive(), primitive());
		long wait = CALL(_hs_function_sleep_until, all_players_inside());
		long block = CALL(_hs_function_begin, door, wait, CALL(AI_RETREAT, primitive()));

		wait_in(block, wait, _hs_script_continuous, FALSE);
		CHECK(hs_runtime_waiting_call_is_a_gate(), "a continuous script's door at its start is not a gate");
		{
			long first_wait = CALL(_hs_function_sleep_until, primitive());
			long second = CALL(_hs_function_sleep_until, all_players_inside());

			block = CALL(_hs_function_begin, first_wait, second, CALL(AI_RETREAT, primitive()));
			wait_in(block, second, _hs_script_continuous, FALSE);
			CHECK(!hs_runtime_waiting_call_is_a_gate(), "a d40 spawner's loop is a gate");
		}
	}
	else if (!strcmp(name, "nested-block"))
	{
		CHECK(gate_with(_hs_script_dormant, TRUE, (long const []){ CALL(AI_RETREAT, primitive()), NONE }),
			"the end of a nested block, whose next steps are unknown, is not a gate");
	}
	else if (!strcmp(name, "wake"))
	{
		CHECK(GATE(CALL(_hs_function_wake, primitive())), "waking a script is not a gate");
	}
	else if (!strcmp(name, "unknown"))
	{
		CHECK(GATE(CALL0(UNNAMED)), "an unnamed function is not a gate");
	}
	else if (!strcmp(name, "not-waiting"))
	{
		waiting = FALSE;
		CHECK(GATE(CALL(AI_RETREAT, primitive())), "outside a sleep_until's wait, not a gate");
	}
	else if (!strcmp(name, "budget"))
	{
		long statements[1100];
		int index;
		long wait = CALL(_hs_function_sleep_until, all_players_inside());

		statements[0] = wait;
		for (index = 1; index < 1099; index++)
			statements[index] = CALL(AI_RETREAT, primitive());
		statements[1099] = NONE;
		wait_in(call_list(_hs_function_begin, statements), wait, _hs_script_dormant, FALSE);
		CHECK(hs_runtime_waiting_call_is_a_gate(), "a scan past its budget is not a gate");
	}
	else if (!strcmp(name, "recursion"))
	{
		scenario.hs_scripts.count = 2;
		scripts[1].root_expression_index = CALL(_hs_function_begin, script_call(1));
		CHECK(GATE(script_call(1)), "a script calling itself is not a gate");
	}
	else
		return 2;
	return 0;
}
