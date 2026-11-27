/*
UE4_PAK.H

An Unreal Engine 4 pak (version 7, not encrypted, entries stored, not
compressed) read in place: MCC's MCC/Content/Paks/MCC-WindowsNoEditor.pak,
for the MCC import (docs/mcc-import.md). Only the footer and the index are
read whole; each asset is read with pread when it is asked for, so the 11 GB
file is never loaded. Its cooked packages are read as tools/
mcc_preview_bake.py reads them: a package's names and imports, a Texture2D's
top mip (DXT1, DXT5 or BC5), and a StaticMesh's LOD0. Every read is bounds
checked; a pak or asset of another layout is refused, never guessed at.
*/

#ifndef UE4_PAK_H
#define UE4_PAK_H

#include <stddef.h>

struct ue4_pak;

/* the pak at `path`: NULL (and *reason) if it is not a version 7 pak */
struct ue4_pak *ue4_pak_open(const char *path, const char **reason);
void ue4_pak_close(struct ue4_pak *pak);
/* how many entries it has, and the CRC-32 of its index (what the import's
note keeps: any MCC update that moves an asset changes it) */
long ue4_pak_count(const struct ue4_pak *pak);
unsigned long ue4_pak_index_crc(const struct ue4_pak *pak);
/* an entry's full name ("MCC/Content/...") */
const char *ue4_pak_name(const struct ue4_pak *pak, long entry);
/* the entry of that full name, or -1 */
long ue4_pak_find(const struct ue4_pak *pak, const char *name);
/* the one entry under `root` named <name>.uasset (in it or any folder
below it), or -1 if none or more than one */
long ue4_pak_find_asset(const struct ue4_pak *pak, const char *root, const char *name);
/* an entry's bytes (malloc'd, *size of them); NULL if it cannot be read */
unsigned char *ue4_pak_read(const struct ue4_pak *pak, long entry, size_t *size);

/* ---------- cooked packages */

struct ue4_package
{
	long name_count;
	char **names;
	long import_count;
	/* each import's class name and object name (indexes into names) */
	long *import_class;
	long *import_object;
};

/* a package's summary's names and imports (its .uasset); 0 if it is not one */
int ue4_package_read(const unsigned char *uasset, size_t size, struct ue4_package *package);
void ue4_package_free(struct ue4_package *package);
/* a name's index, or -1 */
long ue4_package_name(const struct ue4_package *package, const char *name);

/* ---------- textures */

enum
{
	_ue4_format_dxt1,
	_ue4_format_dxt5,
	_ue4_format_bc5,
};

struct ue4_texture
{
	int format;
	long width;
	long height;
	/* the top mip's blocks */
	unsigned char *blocks;
	size_t size;
};

/* the Texture2D whose .uasset is the entry `asset`: its top mip (from its
.ubulk); 0 and *reason if not read */
int ue4_texture_read(const struct ue4_pak *pak, long asset, struct ue4_texture *texture, const char **reason);
void ue4_texture_free(struct ue4_texture *texture);
/* its top mip decoded to 8-bit RGBA (BC5: red and green, blue 0, alpha 255),
malloc'd, width * height * 4 bytes */
unsigned char *ue4_texture_rgba(const struct ue4_texture *texture);

/* ---------- static meshes */

struct ue4_mesh_section
{
	long slot;
	long first_index;
	long triangle_count;
	/* the material slot's MaterialInterface (an import's object name, or "") */
	char material[128];
};

struct ue4_mesh
{
	long vertex_count;
	/* (vertex_count * 3: Unreal's centimeters) */
	double *positions;
	/* the first UV channel (vertex_count * 2) */
	double *uvs;
	/* each vertex's tangent and normal (vertex_count * 3) and its
	bitangent's sign (+1 or -1) */
	double *tangents;
	double *normals;
	double *signs;
	long triangle_count;
	long *triangles;
	/* each triangle's section (-1 for none) */
	long *section_of;
	long section_count;
	struct ue4_mesh_section *sections;
};

/* LOD0 of the cooked static mesh whose .uasset is the entry `asset` (and
its .uexp beside it); 0 and *reason if not read */
int ue4_mesh_read(const struct ue4_pak *pak, long asset, struct ue4_mesh *mesh, const char **reason);
void ue4_mesh_free(struct ue4_mesh *mesh);

#endif
