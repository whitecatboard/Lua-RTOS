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
 * Lua RTOS, Lua net module
 *
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LUA_USE_NET

#include "lua.h"
#include "lauxlib.h"
#include "net.h"
#include "error.h"
#include "sys.h"

#include "modules.h"

#include "net_wifi.inc"
#include "net_eth.inc"
#include "net_spi_eth.inc"
#include "net_service_sntp.inc"
#include "net_service_http.inc"
#include "net_service_telnet.inc"
#include "net_service_curl.inc"
#include "net_service_captivedns.inc"
#include "net_service_mdns.inc"
#include "net_service_can.inc"
#include "net_service_openvpn.inc"
#include "net_service_ssh.inc"
#include "net_ssh.inc"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <lwip/ping.h>
#include "lwip/ip_addr.h"
#include "lwip/ip6_addr.h"

#include <sys/driver.h>
#include <drivers/net.h>

typedef union {
    uint32_t ipaddr;
    uint8_t ipbytes[4];
    uint16_t ipwords[2];
} net_ip;

static lua_callback_t *callback = NULL;

static void callback_func(system_event_t *event) {
    uint8_t for_us = 0; // The event is for us?
    char interface[3];
    char type[5];

    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
        case SYSTEM_EVENT_STA_STOP:
        case SYSTEM_EVENT_STA_CONNECTED:
        case SYSTEM_EVENT_STA_DISCONNECTED:
        case SYSTEM_EVENT_STA_GOT_IP:
        case SYSTEM_EVENT_STA_LOST_IP:
        case SYSTEM_EVENT_STA_AUTHMODE_CHANGE:
            if (!status_get_prev(STATUS_WIFI_CONNECTED | STATUS_WIFI_HAS_IP) && status_get(STATUS_WIFI_CONNECTED | STATUS_WIFI_HAS_IP)) {
                for_us = 1;
                strcpy(interface,"wf");
                strcpy(type,"up");
            } else if (status_get_prev(STATUS_WIFI_CONNECTED | STATUS_WIFI_HAS_IP) && !status_get(STATUS_WIFI_CONNECTED | STATUS_WIFI_HAS_IP)) {
                for_us = 1;
                strcpy(interface,"wf");
                strcpy(type,"down");
            }
            break;

#if CONFIG_LUA_RTOS_ETH_HW_TYPE_RMII
        case SYSTEM_EVENT_ETH_START:
        case SYSTEM_EVENT_ETH_STOP:
        case SYSTEM_EVENT_ETH_CONNECTED:
        case SYSTEM_EVENT_ETH_DISCONNECTED:
        case SYSTEM_EVENT_ETH_GOT_IP:
#endif
#if CONFIG_LUA_RTOS_ETH_HW_TYPE_SPI
        case SYSTEM_EVENT_SPI_ETH_START:
        case SYSTEM_EVENT_SPI_ETH_STOP:
        case SYSTEM_EVENT_SPI_ETH_CONNECTED:
        case SYSTEM_EVENT_SPI_ETH_DISCONNECTED:
        case SYSTEM_EVENT_SPI_ETH_GOT_IP:
#endif
#if (CONFIG_LUA_RTOS_ETH_HW_TYPE_RMII || CONFIG_LUA_RTOS_ETH_HW_TYPE_SPI)
            if (!status_get_prev(STATUS_ETH_CONNECTED | STATUS_ETH_HAS_IP) && status_get(STATUS_ETH_CONNECTED | STATUS_ETH_HAS_IP)) {
                for_us = 1;
                strcpy(interface,"en");
                strcpy(type,"up");
            } else if (status_get_prev(STATUS_ETH_CONNECTED | STATUS_ETH_HAS_IP) && !status_get(STATUS_ETH_CONNECTED | STATUS_ETH_HAS_IP)) {
                for_us = 1;
                strcpy(interface,"en");
                strcpy(type,"down");
            }
            break;
#endif
        default:
            for_us = 0;
            break;
    }

    if (for_us != 0 && callback != NULL) {
        lua_State *state = luaS_callback_state(callback);

        lua_createtable(state, 0, 0);

        lua_pushstring(state, "interface");
        lua_pushstring(state, interface);
        lua_settable(state, -3);

        lua_pushstring(state, "type");
        lua_pushstring(state, type);
        lua_settable(state, -3);

        luaS_callback_call(callback, 1);
    }
}

static int lnet_lookup(lua_State* L) {
    driver_error_t *error;
    struct sockaddr_in address;
    int raw = 0;

    const char* name = luaL_checkstring(L, 1);

    // Check if user wants result as a raw value, or as an string
    if (lua_gettop(L) == 2) {
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        if (lua_toboolean(L, 2)) {
            raw = 1;
        }
    }

    // Resolve name
    if ((error = net_lookup(name, 0, &address))) {
        return luaL_driver_error(L, error);
    }

    if (raw) {
        lua_pushinteger(L, address.sin_addr.s_addr);
    } else {
        lua_pushfstring(L, inet_ntoa(address.sin_addr.s_addr));
    }

    return 1;
}

