#include "harness.h"
#include <math.h>

/* the import's texture-only layouts (mcc_import_bake.c): MCC's texel at the
same UV, the second texture root, the visor kept ours */
#include "defines.inc"
#include "pak.inc"
#include "layout.inc"
#include "under_test.inc"

#define SIDE 64
#define TEXELS (SIDE * SIDE)

static float color[4 * TEXELS], ours[4 * TEXELS], before[4 * TEXELS], normal[4 * TEXELS];
static unsigned char island[TEXELS], visor[TEXELS], rgb[3 * TEXELS];

/* MCC's paint: lighter and greyer than ours, a hole in its alpha (the
visor, painted white) at 20..23 x 20..23, another outside the islands */
static void scene(void)
{
	long x, y;

	for (y = 0; y < SIDE; y++)
		for (x = 0; x < SIDE; x++)
		{
			long at = y * SIDE + x;
			int hole = (x >= 20 && x < 24 && y >= 20 && y < 24) || (x >= 56 && x < 60 && y >= 2 && y < 6);

			island[at] = y < 48 && x < 52;
			color[4 * at] = hole ? 255.0f : 150.0f + (float)((x * 7 + y * 3) % 40);
			color[4 * at + 1] = hole ? 255.0f : 160.0f + (float)((x * 5 + y * 11) % 30);
			color[4 * at + 2] = hole ? 255.0f : 170.0f + (float)((x * 3 + y * 5) % 20);
			color[4 * at + 3] = hole ? 0.0f : 100.0f + (float)((x + y) % 50);
			ours[4 * at] = 40.0f + (float)((x * 13 + y) % 90);
			ours[4 * at + 1] = 30.0f + (float)((x + y * 17) % 60);
			ours[4 * at + 2] = 20.0f + (float)((x * 3 + y * 7) % 50);
			ours[4 * at + 3] = 255.0f;
			normal[4 * at] = 200.0f;
			normal[4 * at + 1] = 60.0f;
			normal[4 * at + 2] = 0.0f;
			normal[4 * at + 3] = 255.0f;
		}
	memcpy(before, ours, sizeof(ours));
}

static void stats(const float *image, int channel, double *mean, double *sd)
{
	double n = 0, sum = 0, squares = 0;
	long texel;

	for (texel = 0; texel < TEXELS; texel++)
		if (island[texel] && !visor[texel])
		{
			n++;
			sum += image[4 * texel + channel];
		}
	*mean = sum / n;
	for (texel = 0; texel < TEXELS; texel++)
		if (island[texel] && !visor[texel])
			squares += (image[4 * texel + channel] - *mean) * (image[4 * texel + channel] - *mean);
	*sd = sqrt(squares / n);
}

/* MCC's texture at our bitmap's size: a copy, texel for texel; at twice our
size, each of our texels the mean of MCC's 2x2 under it */
static void identity(void)
{
	static unsigned char image[4 * 16 * 16];
	static float out[4 * 16 * 16];
	long x, y;
	int channel;

	for (x = 0; x < 4 * 16 * 16; x++)
		image[x] = (unsigned char)((x * 37 + x / 64 * 11) % 251);
	layout_resample(image, 16, 16, 16, 16, out);
	for (x = 0; x < 4 * 16 * 16; x++)
		CHECK(out[x] == image[x], "value %ld: %g, MCC's %d (the same UV, the same texel)", x, out[x], image[x]);
	layout_resample(image, 16, 16, 8, 8, out);
	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x++)
			for (channel = 0; channel < 4; channel++)
			{
				double mean = (image[4 * ((2 * y) * 16 + 2 * x) + channel] + image[4 * ((2 * y) * 16 + 2 * x + 1) + channel] +
					image[4 * ((2 * y + 1) * 16 + 2 * x) + channel] + image[4 * ((2 * y + 1) * 16 + 2 * x + 1) + channel]) / 4.0;

				CHECK(fabs(out[4 * (y * 8 + x) + channel] - mean) < 1e-3, "texel %ld,%ld: %g, the 2x2's %g", x, y,
					out[4 * (y * 8 + x) + channel], mean);
			}
}

/* the shipped layout's textures found under its own folder, which the
fitted meshes' folder does not have */
static void root(void)
{
	static const char *const names[] =
	{
		"MCC/Content/UI/Features/Customization/SharedTextures/H1/H1S2_Warthog.uasset",
		"MCC/Content/UI/Features/Customization/Characters/H1/Spartans/SK_H1_Spartan.uasset",
		LAYOUT_COLOR_PATH ".ubulk",
		LAYOUT_COLOR_PATH ".uasset",
		LAYOUT_COLOR_PATH ".uexp",
	};
	static char text[1024];
	static struct pak_entry entries[5];
	struct ue4_pak pak;
	struct mcc_import_layout layout = { "", "", "", LAYOUT_ROOT, LAYOUT_COLOR, "", "", "" };
	size_t at = 0;
	long entry;

	for (entry = 0; entry < 5; entry++)
	{
		entries[entry].name = at;
		strcpy(text + at, names[entry]);
		at += strlen(names[entry]) + 1;
	}
	memset(&pak, 0, sizeof(pak));
	pak.count = 5;
	pak.entries = entries;
	pak.names = text;
	entry = layout_asset(&pak, &layout, LAYOUT_COLOR);
	CHECK(entry == 3, "%s: entry %ld, not 3 (its .uasset under %s)", LAYOUT_COLOR, entry, LAYOUT_ROOT);
	CHECK(ue4_pak_find_asset(&pak, CUSTOMIZATION_ROOT "SharedTextures/H1/", LAYOUT_COLOR) < 0,
		"the fitted meshes' folder has no %s", LAYOUT_COLOR);
}

