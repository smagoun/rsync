/*
 * NetTest: Phase 0 check for the Classic Mac OS rsync port.
 *
 * Opens a TCP connection through the MacTCP API (served by Open Transport
 * on Mac OS 8.1) to an rsync daemon, performs the "@RSYNCD:" greeting,
 * asks for the module list, and logs everything to "NetTest Log" in the
 * application's folder.  If "NetTest Config" exists there, its first line
 * is "a.b.c.d port"; otherwise 10.0.2.2 8873 (QEMU user-mode host).
 * When the file "NetTest AutoQuit" exists, the Mac shuts down afterwards
 * so the test can run unattended from Startup Items.
 */
#include "mactoolbox.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static short logRef = 0;
static char logbuf[512];

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	long n;

	va_start(ap, fmt);
	vsnprintf(logbuf, sizeof logbuf, fmt, ap);
	va_end(ap);
	fputs(logbuf, stdout);
	fflush(stdout);
	if (logRef) {
		n = strlen(logbuf);
		FSWrite(logRef, &n, logbuf);
	}
}

static Boolean fileExists(ConstStr255Param name)
{
	FInfo fi;
	return GetFInfo(name, 0, &fi) == noErr;
}

static void openLog(void)
{
	Str255 name = "\pNetTest Log";

	FSDelete(name, 0);
	if (Create(name, 0, 'ttxt', 'TEXT') == noErr)
		FSOpen(name, 0, &logRef);
}

static void readConfig(unsigned long *ip, unsigned short *port)
{
	short ref;
	char buf[64];
	long n = sizeof buf - 1;
	unsigned a, b, c, d, p;

	if (FSOpen("\pNetTest Config", 0, &ref) != noErr)
		return;
	FSRead(ref, &n, buf);
	FSClose(ref);
	buf[n] = '\0';
	if (sscanf(buf, "%u.%u.%u.%u %u", &a, &b, &c, &d, &p) == 5) {
		*ip = ((unsigned long)a << 24) | ((unsigned long)b << 16) |
		    (c << 8) | d;
		*port = p;
	}
}

static OSErr tcpCall(TCPiopb *pb, short refNum, short code)
{
	pb->ioCRefNum = refNum;
	pb->csCode = code;
	return PBControlSync((ParmBlkPtr)pb);
}

/* Receive whatever arrives within `secs` seconds; returns bytes read. */
static long tcpRecv(short drv, StreamPtr s, char *buf, unsigned short len,
    Byte secs)
{
	TCPiopb pb;
	OSErr err;

	memset(&pb, 0, sizeof pb);
	pb.tcpStream = s;
	pb.csParam.receive.commandTimeoutValue = secs;
	pb.csParam.receive.rcvBuff = buf;
	pb.csParam.receive.rcvBuffLen = len;
	err = tcpCall(&pb, drv, TCPRcv);
	if (err != noErr) {
		logmsg("  TCPRcv: err %d\n", err);
		return err == commandTimeout ? 0 : -1;
	}
	return pb.csParam.receive.rcvBuffLen;
}

static OSErr tcpSendStr(short drv, StreamPtr s, const char *str)
{
	TCPiopb pb;
	wdsEntry wds[2];

	wds[0].length = strlen(str);
	wds[0].ptr = (Ptr)str;
	wds[1].length = 0;
	wds[1].ptr = NULL;
	memset(&pb, 0, sizeof pb);
	pb.tcpStream = s;
	pb.csParam.send.wdsPtr = (Ptr)wds;
	pb.csParam.send.pushFlag = 1;
	return tcpCall(&pb, drv, TCPSend);
}

int main(void)
{
	short drv;
	OSErr err;
	TCPiopb pb;
	StreamPtr stream;
	Ptr rbuf;
	unsigned long ip;
	unsigned short port = 8873;
	static char data[4096];
	long n, total = 0, t0;
	int i;

	ip = (10UL << 24) | (2UL << 8) | 2UL;	/* 10.0.2.2 */
	openLog();
	readConfig(&ip, &port);
	logmsg("NetTest: rsync daemon check via MacTCP\n");
	logmsg("target %lu.%lu.%lu.%lu:%u\n", ip >> 24, (ip >> 16) & 255,
	    (ip >> 8) & 255, ip & 255, port);

	err = OpenDriver("\p.IPP", &drv);
	logmsg("OpenDriver(.IPP) = %d (refNum %d)\n", err, drv);
	if (err != noErr)
		goto done;

	rbuf = NewPtr(16384);
	memset(&pb, 0, sizeof pb);
	pb.csParam.create.rcvBuff = rbuf;
	pb.csParam.create.rcvBuffLen = 16384;
	err = tcpCall(&pb, drv, TCPCreate);
	stream = pb.tcpStream;
	logmsg("TCPCreate = %d\n", err);
	if (err != noErr)
		goto done;

	memset(&pb, 0, sizeof pb);
	pb.tcpStream = stream;
	pb.csParam.open.remoteHost = ip;
	pb.csParam.open.remotePort = port;
	pb.csParam.open.ulpTimeoutValue = 30;
	pb.csParam.open.ulpTimeoutAction = 1;	/* abort */
	pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
	pb.csParam.open.commandTimeoutValue = 30;
	t0 = TickCount();
	err = tcpCall(&pb, drv, TCPActiveOpen);
	logmsg("TCPActiveOpen = %d (%ld ticks)\n", err, TickCount() - t0);
	if (err != noErr)
		goto release;

	n = tcpRecv(drv, stream, data, sizeof data - 1, 10);
	if (n > 0) {
		data[n] = '\0';
		logmsg("<< %s", data);
	}
	logmsg("send greeting = %d\n", tcpSendStr(drv, stream, "@RSYNCD: 29.0\n"));
	logmsg("send #list = %d\n", tcpSendStr(drv, stream, "#list\n"));
	for (i = 0; i < 20; i++) {
		n = tcpRecv(drv, stream, data, sizeof data - 1, 5);
		if (n <= 0)
			break;
		data[n] = '\0';
		total += n;
		logmsg("<< %s", data);
		if (strstr(data, "@RSYNCD: EXIT"))
			break;
	}
	logmsg("received %ld bytes after greeting\n", total);

	memset(&pb, 0, sizeof pb);
	pb.tcpStream = stream;
	logmsg("TCPClose = %d\n", tcpCall(&pb, drv, TCPClose));
release:
	memset(&pb, 0, sizeof pb);
	pb.tcpStream = stream;
	logmsg("TCPRelease = %d\n", tcpCall(&pb, drv, TCPRelease));
done:
	logmsg("NetTest: done\n");
	if (logRef)
		FSClose(logRef);
	FlushVol(NULL, 0);
	if (fileExists("\pNetTest AutoQuit"))
		ShutDwnPower();
	return 0;
}
