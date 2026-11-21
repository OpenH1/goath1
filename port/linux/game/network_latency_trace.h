/*
NETWORK_LATENCY_TRACE.H

The network tests' latency by hop (network_latency_trace.c,
debug.network_latency_trace). Off, each call costs a test of a flag.
*/

#ifndef __NETWORK_LATENCY_TRACE_H
#define __NETWORK_LATENCY_TRACE_H
#pragma once

extern int network_latency_trace_on;
#define NETWORK_LATENCY_TRACE(call) do { if (network_latency_trace_on) { call; } } while (0)
/* (queued to the machine network_latency_trace_destination set) */
#define NETWORK_LATENCY_TRACE_DESTINATION -2

int network_latency_trace_check(void);
void network_latency_trace_header(int rate, int host);
void network_latency_trace_frame_input(void);
void network_latency_trace_step_begun(void);
void network_latency_trace_tick_begun(long tick, int client, long host_tick);
void network_latency_trace_destination(short to);
void network_latency_trace_sample(char kind, short player, long key, short substep);
void network_latency_trace_queued(char kind, short player, long key, short substep, short to);
void network_latency_trace_sent(short to, unsigned long long handed);
unsigned long long network_latency_trace_clock(void);
void network_latency_trace_datagram_read(unsigned long long kernel_realtime_nanoseconds,
	unsigned long long realtime_now_nanoseconds);
void network_latency_trace_datagram_queued(void const *connection);
void network_latency_trace_datagram_dequeued(void const *connection, int datagram);
void network_latency_trace_queue_reset(void const *connection);
void network_latency_trace_received(char kind, short player, long key, short substep, long tick);
void network_latency_trace_applied(char kind, short player, long key, short substep, long tick, short step);
void network_latency_trace_frame_committed(void);
void network_latency_trace_presented(void);
void network_latency_trace_send(unsigned long address, int datagram);

#endif
