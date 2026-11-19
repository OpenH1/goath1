/*
CHECK.C

display.smooth_lightmaps (port/linux/src/xgpu.h's
XGPU_TEXTURE_MODES_LIGHTMAP_CUBIC; tools/test_lightmap_cubic.py): the
lightmap pass's shape at its simplest, texture 2's texel as the colour, as
the emitters write it with the lightmap's cubic filter and without it:

  check spirv <directory>   <directory>/cubic.spv and bilinear.spv
  check msl                 the Metal emitter's, each after a line
                            "==== cubic" or "==== bilinear"

Built by the test with the shader decoders and both emitters, as
tools/ambient_occlusion_light/check.c is.
*/

#include "xgpu.h"
#include "xgpu_backend.h"
#include "xgpu_shader.h"
#include "xgpu_msl.h"
#include "xgpu_spirv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...) { (void)format; }

/* the colour texture 2's texel (D of the final combiner), its alpha too */
static void program_make(struct xgpu_pixel_program *program, int cubic)
{
	memset(program, 0, sizeof(*program));
	program->stages[2].declared_mode = program->stages[2].mode = XGPU_TEXTURE_PROJECT2D;
	program->stages[2].sampler = _xgpu_sampler_2d;
	program->final.present = 1;
	program->final.inputs[3].reg = XGPU_REGISTER_T2;
	program->final.inputs[6].reg = XGPU_REGISTER_T2;
	program->final.inputs[6].alpha = 1;
	program->lightmap_filter = (unsigned char)cubic;
}

int main(int argc, char **argv)
{
	struct xgpu_pixel_program program;
	int cubic;

	if (argc == 3 && !strcmp(argv[1], "spirv"))
	{
		struct xgpu_spirv_options options;
		struct xgpu_spirv_module module;

		memset(&options, 0, sizeof(options));
		for (cubic = 0; cubic < 2; cubic++)
		{
			char path[1024];
			FILE *file;

			program_make(&program, cubic);
			snprintf(path, sizeof(path), "%s/%s.spv", argv[2], cubic ? "cubic" : "bilinear");
			if (!xgpu_spirv_pixel_shader(&program, &options, &module) || !(file = fopen(path, "wb")))
				return 1;
			fwrite(module.words, 4, module.count, file);
			fclose(file);
			free(module.words);
		}
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "msl"))
	{
		struct xgpu_msl_options options;
		char *source;

		memset(&options, 0, sizeof(options));
		for (cubic = 1; cubic >= 0; cubic--)
		{
			program_make(&program, cubic);
			if (!(source = xgpu_msl_pixel_shader(&program, &options)))
				return 1;
			printf("==== %s\n%s\n", cubic ? "cubic" : "bilinear", source);
			free(source);
		}
		return 0;
	}
	fprintf(stderr, "check spirv <directory> | check msl\n");
	return 2;
}
