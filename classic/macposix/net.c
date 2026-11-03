/*
 * net.c: TCP sockets, poll() and name lookup on MacTCP.
 *
 * Open Transport on Mac OS 8.1 serves the MacTCP driver interface
 * (".IPP"), which Retro68's open-source headers describe; OT's own API
 * would need Apple's SDK.  IPv4 TCP only.  Client calls are synchronous;
 * a listening socket keeps an asynchronous passive open outstanding, and
 * accept() hands that connected stream over and arms a new one (MacTCP
 * streams carry exactly one connection).  Waiting loops call mp_yield()
 * so other applications keep running, and stop with EINTR on Cmd-.
 */
#include "mpint.h"
#include "mactcp.h"

#include <stdlib.h>
#include <string.h>

#define RCVBUF_SIZE	32768

struct mp_sock {
	MTStreamPtr	stream;
	Ptr		rbuf;
	int		connected;
	int		rdshut;		/* remote closed, nothing left */
	MTip_addr		rhost;
	MTtcp_port	rport;
	/* listening sockets */
	int		listening;
	MTtcp_port	lport;		/* from bind() */
	int		pending;	/* passive open outstanding in ppb */
	MTTCPiopb		ppb;
};

static short drv;
int mp_listen_pause;

static int	accept_ready(struct mp_sock *);

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
tcp(MTTCPiopb *pb, short code, MTStreamPtr s)
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
	case MTopenFailed:
	case MTconnectionTerminated:
		errno = ECONNREFUSED;
		break;
	case MTcommandTimeout:
		errno = ETIMEDOUT;
		break;
	case MTconnectionClosing:
	case MTconnectionDoesntExist:
		errno = EPIPE;
		break;
	case MTinsufficientResources:
		errno = ENOBUFS;
		break;
	case MTipBadLapErr:
	case MTipBadCnfgErr:
	case MTipNoCnfgErr:
	case MTipLoadErr:
		errno = ENETDOWN;
		break;
	default:
		errno = EIO;
		break;
	}
	return -1;
}

/* ---- sockets --------------------------------------------------------- */

/* A MacTCP stream with its receive buffer, for s. */
static int
new_stream(struct mp_sock *s)
{
	MTTCPiopb pb;
	OSErr err;

	if ((s->rbuf = NewPtr(RCVBUF_SIZE)) == NULL) {
		errno = ENOMEM;
		return -1;
	}
	memset(&pb, 0, sizeof(pb));
	pb.csParam.create.rcvBuff = s->rbuf;
	pb.csParam.create.rcvBuffLen = RCVBUF_SIZE;
	if ((err = tcp(&pb, MTTCPCreate, NULL)) != noErr) {
		DisposePtr(s->rbuf);
		s->rbuf = NULL;
		return tcperr(err);
	}
	s->stream = pb.tcpStream;
	return 0;
}

int
mp_socket(int domain, int type, int proto)
{
	struct mp_sock *s;
	struct mp_fd *f;
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
	if (new_stream(s) == -1) {
		free(s);
		return -1;
	}
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
	MTTCPiopb pb;
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
	pb.csParam.open.validityFlags = MTtimeoutValue | MTtimeoutAction;
	pb.csParam.open.commandTimeoutValue = 30;
	if ((err = tcp(&pb, MTTCPActiveOpen, s->stream)) != noErr)
		return tcperr(err);
	s->connected = 1;
	s->rhost = sin->sin_addr.s_addr;
	s->rport = sin->sin_port;
	return 0;
}

static int
status(struct mp_sock *s, MTTCPiopb *pb)
{
	memset(pb, 0, sizeof(*pb));
	return tcp(pb, MTTCPStatus, s->stream);
}

/* Bytes waiting; sets *closed when the peer has finished sending. */
static long
unread(struct mp_sock *s, int *closed)
{
	MTTCPiopb pb;
	Byte st;

	*closed = 0;
	if (status(s, &pb) != noErr) {
		*closed = 1;
		return 0;
	}
	st = pb.csParam.status.connectionState;
	if (st != MTTCPSEstablished && st != MTTCPSFinWait1 && st != MTTCPSFinWait2)
		*closed = 1;	/* CloseWait etc.: peer sent FIN */
	return pb.csParam.status.amtUnreadData;
}

