/*
 * Copyright (C) 2015 - 2018, IBEROXARXA SERVICIOS INTEGRALES, S.L.
 * Copyright (C) 2015 - 2018, Jaume Olivé Petrus (jolive@whitecatboard.org)
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the <organization> nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *     * The WHITECAT logotype cannot be changed, you can remove it, but you
 *       cannot change it in any way. The WHITECAT logotype is:
 *
 *          /\       /\
 *         /  \_____/  \
 *        /_____________\
 *        W H I T E C A T
 *
 *     * Redistributions in binary form must retain all copyright notices printed
 *       to any local or remote output device. This include any reference to
 *       Lua RTOS, whitecatboard.org, Lua, and other copyright notices that may
 *       appear in the future.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Lua RTOS, network manager
 *
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LUA_USE_NET

#ifndef NET_H_
#define NET_H_

#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#include <sys/driver.h>
#include <sys/status.h>

#define MAX_NET_EVENT_CALLBACKS 3

#define evWIFI_SCAN_END         ( 1 << 0 )
#define evWIFI_CONNECTED        ( 1 << 1 )
#define evWIFI_CANT_CONNECT     ( 1 << 2 )
#define evSPI_ETH_CONNECTED        ( 1 << 3 )
#define evSPI_ETH_CANT_CONNECT  ( 1 << 4 )
#define evETH_CONNECTED         ( 1 << 5 )
#define evETH_CANT_CONNECT      ( 1 << 6 )

typedef struct {
    ip4_addr_t ip;
    ip4_addr_t netmask;
    ip4_addr_t gw;
    uint8_t    mac[6];
    ip6_addr_t ip6;
} ifconfig_t;

// This macro gets a reference for this driver into drivers array
#define NET_DRIVER driver_get_by_name("net")

// NET errors
#define NET_ERR_NOT_AVAILABLE              (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  0)
#define NET_ERR_INVALID_IP                 (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  1)
#define NET_ERR_NO_MORE_CALLBACKS          (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  2)
#define NET_ERR_NO_CALLBACK_NOT_FOUND      (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  3)
#define NET_ERR_NAME_CANNOT_BE_RESOLVED    (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  4)
#define NET_ERR_NAME_CANNOT_CREATE_SOCKET  (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  5)
#define NET_ERR_NAME_CANNOT_SETUP_SOCKET   (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  6)
#define NET_ERR_NAME_CANNOT_CONNECT        (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  7)
#define NET_ERR_CANNOT_CREATE_SSL          (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  8)
#define NET_ERR_CANNOT_CONNECT_SSL         (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  9)
#define NET_ERR_NOT_ENOUGH_MEMORY          (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  10)
#define NET_ERR_INVALID_RESPONSE           (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  11)
#define NET_ERR_INVALID_CONTENT            (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  12)
#define NET_ERR_NO_OTA                     (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  13)
#define NET_ERR_OTA_NOT_ENABLED            (DRIVER_EXCEPTION_BASE(NET_DRIVER_ID) |  14)

extern const int net_errors;
extern const int net_error_map;

typedef void (*net_event_register_callback_t)(system_event_t *event);

/**
 * @brief Init the network driver. It must be called before use any network function.
 *        The function inits the tcp/ip adapter, the main network event loop, and some
 *        internal structures required by Lua RTOS.
 *
 * @return
 *     - NULL success
 *     - Pointer to driver_error_t if some error occurs. Reserved for future use.
 */
driver_error_t *net_init();

/**
 * @brief Checks the network connectivity. We consider that network is available if
 *        one of the network interface are init and up.
 *
 * @return
 *     - NULL success, the network is available
 *     - Pointer to driver_error_t if some error occurs.
 *
 *          NET_ERR_NOT_AVAILABLE
 */
driver_error_t *net_check_connectivity();

/**
 * @brief Lookup for a hostname, and get the IP, doing a DNS search.
 *
 * @param name The hostname.
 * @param name The port number.
 * @param address A pointer to a sockaddr_in structure where put the IP address.
 *
 * @return
 *     - NULL success, the host name where find by the DNS
 *     - Pointer to driver_error_t if some error occurs.
 *
 *          NET_ERR_NOT_AVAILABLE
 */
driver_error_t *net_lookup(const char *name, int port, struct sockaddr_in *address);

/**
 * @brief Register a function callback in the network event loop. When an event is
 *        received in the event loop the net driver executes the default treatment
 *        for the event, and at the end call to the registered callbacks.
 *
 * @param name A pointer to the callback function.
 *
 * @return
 *     - NULL success
 *     - Pointer to driver_error_t if some error occurs.
 *
 *          NET_ERR_NO_MORE_CALLBACKS
 */
driver_error_t *net_event_register_callback(net_event_register_callback_t func);

/**
 * @brief Unregister a function callback previously registered with the
 *        net_event_register_callback function.
 *
 * @param name A pointer to the callback function.
 *
 * @return
 *     - NULL success
 *     - Pointer to driver_error_t if some error occurs.
 *
 *          NET_ERR_NO_MORE_CALLBACKS
 */
driver_error_t *net_event_unregister_callback(net_event_register_callback_t func);


/**
 * @brief Wait for network init. The network is init when the TCP/IP stack is
 *        started.
 *
 * @param timeout Number of seconds to wait for network init.
 *
 * @return
 *     - 1: network is init
 *     - 0: network isn't init, or timeout
 *
 */
int wait_for_network_init(uint32_t timeout);

/**
 * @brief Wait for network availability.
 *
 * @param timeout Number of seconds to wait for network availability.
 *
 * @return
 *     - 1: network is available
 *     - 0: network isn't available, or timeout
 *
 */
int wait_for_network(uint32_t timeout);

/**
 * @brief Check if network is started.
 *
 * @return
 *     - 1: network is started
 *     - 0: network isn't started
 *
 */
int network_started();

driver_error_t *net_ping(const char *name, int count, int interval, int size, int timeout);

driver_error_t *net_reconnect();
driver_error_t *net_ota(const char *server, const char *project, int verify, int reboot);

#endif

#endif
