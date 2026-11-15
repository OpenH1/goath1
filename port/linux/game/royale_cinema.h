/* royale_cinema.h: Royale's presentation moments: the island flyover before
the drop, the winners' victory orbit and the postgame carnage report
(port/linux/game/royale_cinema.c)

Core (royale.c) calls start/reset/tick/draw; the director (director.c) asks
for the camera, and the objects' lighting (render_objects.c) for the
Pelicans' bay. Presentation only: these read the synced Royale state, never
change it, so every machine shows the same thing from its own copy. */
#ifndef HALO_ROYALE_CINEMA_H
#define HALO_ROYALE_CINEMA_H

#include "cseries.h"

struct royale_map_config;
struct render_lighting;

void royale_cinema_start(struct royale_map_config const *config);
void royale_cinema_reset(void);
void royale_cinema_tick(void);
/* the screen's drawing for this local player (after the world): the
letterbox and its titles, the victory's, or the carnage report */
void royale_cinema_draw(short local_player_index);
/* the cinema has this local player's screen now (the flyover, the orbit, the
report): core draws no other Royale HUD */
boolean royale_cinema_owns_screen(short local_player_index);
/* (hud.c, interface.c, motion_sensor.c) the perspective the HUD draws for:
neutral (no meters, crosshair or motion sensor) while the cinema has the
screen, else the director's */
short royale_cinema_hud_perspective(short local_player_index, short perspective);

/* (director.c) the cinema wants this local player's camera now; the camera
proc (a director_camera_update_proc) and its data's start (in the
director's camera data, 0x40 bytes) */
boolean royale_cinema_camera_wanted(short local_player_index);
void royale_cinema_camera_new(void *data, short local_player_index);
void royale_cinema_camera_update(void *data, void *controls, void *result);

/* (render_objects.c) an object's lighting, once the level's is found: the
Pelicans' bay and their riders lit as by the bay's own lamps */
void royale_cinema_object_lighting(long object_index, struct render_lighting *lighting);

#endif
