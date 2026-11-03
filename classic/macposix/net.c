/*
 * net.c: TCP sockets, poll() and name lookup on MacTCP.
 *
 * Open Transport on Mac OS 8.1 serves the MacTCP driver interface
 * (".IPP"), which Retro68's open-source headers describe; OT's own API
 * would need Apple's SDK.  Only IPv4 TCP client sockets are supported,
 * which is all an rsync daemon client needs.  Calls are synchronous; the
 * waiting loops call mp_yield() so other applications keep running.
 */
#include "mpint.h"

#include <stdlib.h>
#include <string.h>

#define RCVBUF_SIZE	32768

struct mp_sock {
	StreamPtr	stream;
	Ptr		rbuf;
	int		connected;
	int		rdshut;		/* remote closed, nothing left */
	ip_addr		rhost;
	tcp_port	rport;
};

static short drv;

const struct in6_addr in6addr_any;

static int
open_driver(void)
{
	OSErr err;

	if (drv != 0)
		return 0;
	if ((err = OpenDriver("\p.IPP", &drv)) != noErr) {
		drv = 0;
		errno = ENETDOWN;
		return -1;
	}
	return 0;
}

static OSErr
tcp(TCPiopb *pb, short code, StreamPtr s)
{
	pb->ioCRefNum = drv;
	pb->csCode = code;
	pb->tcpStream = s;
	return PBControlSync((ParmBlkPtr)pb);
}

static int
tcperr(OSErr err)
{
	switch (err) {
	case noErr:
		return 0;
	case openFailed:
	case connectionTerminated:
		errno = ECONNREFUSED;
		break;
	case commandTimeout:
		errno = ETIMEDOUT;
		break;
	case connectionClosing:
	case connectionDoesntExist:
		errno = EPIPE;
		break;
	case insufficientResources:
		errno = ENOBUFS;
		break;
	case ipBadLapErr:
	case ipBadCnfgErr:
	case ipNoCnfgErr:
	case ipLoadErr:
		errno = ENETDOWN;
		break;
	default:
		errno = EIO;
		break;
	}
	return -1;
}

/* ---- sockets --------------------------------------------------------- */

