/*
 * Copyright (C) 2015 - 2020, IBEROXARXA SERVICIOS INTEGRALES, S.L.
 * Copyright (C) 2015 - 2020, Jaume Olivé Petrus (jolive@whitecatboard.org)
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
 * Lua RTOS, sys functions
 *
 */

#include "sys.h"
#include "lualib.h"
#include "lauxlib.h"

#include <stdlib.h>

lua_callback_t *luaS_callback_create(lua_State *L, int index) {
    lua_callback_t *callback;

    // Allocate the callback structure
    callback = calloc(1,sizeof(lua_callback_t));
    if (callback == NULL) {
        return NULL;
    }

    // Get the callback reference
    lua_lock(L);
    luaL_checktype(L, index, LUA_TFUNCTION);
    lua_pushvalue(L, index);

    int cbref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Get callback argument reference, if any
    int argref = LUA_REFNIL;

    if (lua_gettop(L) == index + 1) {
        // Callback has argument
        lua_pushvalue(L, index + 1);
        argref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    // Create a new Lua thread for the callback
    //
    // This lua thread has 2 copies of the callback at the top of
    // it's stack. This avoid calling lua_lock / lua_unlock to get
    // the callback reference each time the callback is executed.
    lua_State *TL = lua_newthread(L);
    int tref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Copy callback function to thread
    lua_rawgeti(L, LUA_REGISTRYINDEX, cbref);
    lua_xmove(L, TL, 1);
    lua_unlock(L);

    // Get a copy of the callback
    lua_pushvalue(TL,1);

    callback->L = L;
    callback->TL = TL;
    callback->callback = cbref;
    callback->lthread = tref;
    callback->arg = argref;

    return callback;
}

lua_State *luaS_callback_state(lua_callback_t *callback) {
    assert(callback != NULL);

    return callback->TL;
}

int luaS_callback_call_return(lua_callback_t *callback, int args, int rets) {
    if (callback->arg != LUA_REFNIL) {
        args++;
        lua_rawgeti(callback->TL, LUA_REGISTRYINDEX, callback->arg );
    }

    int rc = lua_pcall(callback->TL, args, rets, 0);

    // Copy callback to thread
    lua_pushvalue(callback->TL, 1);

    return rc;
}

int luaS_callback_call(lua_callback_t *callback, int args) {
    return luaS_callback_call_return(callback, args, 0);
}

void luaS_callback_destroy(lua_callback_t *callback) {
    assert(callback != NULL);

    lua_lock(callback->L);
    luaL_unref(callback->L, LUA_REGISTRYINDEX, callback->callback);
    luaL_unref(callback->L, LUA_REGISTRYINDEX, callback->lthread);
    lua_unlock(callback->L);

    free(callback);
}
