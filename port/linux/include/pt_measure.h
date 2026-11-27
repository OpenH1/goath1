/*
PT_MEASURE.H

Path tracing's stage 0, "Measure" (docs/rt-plan/4-path-tracing.md): the
static fit study, on the CPU, game side (port/linux/game/pt_measure.c), with
HALO_PT_MEASURE_FIT set (its value the samples, 0 or none the default).
Measurement only: nothing here is drawn, read by the simulation, sent or
saved.
*/

#ifndef PT_MEASURE_H
#define PT_MEASURE_H

/* each window's start (world_ao.c's world_ao_window_begin): a share of the
study's work, its results logged once ("pt fit:" lines) */
void pt_measure_frame(void);

#endif
