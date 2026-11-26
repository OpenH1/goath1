/*
MOD_MATERIALS.H

The material maps of mods (mod_materials.c, docs/mods.md "Material maps").
*/

#ifndef __MOD_MATERIALS_H
#define __MOD_MATERIALS_H
#pragma once

/* ---------- prototypes/MOD_MATERIALS.C */

/* the maps of the map's bitmaps read again from the mods played (or,
remove_all, none: mods.c's mods_update): how many */
long mod_materials_update(boolean remove_all, boolean at_load);
void mod_materials_tags_unloaded(void);
/* which of the map's bitmaps have material maps now (the mods' or the MCC
import's), and which of the two each has, as one number: 0 for none. A
map's prepared record is keyed by it (render_prepared.c): a model drawn with
material maps is another draw (its material bits), which a record made
without them, a shipped one, does not hold */
unsigned long mod_materials_identity(void);
/* (rasterizer_xbox_models.c) a shader_model's part is drawn next with this
base map (its texture 0 set): the renderer told its maps, or none */
void mod_materials_model_draw(long base_map_tag_index);

#endif