static int lnet_packip(lua_State *L) {
    net_ip ip;
    unsigned i;

    if (lua_isnumber(L, 1)) {
        unsigned temp;
        for (i = 0; i < 4; i++) {
            temp = luaL_checkinteger(L, i + 1);
            if ((int)temp < 0 || temp > 255)
                   return luaL_exception(L, NET_ERR_INVALID_IP);

            ip.ipbytes[i] = temp;
        }
    }
    else {
        const char* pip = luaL_checkstring(L, 1);
        unsigned temp[4];
        signed len;

        if (sscanf(pip, "%u.%u.%u.%u%n", temp, temp + 1, temp + 2, temp + 3,
                &len) != 4 || len != strlen(pip))
               return luaL_exception(L, NET_ERR_INVALID_IP);

        for (i = 0; i < 4; i++) {
            if (temp[i] > 255)
                   return luaL_exception(L, NET_ERR_INVALID_IP);

            ip.ipbytes[i] = (uint8_t) temp[i];
        }
    }
    lua_pushinteger(L, ip.ipaddr);
    return 1;
}

static int lnet_unpackip(lua_State *L) {
    net_ip ip;
    unsigned i;
    const char* fmt;

    ip.ipaddr = (uint32_t) luaL_checkinteger(L, 1);
    fmt = luaL_checkstring(L, 2);
    if (!strcmp(fmt, "*n")) {
        for (i = 0; i < 4; i++)
            lua_pushinteger(L, ip.ipbytes[i]);
        return 4;
    } else if (!strcmp(fmt, "*s")) {
        lua_pushfstring(L, "%d.%d.%d.%d", (int) ip.ipbytes[0],
                (int) ip.ipbytes[1], (int) ip.ipbytes[2], (int) ip.ipbytes[3]);
        return 1;
    } else
        return luaL_error(L, "invalid format");
}

static int lnet_ping(lua_State* L) {
    driver_error_t *error;

    const char* name = luaL_checkstring(L, 1);
    int top = lua_gettop(L);
    int i, count = 0, interval = 0, size = 0, timeout = 0;

    for (i = 1; i <= top; i++) {
        switch (i) {
        case 2:
            count = luaL_optinteger(L, i, 0);
            break;
        case 3:
            interval = luaL_optinteger(L, i, 0);
            break;
        case 4:
            size = luaL_optinteger(L, i, 0);
            break;
        case 5:
            timeout = luaL_optinteger(L, i, 0);
            break;
        }
    }

    if ((error = net_ping(name, count, interval, size, timeout))) {
        return luaL_driver_error(L, error);
    }

    return 0;
}

static int lnet_stat(lua_State* L) {
    uint8_t table = 0;
    uint8_t interfaces = 0;

    // Check if user wants result as a table, or wants scan's result
    // on the console
    if (lua_gettop(L) == 1) {
        luaL_checktype(L, 1, LUA_TBOOLEAN);
        if (lua_toboolean(L, 1)) {
            table = 1;
        }
    }

    if (table) {
        lua_createtable(L, 1, 0);
        lua_pushinteger(L, interfaces++);
    }

    // This should be done in a more elegant way in future versions ...

#if CONFIG_LUA_RTOS_LUA_USE_NET
    // Call wf.stat
    lua_getglobal(L, "net");
    lua_getfield(L, -1, "wf");
    lua_getfield(L, -1, "stat");
    lua_pushboolean(L, table);

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        return luaL_error(L, lua_tostring(L, -1));
    }

    if (table) {
        lua_remove(L, 4);
        lua_remove(L, 4);
        lua_settable(L, -3);
    } else {
        lua_settop(L, 0);
    }
#endif

#if CONFIG_LUA_RTOS_LUA_USE_NET && (CONFIG_LUA_RTOS_ETH_HW_TYPE_SPI || CONFIG_LUA_RTOS_ETH_HW_TYPE_RMII)
    lua_pushinteger(L, interfaces++);

    // Call wf.stat
    lua_getglobal(L, "net");
    lua_getfield(L, -1, "en");
    lua_getfield(L, -1, "stat");
    lua_pushboolean(L, table);

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        return luaL_error(L, lua_tostring(L, -1));
    }

    if (table) {
        lua_remove(L, 4);
        lua_remove(L, 4);
        lua_settable(L, -3);
    } else {
        lua_settop(L, 0);
    }
#endif

#if CONFIG_LUA_RTOS_LUA_USE_NET && CONFIG_LUA_RTOS_USE_OPENVPN
extern u8_t volatile _openvpn_running;
if (_openvpn_running) {
    lua_pushinteger(L, interfaces++);

    // Call wf.stat
    lua_getglobal(L, "net");
    lua_getfield(L, -1, "tu");
    lua_getfield(L, -1, "stat");
    lua_pushboolean(L, table);

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        return luaL_error(L, lua_tostring(L, -1));
    }

    if (table) {
        lua_remove(L, 4);
        lua_remove(L, 4);
        lua_settable(L, -3);
    } else {
        lua_settop(L, 0);
    }
}
#endif

    return table;
}

