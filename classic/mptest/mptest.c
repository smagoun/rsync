/*
 * MPTest: self-test of the macposix layer on a real Mac OS (QEMU q800).
 *
 * Writes "MPTest Log" next to the application.  "MPTest Config" may hold
 * "a.b.c.d port" of an rsync daemon (default 10.0.2.2 8873, the QEMU host).
 * If "MPTest AutoQuit" exists the Mac shuts down afterwards, so
 * classic/emu/boot-test.sh can run it unattended from Startup Items.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <Multiverse.h>

#include "hfsname.h"
#include "netatalk.h"

static FILE *logf_;
static int fails, passes;

static void
say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	if (logf_ != NULL) {
		va_start(ap, fmt);
		vfprintf(logf_, fmt, ap);
		va_end(ap);
		fflush(logf_);
	}
}

#define CHECK(what, cond) do {						\
	if (cond) {							\
		passes++;						\
		say("ok   - %s\n", what);				\
	} else {							\
		fails++;						\
		say("FAIL - %s (errno %d)\n", what, errno);		\
	}								\
} while (0)

static int
write_file(const char *path, const char *data)
{
	int fd;
	ssize_t n;

	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) == -1)
		return -1;
	n = write(fd, data, strlen(data));
	close(fd);
	return n == (ssize_t)strlen(data) ? 0 : -1;
}

static int
read_file(const char *path, char *buf, size_t len)
{
	int fd;
	ssize_t n;

	if ((fd = open(path, O_RDONLY)) == -1)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return (int)n;
}

static int
dir_has(const char *dir, const char *name)
{
	struct dirent *de;
	DIR *d;
	int found = 0;

	if ((d = opendir(dir)) == NULL)
		return 0;
	while ((de = readdir(d)) != NULL)
		if (strcmp(de->d_name, name) == 0)
			found = 1;
	closedir(d);
	return found;
}

static void
test_files(void)
{
	char cwd[PATH_MAX], buf[256];
	struct stat st;
	struct timespec ts[2];
	int fd, dfd;

	say("# files\n");
	CHECK("getcwd", getcwd(cwd, sizeof(cwd)) != NULL);
	say("     cwd = %s\n", cwd);

	/* Leftovers from an earlier run. */
	unlink("mpt/sub/b.txt");
	unlink("mpt/a.txt");
	rmdir("mpt/sub");
	rmdir("mpt");

	CHECK("mkdir", mkdir("mpt", 0755) == 0);
	CHECK("mkdir again fails with EEXIST",
	    mkdir("mpt", 0755) == -1 && errno == EEXIST);
	CHECK("write a file", write_file("mpt/a.txt", "hello, world\n") == 0);
	CHECK("stat size", stat("mpt/a.txt", &st) == 0 && st.st_size == 13 &&
	    S_ISREG(st.st_mode));
	CHECK("read back", read_file("mpt/a.txt", buf, sizeof(buf)) == 13 &&
	    strcmp(buf, "hello, world\n") == 0);

	ts[0].tv_sec = ts[1].tv_sec = 1700000000;	/* 2023-11-14 22:13 UTC */
	ts[0].tv_nsec = ts[1].tv_nsec = 0;
	CHECK("utimensat", utimensat(AT_FDCWD, "mpt/a.txt", ts, 0) == 0);
	CHECK("mtime round-trips", stat("mpt/a.txt", &st) == 0 &&
	    st.st_mtime == 1700000000);

	/* The *at() calls relative to a directory descriptor. */
	dfd = open("mpt", O_RDONLY | O_DIRECTORY);
	CHECK("open directory", dfd != -1);
	CHECK("mkdirat", mkdirat(dfd, "sub", 0755) == 0);
	fd = openat(dfd, "sub/b.txt", O_RDWR | O_CREAT | O_EXCL, 0644);
	CHECK("openat create", fd != -1);
	CHECK("pwrite", pwrite(fd, "0123456789", 10, 0) == 10);
	CHECK("pread", pread(fd, buf, 4, 3) == 4 && memcmp(buf, "3456", 4) == 0);
	CHECK("lseek end", lseek(fd, 0, SEEK_END) == 10);
	CHECK("ftruncate", ftruncate(fd, 5) == 0 && fstat(fd, &st) == 0 &&
	    st.st_size == 5);
	/* A date set while open must survive close. */
	ts[1].tv_sec = 1600000000;
	CHECK("futimens while open", futimens(fd, ts) == 0);
	CHECK("close", close(fd) == 0);
	CHECK("date applied after close", fstatat(dfd, "sub/b.txt", &st, 0)
	    == 0 && st.st_mtime == 1600000000);
	CHECK("readdir sees sub", dir_has("mpt", "sub"));
	CHECK("readdir sees a.txt", dir_has("mpt", "a.txt"));
	CHECK("stat of missing file is ENOENT",
	    stat("mpt/nope", &st) == -1 && errno == ENOENT);
	close(dfd);
}

