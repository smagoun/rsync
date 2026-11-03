/*
 * mactcp.h: the MacTCP driver interface (.IPP), declared here under our
 * own names (MT...) so the port doesn't depend on which Toolbox headers
 * Retro68 was built with: Multiversal and Apple's Universal Interfaces
 * declare MacTCP differently, or (some Universal Interfaces releases) not
 * at all.  Layouts are those of the MacTCP Programmer's Guide, with the
 * corrections noted in Multiversal (TCPReceivePB field order, TCPStatusPB's
 * connStatPtr), checked against Universal Interfaces 3.4.2 there.
 * The structures are 68k-packed: 2-byte alignment, as the compiler uses.
 */
#ifndef MACTCP_PRIVATE_H
#define MACTCP_PRIVATE_H

#include <stdint.h>

typedef uint32_t	MTip_addr;
typedef uint16_t	MTtcp_port;
typedef char		*MTStreamPtr;

/* csCode values */
enum {
	MTipctlGetAddr = 15,
	MTTCPCreate = 30,
	MTTCPPassiveOpen = 31,
	MTTCPActiveOpen = 32,
	MTTCPSend = 34,
	MTTCPRcv = 37,
	MTTCPClose = 38,
	MTTCPAbort = 39,
	MTTCPStatus = 40,
	MTTCPRelease = 42
};

/* OSErr codes */
enum {
	MTinProgress = 1,
	MTipBadLapErr = -23000,
	MTipBadCnfgErr = -23001,
	MTipNoCnfgErr = -23002,
	MTipLoadErr = -23003,
	MTconnectionClosing = -23005,
	MTconnectionDoesntExist = -23008,
	MTinsufficientResources = -23009,
	MTconnectionTerminated = -23012,
	MTopenFailed = -23015,
	MTcommandTimeout = -23016,
	MTcacheFault = -23042		/* DNR: answer not cached, keep waiting */
};

/* connectionState */
enum {
	MTTCPSClosed = 0,
	MTTCPSListen = 2,
	MTTCPSEstablished = 8,
	MTTCPSFinWait1 = 10,
	MTTCPSFinWait2 = 12,
	MTTCPSCloseWait = 14
};

/* validityFlags */
enum {
	MTtimeoutValue = 128,
	MTtimeoutAction = 64
};

typedef struct {
	uint16_t	length;
	char		*ptr;
} MTwdsEntry;

typedef struct {
	char		*rcvBuff;
	uint32_t	rcvBuffLen;
	void		*notifyProc;
	char		*userDataPtr;
} MTTCPCreatePB;

typedef struct {
	uint8_t		ulpTimeoutValue;
	uint8_t		ulpTimeoutAction;
	uint8_t		validityFlags;
	uint8_t		commandTimeoutValue;
	MTip_addr	remoteHost;
	MTtcp_port	remotePort;
	MTip_addr	localHost;
	MTtcp_port	localPort;
	uint8_t		tosFlags;
	uint8_t		precedence;
	uint8_t		dontFrag;
	uint8_t		timeToLive;
	uint8_t		security;
	uint8_t		optionCnt;
	uint8_t		options[40];
	char		*userDataPtr;
} MTTCPOpenPB;

typedef struct {
	uint8_t		ulpTimeoutValue;
	uint8_t		ulpTimeoutAction;
	uint8_t		validityFlags;
	uint8_t		pushFlag;
	uint8_t		urgentFlag;
	char		*wdsPtr;
	uint32_t	sendFree;
	uint16_t	sendLength;
	char		*userDataPtr;
} MTTCPSendPB;

typedef struct {
	uint8_t		commandTimeoutValue;
	uint8_t		markFlag;
	uint8_t		urgentFlag;
	char		*rcvBuff;
	uint16_t	rcvBuffLen;
	char		*rdsPtr;
	uint16_t	rdsLength;
	uint16_t	secondTimeStamp;
	char		*userDataPtr;
} MTTCPReceivePB;

typedef struct {
	uint8_t		ulpTimeoutValue;
	uint8_t		ulpTimeoutAction;
	uint8_t		validityFlags;
	char		*userDataPtr;
} MTTCPClosePB;

typedef struct {
	uint8_t		ulpTimeoutValue;
	uint8_t		ulpTimeoutAction;
	int32_t		unused;
	MTip_addr	remoteHost;
	MTtcp_port	remotePort;
	MTip_addr	localHost;
	MTtcp_port	localPort;
	uint8_t		tosFlags;
	uint8_t		precedence;
	uint8_t		connectionState;
	uint16_t	sendWindow;
	uint16_t	rcvWindow;
	uint16_t	amtUnackedData;
	uint16_t	amtUnreadData;
	char		*securityLevelPtr;
	uint32_t	sendUnacked;
	uint32_t	sendNext;
	uint32_t	congestionWindow;
	uint32_t	rcvNext;
	uint32_t	srtt;
	uint32_t	lastRTT;
	uint32_t	sendMaxSegSize;
	char		*connStatPtr;
	char		*userDataPtr;
} MTTCPStatusPB;

/* The parameter block for PBControl on the .IPP driver: 102 bytes. */
typedef struct {
	void		*qLink;
	int16_t		qType;
	int16_t		ioTrap;
	char		*ioCmdAddr;
	void		*ioCompletion;
	int16_t		ioResult;
	unsigned char	*ioNamePtr;
	int16_t		ioVRefNum;
	int16_t		ioCRefNum;
	int16_t		csCode;
	MTStreamPtr	tcpStream;
	union {
		MTTCPCreatePB	create;
		MTTCPOpenPB	open;
		MTTCPSendPB	send;
		MTTCPReceivePB	receive;
		MTTCPClosePB	close;
		MTTCPStatusPB	status;
	} csParam;
} MTTCPiopb;

typedef struct {
	void		*qLink;
	int16_t		qType;
	int16_t		ioTrap;
	char		*ioCmdAddr;
	void		*ioCompletion;
	int16_t		ioResult;
	unsigned char	*ioNamePtr;
	int16_t		ioVRefNum;
	int16_t		ioCRefNum;
	int16_t		csCode;
	MTip_addr	ourAddress;
	MTip_addr	ourNetMask;
} MTGetAddrParamBlock;

/* Offsets fixed by the driver; a wrong layout fails to compile. */
_Static_assert(sizeof(MTTCPiopb) == 102, "TCPiopb is 102 bytes");
_Static_assert(__builtin_offsetof(MTTCPiopb, csParam) == 32,
    "csParam at 32");
_Static_assert(__builtin_offsetof(MTTCPiopb, csParam.receive.rcvBuffLen) ==
    32 + 8, "TCPReceivePB.rcvBuffLen");
_Static_assert(__builtin_offsetof(MTTCPiopb,
    csParam.status.amtUnreadData) == 32 + 28, "TCPStatusPB.amtUnreadData");

#endif /* MACTCP_PRIVATE_H */
