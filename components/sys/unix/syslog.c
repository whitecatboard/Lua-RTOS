/*
 * Copyright (c) 1983, 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Copyright (C) 2015 - 2017
 * IBEROXARXA SERVICIOS INTEGRALES, S.L. & CSS IBÉRICA, S.L.
 * 
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 * 
 * All rights reserved.
 *   
 * Some changes are made for convert this utility more suitable for embedded
 * systems (memory size restrictions, usually in stack size)
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "luartos.h"

#if defined(LIBC_SCCS) && !defined(lint)
static char sccsid[] = "@(#)syslog.c	8.5 (Berkeley) 4/29/95";
#endif /* LIBC_SCCS and not lint */

#include <sys/types.h>
#include <sys/syslog.h>
#include <sys/mount.h>
#include <sys/status.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include <sys/fcntl.h>
#include <drivers/net.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if __STDC__
#include <stdarg.h>
#else
#include <varargs.h>
#endif

#define PORT 514

static int   logSock = 0;
static char *logHost = NULL;
struct sockaddr_in logAddr;
static FILE *logFile = NULL;
static int	 logStat = 0;		/* status bits, set by openlog() */
static const char *LogTag = NULL;	/* string to tag the entry with */
static int	logFacility = LOG_USER;	/* default facility code */
static int	logMask = 0b11111111;		/* mask of priorities to be logged */
extern char	*__progname;		/* Program name, from crt0. */

void vsyslog(int pri, register const char *fmt, va_list app);

/*
 * syslog, vsyslog --
 *	print message on log file; output is intended for syslogd(8).
 */
void
#if __STDC__
syslog(int pri, const char *fmt, ...)
#else
syslog(pri, fmt, va_alist)
	int pri;
	char *fmt;
	va_dcl
#endif
{
	if (!fmt) return;

	va_list ap;

#if __STDC__
	va_start(ap, fmt);
#else
	va_start(ap);
#endif
	vsyslog(pri, fmt, ap);
	va_end(ap);
}

void
vsyslog(pri, fmt, ap)
	int pri;
	register const char *fmt;
	va_list ap;
{
	register int cnt;
	register char *p;
	char *tbuf;

	if (!fmt) return;

	time_t now;
	int fd;

	int has_cr_lf = 0;

	#define MAX_BUFF 128
	register int size;

	cnt = strlen(fmt) - 1;
	has_cr_lf = ((fmt[cnt] == '\n') || (fmt[cnt - 1] == '\r'));

	#define	INTERNALLOG LOG_ERR|LOG_CONS|LOG_PERROR|LOG_PID

	/* Check for invalid bits. */
	if (pri & ~(LOG_PRIMASK|LOG_FACMASK)) {
			pri &= LOG_PRIMASK|LOG_FACMASK;
	}

	/* Check priority against setlogmask values. */
	if (!(LOG_MASK(LOG_PRI(pri)) & logMask))
		return;

	// Allocate space
	tbuf = (char *)malloc(MAX_BUFF + 20);
	if (!tbuf) return;

	/* Set default facility if none specified. */
	if ((pri & LOG_FACMASK) == 0)
		pri |= logFacility;

	/* Build the message. */
	size = MAX_BUFF;

	(void)time(&now);
	p = tbuf + snprintf(tbuf, size, "<%d>", pri);
	if ((size = MAX_BUFF - (p - tbuf)) < 0) size = 0;

	if (logStat & LOG_PID) {
		p += snprintf(p, size, "[%d]", getpid());
		if ((size = MAX_BUFF - (p - tbuf)) < 0) size = 0;
	}

	p += vsnprintf(p, size, fmt, ap);
	if ((size = MAX_BUFF - (p - tbuf)) < 0) size = 0;
	cnt = p - tbuf;

	if (logStat & LOG_CONS) {
		fd = fileno(_GLOBAL_REENT->_stdout);
		if (!has_cr_lf) {
			(void)strcat(tbuf, "\r\n");
			cnt += 2;
			has_cr_lf = 1;
		}

		p = index(tbuf, '>') + 1;
		(void)write(fd, p, cnt - (p - tbuf));
	}

	if (NULL != logFile) {
		char *t = tbuf + strlen(tbuf) - 1;

		// Remove end \r | \n
		while ((*t == '\r') || (*t == '\n')) {
			*t = '\0';
			t--;
		}

		(void)strcat(tbuf, "\n");
		cnt += 1;
		p = index(tbuf, '>') + 1;

		fwrite(p, cnt - (p - tbuf), 1, logFile);
		fflush(logFile);
	}

	if (0 != logSock) {
		sendto(logSock,tbuf,strlen(tbuf)+1,0,(struct sockaddr *)&logAddr,sizeof(logAddr));
	}

	free(tbuf);
}

