/*
 * FILE:     net_udp.c
 * AUTHOR:   Colin Perkins 
 * MODIFIED: Orion Hodson & Piers O'Hanlon
 * 
 * Copyright (c) 1998-2000 University College London
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions 
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by the Computer Science
 *      Department at University College London
 * 4. Neither the name of the University nor of the Department may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* If this machine supports IPv6 the symbol HAVE_IPv6 should */
/* be defined in either config_unix.h or config_win32.h. The */
/* appropriate system header files should also be included   */
/* by those files.                                           */

#include "config_unix.h"
#include "config_win32.h"
#include "debug.h"
#include "memory.h"
#include "inet_pton.h"
#include "inet_ntop.h"
#include "net_udp.h"

#ifdef NEED_ADDRINFO_H
#include "addrinfo.h"
#endif

#define IPv4	4
#define IPv6	6

/* This is pretty nasty but it's the simplest way to get round */
/* the Detexis bug that means their MUSICA IPv6 stack uses     */
/* IPPROTO_IP instead of IPPROTO_IPV6 in setsockopt calls      */
/* We also need to define in6addr_any */
#ifdef  MUSICA_IPV6
#define	IPPROTO_IPV6	IPPROTO_IP
struct	in6_addr	in6addr_any = {IN6ADDR_ANY_INIT};

/* These DEF's are required as MUSICA's winsock6.h causes a clash with some of the 
 * standard ws2tcpip.h definitions (eg struct in_addr6).
 * Note: winsock6.h defines AF_INET6 as 24 NOT 23 as in winsock2.h - I have left it
 * set to the MUSICA value as this is used in some of their function calls. 
 */
//#define AF_INET6        23
#define IP_MULTICAST_LOOP      11 /*set/get IP multicast loopback */
#define	IP_MULTICAST_IF		9 /* set/get IP multicast i/f  */
#define	IP_MULTICAST_TTL       10 /* set/get IP multicast ttl */
#define	IP_MULTICAST_LOOP      11 /*set/get IP multicast loopback */
#define	IP_ADD_MEMBERSHIP      12 /* add an IP group membership */
#define	IP_DROP_MEMBERSHIP     13/* drop an IP group membership */

#define IN6_IS_ADDR_UNSPECIFIED(a) (((a)->s6_addr32[0] == 0) && \
									((a)->s6_addr32[1] == 0) && \
									((a)->s6_addr32[2] == 0) && \
									((a)->s6_addr32[3] == 0))
struct ip_mreq {
	struct in_addr imr_multiaddr;	/* IP multicast address of group */
	struct in_addr imr_interface;	/* local IP address of interface */
};
#endif

#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffff
#endif

struct _socket_udp {
	int	 	 mode;	/* IPv4 or IPv6 */
        char	        *addr;
	uint16_t	 rx_port;
	uint16_t	 tx_port;
	ttl_t	 	 ttl;
	fd_t	 	 fd;
	struct in_addr	 addr4;
#ifdef HAVE_IPv6
	struct in6_addr	 addr6;
#endif /* HAVE_IPv6 */
};

#ifdef WIN32
/* Want to use both Winsock 1 and 2 socket options, but since
* IPv6 support requires Winsock 2 we have to add own backwards
* compatibility for Winsock 1.
*/
#define SETSOCKOPT winsock_versions_setsockopt
#else
#define SETSOCKOPT setsockopt
#endif /* WIN32 */

/*****************************************************************************/
/* Support functions...                                                      */
/*****************************************************************************/

