/*
CHECK.C

display.temporal_aa's motion in the Metal emitter (xgpu_msl_options' motion,
docs/temporal-aa.md; tools/test_temporal_metal.py):

  check <capture dir> <out dir>   the game's vertex programs (vsNNN.bin, as
                                  msl_corpus reads them), unlit, lit for each
                                  pixel and a structure's, and the synthetic
                                  programs (tools/msl_corpus), each written
                                  with the motion and without
                                  (<name>_<lighting>_<motion>.metal,
                                  ps_<name>_<motion>.metal): the motion's
                                  outputs only in the motion's variants, the
                                  previous frame's registers read there alone,
                                  and none in a World AO pre-pass twin's

Built by tools/test_temporal_metal.py with the shader decoders and the Metal
emitter, as tools/msl_corpus is.
*/

#include "xgpu.h"
#include "xgpu_shader.h"
#include "xgpu_msl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...) { (void)format; }

#include "synthetic.c"

static const char *out_dir;
static int failures, vertex_count, pixel_count;

static void write_source(const char *name, const char *source)
{
	char path[1024];
	FILE *file;

	snprintf(path, sizeof(path), "%s/%s.metal", out_dir, name);
	if (!(file = fopen(path, "w")))
	{
		perror(path);
		exit(1);
	}
	fputs(source, file);
	fclose(file);
}

/* the motion's parts in a source: its outputs (the vertex's or the pixel's)
and the previous frame's registers */
static int has_motion(const char *source)
{
	return strstr(source, "xMotionPrevious") || strstr(source, "[[color(1)]]") || strstr(source, "motion_registers");
}

static void vertex(const char *name, const struct xgpu_vertex_program *program)
{
	static const struct nv2a_vertex_lighting environment = { XGPU_ENVIRONMENT_LIGHTING };
	static const char *const lighting_names[3] = { "unlit", "model", "environment" };
	struct nv2a_vertex_lighting model;
	const struct nv2a_vertex_lighting *lightings[3];
	int lighting, motion;

	memset(&model, 0, sizeof(model));
	model.lights = 2;
	lightings[0] = NULL;
	lightings[1] = &model;
	lightings[2] = &environment;
	for (lighting = 0; lighting < 3; lighting++)
	{
		for (motion = 0; motion < 2; motion++)
		{
			struct xgpu_msl_options options;
			char file[128];
			char *source;

			memset(&options, 0, sizeof(options));
			options.motion = motion;
			source = xgpu_msl_vertex_shader(program, 0, 0, lightings[lighting], &options);
			if (!source)
			{
				printf("%s %s: no source\n", name, lighting_names[lighting]);
				failures++;
				continue;
			}
			if (!has_motion(source) != !motion ||
				(motion && (!strstr(source, "output.xMotionCurrent") || !strstr(source, "motion_registers + 192"))))
			{
				printf("%s %s: the motion %s\n", name, lighting_names[lighting], motion ? "missing" : "where it is off");
				failures++;
			}
			snprintf(file, sizeof(file), "%s_%s_%s", name, lighting_names[lighting], motion ? "motion" : "plain");
			write_source(file, source);
			free(source);
			vertex_count++;
		}
	}
}

static void pixel(const char *name, const struct xgpu_pixel_program *program)
{
	int motion, twin;

	for (twin = 0; twin < 2; twin++)
	{
		struct xgpu_pixel_program variant = *program;

		/* (a World AO pre-pass occluder's twin: its inputs in the colour's
		place, never the motion) */
		if (twin)
			variant.ambient_occlusion = XGPU_AMBIENT_OCCLUSION_WORLD_INPUTS;
		for (motion = 0; motion < 2; motion++)
		{
			struct xgpu_msl_options options;
			char file[160];
			char *source;
			int expected = motion && !twin;

			memset(&options, 0, sizeof(options));
			options.motion = motion;
			source = xgpu_msl_pixel_shader(&variant, &options);
			if (!source)
			{
				printf("%s: no source\n", name);
				failures++;
				continue;
			}
			if (!has_motion(source) != !expected || (expected && !strstr(source, "output.motion = uint2(")))
			{
				printf("%s%s: the motion %s\n", name, twin ? " twin" : "", expected ? "missing" : "where it is off");
				failures++;
			}
			if (!twin)
			{
				snprintf(file, sizeof(file), "ps_%s_%s", name, motion ? "motion" : "plain");
				write_source(file, source);
			}
			free(source);
			pixel_count++;
		}
	}
}

static void synthetic_pixel(const char *name, const struct xgpu_pixel_program *program)
{
	pixel(name, program);
}

static void synthetic_vertex(const char *name, const struct xgpu_vertex_program *program)
{
	vertex(name, program);
}

int main(int argc, char **argv)
{
	int number;

	if (argc != 3)
	{
		fprintf(stderr, "usage: check <capture dir> <out dir>\n");
		return 2;
	}
	out_dir = argv[2];
	for (number = 1; number <= 999; number++)
	{
		char path[1024], name[16];
		static unsigned char data[16384];
		static struct xgpu_vertex_program program;
		FILE *file;
		long size;

		snprintf(path, sizeof(path), "%s/vs%03d.bin", argv[1], number);
		if (!(file = fopen(path, "rb")))
			break;
		size = (long)fread(data, 1, sizeof(data), file);
		fclose(file);
		nv2a_vertex_program_decode((const DWORD *)(data + 4), (unsigned long)(size - 4) / 16, &program);
		snprintf(name, sizeof(name), "vs%03d", number);
		vertex(name, &program);
	}
	synthetic_pixel_programs(synthetic_pixel);
	synthetic_vertex_programs(synthetic_vertex);
	printf("vertex %d pixel %d failures %d\n", vertex_count, pixel_count, failures);
	return failures ? 1 : 0;
}
