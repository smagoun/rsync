/*
 * macposix.h: the POSIX subset openrsync needs, implemented for Classic
 * Mac OS on the File Manager (files, folders) and MacTCP (sockets).
 *
 * Retro68's newlib provides stdio, strings, malloc, time and a few file
 * calls; everything below that newlib lacks or that must go through our
 * own descriptor table is declared here.  The standard header names
 * (<dirent.h>, <poll.h>, <sys/socket.h>, ...) in this directory just
 * include this file.
 *
 * Paths are POSIX-style, UTF-8: "/Volume Name/Folder/file" or relative to
 * the current directory (the application's folder at launch).  The front
 * end converts HFS paths typed by the user ("Volume:Folder") to this form.
 * Each component is mapped to its HFS name by hfsname.c.
 */
#ifndef MACPOSIX_H
#define MACPOSIX_H

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- descriptors and files ---------------------------------------- */

#define MP_OPEN_MAX	64

int	mp_open(const char *, int, ...);
int	mp_openat(int, const char *, int, ...);
int	mp_close(int);
ssize_t	mp_read(int, void *, size_t);
ssize_t	mp_write(int, const void *, size_t);
ssize_t	mp_pread(int, void *, size_t, off_t);
ssize_t	mp_pwrite(int, const void *, size_t, off_t);
off_t	mp_lseek(int, off_t, int);
int	mp_ftruncate(int, off_t);
int	mp_fsync(int);
int	mp_dup(int);
int	mp_dup2(int, int);
int	mp_fcntl(int, int, ...);
int	mp_isatty(int);

int	mp_stat(const char *, struct stat *);
int	mp_lstat(const char *, struct stat *);
int	mp_fstat(int, struct stat *);
int	mp_fstatat(int, const char *, struct stat *, int);
int	mp_access(const char *, int);
int	mp_faccessat(int, const char *, int, int);
int	mp_mkdir(const char *, mode_t);
int	mp_mkdirat(int, const char *, mode_t);
int	mp_rmdir(const char *);
int	mp_unlink(const char *);
int	mp_unlinkat(int, const char *, int);
int	mp_rename(const char *, const char *);
int	mp_renameat(int, const char *, int, const char *);
int	mp_chdir(const char *);
int	mp_fchdir(int);
char	*mp_getcwd(char *, size_t);
char	*mp_realpath(const char *, char *);
int	mp_mkstemp(char *);
int	mp_mkstempat(int, char *);
int	mp_tmpfd(void);
/* Absolute, normalized form of a path that need not exist. */
int	mp_abspath(const char *, char *);

/* Times: HFS has 1-second dates in local time; see time.c. */
#ifndef UTIME_NOW
#define UTIME_NOW	((1L << 30) - 1L)
#define UTIME_OMIT	((1L << 30) - 2L)
#endif
int	mp_utimensat(int, const char *, const struct timespec [2], int);
int	mp_futimens(int, const struct timespec [2]);
int	mp_utimes(const char *, const struct timeval [2]);

/* Ownership, modes, links: HFS has none of these; harmless no-ops. */
int	mp_chmod(const char *, mode_t);
int	mp_fchmod(int, mode_t);
int	mp_fchmodat(int, const char *, mode_t, int);
int	mp_fchown(int, uid_t, gid_t);
int	mp_fchownat(int, const char *, uid_t, gid_t, int);
int	mp_lchown(const char *, uid_t, gid_t);
int	mp_link(const char *, const char *);
int	mp_linkat(int, const char *, int, const char *, int);
int	mp_symlink(const char *, const char *);
int	mp_symlinkat(const char *, int, const char *);
ssize_t	mp_readlink(const char *, char *, size_t);
ssize_t	mp_readlinkat(int, const char *, char *, size_t);
int	mp_mknod(const char *, mode_t, dev_t);
int	mp_mknodat(int, const char *, mode_t, dev_t);
int	mp_mkfifo(const char *, mode_t);
int	mp_mkfifoat(int, const char *, mode_t);

#ifndef AT_FDCWD
#define AT_FDCWD		-100
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW	0x0100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR		0x0200
#endif
#ifndef AT_SYMLINK_FOLLOW
#define AT_SYMLINK_FOLLOW	0x0400
#endif
#ifndef AT_EACCESS
#define AT_EACCESS		0x0800
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY		0x200000
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW		0x100000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC		0x40000
#endif
#ifndef O_SYMLINK
#define O_SYMLINK		0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK		0x4000
#endif

