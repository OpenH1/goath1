/*
HRTF_SOFA.H

A SOFA file's measured head-related impulse responses (AES69, the
SimpleFreeFieldHRIR convention), for audio.hrtf_file (dsound_sdl.c's
hrtf_load, which fits them into the HRTF's table: hrtf_fit).
*/

#ifndef HRTF_SOFA_H
#define HRTF_SOFA_H

#include <stddef.h>

struct hrtf_sofa
{
	/* the directions measured (M) and each response's samples (N), at
	rate (Hz) */
	unsigned long measurements, samples;
	double rate;
	/* measurements x 2 ears (the left, then the right) x samples */
	float *responses;
	/* measurements x 2: the azimuth (degrees, counterclockwise from
	ahead: the left positive, as SOFA's) and the elevation (degrees, up
	positive) */
	double *directions;
	/* measurements x 2 ears: each response's delay before its first
	sample (samples, Data.Delay; 0 where the file has none) */
	double *delays;
};

/* the responses of the SOFA file at path; FALSE (0), and why (a sentence's
end, no full stop), if it cannot be read */
int hrtf_sofa_read(const char *path, struct hrtf_sofa *sofa, char *why, size_t why_size);
void hrtf_sofa_free(struct hrtf_sofa *sofa);

#endif