static void
socket_error(const char *msg)
{
#ifdef WIN32
#define WSERR(x) {#x,x}
	struct wse {
		char  errname[20];
		int errno;
	};
	struct wse ws_errs[] = {
		WSERR(WSANOTINITIALISED), WSERR(WSAENETDOWN),     WSERR(WSAEACCES),
		WSERR(WSAEINVAL),         WSERR(WSAEINTR),        WSERR(WSAEINPROGRESS),
		WSERR(WSAEFAULT),         WSERR(WSAENETRESET),    WSERR(WSAENOBUFS),
		WSERR(WSAENOTCONN),       WSERR(WSAENOTSOCK),     WSERR(WSAEOPNOTSUPP),
		WSERR(WSAESHUTDOWN),      WSERR(WSAEWOULDBLOCK),  WSERR(WSAEMSGSIZE),
		WSERR(WSAEHOSTUNREACH),   WSERR(WSAECONNABORTED), WSERR(WSAECONNRESET),
		WSERR(WSAEADDRNOTAVAIL),  WSERR(WSAEAFNOSUPPORT), WSERR(WSAEDESTADDRREQ),
		WSERR(WSAENETUNREACH),    WSERR(WSAETIMEDOUT),    WSERR(0)
	};
	
	int i, e = WSAGetLastError();
	i = 0;
	while(ws_errs[i].errno && ws_errs[i].errno != e) {
		i++;
	}
	printf("ERROR: %s, (%d - %s)\n", msg, e, ws_errs[i].errname);
#else
	perror(msg);
#endif
	abort();
}

#ifdef WIN32
/* ws2tcpip.h defines these constants with different values from
* winsock.h so files that use winsock 2 values but try to use 
* winsock 1 fail.  So what was the motivation in changing the
* constants ?
*/
#define WS1_IP_MULTICAST_IF     2 /* set/get IP multicast interface   */
#define WS1_IP_MULTICAST_TTL    3 /* set/get IP multicast timetolive  */
#define WS1_IP_MULTICAST_LOOP   4 /* set/get IP multicast loopback    */
#define WS1_IP_ADD_MEMBERSHIP   5 /* add  an IP group membership      */
#define WS1_IP_DROP_MEMBERSHIP  6 /* drop an IP group membership      */

/* winsock_versions_setsockopt tries 1 winsock version of option 
* optname and then winsock 2 version if that failed.
*/

static int
winsock_versions_setsockopt(SOCKET s, int level, int optname, const char FAR * optval, int optlen)
{
	int success = -1;
	switch (optname) {
	case IP_MULTICAST_IF:
		success = setsockopt(s, level, WS1_IP_MULTICAST_IF, optval, optlen);
		break;
	case IP_MULTICAST_TTL:
		success = setsockopt(s, level, WS1_IP_MULTICAST_TTL, optval, optlen);
		break;
	case IP_MULTICAST_LOOP:
		success = setsockopt(s, level, WS1_IP_MULTICAST_LOOP, optval, optlen);
		break;
	case IP_ADD_MEMBERSHIP: 
		success = setsockopt(s, level, WS1_IP_ADD_MEMBERSHIP, optval, optlen);
		break;
	case IP_DROP_MEMBERSHIP: 
		success = setsockopt(s, level, WS1_IP_DROP_MEMBERSHIP, optval, optlen);
		break;
	}
	if (success != -1) {
		return success;
	}
	return setsockopt(s, level, optname, optval, optlen);
}
#endif

#ifdef NEED_INET_ATON
#ifdef NEED_INET_ATON_STATIC
static 
#endif
int inet_aton(const char *name, struct in_addr *addr)
{
	addr->s_addr = inet_addr(name);
	return (addr->s_addr != (in_addr_t) INADDR_NONE);
}
#endif

#ifdef NEED_IN6_IS_ADDR_MULTICAST
#define IN6_IS_ADDR_MULTICAST(addr) ((addr)->s6_addr[0] == 0xffU)
#endif

#if defined(NEED_IN6_IS_ADDR_UNSPECIFIED) && defined(MUSICA_IPV6)
#define IN6_IS_ADDR_UNSPECIFIED(addr) IS_UNSPEC_IN6_ADDR(*addr)
#endif


/*****************************************************************************/
/* IPv4 specific functions...                                                */
/*****************************************************************************/

static int udp_addr_valid4(const char *dst)
{
        struct in_addr addr4;
	if (inet_pton(AF_INET, dst, &addr4) || 
            gethostbyname(dst) != NULL) {
                return TRUE;
        }
        return FALSE;
}