/* ---- directories ---------------------------------------------------- */

#define DT_UNKNOWN	0
#define DT_DIR		4
#define DT_REG		8
#define DT_LNK		10
#define IFTODT(mode)	(((mode) & 0170000) >> 12)
#define DTTOIF(dt)	((dt) << 12)

struct dirent {
	ino_t		d_ino;
	unsigned char	d_type;
	unsigned short	d_namlen;
	char		d_name[256];	/* UTF-8; an HFS name is <= 93 bytes */
};
typedef struct mp_dir DIR;

DIR		*mp_opendir(const char *);
DIR		*mp_fdopendir(int);
struct dirent	*mp_readdir(DIR *);
void		 mp_rewinddir(DIR *);
int		 mp_closedir(DIR *);
int		 mp_dirfd(DIR *);

/* fts(3) comes from openrsync's compats.c (oconfigure), built on the above. */

/* ---- memory mapping: not available; openrsync falls back to reads --- */

#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define MAP_SHARED	0x1
#define MAP_PRIVATE	0x2
#define MAP_FAILED	((void *)-1)
void	*mp_mmap(void *, size_t, int, int, int, off_t);
int	 mp_munmap(void *, size_t);

/* ---- sockets (MacTCP; TCP over IPv4 only) --------------------------- */

typedef uint32_t	socklen_t;
typedef uint16_t	sa_family_t;
typedef uint16_t	in_port_t;
typedef uint32_t	in_addr_t;

#define AF_UNSPEC	0
#define AF_UNIX		1
#define AF_LOCAL	AF_UNIX
#define AF_INET		2
#define AF_INET6	30
#define PF_UNSPEC	AF_UNSPEC
#define PF_UNIX		AF_UNIX
#define PF_INET		AF_INET
#define PF_INET6	AF_INET6
#define SOCK_STREAM	1
#define SOCK_DGRAM	2
#define SOCK_NONBLOCK	0x4000
#define SOCK_CLOEXEC	0x40000
#define IPPROTO_IP	0
#define IPPROTO_TCP	6
#define IPPROTO_IPV6	41
#define SOL_SOCKET	0xffff
#define SO_KEEPALIVE	0x0008
#define SO_REUSEADDR	0x0004
#define SO_SNDBUF	0x1001
#define SO_RCVBUF	0x1002
#define SO_SNDLOWAT	0x1003
#define SO_RCVLOWAT	0x1004
#define SO_SNDTIMEO	0x1005
#define SO_RCVTIMEO	0x1006
#define SO_ERROR	0x1007
#define TCP_NODELAY	0x01
#define IPTOS_LOWDELAY	0x10
#define IPTOS_THROUGHPUT 0x08
#define IP_TOS		3
#define SHUT_RD		0
#define SHUT_WR		1
#define SHUT_RDWR	2
#define INADDR_ANY	((in_addr_t)0)
#define INADDR_LOOPBACK	((in_addr_t)0x7f000001)
#define INET_ADDRSTRLEN		16
#define INET6_ADDRSTRLEN	46

struct sockaddr {
	uint8_t		sa_len;
	sa_family_t	sa_family;
	char		sa_data[14];
};
struct sockaddr_storage {
	uint8_t		ss_len;
	sa_family_t	ss_family;
	char		ss_pad[126];
};
struct in_addr {
	in_addr_t	s_addr;
};
struct sockaddr_in {
	uint8_t		sin_len;
	sa_family_t	sin_family;
	in_port_t	sin_port;
	struct in_addr	sin_addr;
	char		sin_zero[8];
};
struct in6_addr {
	uint8_t		s6_addr[16];
};
struct sockaddr_in6 {
	uint8_t		sin6_len;
	sa_family_t	sin6_family;
	in_port_t	sin6_port;
	uint32_t	sin6_flowinfo;
	struct in6_addr	sin6_addr;
	uint32_t	sin6_scope_id;
};
struct sockaddr_un {
	uint8_t		sun_len;
	sa_family_t	sun_family;
	char		sun_path[104];
};
extern const struct in6_addr in6addr_any;
#define IN6_IS_ADDR_V4MAPPED(a)	0
#define IN6_IS_ADDR_UNSPECIFIED(a) 0

