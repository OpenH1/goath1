/*
DLSS_ADDON_FOLDER.H

Where the NVIDIA DLSS add-on is (docs/temporal-aa.md, "The add-on"): the
player's own folder, never the game's (which may be read-only, or a
package manager's). The game (port/linux/src/dlss_addon.c) installs it
there and the renderer's host (port/linux/vulkan/xgpu_vulkan_dlss.c) loads
its module from there, so both read the folder from this one place:

	$XDG_DATA_HOME/openh1/addons (~/.local/share/openh1/addons) on Linux,
	%LOCALAPPDATA%\OpenH1\addons on Windows,
	HALO_DLSS_ADDON_FOLDER if it is set (a test's);

the add-on itself in its dlss folder: the module, NVIDIA's DLSS runtime,
the licenses and installed.txt (its version). A remove file there
(DLSS_ADDON_REMOVE_MARK) says it is to be deleted at the next start: the
host does not load it, and the game deletes it before the renderer starts.

Only the C library: the guest (the game) and the host each compile it.
*/

#ifndef DLSS_ADDON_FOLDER_H
#define DLSS_ADDON_FOLDER_H

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(HALO_WINDOWS)
#define DLSS_ADDON_SEPARATOR "\\"
#define DLSS_ADDON_MODULE "openh1_dlss.dll"
#define DLSS_ADDON_RUNTIME "nvngx_dlss.dll"
#else
#define DLSS_ADDON_SEPARATOR "/"
#define DLSS_ADDON_MODULE "libopenh1_dlss.so"
#define DLSS_ADDON_RUNTIME "libnvidia-ngx-dlss.so.310.9.16"
#endif
/* the add-on's folder in the add-ons' folder, its version's file, its
removal's mark and NVIDIA's license (as tools/dlss_addon.py packs it) */
#define DLSS_ADDON_NAME "dlss"
#define DLSS_ADDON_INSTALLED "installed.txt"
#define DLSS_ADDON_REMOVE_MARK "remove"
#define DLSS_ADDON_LICENSE "NVIDIA-DLSS-LICENSE.txt"
#define DLSS_ADDON_MODULE_LICENSE "OPENH1-DLSS-LICENSE.txt"

/* the add-ons' folder (not made), into folder; 0 if there is none (no home
folder) */
static inline int dlss_addon_folder(char *folder, size_t size)
{
	const char *chosen = getenv("HALO_DLSS_ADDON_FOLDER");

	if (chosen && *chosen)
	{
		snprintf(folder, size, "%s", chosen);
		return 1;
	}
#if defined(_WIN32) || defined(HALO_WINDOWS)
	{
		const char *local = getenv("LOCALAPPDATA");

		if (!local || !*local)
			return 0;
		snprintf(folder, size, "%s\\OpenH1\\addons", local);
	}
#else
	{
		const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");

		if (data && data[0] == '/')
			snprintf(folder, size, "%s/openh1/addons", data);
		else if (home && home[0] == '/')
			snprintf(folder, size, "%s/.local/share/openh1/addons", home);
		else
			return 0;
	}
#endif
	return 1;
}

#endif
