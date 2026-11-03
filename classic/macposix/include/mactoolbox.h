/*
 * mactoolbox.h: the Mac OS Toolbox headers, for either header set Retro68
 * can be built with:
 *
 *   Multiversal Interfaces (the default): one header, Multiverse.h, holds
 *	everything; the per-manager headers just include it.
 *   Apple's Universal Interfaces (when installed while building
 *	Retro68): one header per manager.
 *
 * The port is built and tested with Multiversal.  MacTCP is declared
 * separately (mactcp.h), the same way under both.
 */
#ifndef MACTOOLBOX_H
#define MACTOOLBOX_H

#if defined(__has_include) && __has_include(<Multiverse.h>)
#include <Multiverse.h>
#else
#include <MacTypes.h>
#include <MacMemory.h>
#include <Errors.h>
#include <Files.h>
#include <Folders.h>
#include <Devices.h>
#include <Events.h>
#include <OSUtils.h>
#include <LowMem.h>
#include <Processes.h>
#include <Gestalt.h>
#include <Resources.h>
#include <Script.h>
#include <TextUtils.h>
#include <ToolUtils.h>
#include <Traps.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <Controls.h>
#include <Lists.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Scrap.h>
#include <StandardFile.h>
#include <Sound.h>
#include <ShutDown.h>
#endif

#endif /* MACTOOLBOX_H */
