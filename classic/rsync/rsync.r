/*
 * Memory: about 0.7 MB for the code, 0.2 MB per run, and 320 bytes per
 * file-list entry (files, folders and ._ sidecars alike), plus about
 * 0.3 MB with -z.  6 MB holds about 15,000 entries, 3 MB about 6,000.
 * See docs/CLASSIC_MACOS_PORT.md, "Memory".
 */
#include "Processes.r"

resource 'SIZE' (-1) {
	reserved,
	acceptSuspendResumeEvents,
	reserved,
	canBackground,
	doesActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreChildDiedEvents,
	is32BitCompatible,
	notHighLevelEventAware,
	onlyLocalHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	6 * 1024 * 1024,
	3 * 1024 * 1024
};