int
mp_socket(int domain, int type, int proto)
{
	struct mp_sock *s;
	struct mp_fd *f;
	TCPiopb pb;
	OSErr err;
	int fd;

	(void)proto;
	if (domain != AF_INET || (type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
	    != SOCK_STREAM) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if (open_driver() == -1)
		return -1;
	if ((s = calloc(1, sizeof(*s))) == NULL)
		return -1;
	if ((s->rbuf = NewPtr(RCVBUF_SIZE)) == NULL) {
		free(s);
		errno = ENOMEM;
		return -1;
	}
	memset(&pb, 0, sizeof(pb));
	pb.csParam.create.rcvBuff = s->rbuf;
	pb.csParam.create.rcvBuffLen = RCVBUF_SIZE;
	if ((err = tcp(&pb, TCPCreate, NULL)) != noErr) {
		DisposePtr(s->rbuf);
		free(s);
		return tcperr(err);
	}
	s->stream = pb.tcpStream;
	if ((fd = mp_fdalloc(&f)) == -1) {
		mp_sock_close(s);
		return -1;
	}
	f->type = MPF_SOCK;
	f->flags = O_RDWR;
	f->sock = s;
	return fd;
}

static struct mp_sock *
sock(int fd)
{
	struct mp_fd *f;

	if ((f = mp_fdget(fd)) == NULL)
		return NULL;
	if (f->type != MPF_SOCK) {
		errno = ENOTSOCK;
		return NULL;
	}
	return f->sock;
}

int
mp_connect(int fd, const struct sockaddr *sa, socklen_t len)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
	struct mp_sock *s;
	TCPiopb pb;
	OSErr err;

	if ((s = sock(fd)) == NULL)
		return -1;
	if (len < sizeof(*sin) || sin->sin_family != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	memset(&pb, 0, sizeof(pb));
	pb.csParam.open.remoteHost = sin->sin_addr.s_addr;
	pb.csParam.open.remotePort = sin->sin_port;
	pb.csParam.open.ulpTimeoutValue = 60;
	pb.csParam.open.ulpTimeoutAction = 1;	/* abort */
	pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
	pb.csParam.open.commandTimeoutValue = 30;
	if ((err = tcp(&pb, TCPActiveOpen, s->stream)) != noErr)
		return tcperr(err);
	s->connected = 1;
	s->rhost = sin->sin_addr.s_addr;
	s->rport = sin->sin_port;
	return 0;
}

static int
status(struct mp_sock *s, TCPiopb *pb)
{
	memset(pb, 0, sizeof(*pb));
	return tcp(pb, TCPStatus, s->stream);
}

/* Bytes waiting; sets *closed when the peer has finished sending. */
static long
unread(struct mp_sock *s, int *closed)
{
	TCPiopb pb;
	Byte st;

	*closed = 0;
	if (status(s, &pb) != noErr) {
		*closed = 1;
		return 0;
	}
	st = pb.csParam.status.connectionState;
	if (st != TCPSEstablished && st != TCPSFinWait1 && st != TCPSFinWait2)
		*closed = 1;	/* CloseWait etc.: peer sent FIN */
	return pb.csParam.status.amtUnreadData;
}

ssize_t
mp_sock_read(struct mp_sock *s, void *buf, size_t len)
{
	TCPiopb pb;
	long avail;
	int closed;
	OSErr err;

	if (s->rdshut || !s->connected)
		return 0;
	for (;;) {
		avail = unread(s, &closed);
		if (avail > 0)
			break;
		if (closed) {
			s->rdshut = 1;
			return 0;
		}
		mp_yield();
	}
	if ((long)len > avail)
		len = (size_t)avail;
	if (len > 65535)
		len = 65535;
	memset(&pb, 0, sizeof(pb));
	pb.csParam.receive.commandTimeoutValue = 1;
	pb.csParam.receive.rcvBuff = buf;
	pb.csParam.receive.rcvBuffLen = (uint16_t)len;
	err = tcp(&pb, TCPRcv, s->stream);
	if (err == connectionClosing || err == connectionTerminated) {
		s->rdshut = 1;
		return 0;
	}
	if (err != noErr)
		return tcperr(err);
	return pb.csParam.receive.rcvBuffLen;
}

ssize_t
mp_sock_write(struct mp_sock *s, const void *buf, size_t len)
{
	wdsEntry wds[2];
	TCPiopb pb;
	size_t chunk;
	OSErr err;

	if (!s->connected) {
		errno = ENOTCONN;
		return -1;
	}
	chunk = len > 16384 ? 16384 : len;
	wds[0].length = (uint16_t)chunk;
	wds[0].ptr = (Ptr)buf;
	wds[1].length = 0;
	wds[1].ptr = NULL;
	memset(&pb, 0, sizeof(pb));
	pb.csParam.send.ulpTimeoutValue = 60;
	pb.csParam.send.ulpTimeoutAction = 1;
	pb.csParam.send.validityFlags = timeoutValue | timeoutAction;
	pb.csParam.send.pushFlag = 1;
	pb.csParam.send.wdsPtr = (Ptr)wds;
	if ((err = tcp(&pb, TCPSend, s->stream)) != noErr)
		return tcperr(err);
	return (ssize_t)chunk;
}

int
mp_sock_close(struct mp_sock *s)
{
	TCPiopb pb;

	if (s->connected) {
		memset(&pb, 0, sizeof(pb));
		pb.csParam.close.ulpTimeoutValue = 10;
		pb.csParam.close.validityFlags = timeoutValue;
		tcp(&pb, TCPClose, s->stream);
	}
	memset(&pb, 0, sizeof(pb));
	tcp(&pb, TCPRelease, s->stream);
	DisposePtr(s->rbuf);
	free(s);
	return 0;
}

int
mp_sock_poll(struct mp_sock *s, short events, short *revents)
{
	long avail;
	int closed;

	*revents = 0;
	if (!s->connected) {
		*revents = POLLHUP;
		return 0;
	}
	avail = unread(s, &closed);
	if ((events & POLLIN) && (avail > 0 || closed))
		*revents |= POLLIN;
	if (closed && avail == 0)
		*revents |= POLLHUP;
	if ((events & POLLOUT) && !closed)
		*revents |= POLLOUT;
	return (int)avail;
}

int
mp_shutdown(int fd, int how)
{
	struct mp_sock *s;
	TCPiopb pb;

	if ((s = sock(fd)) == NULL)
		return -1;
	if (how == SHUT_WR || how == SHUT_RDWR) {
		memset(&pb, 0, sizeof(pb));
		pb.csParam.close.ulpTimeoutValue = 10;
		pb.csParam.close.validityFlags = timeoutValue;
		tcp(&pb, TCPClose, s->stream);
	}
	return 0;
}

int
mp_setsockopt(int fd, int level, int opt, const void *val, socklen_t len)
{
	(void)level;
	(void)opt;
	(void)val;
	(void)len;
	return sock(fd) == NULL ? -1 : 0;
}

int
mp_getsockopt(int fd, int level, int opt, void *val, socklen_t *len)
{
	(void)level;
	if (sock(fd) == NULL)
		return -1;
	if (opt == SO_SNDLOWAT || opt == SO_ERROR) {
		/* SO_SNDLOWAT 0: openrsync then sizes its own chunks. */
		if (*len >= sizeof(int))
			*(int *)val = 0;
		*len = sizeof(int);
		return 0;
	}
	errno = ENOPROTOOPT;
	return -1;
}

int
mp_getpeername(int fd, struct sockaddr *sa, socklen_t *len)
{
	struct sockaddr_in sin;
	struct mp_sock *s;

	if ((s = sock(fd)) == NULL)
		return -1;
	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = s->rport;
	sin.sin_addr.s_addr = s->rhost;
	if (*len > sizeof(sin))
		*len = sizeof(sin);
	memcpy(sa, &sin, *len);
	return 0;
}

int
mp_getsockname(int fd, struct sockaddr *sa, socklen_t *len)
{
	return mp_getpeername(fd, sa, len);
}

ssize_t
mp_send(int fd, const void *buf, size_t len, int flags)
{
	(void)flags;
	return mp_write(fd, buf, len);
}

ssize_t
mp_recv(int fd, void *buf, size_t len, int flags)
{
	(void)flags;
	return mp_read(fd, buf, len);
}

ssize_t
mp_writev(int fd, const struct iovec *iov, int n)
{
	ssize_t total = 0, w;
	size_t off;
	int i;

	for (i = 0; i < n; i++)
		for (off = 0; off < iov[i].iov_len; off += (size_t)w) {
			w = mp_write(fd, (char *)iov[i].iov_base + off,
			    iov[i].iov_len - off);
			if (w <= 0)
				return total ? total : -1;
			total += w;
		}
	return total;
}

/* Server-side sockets and local socket pairs are not supported (yet). */
int
mp_bind(int fd, const struct sockaddr *sa, socklen_t len)
{
	(void)fd;
	(void)sa;
	(void)len;
	errno = EOPNOTSUPP;
	return -1;
}

int
mp_listen(int fd, int n)
{
	(void)fd;
	(void)n;
	errno = EOPNOTSUPP;
	return -1;
}

int
mp_accept(int fd, struct sockaddr *sa, socklen_t *len)
{
	(void)fd;
	(void)sa;
	(void)len;
	errno = EOPNOTSUPP;
	return -1;
}

int
mp_socketpair(int d, int t, int p, int sv[2])
{
	(void)d;
	(void)t;
	(void)p;
	(void)sv;
	errno = EOPNOTSUPP;
	return -1;
}

/* ---- poll ------------------------------------------------------------ */

int
mp_poll(struct pollfd *pfd, nfds_t n, int timeout)
{
	unsigned long deadline;
	struct mp_fd *f;
	nfds_t i;
	int ready;

	MPTRACE("[poll n=%u t=%d]", (unsigned)n, timeout);
	deadline = TickCount() + (timeout < 0 ? 0 :
	    ((unsigned long)timeout * 60 + 999) / 1000);
	for (;;) {
		ready = 0;
		for (i = 0; i < n; i++) {
			pfd[i].revents = 0;
			if (pfd[i].fd < 0)
				continue;
			if ((f = mp_fdget(pfd[i].fd)) == NULL)
				pfd[i].revents = POLLNVAL;
			else if (f->type == MPF_SOCK)
				mp_sock_poll(f->sock, pfd[i].events,
				    &pfd[i].revents);
			else
				pfd[i].revents = pfd[i].events &
				    (POLLIN | POLLOUT);
			if (pfd[i].revents)
				ready++;
		}
		if (ready || timeout == 0 ||
		    (timeout > 0 && TickCount() >= deadline))
			return ready;
		mp_yield();
	}
}

/* ---- addresses ------------------------------------------------------- */

int
mp_inet_pton(int af, const char *src, void *dst)
{
	unsigned long a[4];
	char tail;

	if (af != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if (sscanf(src, "%lu.%lu.%lu.%lu%c", &a[0], &a[1], &a[2], &a[3],
	    &tail) != 4 || a[0] > 255 || a[1] > 255 || a[2] > 255 ||
	    a[3] > 255)
		return 0;
	((struct in_addr *)dst)->s_addr =
	    (a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
	return 1;
}

const char *
mp_inet_ntop(int af, const void *src, char *dst, socklen_t len)
{
	in_addr_t a;

	if (af != AF_INET) {
		errno = EAFNOSUPPORT;
		return NULL;
	}
	a = ((const struct in_addr *)src)->s_addr;
	if (snprintf(dst, len, "%lu.%lu.%lu.%lu",
	    (unsigned long)(a >> 24), (unsigned long)((a >> 16) & 255),
	    (unsigned long)((a >> 8) & 255), (unsigned long)(a & 255))
	    >= (int)len) {
		errno = ENOSPC;
		return NULL;
	}
	return dst;
}

/*
 * MacTCP DNR: the resolver is a code resource ('dnrp' 1) in the
 * "MacTCP DNR" file in the System Folder, called with a selector.
 */
#define DNR_OPEN	1
#define DNR_STRTOADDR	3

struct hostInfo {
	long		rtnCode;
	char		cname[255];
	unsigned long	addr[4];
};

typedef long (*dnr_proc)(long, ...);
static dnr_proc dnr;

static pascal void
dnr_done(struct hostInfo *hi, char *user)
{
	(void)hi;
	*(volatile int *)user = 1;
}

static int
dnr_open(void)
{
	Handle h;
	short vref, ref;
	long dirid;

	if (dnr != NULL)
		return 0;
	if (open_driver() == -1)
		return -1;
	if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder,
	    &vref, &dirid) != noErr)
		return -1;
	if ((ref = HOpenResFile(vref, dirid, "\pMacTCP DNR", fsRdPerm)) == -1)
		return -1;
	h = Get1IndResource('dnrp', 1);
	if (h == NULL) {
		CloseResFile(ref);
		return -1;
	}
	DetachResource(h);
	CloseResFile(ref);
	HLock(h);
	dnr = (dnr_proc)*h;
	if (dnr(DNR_OPEN, NULL) != noErr) {
		dnr = NULL;
		return -1;
	}
	return 0;
}

static int
dnr_lookup(const char *name, in_addr_t *addr)
{
	static struct hostInfo hi;
	volatile int done = 0;
	unsigned long start;

	if (dnr_open() == -1)
		return EAI_FAIL;
	memset(&hi, 0, sizeof(hi));
	hi.rtnCode = dnr(DNR_STRTOADDR, name, &hi, dnr_done, (char *)&done);
	if (hi.rtnCode == cacheFault) {
		start = TickCount();
		while (!done && TickCount() - start < 60 * 30)
			mp_yield();
	}
	if (hi.rtnCode != noErr || hi.addr[0] == 0)
		return EAI_NONAME;
	*addr = hi.addr[0];
	return 0;
}

int
mp_getaddrinfo(const char *host, const char *serv,
    const struct addrinfo *hints, struct addrinfo **res)
{
	struct addrinfo *ai;
	struct sockaddr_in *sin;
	struct in_addr a;
	struct servent *se;
	char *end;
	long port = 0;
	int rc;

	if (hints != NULL && hints->ai_family != AF_UNSPEC &&
	    hints->ai_family != AF_INET)
		return EAI_FAMILY;
	if (serv != NULL) {
		port = strtol(serv, &end, 10);
		if (*end != '\0') {
			if ((se = mp_getservbyname(serv, "tcp")) == NULL)
				return EAI_SERVICE;
			port = se->s_port;
		}
	}
	if (host == NULL)
		a.s_addr = (hints && (hints->ai_flags & AI_PASSIVE)) ?
		    INADDR_ANY : INADDR_LOOPBACK;
	else if (mp_inet_pton(AF_INET, host, &a) != 1) {
		if (hints && (hints->ai_flags & AI_NUMERICHOST))
			return EAI_NONAME;
		if ((rc = dnr_lookup(host, &a.s_addr)) != 0)
			return rc;
	}
	if ((ai = calloc(1, sizeof(*ai) + sizeof(*sin))) == NULL)
		return EAI_MEMORY;
	sin = (struct sockaddr_in *)(ai + 1);
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_port = (in_port_t)port;
	sin->sin_addr = a;
	ai->ai_family = AF_INET;
	ai->ai_socktype = SOCK_STREAM;
	ai->ai_protocol = IPPROTO_TCP;
	ai->ai_addrlen = sizeof(*sin);
	ai->ai_addr = (struct sockaddr *)sin;
	*res = ai;
	return 0;
}

void
mp_freeaddrinfo(struct addrinfo *ai)
{
	struct addrinfo *next;

	for (; ai != NULL; ai = next) {
		next = ai->ai_next;
		free(ai->ai_canonname);
		free(ai);
	}
}

const char *
mp_gai_strerror(int e)
{
	switch (e) {
	case EAI_NONAME:
		return "host name not found (check TCP/IP and DNS settings)";
	case EAI_FAIL:
		return "name resolver (MacTCP DNR) unavailable";
	case EAI_SERVICE:
		return "unknown service";
	case EAI_FAMILY:
		return "address family not supported";
	case EAI_MEMORY:
		return "out of memory";
	default:
		return "name lookup failed";
	}
}

int
mp_getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
    socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

	(void)salen;
	(void)flags;
	if (sa->sa_family != AF_INET)
		return EAI_FAMILY;
	if (host != NULL && hostlen > 0 &&
	    mp_inet_ntop(AF_INET, &sin->sin_addr, host, hostlen) == NULL)
		return EAI_SYSTEM;
	if (serv != NULL && servlen > 0)
		snprintf(serv, servlen, "%u", (unsigned)sin->sin_port);
	return 0;
}

struct servent *
mp_getservbyname(const char *name, const char *proto)
{
	static struct servent rsync = { "rsync", NULL, 873, "tcp" };

	(void)proto;
	if (strcmp(name, "rsync") == 0)
		return &rsync;
	return NULL;
}
