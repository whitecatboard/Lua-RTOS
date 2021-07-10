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

#ifndef _LUA_SYS_H_
#define _LUA_SYS_H_

#include "lua.h"

typedef struct {
    lua_State *L;  // Parent Lua thread
    lua_State *TL; // Callback Lua thread
    int callback;  // Callback reference (in parent thread)
    int lthread;   // Lua thread reference (in parent thread)
    int arg;       // Argument reference (in parent thread)
} lua_callback_t;

/**
 * @brief Create a lua callback.
 *
 * @param L Calling Lua state.
 * @param index The index on the L stack, where the callback's lua function is is located.
 *
 * @return
 *     - NULL if there is not enough memory.
 *     - Pointer to a lua_callback_t structure, with the callback's handle.
 */
lua_callback_t *luaS_callback_create(lua_State *L, int index);

/**
 * @brief Get the callback's lua state.
 *
 * @param callback A pointer to a callback handler created with the luaS_callback_create
 *                 function.
 *
 * @return
 *     - Pointer to the callback's lua state.
 */
lua_State *luaS_callback_state(lua_callback_t *callback);

/**
 * @brief Call the callback's lua function.
 *
 * @param callback A pointer to a callback handler created with the luaS_callback_create
 *                 function.
 * @param args Number of arguments of the function.
 */
int luaS_callback_call(lua_callback_t *callback, int args);

/**
 * @brief Call the callback's lua function.
 *
 * @param callback A pointer to a callback handler created with the luaS_callback_create
 *                 function.
 * @param args Number of arguments of the function.
 * @param rets Number of return values of the function.
 */
int luaS_callback_call_return(lua_callback_t *callback, int args, int rets);

/**
 * @brief Destroy a callback.
 *
 * @param callback A pointer to a callback handler created with the luaS_callback_create
 *                 function.
 */
void luaS_callback_destroy(lua_callback_t *callback);

#endif /* _LUA_SYS_H_ */