static socket_udp *udp_init4(const char *addr, const char *iface, uint16_t rx_port, uint16_t tx_port, int ttl)
{
	int                 	 reuse = 1;
	struct sockaddr_in  	 s_in;
	struct in_addr		 iface_addr;
	socket_udp         	*s = (socket_udp *)malloc(sizeof(socket_udp));
	s->mode    = IPv4;
	s->addr    = NULL;
	s->rx_port = rx_port;
	s->tx_port = tx_port;
	s->ttl     = ttl;
	if (inet_pton(AF_INET, addr, &s->addr4) != 1) {
		struct hostent *h = gethostbyname(addr);
		if (h == NULL) {
                        free(s);
			return NULL;
		}
		memcpy(&(s->addr4), h->h_addr_list[0], sizeof(s->addr4));
	}
	if (iface != NULL) {
		if (inet_pton(AF_INET, iface, &iface_addr) != 1) {
			debug_msg("Illegal interface specification\n");
                        free(s);
			return NULL;
		}
	} else {
		iface_addr.s_addr = 0;
	}
	s->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (s->fd < 0) {
		socket_error("socket");
		abort();
	}
	if (SETSOCKOPT(s->fd, SOL_SOCKET, SO_REUSEADDR, (char *) &reuse, sizeof(reuse)) != 0) {
		socket_error("setsockopt SO_REUSEADDR");
		abort();
	}
#ifdef SO_REUSEPORT
	if (SETSOCKOPT(s->fd, SOL_SOCKET, SO_REUSEPORT, (char *) &reuse, sizeof(reuse)) != 0) {
		socket_error("setsockopt SO_REUSEPORT");
		abort();
	}
#endif
	s_in.sin_family      = AF_INET;
	s_in.sin_addr.s_addr = INADDR_ANY;
	s_in.sin_port        = htons(rx_port);
	if (bind(s->fd, (struct sockaddr *) &s_in, sizeof(s_in)) != 0) {
		socket_error("bind");
		abort();
	}
	if (IN_MULTICAST(ntohl(s->addr4.s_addr))) {
		char            loop = 1;
		struct ip_mreq  imr;
		
		imr.imr_multiaddr.s_addr = s->addr4.s_addr;
		imr.imr_interface.s_addr = iface_addr.s_addr;
		
		if (SETSOCKOPT(s->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *) &imr, sizeof(struct ip_mreq)) != 0) {
			socket_error("setsockopt IP_ADD_MEMBERSHIP");
			abort();
		}
#ifndef WIN32
		if (SETSOCKOPT(s->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) != 0) {
			socket_error("setsockopt IP_MULTICAST_LOOP");
			abort();
		}
#endif
		if (SETSOCKOPT(s->fd, IPPROTO_IP, IP_MULTICAST_TTL, (char *) &s->ttl, sizeof(s->ttl)) != 0) {
			socket_error("setsockopt IP_MULTICAST_TTL");
			abort();
		}
		if (iface_addr.s_addr != 0) {
			if (SETSOCKOPT(s->fd, IPPROTO_IP, IP_MULTICAST_IF, (char *) &iface_addr, sizeof(iface_addr)) != 0) {
				socket_error("setsockopt IP_MULTICAST_IF");
				abort();
			}
		}
	}
        s->addr = strdup(addr);
	return s;
}

static void udp_exit4(socket_udp *s)
{
	if (IN_MULTICAST(ntohl(s->addr4.s_addr))) {
		struct ip_mreq  imr;
		imr.imr_multiaddr.s_addr = s->addr4.s_addr;
		imr.imr_interface.s_addr = INADDR_ANY;
		if (SETSOCKOPT(s->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, (char *) &imr, sizeof(struct ip_mreq)) != 0) {
			socket_error("setsockopt IP_DROP_MEMBERSHIP");
			abort();
		}
	}
	close(s->fd);
        free(s->addr);
	free(s);
}