ssize_t
mp_sock_read(struct mp_sock *s, void *buf, size_t len)
{
	MTTCPiopb pb;
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
		if (mp_interrupted) {
			errno = EINTR;
			return -1;
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
	err = tcp(&pb, MTTCPRcv, s->stream);
	if (err == MTconnectionClosing || err == MTconnectionTerminated) {
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
	MTwdsEntry wds[2];
	MTTCPiopb pb;
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
	pb.csParam.send.validityFlags = MTtimeoutValue | MTtimeoutAction;
	pb.csParam.send.pushFlag = 1;
	pb.csParam.send.wdsPtr = (Ptr)wds;
	if ((err = tcp(&pb, MTTCPSend, s->stream)) != noErr)
		return tcperr(err);
	return (ssize_t)chunk;
}

int
mp_sock_close(struct mp_sock *s)
{
	MTTCPiopb pb;

	if (s->stream == NULL) {
		free(s);
		return 0;
	}
	if (s->pending) {
		/* Cancel the outstanding passive open. */
		memset(&pb, 0, sizeof(pb));
		tcp(&pb, MTTCPAbort, s->stream);
		while (s->ppb.ioResult == MTinProgress)
			mp_yield();
	}
	if (s->connected) {
		memset(&pb, 0, sizeof(pb));
		pb.csParam.close.ulpTimeoutValue = 10;
		pb.csParam.close.validityFlags = MTtimeoutValue;
		tcp(&pb, MTTCPClose, s->stream);
	}
	memset(&pb, 0, sizeof(pb));
	tcp(&pb, MTTCPRelease, s->stream);
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
	if (s->listening) {
		if ((events & POLLIN) && accept_ready(s))
			*revents = POLLIN;
		return 0;
	}
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
	MTTCPiopb pb;

	if ((s = sock(fd)) == NULL)
		return -1;
	if (how == SHUT_WR || how == SHUT_RDWR) {
		memset(&pb, 0, sizeof(pb));
		pb.csParam.close.ulpTimeoutValue = 10;
		pb.csParam.close.validityFlags = MTtimeoutValue;
		tcp(&pb, MTTCPClose, s->stream);
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

/* ---- listening ------------------------------------------------------ */

int
mp_bind(int fd, const struct sockaddr *sa, socklen_t len)
{
	const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
	struct mp_sock *s;

	if ((s = sock(fd)) == NULL)
		return -1;
	if (len < sizeof(*sin) || sin->sin_family != AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	s->lport = sin->sin_port;
	return 0;
}

/* Start an asynchronous passive open (wait for one connection). */
static int
arm(struct mp_sock *s)
{
	OSErr err;

	if (s->pending)
		return 0;
	memset(&s->ppb, 0, sizeof(s->ppb));
	s->ppb.ioCRefNum = drv;
	s->ppb.csCode = MTTCPPassiveOpen;
	s->ppb.tcpStream = s->stream;
	s->ppb.csParam.open.ulpTimeoutValue = 60;
	s->ppb.csParam.open.ulpTimeoutAction = 1;
	s->ppb.csParam.open.validityFlags = MTtimeoutValue | MTtimeoutAction;
	s->ppb.csParam.open.commandTimeoutValue = 0;	/* wait forever */
	s->ppb.csParam.open.localPort = s->lport;
	if ((err = PBControlAsync((ParmBlkPtr)&s->ppb)) != noErr)
		return tcperr(err);
	s->pending = 1;
	return 0;
}

int
mp_listen(int fd, int backlog)
{
	struct mp_sock *s;

	(void)backlog;	/* one connection at a time */
	if ((s = sock(fd)) == NULL)
		return -1;
	s->listening = 1;
	return arm(s);
}

/* Has the passive open finished?  (>0 yes, 0 not yet) */
static int
accept_ready(struct mp_sock *s)
{
	if (!s->pending && arm(s) == -1)
		return 1;	/* report the error through accept() */
	return s->ppb.ioResult != MTinProgress;
}

int
mp_accept(int fd, struct sockaddr *sa, socklen_t *len)
{
	struct sockaddr_in sin;
	struct mp_sock *s, *c;
	struct mp_fd *f;
	OSErr err;
	int cfd;

	if ((s = sock(fd)) == NULL)
		return -1;
	if (!s->listening) {
		errno = EINVAL;
		return -1;
	}
	while (!accept_ready(s)) {
		if (mp_interrupted) {
			errno = EINTR;
			return -1;
		}
		mp_yield();
	}
	s->pending = 0;
	if ((err = s->ppb.ioResult) != noErr)
		return tcperr(err);

	/* The listening stream is now the connection: hand it over. */
	if ((c = calloc(1, sizeof(*c))) == NULL)
		return -1;
	c->stream = s->stream;
	c->rbuf = s->rbuf;
	c->connected = 1;
	c->rhost = s->ppb.csParam.open.remoteHost;
	c->rport = s->ppb.csParam.open.remotePort;
	if ((cfd = mp_fdalloc(&f)) == -1) {
		mp_sock_close(c);
		return -1;
	}
	f->type = MPF_SOCK;
	f->flags = O_RDWR;
	f->sock = c;

	/* And give the listener a fresh stream for the next client. */
	s->stream = NULL;
	s->rbuf = NULL;
	if (new_stream(s) == 0)
		arm(s);

	if (sa != NULL && len != NULL) {
		memset(&sin, 0, sizeof(sin));
		sin.sin_len = sizeof(sin);
		sin.sin_family = AF_INET;
		sin.sin_port = c->rport;
		sin.sin_addr.s_addr = c->rhost;
		if (*len > sizeof(sin))
			*len = sizeof(sin);
		memcpy(sa, &sin, *len);
	}
	MPTRACE("[accept %d from %lu.%lu.%lu.%lu]\n", cfd,
	    (unsigned long)(c->rhost >> 24), (unsigned long)(c->rhost >> 16 & 255),
	    (unsigned long)(c->rhost >> 8 & 255), (unsigned long)(c->rhost & 255));
	return cfd;
}

/* Local socket pairs are not supported (yet: local mode needs threads). */
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
	int ready, listener;

	deadline = TickCount() + (timeout < 0 ? 0 :
	    ((unsigned long)timeout * 60 + 999) / 1000);
	for (;;) {
		ready = 0;
		listener = 0;
		for (i = 0; i < n; i++) {
			pfd[i].revents = 0;
			if (pfd[i].fd < 0)
				continue;
			if ((f = mp_fdget(pfd[i].fd)) == NULL)
				pfd[i].revents = POLLNVAL;
			else if (f->type == MPF_SOCK) {
				listener |= f->sock->listening;
				mp_sock_poll(f->sock, pfd[i].events,
				    &pfd[i].revents);
			}
			else
				pfd[i].revents = pfd[i].events &
				    (POLLIN | POLLOUT);
			if (pfd[i].revents)
				ready++;
		}
		if (ready || timeout == 0 ||
		    (timeout > 0 && TickCount() >= deadline))
			return ready;
		/*
		 * Interrupted (Cmd-.), or a server asked to pause while it
		 * waits for the next client (the GUI runs a sync meanwhile).
		 */
		if (mp_interrupted || (listener && mp_listen_pause)) {
			errno = EINTR;
			return -1;
		}
		mp_yield();
	}
}

/* ---- addresses ------------------------------------------------------- */

/* This Mac's IP address (0 if TCP/IP is not set up). */
unsigned long
mp_local_ip(void)
{
	struct {
		QElemPtr qLink;
		INTEGER qType, ioTrap;
		Ptr ioCmdAddr;
		ProcPtr ioCompletion;
		OSErr ioResult;
		StringPtr ioNamePtr;
		INTEGER ioVRefNum, ioCRefNum, csCode;
		unsigned long ourAddress, ourNetMask;
	} pb;

	if (open_driver() == -1)
		return 0;
	memset(&pb, 0, sizeof(pb));
	pb.ioCRefNum = drv;
	pb.csCode = MTipctlGetAddr;
	if (PBControlSync((ParmBlkPtr)&pb) != noErr)
		return 0;
	return pb.ourAddress;
}

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

/* Why the resolver could not be loaded (shown by gai_strerror). */
static char dnr_why[96];

static int
dnr_fail(const char *step, long err)
{
	snprintf(dnr_why, sizeof(dnr_why), "MacTCP DNR: %s failed (%ld)", step,
	    err);
	return -1;
}

static int
dnr_open(void)
{
	Handle h;
	short vref, ref;
	long dirid, rc;
	OSErr err;

	if (dnr != NULL)
		return 0;
	if (open_driver() == -1)
		return dnr_fail("opening .IPP", 0);
	if ((err = FindFolder(kOnSystemDisk, kSystemFolderType,
	    kDontCreateFolder, &vref, &dirid)) != noErr)
		return dnr_fail("FindFolder", err);
	if ((ref = HOpenResFile(vref, dirid, "\pMacTCP DNR", fsRdPerm)) == -1)
		return dnr_fail("opening System Folder:MacTCP DNR", ResError());
	h = Get1IndResource('dnrp', 1);
	if (h == NULL) {
		err = ResError();
		CloseResFile(ref);
		return dnr_fail("loading 'dnrp' resource", err);
	}
	DetachResource(h);
	CloseResFile(ref);
	HLock(h);
	dnr = (dnr_proc)*h;
	/* The resolver returns a 16-bit OSErr in D0; the high word is junk. */
	if ((rc = (short)dnr(DNR_OPEN, NULL)) != noErr) {
		dnr = NULL;
		return dnr_fail("OpenResolver", rc);
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
	if ((short)dnr(DNR_STRTOADDR, name, &hi, dnr_done, (char *)&done) ==
	    cacheFault) {
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
		return dnr_why[0] ? dnr_why :
		    "name resolver (MacTCP DNR) unavailable";
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
