/*
RT_BOUNCE.H

display.rt_bounce (docs/rt-plan/3-gi.md, stage 3): what the game
(port/linux/game/dynamic_lights_bounce.c) hands the backend
(xgpu_backend.h's xgpu_backend_rt_bounce) of a window's lights that bounce.
The game's copy of xgpu_backend.h's struct xgpu_rt_bounce, member for
member (the game does not include the backend's headers): change both.
*/

#ifndef RT_BOUNCE_H
#define RT_BOUNCE_H

#ifndef XGPU_RT_BOUNCE_LIGHTS

/* a light that bounces: its position, reach (world units), linear color
and cone (the enhanced lights': its axis, scale and offset; omni 0, 0 and
1) */
#define XGPU_RT_BOUNCE_LIGHTS 8
struct xgpu_rt_bounce_light
{
	float position[3];
	float radius;
	float color[3];
	float cone_offset;
	float axis[3];
	float cone_scale;
};
/* a window's (0 to 3): its lights (count of them), and the seconds since
its last */
struct xgpu_rt_bounce
{
	int count, window;
	float seconds;
	struct xgpu_rt_bounce_light lights[XGPU_RT_BOUNCE_LIGHTS];
};

#endif

#endif
