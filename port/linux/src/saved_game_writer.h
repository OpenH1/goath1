/*
SAVED_GAME_WRITER.H

A checkpoint's file written on a thread of its own (saved_game_writer.c).
*/

#ifndef SAVED_GAME_WRITER_H
#define SAVED_GAME_WRITER_H

/* writes the image to the file (an Xbox path) on the writer's thread, after
the write before it: the image must stay as it is until
saved_game_writer_wait returns */
void saved_game_writer_begin(char const *xbox_path, void const *image, unsigned long size);
/* the image of the last write also written to this file (an Xbox path), on
the writer's thread after it, in the same way: a network co-op host's own
saved game. Its failure is logged, not returned by saved_game_writer_wait,
which waits for it too */
void saved_game_writer_copy(char const *xbox_path);
/* waits for the write in progress, if any: 0 if the last write succeeded
(or there was none), else its errno */
int saved_game_writer_wait(void);

#endif
