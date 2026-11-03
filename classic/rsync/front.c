/*
 * front.c: the console front end for rsync on Classic Mac OS.
 *
 * A Retro68 console window with an "rsync>" prompt; commands run through
 * engine.c.
 *
 * Unattended mode (used by classic/emu/boot-test.sh): if a file named
 * "rsync script" is next to the application, each of its lines is run as
 * a command and all output is also written to "rsync log"; if a file
 * named "rsync autoquit" exists too, the Mac shuts down afterwards.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Multiverse.h>

#include "engine.h"

#define LINEMAX	2048

/* ---- console tee to "rsync log" (linked with --wrap=_consolewrite) ---- */

ssize_t	__real__consolewrite(int, const void *, size_t);
static short logref;

ssize_t
__wrap__consolewrite(int fd, const void *buf, size_t len)
{
	char conv[512], crbuf[512];
	const unsigned char *p = buf;
	size_t chunk, n, i;
	long cnt;

	while (len > 0) {
		chunk = len > sizeof(conv) ? sizeof(conv) : len;
		n = engine_utf8_to_macroman(p, chunk, conv);
		if (logref != 0) {
			/* A Mac text file: CR line ends. */
			memcpy(crbuf, conv, n);
			for (i = 0; i < n; i++)
				if (crbuf[i] == '\n')
					crbuf[i] = '\r';
			cnt = (long)n;
			FSWrite(logref, &cnt, crbuf);
			/* Flush so the log survives a crash. */
			FlushVol(NULL, 0);
		}
		__real__consolewrite(fd, conv, n);
		p += chunk;
		len -= chunk;
	}
	return (ssize_t)(p - (const unsigned char *)buf);
}

static void
log_open(void)
{
	FSDelete("\prsync log", 0);
	if (Create("\prsync log", 0, 'ttxt', 'TEXT') == noErr &&
	    FSOpen("\prsync log", 0, &logref) != noErr)
		logref = 0;
}

static void
log_close(void)
{
	if (logref != 0) {
		FSClose(logref);
		logref = 0;
		FlushVol(NULL, 0);
	}
}

/* ---- main ------------------------------------------------------------------ */

static int
read_line(FILE *f, char *buf, size_t len)
{
	size_t n = 0;
	int c;

	while ((c = getc(f)) != EOF && c != '\r' && c != '\n')
		if (n + 1 < len)
			buf[n++] = (char)c;
	buf[n] = '\0';
	return (c == EOF && n == 0) ? -1 : 0;
}

static void
banner(void)
{
	printf("rsync for Mac OS (openrsync, protocol 29)\n");
	printf("Type a command, e.g.\n");
	printf("  rsync -av --delete server::module/dir \"Macintosh HD:Projects:\"\n");
	printf("HFS paths (Disk:Folder:) are fine.  $(NAME) comes from "
	    "\"rsync prefs\".\n");
	printf("\"rsync --daemon\" serves the modules in \"rsyncd.conf\" (Cmd-. stops).\n");
	printf("Type \"quit\" to leave.\n\n");
}

int
main(void)
{
	char line[LINEMAX];
	struct stat st;
	FILE *script;
	size_t len;
	int rc;

	engine_grow_stack(256L * 1024);

	if ((script = fopen("rsync script", "r")) != NULL) {
		log_open();
		banner();
		/* Lines may end in CR (Mac) or LF (Unix). */
		while (read_line(script, line, sizeof(line)) != -1) {
			if (line[0] == '\0' || line[0] == '#')
				continue;
			printf("rsync> %s\n", line);
			if (engine_builtin(line))
				continue;
			rc = engine_run_line(line);
			printf("[exit %d]\n", rc);
		}
		fclose(script);
		log_close();
		if (stat("rsync autoquit", &st) == 0)
			ShutDwnPower();
		return 0;
	}

	banner();
	for (;;) {
		printf("rsync> ");
		fflush(stdout);
		if (fgets(line, sizeof(line), stdin) == NULL)
			break;
		len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0)
			break;
		if (engine_builtin(line))
			continue;
		rc = engine_run_line(line);
		if (rc != 0)
			printf("[exit %d]\n", rc);
	}
	return 0;
}
