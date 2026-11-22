/*
DLSS_ADDON.H

The NVIDIA DLSS add-on (docs/temporal-aa.md, "The add-on"), which the game
downloads from inside it: its offer, its screen's words and actions
(port/assets/menus/screens/dlss_addon.xml), and its removal.
*/

#ifndef DLSS_ADDON_H
#define DLSS_ADDON_H

struct menu_table_source;

/* as the game starts, before the renderer: what an install or a removal
left behind deleted (a removal asked for while the module was loaded) */
void dlss_addon_start(void);

/* the download offered: an NVIDIA GPU on Vulkan, an add-on this build pins
for this platform, and that add-on not installed */
int dlss_addon_offered(void);
/* the add-on installed (any version, not being removed) */
int dlss_addon_installed(void);

/* a restart is what the add-on's install or removal waits for (RESTART
NOW, "dlss.can_restart", while one may be: pc_menu_restart_allowed) */
int dlss_addon_restart_needed(void);

/* NVIDIA's attribution, shown while the add-on is installed (the about
box's, the add-on's screen); NULL while it is not */
const char *dlss_addon_attribution(void);

/* the screen's: opened (NVIDIA's license fetched, if the download is
offered), its bindings' values ("dlss.<name>", UTF-8: nonzero if it has one),
its actions ("dlss accept", "dlss remove", "dlss restart", "dlss page up",
"dlss page down": nonzero if done; "dlss restart" returns only if the game
could not be started again), and its table's rows, NVIDIA's license's lines */
void dlss_addon_screen_opened(void);
int dlss_addon_value(const char *name, char *value, unsigned long size);
int dlss_addon_action(const char *name);
extern struct menu_table_source const dlss_addon_license_source;

#endif