struct iovec {
	void	*iov_base;
	size_t	 iov_len;
};

/* Big-endian CPU: network order is host order. */
#define htons(x)	((uint16_t)(x))
#define ntohs(x)	((uint16_t)(x))
#define htonl(x)	((uint32_t)(x))
#define ntohl(x)	((uint32_t)(x))

int	mp_socket(int, int, int);
int	mp_socketpair(int, int, int, int [2]);
int	mp_connect(int, const struct sockaddr *, socklen_t);
int	mp_bind(int, const struct sockaddr *, socklen_t);
int	mp_listen(int, int);
int	mp_accept(int, struct sockaddr *, socklen_t *);
int	mp_shutdown(int, int);
int	mp_setsockopt(int, int, int, const void *, socklen_t);
int	mp_getsockopt(int, int, int, void *, socklen_t *);
int	mp_getpeername(int, struct sockaddr *, socklen_t *);
int	mp_getsockname(int, struct sockaddr *, socklen_t *);
ssize_t	mp_send(int, const void *, size_t, int);
ssize_t	mp_recv(int, void *, size_t, int);
ssize_t	mp_writev(int, const struct iovec *, int);
const char *mp_inet_ntop(int, const void *, char *, socklen_t);
int	mp_inet_pton(int, const char *, void *);

/* Name lookup through MacTCP DNR. */
struct addrinfo {
	int		 ai_flags;
	int		 ai_family;
	int		 ai_socktype;
	int		 ai_protocol;
	socklen_t	 ai_addrlen;
	struct sockaddr	*ai_addr;
	char		*ai_canonname;
	struct addrinfo	*ai_next;
};
struct servent {
	char	*s_name;
	char   **s_aliases;
	int	 s_port;
	char	*s_proto;
};
#define AI_PASSIVE	0x01
#define AI_CANONNAME	0x02
#define AI_NUMERICHOST	0x04
#define AI_NUMERICSERV	0x10
#define NI_NUMERICHOST	0x01
#define NI_NUMERICSERV	0x02
#define NI_MAXHOST	1025
#define NI_MAXSERV	32
#define EAI_AGAIN	2
#define EAI_FAIL	4
#define EAI_FAMILY	5
#define EAI_MEMORY	6
#define EAI_NONAME	8
#define EAI_SERVICE	9
#define EAI_SOCKTYPE	10
#define EAI_SYSTEM	11
int	mp_getaddrinfo(const char *, const char *, const struct addrinfo *,
	    struct addrinfo **);
void	mp_freeaddrinfo(struct addrinfo *);
const char *mp_gai_strerror(int);
int	mp_getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t,
	    char *, socklen_t, int);
struct servent *mp_getservbyname(const char *, const char *);

/* ---- poll ----------------------------------------------------------- */

struct pollfd {
	int	fd;
	short	events;
	short	revents;
};
typedef unsigned int nfds_t;
#define POLLIN		0x0001
#define POLLPRI		0x0002
#define POLLOUT		0x0004
#define POLLERR		0x0008
#define POLLHUP		0x0010
#define POLLNVAL	0x0020
#define POLLRDNORM	0x0040
#define POLLWRNORM	POLLOUT
#ifndef INFTIM
#define INFTIM		(-1)
#endif
int	mp_poll(struct pollfd *, nfds_t, int);

/* ---- processes, signals, users: single process, single user --------- */

typedef jmp_buf sigjmp_buf;
#define sigsetjmp(env, save)	setjmp(env)
#define siglongjmp(env, val)	longjmp(env, val)

pid_t	mp_getpid(void);
pid_t	mp_fork(void);
int	mp_execvp(const char *, char *const []);
pid_t	mp_waitpid(pid_t, int *, int);
int	mp_kill(pid_t, int);
mode_t	mp_umask(mode_t);
uid_t	mp_getuid(void);
uid_t	mp_geteuid(void);
gid_t	mp_getgid(void);
gid_t	mp_getegid(void);
int	mp_setsid(void);
int	mp_chroot(const char *);
unsigned mp_sleep(unsigned);
int	mp_usleep(useconds_t);
int	mp_pipe(int [2]);