static int udp_send4(socket_udp *s, char *buffer, int buflen)
{
	struct sockaddr_in	s_in;
	
	assert(s != NULL);
	assert(s->mode == IPv4);
	assert(buffer != NULL);
	assert(buflen > 0);
	
	s_in.sin_family      = AF_INET;
	s_in.sin_addr.s_addr = s->addr4.s_addr;
	s_in.sin_port        = htons(s->tx_port);
	return sendto(s->fd, buffer, buflen, 0, (struct sockaddr *) &s_in, sizeof(s_in));
}

static const char *udp_host_addr4(void)
{
	static char    		 hname[MAXHOSTNAMELEN];
	struct hostent 		*hent;
	struct in_addr  	 iaddr;
	
	if (gethostname(hname, MAXHOSTNAMELEN) != 0) {
		debug_msg("Cannot get hostname!");
		abort();
	}
	hent = gethostbyname(hname);
	assert(hent->h_addrtype == AF_INET);
	memcpy(&iaddr.s_addr, hent->h_addr, sizeof(iaddr.s_addr));
	strncpy(hname, inet_ntoa(iaddr), MAXHOSTNAMELEN);
	return (const char*)hname;
}

/*****************************************************************************/
/* IPv6 specific functions...                                                */
/*****************************************************************************/

static int udp_addr_valid6(const char *dst)
{
#ifdef HAVE_IPv6
        struct in6_addr addr6;
	switch (inet_pton(AF_INET6, dst, &addr6)) {
        case 1:  
                return TRUE;
                break;
        case 0: 
                return FALSE;
                break;
        case -1: 
                debug_msg("inet_pton failed\n");
                errno = 0;
        }
#endif /* HAVE_IPv6 */
        UNUSED(dst);
        return FALSE;
}

static socket_udp *udp_init6(const char *addr, const char *iface, uint16_t rx_port, uint16_t tx_port, int ttl)
{
#ifdef HAVE_IPv6
	int                 reuse = 1;
	struct sockaddr_in6 s_in;
	socket_udp         *s = (socket_udp *) malloc(sizeof(socket_udp));
	s->mode    = IPv6;
	s->addr    = NULL;
	s->rx_port = rx_port;
	s->tx_port = tx_port;
	s->ttl     = ttl;
	
	if (iface != NULL) {
		debug_msg("Not yet implemented\n");
		abort();
	}

	if (inet_pton(AF_INET6, addr, &s->addr6) != 1) {
		/* We should probably try to do a DNS lookup on the name */
		/* here, but I'm trying to get the basics going first... */
		debug_msg("IPv6 address conversion failed\n");
                free(s);
		return NULL;	
	}
	s->fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (s->fd < 0) {
		socket_error("socket");
		abort();
	}
	if (SETSOCKOPT(s->fd, SOL_SOCKET, SO_REUSEADDR, (char *) &reuse, sizeof(reuse)) != 0) {
		socket_error("setsockopt SO_REUSEADDR");
		abort();
	}
#ifdef SO_REUSEPORT
	if (SETSOCKOPT(s->fd, SOL_SOCKET, SO_REUSEPORT, (char *) &reuse, sizeof(reuse)) != 0) {
		socket_error("setsockopt SO_REUSEPORT");
		abort();
	}
#endif
	
	memset((char *)&s_in, 0, sizeof(s_in));
	s_in.sin6_family = AF_INET6;
	s_in.sin6_port   = htons(rx_port);
#ifdef HAVE_SIN6_LEN
	s_in.sin6_len    = sizeof(s_in);
#endif
	memcpy(s_in.sin6_addr.s6_addr, &s->addr6, sizeof(struct in6_addr));
	if (bind(s->fd, (struct sockaddr *) &s_in, sizeof(s_in)) != 0) {
		/* bind to group address failed, try generic address. */
		s_in.sin6_addr = in6addr_any;
		if (bind(s->fd, (struct sockaddr *) &s_in, sizeof(s_in)) != 0) {
			socket_error("bind");
			abort();
		}
	}
	
	if (IN6_IS_ADDR_MULTICAST(&(s->addr6))) {
		unsigned int      loop = 1;
		struct ipv6_mreq  imr;
#ifdef MUSICA_IPV6
		imr.i6mr_interface = 1;
		imr.i6mr_multiaddr = s->addr6;
#else
		imr.ipv6mr_multiaddr = s->addr6;
		imr.ipv6mr_interface = 0;
#endif
		
		if (SETSOCKOPT(s->fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (char *) &imr, sizeof(struct ipv6_mreq)) != 0) {
			socket_error("setsockopt IPV6_ADD_MEMBERSHIP");
			abort();
		}
		
		if (SETSOCKOPT(s->fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (char *) &loop, sizeof(loop)) != 0) {
			socket_error("setsockopt IPV6_MULTICAST_LOOP");
			abort();
		}
		if (SETSOCKOPT(s->fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (char *) &ttl, sizeof(ttl)) != 0) {
			socket_error("setsockopt IPV6_MULTICAST_HOPS");
			abort();
		}
	}
	assert(s != NULL);

        s->addr = strdup(addr);
	return s;
#else
	UNUSED(addr);
	UNUSED(iface);
	UNUSED(rx_port);
	UNUSED(tx_port);
	UNUSED(ttl);
	return NULL;
#endif
}