static int lnet_connected(lua_State* L) {
  lua_pushboolean( L, NETWORK_AVAILABLE() );
  return 1;
}

static int lnet_ota(lua_State *L) {
    const char *server = luaL_optstring( L, 1, NULL );
    const char *project = luaL_optstring( L, 2, NULL );
    driver_error_t *error;
    int reboot = 1;
    if (lua_gettop(L) > 2) {
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        reboot = lua_toboolean( L, 3 );
    }
    int check = 1;
    if (lua_gettop(L) > 3) {
        luaL_checktype(L, 4, LUA_TBOOLEAN);
        check = lua_toboolean( L, 4 );
    }

    if ((error = net_ota(server, project, check, reboot))) {
        return luaL_driver_error(L, error);
    }

    return 0;
}

static int lnet_callback(lua_State *L) {
    driver_error_t *error;

    if (callback != NULL) {
        luaS_callback_destroy(callback);
        callback = NULL;
    }

    callback = luaS_callback_create(L, 1);
    if (callback == NULL) {
        return luaL_exception(L, NET_ERR_NOT_ENOUGH_MEMORY);
    }

    if ((error = net_event_register_callback(callback_func))) {
        return luaL_driver_error(L, error);
    }

    return 0;
}

static const LUA_REG_TYPE service_map[] = {
    { LSTRKEY( "sntp" ), LROVAL ( sntp_map ) },
#if CONFIG_LUA_RTOS_USE_HTTP_SERVER
    { LSTRKEY( "http" ), LROVAL ( http_map ) },
    { LSTRKEY( "captivedns" ), LROVAL ( captivedns_map ) },
#endif
#if CONFIG_LUA_RTOS_USE_TELNET_SERVER
    { LSTRKEY( "telnet" ), LROVAL ( telnet_map ) },
#endif
#if CONFIG_LUA_RTOS_LUA_USE_CAN
    { LSTRKEY( "can" ), LROVAL ( can_map ) },
#endif
#if CONFIG_LUA_RTOS_USE_OPENVPN
    { LSTRKEY( "openvpn" ), LROVAL ( openvpn_map ) },
#endif
#if CONFIG_LUA_RTOS_LUA_USE_MDNS
    { LSTRKEY( "mdns" ), LROVAL ( mdns_map ) },
#endif
#if CONFIG_LUA_RTOS_USE_SSH_SERVER
    { LSTRKEY( "ssh" ), LROVAL ( sshs_map ) },
#endif
};

static const LUA_REG_TYPE net_map[] = {
    { LSTRKEY( "stat" ),      LFUNCVAL ( lnet_stat ) },
    { LSTRKEY( "connected" ), LFUNCVAL ( lnet_connected ) },
    { LSTRKEY( "lookup" ),    LFUNCVAL ( lnet_lookup ) },
    { LSTRKEY( "packip" ),    LFUNCVAL ( lnet_packip ) },
    { LSTRKEY( "unpackip" ),  LFUNCVAL ( lnet_unpackip ) },
    { LSTRKEY( "ping" ),      LFUNCVAL ( lnet_ping ) },
    { LSTRKEY( "ota" ),       LFUNCVAL ( lnet_ota ) },
    { LSTRKEY( "callback" ),  LFUNCVAL ( lnet_callback ) },

#if CONFIG_LUA_RTOS_LUA_USE_SCP_NET
    { LSTRKEY( "scp" ), LROVAL ( scp_map ) },
    { LSTRKEY( "ssh" ), LROVAL ( ssh_map ) },
#endif

    { LSTRKEY( "wf" ), LROVAL ( wifi_map ) },

#if CONFIG_LUA_RTOS_ETH_HW_TYPE_SPI && CONFIG_LUA_RTOS_LUA_USE_NET
    { LSTRKEY( "en" ), LROVAL ( spi_eth_map ) },
#endif

#if CONFIG_LUA_RTOS_ETH_HW_TYPE_RMII && CONFIG_LUA_RTOS_LUA_USE_NET
    { LSTRKEY( "en" ), LROVAL ( eth_map ) },
#endif

#if CONFIG_LUA_RTOS_LUA_USE_NET && CONFIG_LUA_RTOS_USE_OPENVPN
    { LSTRKEY( "tu" ), LROVAL ( tun_map ) },
#endif

#if CONFIG_LUA_RTOS_LUA_USE_CURL_NET
    { LSTRKEY( "curl" ), LROVAL ( curl_map ) },
#endif

    { LSTRKEY( "service" ), LROVAL ( service_map ) },
    DRIVER_REGISTER_LUA_ERRORS(net)
    { LNILKEY, LNILVAL }
};

int luaopen_net(lua_State* L) {
#if CONFIG_LUA_RTOS_LUA_USE_MDNS
    luaopen_mdns(L);
#endif
    return 0;
}

MODULE_REGISTER_ROM(NET, net, net_map, luaopen_net, 1);

/*

 net.wf.setup(net.wf.mode.STA, "CITILAB","wifi@citilab")
 net.wf.start()
 net.service.sntp.start()
 net.service.sntp.stop()

 net.lookup("whitecarboard.org")
 */

#endif
