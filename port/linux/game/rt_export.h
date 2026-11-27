/*
RT_EXPORT.H

debug.rt_export ("<second> <folder>"): one moment of the game written to a
scene folder for the ray-traced truth renderer (the rt2 scene format, local
only): its camera, the loaded structure BSP's render triangles, every object
posed and skinned (the first-person weapon and hands too), the BSP's
lightmap pages, the materials' tags, and the classic renderer's own frame of
that moment. Render-only and read only: nothing the simulation reads.
*/

#ifndef RT_EXPORT_H
#define RT_EXPORT_H

/* each frame, from the main loop (main.c, beside network_test_update):
before the frame's ticks. At the setting's second (the cues' clock,
network_test_cues_start_tick) the frame drawn is saved, and on the next
call, the game still as that frame drew it, the scene written; the first
window's camera (a struct render_camera: main.c's, as that frame drew it) */
void rt_export_frame(int main_menu_loaded, void const *first_window_camera);

#endif