/* each channel's mean and contrast over the islands set to ours */
static void color_match(void)
{
	int channel;

	scene();
	memset(visor, 0, sizeof(visor));
	CHECK(layout_color(color, ours, island, visor, TEXELS), "matched");
	for (channel = 0; channel < 3; channel++)
	{
		double mean, sd, our_mean, our_sd;

		stats(ours, channel, &mean, &sd);
		stats(before, channel, &our_mean, &our_sd);
		CHECK(fabs(mean - our_mean) < 1e-2 && fabs(sd - our_sd) < 1e-2, "channel %d: mean %g sd %g, ours %g and %g",
			channel, mean, sd, our_mean, our_sd);
	}
}

/* our texels kept on the visor (the alpha's hole on an island, and its
edge), flat normals and the classic look there; MCC's elsewhere, its own
hole off the islands too, and the visor left out of the match */
static void visor_rule(void)
{
	long count, x, y;
	int channel;

	scene();
	count = layout_visor(color, island, SIDE, SIDE, visor);
	CHECK(count == 6 * 6 - 4, "the visor's %ld texels, not the hole's 16 and its edge's 16", count);
	CHECK(!visor[3 * SIDE + 57], "MCC's hole off the islands is no visor");
	CHECK(layout_color(color, ours, island, visor, TEXELS), "matched");
	for (y = 19; y <= 24; y++)
		for (x = 19; x <= 24; x++)
		{
			long at = y * SIDE + x;
			int corner = (x == 19 || x == 24) && (y == 19 || y == 24);

			if (corner)
				continue;
			for (channel = 0; channel < 3; channel++)
				CHECK(ours[4 * at + channel] == before[4 * at + channel], "visor texel %ld,%ld: %g, ours %g", x, y,
					ours[4 * at + channel], before[4 * at + channel]);
		}
	CHECK(ours[4 * (3 * SIDE + 57)] != before[4 * (3 * SIDE + 57)], "MCC's texel off the islands");
	for (channel = 0; channel < 3; channel++)
	{
		double mean, sd, our_mean, our_sd;

		stats(ours, channel, &mean, &sd);
		stats(before, channel, &our_mean, &our_sd);
		CHECK(fabs(mean - our_mean) < 1e-2 && fabs(sd - our_sd) < 1e-2, "channel %d (the visor left out): mean %g sd %g, "
			"ours %g and %g", channel, mean, sd, our_mean, our_sd);
	}
	layout_normal(normal, visor, TEXELS, rgb);
	CHECK(rgb[3 * (21 * SIDE + 21)] == 128 && rgb[3 * (21 * SIDE + 21) + 1] == 128 && rgb[3 * (21 * SIDE + 21) + 2] == 255,
		"the visor's normal flat: %d %d %d", rgb[3 * (21 * SIDE + 21)], rgb[3 * (21 * SIDE + 21) + 1],
		rgb[3 * (21 * SIDE + 21) + 2]);
	CHECK(layout_orm(color, color, color, island, visor, TEXELS, rgb), "orm");
	CHECK(rgb[3 * (21 * SIDE + 21)] == 255 && rgb[3 * (21 * SIDE + 21) + 1] == 0 && rgb[3 * (21 * SIDE + 21) + 2] == 0,
		"the visor's classic look: %d %d %d", rgb[3 * (21 * SIDE + 21)], rgb[3 * (21 * SIDE + 21) + 1],
		rgb[3 * (21 * SIDE + 21) + 2]);
}

/* DirectX's green (toward the picture's bottom) flipped to ours (toward its top) */
static void normal_green(void)
{
	scene();
	memset(visor, 0, sizeof(visor));
	layout_normal(normal, visor, TEXELS, rgb);
	CHECK(rgb[0] == 200 && rgb[1] >= 194 && rgb[1] <= 196 && rgb[2] > 128, "200 60 -> %d %d %d, not 200 195 and up",
		rgb[0], rgb[1], rgb[2]);
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";

	if (!strcmp(mode, "identity"))
		identity();
	else if (!strcmp(mode, "root"))
		root();
	else if (!strcmp(mode, "color"))
		color_match();
	else if (!strcmp(mode, "visor"))
		visor_rule();
	else if (!strcmp(mode, "normal"))
		normal_green();
	else
		CHECK(0, "unknown case %s", mode);
	return 0;
}
