/*
 * Copyright (c) 1983, 1988, 1993
 *    The Regents of the University of California.  All rights reserved.
 *
 * Copyright (C) 2015 - 2020
 * IBEROXARXA SERVICIOS INTEGRALES, S.L.
 * 
 * Author: Jaume Oliv� (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 *         Thomas E. Horner (whitecatboard.org@horner.it)
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
 *    This product includes software developed by the University of
 *    California, Berkeley and its contributors.
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
#include <sys/path.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcpip_priv.h"
#include <sys/fcntl.h>
#include <drivers/net.h>
#include "esp_log.h"

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#if __STDC__
#include <stdarg.h>
#else
#include <varargs.h>
#endif

#define MAX_BUFF 512

#if CONFIG_LUA_RTOS_USE_RSYSLOG
static int   logSock = 0;
static char *logHost = NULL;
static const char *logHostDefault = CONFIG_LUA_RTOS_RSYSLOG_SERVER;
struct sockaddr_in logAddr;
#endif
static FILE *logFile = NULL;
static int logStat = 0;               /* status bits, set by openlog() */
static int logFacility = LOG_USER;    /* default facility code */
static int logMask = 0b11111111;      /* mask of priorities to be logged */

void vsyslog(int pri, register const char *fmt, va_list app);

#if CONFIG_LUA_RTOS_LUA_USE_MQTT
#include "lauxlib.h"
static int   logMQTT = 0;
static char *logTopic = NULL;
static int msyslog_vprintf( const char *str, va_list l );
#endif

/*
 * syslog, vsyslog --
 *    print message on log file; output is intended for syslogd(8).
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
    time_t now;
    int fd;
    int has_cr_lf = 0;

    #define    INTERNALLOG LOG_ERR|LOG_CONS|LOG_PERROR|LOG_PID

    if (!fmt) return;

    /* Check for invalid bits. */
    if (pri & ~(LOG_PRIMASK|LOG_FACMASK)) {
            pri &= LOG_PRIMASK|LOG_FACMASK;
    }

    /* Check priority against setlogmask values. */
    if (!(LOG_MASK(LOG_PRI(pri)) & logMask))
        return;

    // Allocate space
    tbuf = (char *)malloc(MAX_BUFF + 3);
    if (!tbuf) {
        //emergency print (!)
        fprintf(stderr, "syslog: Out of memory! Printing message to stderr\n");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        return;
    }

    /* Set default facility if none specified. */
    if ((pri & LOG_FACMASK) == 0)
        pri |= logFacility;

    /* Build the message. */

    (void)time(&now);
    p = tbuf + snprintf(tbuf, MAX_BUFF, "<%d>", pri);
    if (logStat & LOG_PID) {
        p += snprintf(p, MAX_BUFF - (p - tbuf), "[%d]", getpid());
    }
    cnt = vsnprintf(p, MAX_BUFF - (p - tbuf), fmt, ap);
    if (cnt > MAX_BUFF - (p - tbuf)) {
        p += (MAX_BUFF - (p - tbuf)) - 1;
    }
    else {
        p += cnt;
    }

    cnt = p - tbuf;
    has_cr_lf = ((tbuf[cnt-1] == '\n') && (tbuf[cnt-2] == '\r'));

    if (logStat & LOG_CONS) {
        fd = fileno(_GLOBAL_REENT->_stdout);
        if (!has_cr_lf) {
            while(cnt && tbuf[cnt-1] == '\n') cnt--;

            (void)strcat(tbuf, "\r\n");
            cnt += 2;
            has_cr_lf = 1;
        }

        p = index(tbuf, '>') + 1;
        (void)write(fd, p, cnt - (p - tbuf));
    }

    if (NULL != logFile) {
        char *t = tbuf + strlen(tbuf);

        // Remove end \r | \n
        while ((*t == '\r') || (*t == '\n')) {
            *t = '\0';
            t--;
            cnt--;
        }

        (void)strcat(tbuf, "\n");
        cnt += 1;
        p = index(tbuf, '>') + 1;

        fwrite(p, cnt - (p - tbuf), 1, logFile);
        fflush(logFile);
    }

#if CONFIG_LUA_RTOS_USE_RSYSLOG
    if (0 != logSock) {
        LOCK_TCPIP_CORE()
        sendto(logSock,tbuf,cnt,0,(struct sockaddr *)&logAddr,sizeof(logAddr));
        UNLOCK_TCPIP_CORE()
    }
#endif

#if CONFIG_LUA_RTOS_LUA_USE_MQTT
    if (0 != logMQTT) {
        lua_State *L = pvGetLuaState(); // Get the thread's Lua state
        lua_rawgeti(L, LUA_REGISTRYINDEX, logMQTT);
        lua_getfield(L, -1, "publish");
        lua_rawgeti(L, LUA_REGISTRYINDEX, logMQTT);
        lua_pushstring(L, logTopic ? logTopic : "syslog");
        lua_pushlstring(L, tbuf, cnt);
        lua_pushinteger(L, 0);
        lua_pcall(L, 4, 0, 0);
    }
#endif

    free(tbuf);
}

