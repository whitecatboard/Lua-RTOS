/*
 * Lua RTOS, Lua event module
 *
 * Copyright (C) 2015 - 2017
 * IBEROXARXA SERVICIOS INTEGRALES, S.L. & CSS IBÉRICA, S.L.
 *
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 *
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

#include "luartos.h"

#if CONFIG_LUA_RTOS_LUA_USE_EVENT

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "auxmods.h"
#include "error.h"
#include "event.h"
#include "modules.h"

#include <sys/driver.h>

// This variables are defined at linker time
extern LUA_REG_TYPE event_error_map[];

// Module errors

#define EVENT_ERR_NOT_ENOUGH_MEMORY (DRIVER_EXCEPTION_BASE(EVENT_DRIVER_ID) |  0)

DRIVER_REGISTER_ERROR(EVENT, event, NotEnoughtMemory, "not enough memory", EVENT_ERR_NOT_ENOUGH_MEMORY);

static int levent_create( lua_State* L ) {
	// Create user data
    event_userdata *udata = (event_userdata *)lua_newuserdata(L, sizeof(event_userdata));
    if (!udata) {
    	return luaL_exception(L, EVENT_ERR_NOT_ENOUGH_MEMORY);
    }

    // Init mutex
    mtx_init(&udata->mtx, NULL, NULL, 0);

    // Create listener list
    list_init(&udata->listener_list, 1);

    luaL_getmetatable(L, "event.ins");
    lua_setmetatable(L, -2);

    return 1;
}

static int levent_wait( lua_State* L ) {
    event_userdata *udata = NULL;
    xQueueHandle q;
    int id;
    int ret;

    // Get user data
    udata = (event_userdata *)luaL_checkudata(L, 1, "event.ins");
    luaL_argcheck(L, udata, 1, "event expected");

    mtx_lock(&udata->mtx);

    // Create a queue
    q = xQueueCreate(1, sizeof(uint32_t));
    if (!q) {
    	printf("error\r\n");
    	mtx_unlock(&udata->mtx);
    	return luaL_exception(L, EVENT_ERR_NOT_ENOUGH_MEMORY);
    }

    //Add queue
    ret = list_add(&udata->listener_list, q, &id);
    if (ret) {
    	mtx_unlock(&udata->mtx);
    	return luaL_exception(L, EVENT_ERR_NOT_ENOUGH_MEMORY);
    }

    mtx_unlock(&udata->mtx);

    // Wait
    uint32_t d;

    xQueueReceive(q, &d, portMAX_DELAY);

    return 0;
}

static int levent_broadcast( lua_State* L ) {
    event_userdata *udata = NULL;
    uint32_t q;

    int idx = 1;

    // Get user data
	udata = (event_userdata *)luaL_checkudata(L, 1, "event.ins");
    luaL_argcheck(L, udata, 1, "event expected");

    mtx_lock(&udata->mtx);

    // Call to all listeners
    while (idx >= 1) {
    	// Get queue
        if (list_get(&udata->listener_list, idx, (void **)&q)) {
        	break;
        }

        // Unblock waiting thread on this listener
    	uint32_t d = 0;
    	xQueueSend((xQueueHandle)q, &d, portMAX_DELAY);

        // Remove this listener
        vQueueDelete((xQueueHandle)q);
        list_remove(&udata->listener_list, idx, 0);

        // Next listener
        idx = list_next(&udata->listener_list, idx);
    }

    mtx_unlock(&udata->mtx);

    return 0;
}

// Destructor
static int levent_ins_gc (lua_State *L) {
    event_userdata *udata = NULL;
    uint32_t q;
    int res;
    int idx = 1;

    udata = (event_userdata *)luaL_checkudata(L, 1, "event.ins");
	if (udata) {
	    // Destroy all listeners
	    while (idx >= 0) {
	        res = list_get(&udata->listener_list, idx, (void **)&q);
	        if (res) {
	        	break;
	        }

	        vQueueDelete((xQueueHandle)q);

	        list_remove(&udata->listener_list, idx, 0);
	        idx = list_next(&udata->listener_list, idx);
	    }

	    mtx_destroy(&udata->mtx);
	    list_destroy(&udata->listener_list, 0);
	}

	return 0;
}

static const LUA_REG_TYPE levent_map[] = {
    { LSTRKEY( "create"  ),			LFUNCVAL( levent_create ) },
	{ LSTRKEY( "error"   ), 		LROVAL( event_error_map )},
    { LNILKEY, LNILVAL }
};

static const LUA_REG_TYPE levent_ins_map[] = {
	{ LSTRKEY( "wait"        ),		LFUNCVAL( levent_wait        ) },
  	{ LSTRKEY( "broadcast"   ),		LFUNCVAL( levent_broadcast   ) },
	{ LSTRKEY( "__metatable" ),    	LROVAL  ( levent_ins_map     ) },
	{ LSTRKEY( "__index"     ),   	LROVAL  ( levent_ins_map     ) },
	{ LSTRKEY( "__gc"        ),   	LROVAL  ( levent_ins_gc      ) },
    { LNILKEY, LNILVAL }
};

LUALIB_API int luaopen_event( lua_State *L ) {
    luaL_newmetarotable(L,"event.ins", (void*)levent_ins_map);
    return 0;
}

MODULE_REGISTER_MAPPED(EVENT, event, levent_map, luaopen_event);
DRIVER_REGISTER(EVENT,event,NULL,NULL,NULL);

#endif


/*

a = true
e1 = event.create()
e2 = event.create()

thread.start(function()
  while true do
  	  e1:wait()
  	  while a do
	  	  tmr.delay(2)
	  end
	  print("hi from 1")
  end
end)

thread.start(function()
  while true do
  	  e2:wait()
  	  e1:broadcast()
  	  tmr.delay(4)
	  print("hi from 2")
  end
end)

e1:broadcast(false)
e2:broadcast(false)

*/
