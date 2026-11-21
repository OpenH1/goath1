/*
P2P_UPNP.C (test)

Internet play's UPnP forwarding (port/linux/src/p2p.c): upnp_thread's
answer from the router (posix_upnp.c's, the test's) kept or withdrawn, as
p2p_local_candidates offers it and p2p_nat_strict counts it.
test_p2p_upnp.py takes the three functions as they are; the state they
read is the test's.
*/

#include "harness.h"

typedef unsigned long DWORD;
typedef unsigned long posix_ulong;
#define WINAPI

struct p2p_candidate
{
	unsigned long address;
	unsigned short port;
};

enum
{
	LOG_ADDRESS_SIZE = 56,
	MAXIMUM_STUN = 2,
	ROUTER = 0x51000001,
	STUN = 0x52000002,
	LAN = 0xC0A80002,
	TUNNEL_PORT = 2302,
	FORWARDED_PORT = 40000,
};

static struct
{
	int running;
	int reported_symmetric;
	int upnp_working, upnp_asked, upnp_forwarded;
	struct p2p_candidate upnp_candidate;
	unsigned long upnp_time;
	unsigned short tunnel_port;
	int has_public;
	struct p2p_candidate public_candidate;
	struct
	{
		int has_mapped;
		struct p2p_candidate mapped;
	} stun[MAXIMUM_STUN];
	int stun_count;
} p2p;

static int p2p_lock;
static int router_answers;
static int stopped_calls;

#define pthread_mutex_lock(lock) ((void)(lock))
#define pthread_mutex_unlock(lock) ((void)(lock))

static unsigned long p2p_now(void)
{
	return 1000;
}

static void p2p_log(const char *format, ...)
{
	(void)format;
}

static const char *log_address_ipv4(unsigned long address, unsigned short port, char *text, int size)
{
	snprintf(text, (size_t)size, "%lx:%u", address, port);
	return text;
}

static unsigned long network_long(unsigned long value)
{
	return value;
}

static unsigned long posix_local_ipv4_address(void)
{
	return LAN;
}

static int p2p_front_public(struct p2p_candidate *candidate)
{
	(void)candidate;
	return 0;
}

/* the router: forwards (the same port each time) while it answers */
static int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	(void)port;
	(void)preferred_port;
	if (!router_answers)
	{
		snprintf(error, (size_t)error_size, "the router did not answer");
		return 0;
	}
	*external_address = ROUTER;
	*external_port = FORWARDED_PORT;
	return 1;
}

static void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
	stopped_calls++;
}

#include "under_test.inc"

/* ---------- helpers */

#define REQUIRE(condition) CHECK(condition, "%s", "")

/* whether this machine's addresses offer the router's forwarded port */
static int forward_offered(void)
{
	struct p2p_candidate candidates[8];
	int count = p2p_local_candidates(candidates, 8);
	int index;

	for (index = 0; index < count; index++)
	{
		if (candidates[index].address == ROUTER && candidates[index].port == FORWARDED_PORT)
			return 1;
	}
	return 0;
}

/* ---------- cases */

/* forwarded, renewed, then a renewal the router does not answer (its
reboot): withdrawn, not offered, the NAT strict again; a failure with
nothing forwarded changes nothing */
static int case_renewal(void)
{
	p2p.running = 1;
	p2p.tunnel_port = TUNNEL_PORT;
	p2p.reported_symmetric = 1;
	p2p.stun_count = 1;
	p2p.stun[0].has_mapped = 1;
	p2p.stun[0].mapped.address = STUN;
	p2p.stun[0].mapped.port = 50000;
	/* nothing forwarded, the router silent: nothing changes */
	router_answers = 0;
	upnp_thread(NULL);
	REQUIRE(!p2p.upnp_forwarded && !forward_offered() && p2p_nat_strict());
	/* forwarded: offered, not strict */
	router_answers = 1;
	upnp_thread(NULL);
	REQUIRE(p2p.upnp_forwarded && forward_offered() && !p2p_nat_strict());
	/* renewed */
	upnp_thread(NULL);
	REQUIRE(p2p.upnp_forwarded && forward_offered() && !p2p_nat_strict() && !stopped_calls);
	/* the renewal fails: withdrawn */
	router_answers = 0;
	upnp_thread(NULL);
	REQUIRE(!p2p.upnp_forwarded);
	REQUIRE(!forward_offered());
	REQUIRE(p2p_nat_strict());
	/* asked again later (as one never had), and forwarded again */
	router_answers = 1;
	upnp_thread(NULL);
	REQUIRE(p2p.upnp_forwarded && forward_offered() && !p2p_nat_strict());
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (!strcmp(argv[1], "renewal"))
		return case_renewal();
	return 2;
}