#if CONFIG_LUA_RTOS_USE_RSYSLOG
static int rsyslog_vprintf( const char *str, va_list l ) {
    // Allocate space
    char* tbuf = (char *)malloc(MAX_BUFF+1);
    if (tbuf) {
        int len = vsnprintf((char*)tbuf, MAX_BUFF, str, l);

        if (0 != logSock) {
            LOCK_TCPIP_CORE()
            sendto(logSock,tbuf,len,0,(struct sockaddr *)&logAddr,sizeof(logAddr));
            UNLOCK_TCPIP_CORE()
        }

        free(tbuf);
    }

    return vprintf( str, l );
}

static void reconnect_syslog() {
    if (0 != logSock) {
        close(logSock);
    }
    logSock = 0;

    esp_log_set_vprintf(vprintf);

    if (NETWORK_AVAILABLE()) {
        if (!logHost) logHost = (char*)logHostDefault;
        if (0 == strlen(logHost) || 0 == strcmp(logHost,"0.0.0.0"))
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
                p->sin_port = htons(CONFIG_LUA_RTOS_RSYSLOG_PORT);
                memcpy(&logAddr, p, sizeof(struct sockaddr_in));
            } else if (result->ai_family == AF_INET6) {
                struct sockaddr_in6 *p = (struct sockaddr_in6 *)result->ai_addr;
                p->sin6_port = htons(CONFIG_LUA_RTOS_RSYSLOG_PORT);
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

                    esp_log_set_vprintf(rsyslog_vprintf);
                }
            }
        }
    }

#if CONFIG_LUA_RTOS_LUA_USE_MQTT
    if (logMQTT) {
        esp_log_set_vprintf(msyslog_vprintf);
    }
#endif
}

static void syslog_net_callback(system_event_t *event){
    if ( (NETWORK_AVAILABLE() && (0 == logSock)) ||
        (!NETWORK_AVAILABLE() && (0 != logSock)) ) {
        reconnect_syslog();
    }
}
#endif

int openlog(logstat, logfac)
    int logstat, logfac;
{
    logStat = logstat;
    if (logfac != 0 && (logfac &~ LOG_FACMASK) == 0)
        logFacility = logfac;

    if (NULL != logFile) {
        fclose(logFile);
    }

    logFile = NULL;

    char file[PATH_MAX + 1];

    if (mount_messages_file(file, sizeof(file))) {
        mkfile(file);

        logFile = fopen(file,"a+");
    }

    if (NULL != logFile) {
        fflush(logFile);
    }

#if CONFIG_LUA_RTOS_USE_RSYSLOG
    reconnect_syslog();

    driver_error_t *error;
    if ((error = net_event_register_callback(syslog_net_callback))) {
        printf("couldn't register net callback, please restart syslog service from lua after changing connectivity\n");
        printf("you may use the command 'os.logcons(os.logcons())' to restart the syslog service from lua\n");
    }
#endif

    return (logFile == NULL);
}

void closelog() {
    if (NULL != logFile) {
        fclose(logFile);
    }
    logFile = NULL;

#if CONFIG_LUA_RTOS_USE_RSYSLOG
    if (0 != logSock) {
        close(logSock);
    }
    logSock = 0;

    driver_error_t *error;
    if ((error = net_event_unregister_callback(syslog_net_callback))) {
        printf("couldn't unregister net callback\n");
    }
#endif
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

#if CONFIG_LUA_RTOS_USE_RSYSLOG
const char *syslog_setloghost (const char *host)
{
    if (logHost && logHost != logHostDefault)
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
#endif

#if CONFIG_LUA_RTOS_LUA_USE_MQTT
static int msyslog_vprintf( const char *str, va_list l ) {
    // Allocate space
    char* tbuf = (char *)malloc(MAX_BUFF+1);
    if (tbuf) {
        int len = vsnprintf((char*)tbuf, MAX_BUFF, str, l);

        if (0 != logMQTT) {
            lua_State *L = pvGetLuaState(); // Get the thread's Lua state
            lua_rawgeti(L, LUA_REGISTRYINDEX, logMQTT);
            lua_getfield(L, -1, "publish");
            lua_rawgeti(L, LUA_REGISTRYINDEX, logMQTT);
            lua_pushstring(L, logTopic ? logTopic : "syslog");
            lua_pushlstring(L, tbuf, len);
            lua_pushinteger(L, 0);
            lua_pcall(L, 4, 0, 0);
        }

        free(tbuf);
    }

#if CONFIG_LUA_RTOS_USE_RSYSLOG
    if (0 != logSock) {
        return rsyslog_vprintf( str, l );
    }
#endif
    return vprintf( str, l );
}

int syslog_setlogmqtt (const int mqtt_ref, const char *topic)
{
    if (logMQTT) {
        lua_State *L = pvGetLuaState(); // Get the thread's Lua state
        luaL_unref(L, LUA_REGISTRYINDEX, logMQTT);
        logMQTT = 0;

        esp_log_set_vprintf(vprintf);
#if CONFIG_LUA_RTOS_USE_RSYSLOG
        if (0 != logSock) {
            esp_log_set_vprintf(rsyslog_vprintf);
        }
#endif
    }
    logMQTT = mqtt_ref;

    if (logTopic) {
        free(logTopic);
        logTopic = 0;
    }
    if (topic) {
        logTopic = strdup(topic);
    }

    if (logMQTT) {
        esp_log_set_vprintf(msyslog_vprintf);
    }

    return logMQTT;
}
int syslog_getlogmqtt ()
{
    return logMQTT;
}
#endif