/* BSD getopt_long (getopt_long.c): set optreset = 1 to restart. */
extern int optreset;

/* Called often in long loops: lets the Mac breathe and checks Cmd-. */
void	mp_yield(void);
/* Set by mp_yield() when the user presses Cmd-.; waits return EINTR. */
extern int mp_interrupted;
/* GUI hook: every event seen while rsync waits (see misc.c). */
struct EventRecord;
extern void (*mp_event_hook)(struct EventRecord *);
/* Ask a listening server to return between connections (poll EINTR). */
extern int mp_listen_pause;
unsigned long mp_local_ip(void);
int	mp_vsnprintf(char *, size_t, const char *, va_list);
extern int mp_tracing;

/* ---- syslog: goes to the console window ----------------------------- */

#define LOG_EMERG	0
#define LOG_ALERT	1
#define LOG_CRIT	2
#define LOG_ERR		3
#define LOG_WARNING	4
#define LOG_NOTICE	5
#define LOG_INFO	6
#define LOG_DEBUG	7
#define LOG_PID		0x01
#define LOG_NDELAY	0x08
#define LOG_DAEMON	(3 << 3)
#define LOG_USER	(1 << 3)
#define LOG_KERN	(0 << 3)
#define LOG_MAIL	(2 << 3)
#define LOG_AUTH	(4 << 3)
#define LOG_SYSLOG	(5 << 3)
#define LOG_LPR		(6 << 3)
#define LOG_NEWS	(7 << 3)
#define LOG_UUCP	(8 << 3)
#define LOG_CRON	(9 << 3)
#define LOG_AUTHPRIV	(10 << 3)
#define LOG_FTP		(11 << 3)
#define LOG_LOCAL0	(16 << 3)
#define LOG_LOCAL1	(17 << 3)
#define LOG_LOCAL2	(18 << 3)
#define LOG_LOCAL3	(19 << 3)
#define LOG_LOCAL4	(20 << 3)
#define LOG_LOCAL5	(21 << 3)
#define LOG_LOCAL6	(22 << 3)
#define LOG_LOCAL7	(23 << 3)
void	mp_openlog(const char *, int, int);
void	mp_syslog(int, const char *, ...);
void	mp_vsyslog(int, const char *, va_list);
void	mp_closelog(void);
#define openlog		mp_openlog
#define syslog		mp_syslog
#define vsyslog		mp_vsyslog
#define closelog	mp_closelog

/* ---- misc ----------------------------------------------------------- */

#ifndef roundup
#define roundup(x, y)	((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef MIN
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))
#endif
#ifndef PATH_MAX
#define PATH_MAX	1024
#endif
#ifndef NAME_MAX
#define NAME_MAX	255
#endif
#ifndef PIPE_BUF
#define PIPE_BUF	512
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX	0x7fffffffL
#endif

/* Devices do not exist on HFS. */
#define major(d)	(((d) >> 8) & 0xff)
#define minor(d)	((d) & 0xff)
#define makedev(a, b)	((dev_t)(((a) << 8) | (b)))

/* Byte order: the 68k is big-endian. */
#define htobe16(x)	((uint16_t)(x))
#define htobe32(x)	((uint32_t)(x))
#define htobe64(x)	((uint64_t)(x))
#define be16toh(x)	((uint16_t)(x))
#define be32toh(x)	((uint32_t)(x))
#define be64toh(x)	((uint64_t)(x))
#define htole16(x)	__builtin_bswap16(x)
#define htole32(x)	__builtin_bswap32(x)
#define htole64(x)	__builtin_bswap64(x)
#define le16toh(x)	__builtin_bswap16(x)
#define le32toh(x)	__builtin_bswap32(x)
#define le64toh(x)	__builtin_bswap64(x)

/* ioctl: only FIONREAD (bytes waiting on a socket). */
#define FIONREAD	0x4004667f
int	mp_ioctl(int, unsigned long, ...);
#define ioctl		mp_ioctl

const char *getprogname(void);
void	setprogname(const char *);
ssize_t	getdelim(char **, size_t *, int, FILE *);
#ifndef MAXPATHLEN
#define MAXPATHLEN	PATH_MAX
#endif