static void reconnect_syslog() {
	if (0 != logSock) {
		close(logSock);
	}
	logSock = 0;

	if (NETWORK_AVAILABLE()) {
		if (!logHost) logHost = strdup("10.0.0.10");
		if (0 == strcmp(logHost,"0.0.0.0"))
		   return; //user wants to disable remote logging

		// Resolve name
		const struct addrinfo hints = {
			.ai_family = AF_INET,
			.ai_socktype = SOCK_STREAM,
		};

		struct addrinfo *result;
		int err = getaddrinfo(logHost, NULL, &hints, &result);
		if (err != 0 || result == NULL) {
			printf("could not resolve host %s\n", logHost);
		}
		else {
			(void)memset((void *)&logAddr, 0x00,sizeof(logAddr));

			if (result->ai_family == AF_INET) {
			    struct sockaddr_in *p = (struct sockaddr_in *)result->ai_addr;
			    p->sin_port = htons(PORT);
			    memcpy(&logAddr, p, sizeof(struct sockaddr_in));
			} else if (result->ai_family == AF_INET6) {
			    struct sockaddr_in6 *p = (struct sockaddr_in6 *)result->ai_addr;
			    p->sin6_port = htons(PORT);
			    p->sin6_family = AF_INET6;
			    memcpy(&logAddr, p, sizeof(struct sockaddr_in6));
			} else {
			    printf("Unsupported protocol family %d", result->ai_family);
			}

			freeaddrinfo(result);

			if (logAddr.sin_port != 0) {
				logSock = socket(AF_INET, SOCK_DGRAM, 0);
				if (0 != logSock) {
					fcntl(logSock, F_SETFL, O_NONBLOCK);
					int reuse = 1;
					setsockopt(logSock,SOL_SOCKET,SO_REUSEADDR,(void *)&reuse,sizeof(reuse));
				}
			}
		}
	}
}

static void syslog_net_callback(system_event_t *event){
	if ( (NETWORK_AVAILABLE() && (0 == logSock)) ||
	    (!NETWORK_AVAILABLE() && (0 != logSock)) ) {
		reconnect_syslog();
	}
}

void openlog(ident, logstat, logfac)
	const char *ident;
	int logstat, logfac;
{
	if (ident != NULL)
		LogTag = ident;

	logStat = logstat;
	if (logfac != 0 && (logfac &~ LOG_FACMASK) == 0)
		logFacility = logfac;

	if (NULL != logFile) {
		fclose(logFile);
	}

	logFile = NULL;

	if (mount_is_mounted("fat")) {
		if (mount_is_mounted("spiffs")) {
			logFile = fopen("/sd/log/messages.log","a+");
		} else {
			logFile = fopen("/log/messages.log","a+");
		}
	}

	if (NULL != logFile) {
		fflush(logFile);
	}

	reconnect_syslog();

	driver_error_t *error;
	if ((error = net_event_register_callback(syslog_net_callback))) {
		printf("couldn't register net callback, please restart syslog service from lua using after changing connectivity\n");
		printf("you may use the command 'os.logcons(os.logcons())' to restart the syslog service from lua\n");
	}
}

void closelog() {
	if (NULL != logFile) {
		fclose(logFile);
	}
	logFile = NULL;

	if (0 != logSock) {
		close(logSock);
	}
	logSock = 0;

	driver_error_t *error;
	if ((error = net_event_unregister_callback(syslog_net_callback))) {
		printf("couldn't unregister net callback\n");
	}
}

/* setlogmask -- set the log mask level */
int setlogmask(int pmask) {
	int omask;

	omask = logMask;
	if (pmask != 0)
		logMask = pmask;

	return (omask);
}

int getlogmask() {
	return logMask;
}

int getlogstat() {
	return logStat;
}

const char *syslog_setloghost (const char *host)
{
  if (logHost)
     free (logHost);
  logHost = NULL;
  if (!host)
     return (NULL);

  logHost = strdup (host);
	reconnect_syslog();

  return logHost;
}

const char *syslog_getloghost ()
{
  return logHost;
}
