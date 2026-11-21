/*
HOST_LOG_QUEUE.H

The log's queue (host_log_queue.c): the lines the game's threads log, and
what the guest writes to its standard output and error, written by a
thread of their own, so that a log whose reader is slow (a pipe of Steam's
or a desktop launcher's) never holds up the thread that logs.
*/

#ifndef HOST_LOG_QUEUE_H
#define HOST_LOG_QUEUE_H

#include <stddef.h>

/* writes a record of the queue: a line (its bytes ended by a 0, size not
counting it) or bytes as they came; tag is the line's priority or the
bytes' descriptor */
typedef void (*host_log_writer)(int tag, const char *bytes, size_t size);

/* the writer thread started; note writes the queue's own lines (the lines
it dropped, at their place); 0 or an errno value */
int host_log_queue_start(host_log_writer note);
/* a line (cut at HOST_LOG_QUEUE_RECORD bytes), or bytes in count parts (a
write's vector: together, no other caller's between them), added to the
queue with the writer that is to write them: 1 if they were added (or
dropped, counted, as the queue was full), 0 if the caller is to write them
itself (no queue, or the log written directly now: host_log_queue_direct) */
int host_log_queue_line(host_log_writer writer, int tag, const char *line);
int host_log_queue_bytes(host_log_writer writer, int tag, int count, const char *const *bytes, const size_t *sizes);
/* every record queued so far written, on the calling thread once the
writer thread has finished its record; if it does not within timeout_ms
(it is blocked), left to it, unless the log is written directly now
(host_log_queue_direct), when it is taken over (a crash's handler may call
it: no locks, no allocation, no stdio) */
void host_log_queue_flush(unsigned timeout_ms);
/* the queue flushed and given up: each line after it written directly, by
its caller (a fatal error, a crash, the exit) */
void host_log_queue_direct(unsigned timeout_ms);

#ifndef _WIN32
struct iovec;
/* (the POSIX hosts' system calls) the guest's write of count parts to its
standard output or error (fd 1 or 2) queued: its size, or -1 if it is to
be written as any other (another descriptor, no queue) */
long host_log_queue_output(int fd, const struct iovec *vector, int count);
#endif

/* the longest record */
#define HOST_LOG_QUEUE_RECORD 4096

#endif