int	asprintf(char **, const char *, ...);
int	vasprintf(char **, const char *, va_list);
ssize_t	getline(char **, size_t *, FILE *);

/* readpassphrase(3): prompts in the console window without echo. */
#define RPP_ECHO_OFF	0x00
#define RPP_ECHO_ON	0x01
#define RPP_REQUIRE_TTY	0x02
#define RPP_STDIN	0x20
char	*mp_readpassphrase(const char *, char *, size_t, int);
#define readpassphrase	mp_readpassphrase

#ifdef __cplusplus
}
#endif

/*
 * Route the POSIX names to our implementations.  Done last so that the
 * newlib prototypes above (pulled in by <unistd.h> etc.) are not renamed.
 * macposix's own sources define MACPOSIX_IMPL to see the real names.
 */
#ifndef MACPOSIX_IMPL
#define open		mp_open
#define openat		mp_openat
#define close		mp_close
#define read		mp_read
#define write		mp_write
#define pread		mp_pread
#define pwrite		mp_pwrite
#define lseek		mp_lseek
#define ftruncate	mp_ftruncate
#define fsync		mp_fsync
#define dup		mp_dup
#define dup2		mp_dup2
#define fcntl		mp_fcntl
#define isatty		mp_isatty
#define stat(p, s)	mp_stat((p), (s))
#define lstat		mp_lstat
#define fstat		mp_fstat
#define fstatat		mp_fstatat
#define access		mp_access
#define faccessat	mp_faccessat
#define mkdir		mp_mkdir
#define mkdirat		mp_mkdirat
#define rmdir		mp_rmdir
#define unlink		mp_unlink
#define unlinkat	mp_unlinkat
#define rename		mp_rename
#define renameat	mp_renameat
#define chdir		mp_chdir
#define fchdir		mp_fchdir
#define getcwd		mp_getcwd
#define realpath	mp_realpath
#define mkstemp		mp_mkstemp
#define utimensat	mp_utimensat
#define futimens	mp_futimens
#define utimes		mp_utimes
#define chmod		mp_chmod
#define fchmod		mp_fchmod
#define fchmodat	mp_fchmodat
#define fchown		mp_fchown
#define fchownat	mp_fchownat
#define lchown		mp_lchown
#define link		mp_link
#define linkat		mp_linkat
#define symlink		mp_symlink
#define symlinkat	mp_symlinkat
#define readlink	mp_readlink
#define readlinkat	mp_readlinkat
#define mknod		mp_mknod
#define mknodat		mp_mknodat
#define mkfifo		mp_mkfifo
#define mkfifoat	mp_mkfifoat
#define opendir		mp_opendir
#define fdopendir	mp_fdopendir
#define readdir		mp_readdir
#define rewinddir	mp_rewinddir
#define closedir	mp_closedir
#define dirfd		mp_dirfd
#define mmap		mp_mmap
#define munmap		mp_munmap
#define socket		mp_socket
#define socketpair	mp_socketpair
#define connect		mp_connect
#define bind		mp_bind
#define listen		mp_listen
#define accept		mp_accept
#define shutdown	mp_shutdown
#define setsockopt	mp_setsockopt
#define getsockopt	mp_getsockopt
#define getpeername	mp_getpeername
#define getsockname	mp_getsockname
#define send		mp_send
#define recv		mp_recv
#define writev		mp_writev
#define inet_ntop	mp_inet_ntop
#define inet_pton	mp_inet_pton
#define getaddrinfo	mp_getaddrinfo
#define freeaddrinfo	mp_freeaddrinfo
#define gai_strerror	mp_gai_strerror
#define getnameinfo	mp_getnameinfo
#define getservbyname	mp_getservbyname
#define poll		mp_poll
#define getpid		mp_getpid
#define fork		mp_fork
#define execvp		mp_execvp
#define waitpid		mp_waitpid
#define kill		mp_kill
#define umask		mp_umask
#define getuid		mp_getuid
#define geteuid		mp_geteuid
#define getgid		mp_getgid
#define getegid		mp_getegid
#define setsid		mp_setsid
#define chroot		mp_chroot
#define sleep		mp_sleep
#define usleep		mp_usleep
#define pipe		mp_pipe
#endif /* !MACPOSIX_IMPL */

#endif /* MACPOSIX_H */