static void udp_exit6(socket_udp *s)
{
#ifdef HAVE_IPv6
	if (IN6_IS_ADDR_MULTICAST(&(s->addr6))) {
		struct ipv6_mreq  imr;
#ifdef MUSICA_IPV6
		imr.i6mr_interface = 1;
		imr.i6mr_multiaddr = s->addr6;
#else
		imr.ipv6mr_multiaddr = s->addr6;
		imr.ipv6mr_interface = 0;
#endif
		
		if (SETSOCKOPT(s->fd, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, (char *) &imr, sizeof(struct ipv6_mreq)) != 0) {
			socket_error("setsockopt IPV6_DROP_MEMBERSHIP");
			abort();
		}
	}
	close(s->fd);
        free(s->addr);
	free(s);
#else
	UNUSED(s);
#endif  /* HAVE_IPv6 */
}

static int udp_send6(socket_udp *s, char *buffer, int buflen)
{
#ifdef HAVE_IPv6
	struct sockaddr_in6	s_in;
	
	assert(s != NULL);
	assert(s->mode == IPv6);
	assert(buffer != NULL);
	assert(buflen > 0);
	
	memset((char *)&s_in, 0, sizeof(s_in));
	s_in.sin6_family = AF_INET6;
	s_in.sin6_addr   = s->addr6;
	s_in.sin6_port   = htons(s->tx_port);
#ifdef HAVE_SIN6_LEN
	s_in.sin6_len    = sizeof(s_in);
#endif
	return sendto(s->fd, buffer, buflen, 0, (struct sockaddr *) &s_in, sizeof(s_in));
#else
	UNUSED(s);
	UNUSED(buffer);
	UNUSED(buflen);
	return -1;
#endif
}

static const char *udp_host_addr6(socket_udp *s)
{
#ifdef HAVE_IPv6
	static char		 hname[MAXHOSTNAMELEN];
	int 			 gai_err;
	struct addrinfo 	 hints, *ai;
	struct sockaddr_in6 	 local;
	int len = sizeof(local), result = 0;

	memset((char *)&local, 0, len);
	local.sin6_family = AF_INET6;
#ifdef HAVE_SIN6_LEN
	local.sin6_len    = len;
#endif

	if ((result = getsockname(s->fd,(struct sockaddr *)&local, &len)) < 0){
		local.sin6_addr = in6addr_any;
		local.sin6_port = 0;
		debug_msg("getsockname failed\n");
	}
	if (IN6_IS_ADDR_UNSPECIFIED(&local.sin6_addr) || IN6_IS_ADDR_MULTICAST(&local.sin6_addr)) {
		if (gethostname(hname, MAXHOSTNAMELEN) != 0) {
			debug_msg("gethostname failed\n");
			abort();
		}
		
		hints.ai_protocol  = IPPROTO_IPV6;
		hints.ai_flags     = 0;
		hints.ai_family    = AF_INET6;
		hints.ai_socktype  = SOCK_DGRAM;
		hints.ai_protocol  = IPPROTO_IPV6;
		hints.ai_addrlen   = 0;
		hints.ai_canonname = NULL;
		hints.ai_addr      = NULL;
		hints.ai_next      = NULL;

		if ((gai_err = getaddrinfo(hname, NULL, &hints, &ai))) {
			debug_msg("getaddrinfo: %s: %s\n", hname, gai_strerror(gai_err));
			abort();
		}
		
		if (inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)(ai->ai_addr))->sin6_addr), hname, MAXHOSTNAMELEN) == NULL) {
			debug_msg("inet_ntop: %s: \n", hname);
			abort();
		}
		freeaddrinfo(ai);
		return (const char*)hname;
	}
	if (inet_ntop(AF_INET6, &local.sin6_addr, hname, MAXHOSTNAMELEN) == NULL) {
		debug_msg("inet_ntop: %s: \n", hname);
		abort();
	}
	return (const char*)hname;