static void
test_names(void)
{
	const char *list[2];
	char path[PATH_MAX], cwd[PATH_MAX];
	const char *longname = "ThisIsAVeryLongSourceFileName_Helpers.c";

	say("# names\n");
	snprintf(path, sizeof(path), "mpt/%s", longname);
	CHECK("create a 39-character name", write_file(path, "x") == 0);
	CHECK("stat it back by its long name", access(path, F_OK) == 0);
	CHECK("readdir shows the mangled HFS name without the file list",
	    !dir_has("mpt", longname));
	getcwd(cwd, sizeof(cwd));
	snprintf(path, sizeof(path), "%s/mpt/%s", cwd, longname);
	list[0] = path;
	hfs_names_register(list, 1);
	snprintf(path, sizeof(path), "%s/mpt", cwd);
	CHECK("readdir maps it back once registered", dir_has(path, longname));
	hfs_names_reset();
	snprintf(path, sizeof(path), "mpt/%s", longname);
	CHECK("unlink by long name", unlink(path) == 0);

	CHECK("MacRoman name (Caf\xc3\xa9 \xe2\x84\xa2)",
	    write_file("mpt/Caf\xc3\xa9 \xe2\x84\xa2", "x") == 0 &&
	    dir_has("mpt", "Caf\xc3\xa9 \xe2\x84\xa2"));
	CHECK("colon in a name is stored as slash",
	    write_file("mpt/a:b", "x") == 0 && dir_has("mpt", "a:b"));
	unlink("mpt/Caf\xc3\xa9 \xe2\x84\xa2");
	unlink("mpt/a:b");
}

static void
test_forks(void)
{
	uint8_t finfo[32] = "APPLTEST";
	struct nt_meta m;
	struct stat st;
	char rsrc[3000];
	int fd, i;

	say("# resource forks and rename\n");
	for (i = 0; i < (int)sizeof(rsrc); i++)
		rsrc[i] = (char)i;
	fd = open("mpt/rsrc.src", O_RDWR | O_CREAT | O_TRUNC, 0644);
	write(fd, rsrc, sizeof(rsrc));
	CHECK("ntb_set writes fork + Finder info",
	    ntb_set(AT_FDCWD, "mpt/a.txt", finfo, fd, 0, sizeof(rsrc),
	    1650000000) == 0);
	close(fd);
	unlink("mpt/rsrc.src");
	CHECK("ntb_get reads them back", ntb_get(AT_FDCWD, "mpt/a.txt", &m)
	    == 0 && m.rsrc_len == sizeof(rsrc) &&
	    memcmp(m.finfo, "APPLTEST", 8) == 0 &&
	    m.sidecar_mtime == 1650000000);
	CHECK("data mtime unchanged by fork write",
	    stat("mpt/a.txt", &st) == 0 && st.st_mtime == 1700000000);

	/* rsync's temp-file-then-rename must keep the fork. */
	CHECK("write temp file", write_file("mpt/.a.txt.XXXX", "new data") == 0);
	CHECK("rename over the file", rename("mpt/.a.txt.XXXX", "mpt/a.txt")
	    == 0);
	CHECK("new data in place", stat("mpt/a.txt", &st) == 0 &&
	    st.st_size == 8);
	CHECK("fork and Finder info kept", ntb_get(AT_FDCWD, "mpt/a.txt", &m)
	    == 0 && m.rsrc_len == sizeof(rsrc) &&
	    memcmp(m.finfo, "APPLTEST", 8) == 0 &&
	    m.sidecar_mtime == 1650000000);
	CHECK("temp file gone", access("mpt/.a.txt.XXXX", F_OK) == -1);
	CHECK("ntb_clear", ntb_clear(AT_FDCWD, "mpt/a.txt") == 0 &&
	    ntb_get(AT_FDCWD, "mpt/a.txt", &m) == 0 && m.rsrc_len == 0);

	CHECK("rename into a subfolder",
	    rename("mpt/a.txt", "mpt/sub/a2.txt") == 0 &&
	    access("mpt/sub/a2.txt", F_OK) == 0);
	CHECK("unlink files", unlink("mpt/sub/a2.txt") == 0 &&
	    unlink("mpt/sub/b.txt") == 0);
	CHECK("rmdir non-empty fails", rmdir("mpt") == -1 &&
	    errno == ENOTEMPTY);
	CHECK("rmdir", rmdir("mpt/sub") == 0 && rmdir("mpt") == 0);
}

