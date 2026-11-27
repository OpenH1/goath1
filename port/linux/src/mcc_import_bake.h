/*
MCC_IMPORT_BAKE.H

Our weapons' and vehicles' textures baked from MCC's 3D preview meshes, in C
(docs/mcc-import.md, step 3; tools/mcc_preview_bake.py is the same math):
for each texel of our bitmap, its 3D point on our model, moved by the
shipped fit onto MCC's mesh, the closest point there, that point's UV and
its section's textures, MCC's texel there. Runs on the import's thread
(mcc_import.c).
*/

#ifndef MCC_IMPORT_BAKE_H
#define MCC_IMPORT_BAKE_H

#include <stddef.h>

struct ue4_pak;

/* one of our models and its fit onto an MCC preview mesh (mcc_import.json,
numbers only: tools/mcc_preview_bake.py --fits-out) */
struct mcc_import_fit
{
	/* the preview mesh, the material instance its sections fall back to
	("" for none), and its LOD0's vertex and triangle counts (the mesh the
	fit was made for) */
	const char *mesh;
	const char *fallback;
	long vertices;
	long triangles;
	/* our model tag and the stock map it is read from */
	const char *model;
	const char *map;
	/* ours moved onto MCC's: scale * rotation * point + shift (the rotation
	row major, a mirror when its determinant is negative), and how far from
	MCC's surface is still close */
	double scale;
	double rotation[9];
	double shift[3];
	double near;
};

extern const struct mcc_import_fit mcc_import_fits[];
extern const unsigned int mcc_import_fit_count;

/* one of our bitmaps whose UV layout MCC's own textures already have (MCC's
model is ours, the same UVs): texel for texel, no mesh and no fit
(mcc_import.json's "layouts"). As MCC's character textures are: the color's
alpha is the roughness, with a hole where the visor is (painted white there,
so our texels are kept), and the normal map is DirectX's (green down). */
struct mcc_import_layout
{
	/* our model (its parts mark the bitmap's islands), the stock map it is
	read from, and the bitmap */
	const char *model;
	const char *map;
	const char *bitmap;
	/* the textures' folder under MCC's Customization folder (ending in /),
	and each texture's name ("" for none) */
	const char *root;
	const char *color;
	const char *normal;
	const char *metallic;
	const char *occlusion;
};

extern const struct mcc_import_layout mcc_import_layouts[];
extern const unsigned int mcc_import_layout_count;

/* the import's progress and cancel: called now and then with 0 to 1, a
nonzero return cancels */
typedef int (*mcc_bake_progress_proc)(void *context, float fraction);

struct mcc_bake_options
{
	/* the player's maps folder (host path, ending in a separator) */
	const char *maps_folder;
	/* the import's folder: its tags/ is written */
	const char *out_folder;
	/* only the meshes whose names contain one of these words (comma
	separated, any case); NULL or "" for all */
	const char *only;
	/* the threads that find closest points (at least 1) */
	int threads;
	/* the largest side baked (2048) */
	long max_size;
	mcc_bake_progress_proc progress;
	void *context;
	/* appended to: a line per bitmap written, "bitmap <its stock pixels'
	CRC-32, 8 hex digits> <tag>", and per map read, "map <crc> <name>"
	(the import's note) */
	char *note;
	size_t note_size;
	/* a line per bitmap baked, for the log (as the prototype prints) */
	void (*say)(const char *line);
};

/* Bakes every fit's and layout's bitmaps from the pak; how many files were written (a
.bitmap, .normal.png or .orm.png each), or -1 if canceled; report says what
happened in a line. */
long mcc_bake_textures(const struct ue4_pak *pak, const struct mcc_bake_options *options, char *report,
	size_t report_size);

#endif