#else  /* HAVE_IPv6 */
	UNUSED(s);
	return "::";	/* The unspecified address... */
#endif /* HAVE_IPv6 */
}
	
/*****************************************************************************/
/* Generic functions, which call the appropriate protocol specific routines. */
/*****************************************************************************/

int udp_addr_valid(const char *addr)
{
        return udp_addr_valid4(addr) | udp_addr_valid6(addr);
}

socket_udp *udp_init(const char *addr, uint16_t rx_port, uint16_t tx_port, int ttl)
{
	return udp_init_if(addr, NULL, rx_port, tx_port, ttl);
}

socket_udp *udp_init_if(const char *addr, const char *iface, uint16_t rx_port, uint16_t tx_port, int ttl)
{
	socket_udp *res;
	
	if (strchr(addr, ':') == NULL) {
		res = udp_init4(addr, iface, rx_port, tx_port, ttl);
	} else {
		res = udp_init6(addr, iface, rx_port, tx_port, ttl);
	}
	return res;
}

void udp_exit(socket_udp *s)
{
    switch(s->mode) {
    case IPv4 : udp_exit4(s); break;
    case IPv6 : udp_exit6(s); break;
    default   : abort();
    }
}

int udp_send(socket_udp *s, char *buffer, int buflen)
{
	switch (s->mode) {
	case IPv4 : return udp_send4(s, buffer, buflen);
	case IPv6 : return udp_send6(s, buffer, buflen);
	default   : abort();
	}
	return -1;
}

int udp_recv(socket_udp *s, char *buffer, int buflen)
{
	/* Reads data into the buffer, returning the number of bytes read.   */
	/* If no data is available, this returns the value zero immediately. */
	/* Note: since we don't care about the source address of the packet  */
	/* we receive, this function becomes protocol independent.           */
	int		len;

	assert(buffer != NULL);
	assert(buflen > 0);

	len = recvfrom(s->fd, buffer, buflen, 0, 0, 0);
	if (len > 0) {
		return len;
	}
	if (errno != ECONNREFUSED) {
		socket_error("recvfrom");
	}
	return 0;
}

static fd_set	rfd;
static fd_t	max_fd;

void udp_fd_zero(void)
{
	FD_ZERO(&rfd);
	max_fd = 0;
}

void udp_fd_set(socket_udp *s)
{
	FD_SET(s->fd, &rfd);
	if (s->fd > (fd_t)max_fd) {
		max_fd = s->fd;
	}
}

int udp_fd_isset(socket_udp *s)
{
	return FD_ISSET(s->fd, &rfd);
}

int udp_select(struct timeval *timeout)
{
	return select(max_fd + 1, &rfd, NULL, NULL, timeout);
}

const char *udp_host_addr(socket_udp *s)
{
	switch (s->mode) {
	case IPv4 : return udp_host_addr4();
	case IPv6 : return udp_host_addr6(s);
	default   : abort();
	}
	return NULL;
}