static void
test_net(void)
{
	struct addrinfo hints, *ai = NULL;
	struct pollfd pfd;
	char host[64] = "10.0.2.2", port[16] = "8873", buf[512];
	FILE *cf;
	ssize_t n;
	int s, rc;
	unsigned long t0;

	say("# network\n");
	if ((cf = fopen("MPTest Config", "r")) != NULL) {
		if (fscanf(cf, "%63s %15s", host, port) != 2)
			say("     (bad MPTest Config)\n");
		fclose(cf);
	}
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	CHECK("getaddrinfo numeric", getaddrinfo(host, port, &hints, &ai) == 0);
	if (ai == NULL)
		return;
	s = socket(AF_INET, SOCK_STREAM, 0);
	CHECK("socket", s != -1);
	t0 = TickCount();
	CHECK("connect", connect(s, ai->ai_addr, ai->ai_addrlen) == 0);
	say("     connected in %lu ticks\n", TickCount() - t0);
	freeaddrinfo(ai);
	pfd.fd = s;
	pfd.events = POLLIN;
	CHECK("poll for greeting", poll(&pfd, 1, 10000) == 1 &&
	    (pfd.revents & POLLIN));
	n = read(s, buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = '\0';
	CHECK("greeting received", n > 0 && strncmp(buf, "@RSYNCD:", 8) == 0);
	say("     << %s", buf);
	CHECK("send greeting + #list",
	    write(s, "@RSYNCD: 29.0\n#list\n", 20) == 20);
	buf[0] = '\0';
	for (n = 0; n < (ssize_t)sizeof(buf) - 1; ) {
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 5000) != 1)
			break;
		rc = (int)read(s, buf + n, sizeof(buf) - 1 - n);
		if (rc <= 0)
			break;
		n += rc;
		buf[n] = '\0';
		if (strstr(buf, "@RSYNCD: EXIT"))
			break;
	}
	CHECK("module list ends with EXIT", strstr(buf, "@RSYNCD: EXIT") != NULL);
	say("     << %s", buf);
	n = read(s, buf, sizeof(buf));
	CHECK("EOF after the server closes", n == 0);
	CHECK("close socket", close(s) == 0);

	rc = getaddrinfo("example.com", "873", &hints, &ai);
	say("     DNS example.com: %s\n", rc == 0 ? "resolved" :
	    gai_strerror(rc));
	if (rc == 0)
		freeaddrinfo(ai);
}

int
main(void)
{
	struct stat st;

	logf_ = fopen("MPTest Log", "w");
	say("MPTest: macposix self-test\n");
	test_files();
	test_names();
	test_forks();
	test_net();
	say("%d passed, %d failed\n", passes, fails);
	if (logf_ != NULL)
		fclose(logf_);
	if (stat("MPTest AutoQuit", &st) == 0)
		ShutDwnPower();
	return 0;
}
