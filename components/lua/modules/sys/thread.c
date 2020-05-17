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
 * Lua RTOS, Lua thread module
 *
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LUA_USE_THREAD

#include "lua.h"
#include "lapi.h"
#include "lauxlib.h"
#include "lgc.h"
#include "lmem.h"
#include "ldo.h"
#include "thread.h"
#include "error.h"
#include "blocks.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <errno.h>

#include <pthread.h>

#include <drivers/uart.h>
#include <sys/console.h>
#include <sys/fcntl.h>

// Module errors
#define LUA_THREAD_ERR_NOT_ENOUGH_MEMORY        (DRIVER_EXCEPTION_BASE(THREAD_DRIVER_ID) |  0)
#define LUA_THREAD_ERR_NOT_ALLOWED              (DRIVER_EXCEPTION_BASE(THREAD_DRIVER_ID) |  1)
#define LUA_THREAD_ERR_NON_EXISTENT             (DRIVER_EXCEPTION_BASE(THREAD_DRIVER_ID) |  2)
#define LUA_THREAD_ERR_CANNOT_START             (DRIVER_EXCEPTION_BASE(THREAD_DRIVER_ID) |  3)
#define LUA_THREAD_ERR_INVALID_STACK_SIZE       (DRIVER_EXCEPTION_BASE(THREAD_DRIVER_ID) |  4)
#define LUA_THREAD_ERR_INVALID_PRIORITY         (DRIVER_EXCEPTION_BASE(THREAD_DRIVER_ID) |  5)
#define LUA_THREAD_ERR_INVALID_CPU_AFFINITY     (DRIVER_EXCEPTION_BASE(THREAD_DRIVER_ID) |  6)
#define LUA_THREAD_ERR_CANNOT_MONITOR_AS_TABLE     (DRIVER_EXCEPTION_BASE(THREAD_DRIVER_ID) |  7)
#define LUA_THREAD_ERR_INVALID_THREAD_ID        (DRIVER_EXCEPTION_BASE(THREAD_DRIVER_ID) |  8)
#define LUA_THREAD_ERR_GET_TASKLIST              (DRIVER_EXCEPTION_BASE(THREAD_DRIVER_ID) |  9)

// Register driver and messages
DRIVER_REGISTER_BEGIN(THREAD,thread,0,NULL,NULL);
    DRIVER_REGISTER_ERROR(THREAD, thread, NotEnoughtMemory, "not enough memory", LUA_THREAD_ERR_NOT_ENOUGH_MEMORY);
    DRIVER_REGISTER_ERROR(THREAD, thread, NotAllowed, "not allowed", LUA_THREAD_ERR_NOT_ALLOWED);
    DRIVER_REGISTER_ERROR(THREAD, thread, NonExistentThread, "non-existent thread", LUA_THREAD_ERR_NON_EXISTENT);
    DRIVER_REGISTER_ERROR(THREAD, thread, CannotStart, "cannot start thread", LUA_THREAD_ERR_CANNOT_START);
    DRIVER_REGISTER_ERROR(THREAD, thread, InvalidStackSize, "invalid stack size", LUA_THREAD_ERR_INVALID_STACK_SIZE);
    DRIVER_REGISTER_ERROR(THREAD, thread, InvalidPriority, "invalid priority", LUA_THREAD_ERR_INVALID_PRIORITY);
    DRIVER_REGISTER_ERROR(THREAD, thread, InvalidCPUAffinity, "invalid CPU affinity", LUA_THREAD_ERR_INVALID_CPU_AFFINITY);
    DRIVER_REGISTER_ERROR(THREAD, thread, CannotMonitorAsTable, "you can't monitor thread as table", LUA_THREAD_ERR_CANNOT_MONITOR_AS_TABLE);
    DRIVER_REGISTER_ERROR(THREAD, thread, InvalidThreadId, "invalid thread id", LUA_THREAD_ERR_INVALID_THREAD_ID);
    DRIVER_REGISTER_ERROR(THREAD, thread, GetTaskList, "cannot get tasklist", LUA_THREAD_ERR_GET_TASKLIST);
DRIVER_REGISTER_END(THREAD,thread,0,NULL,NULL);

typedef struct {
    pthread_t pthread;
    lthread_t *lthread;
} lcleanup_info_t;

extern pthread_t lua_thread;

static void cleanup(void *args) {
    lcleanup_info_t *info = (lcleanup_info_t *)args;

    // Get pthread
    struct pthread *pthread = _pthread_get(info->pthread);

    if (pthread) {
        lua_lock(info->lthread->PL);
        luaL_unref(info->lthread->L, LUA_REGISTRYINDEX, info->lthread->function_ref);
        luaL_unref(info->lthread->PL, LUA_REGISTRYINDEX, info->lthread->thread_ref);
        lua_unlock(info->lthread->PL);
    }

    free(info->lthread);
    free(info);
}

void lthread_init(void *arg) {
    lthread_t *thread = (struct lthread *)arg;

    uxSetLThread(thread);
}

void *lthread_start_task(void *arg) {
    lthread_t *thread = (struct lthread *)arg;

    luaL_checktype(thread->L, 1, LUA_TFUNCTION);

    // Create and populate cleanup info
    lcleanup_info_t *info = malloc(sizeof(lcleanup_info_t));
    if (!info) {
        lua_writestringerror("%s\n", "not enough memory");
        pthread_exit(NULL);
    }

    info->pthread = pthread_self();
    info->lthread = thread;

    // Set cleanup handlers
    pthread_cleanup_push(cleanup, (void *)info);

    // Execute thread function
    int status = lua_pcall(thread->L, 0, 0, 0);
    if (status != LUA_OK) {
        BlockContext *bctx;
        if ((bctx = luaVB_getBlock(thread->L, NULL)) != NULL) {
            lua_pop(thread->L, 1);
        } else {
            // If error have not been raised inside a block execution context, write it
            // to the console
            const char *msg = lua_tostring(thread->L, -1);
            lua_writestringerror("%s\n", msg);
            lua_pop(thread->L, 1);
        }

        pthread_exit(NULL);
    }

    pthread_cleanup_pop(1);

    pthread_exit(NULL);
}

static int lthread_suspend_pthreads(lua_State *L, int thid) {
    task_info_t *info;
    task_info_t *cinfo;
    int suspended = 0;
    int check = 0;

    // Sanity checks
    if ((thid < 1) && (thid != -1)) {
        luaL_exception(L, LUA_THREAD_ERR_INVALID_THREAD_ID);
    }

    if (thid == lua_thread) {
        luaL_exception_extended(L, LUA_THREAD_ERR_NOT_ALLOWED, "lua main thread can not be suspended");
    }

    // Check that thid exists
    if (thid != -1) {
        check = 1;
    }

    info = GetTaskInfo();
    if (!info) {
        luaL_exception(L, LUA_THREAD_ERR_GET_TASKLIST);
    }

    cinfo = info;
    while (cinfo->stack_size > 0) {
        if ((cinfo->task_type == TASK_LUA) && (cinfo->thid != lua_thread)) {
            if (thid && (cinfo->thid == thid)) {
                _pthread_suspend(cinfo->thid);
                suspended++;
                break;
            } else if (thid == -1) {
                _pthread_suspend(cinfo->thid);
                suspended++;
            }
        }
        cinfo++;
    }

    free(info);

    if (!suspended && check) {
        luaL_exception_extended(L, LUA_THREAD_ERR_NON_EXISTENT, "or is not a Lua thread");
    }

    return 0;
}

static int lthread_resume_pthreads(lua_State *L, int thid) {
    task_info_t *info;
    task_info_t *cinfo;
    int resumed = 0;
    int check = 0;

    if ((thid < 1) && (thid != -1)) {
        luaL_exception(L, LUA_THREAD_ERR_INVALID_THREAD_ID);
    }

    if (thid == lua_thread) {
        luaL_exception_extended(L, LUA_THREAD_ERR_NOT_ALLOWED, "lua main thread can not be suspended");
    }

    // Check that thid exists
    if (thid != -1) {
        check = 1;
    }

    info = GetTaskInfo();
    if (!info) {
        luaL_exception(L, LUA_THREAD_ERR_GET_TASKLIST);
    }

    cinfo = info;
    while (cinfo->stack_size > 0) {
        if ((cinfo->task_type == TASK_LUA) && (cinfo->thid != lua_thread)) {
            if (thid && (cinfo->thid == thid)) {
                _pthread_resume(cinfo->thid);
                resumed++;
                break;
            } else if (thid == -1) {
                _pthread_resume(cinfo->thid);
                resumed++;
            }
        }
        cinfo++;
    }

    free(info);

    if (!resumed && check) {
        luaL_exception_extended(L, LUA_THREAD_ERR_NON_EXISTENT, "or is not a Lua thread");
    }

    return 0;
}

static int lthread_stop_pthreads(lua_State *L, int thid) {
    task_info_t *info;
    task_info_t *cinfo;
    int stopped = 0;
    int check = 0;

    // Sanity checks
    if ((thid < 1) && (thid != -1)) {
        luaL_exception(L, LUA_THREAD_ERR_INVALID_THREAD_ID);
    }

    if (thid == lua_thread) {
        luaL_exception_extended(L, LUA_THREAD_ERR_NOT_ALLOWED, "lua main thread can not be stopped");
    }

    // Check that thid exists
    if (thid != -1) {
        check = 1;
    }

    info = GetTaskInfo();
    if (!info) {
        luaL_exception(L, LUA_THREAD_ERR_GET_TASKLIST);
    }

    cinfo = info;
    while (cinfo->stack_size > 0) {
        if ((cinfo->task_type == TASK_LUA) && (cinfo->thid != lua_thread)) {
            if (thid && (cinfo->thid == thid)) {
                _pthread_stop(cinfo->thid);

                luaL_unref(L, LUA_REGISTRYINDEX, cinfo->lthread->function_ref);
                luaL_unref(L, LUA_REGISTRYINDEX, cinfo->lthread->thread_ref);

                _pthread_free(cinfo->thid, 1);

                stopped++;
                break;
            } else if (thid == -1) {
                _pthread_stop(cinfo->thid);

                luaL_unref(L, LUA_REGISTRYINDEX, cinfo->lthread->function_ref);
                luaL_unref(L, LUA_REGISTRYINDEX, cinfo->lthread->thread_ref);

                _pthread_free(cinfo->thid, 1);

                stopped++;
            }
        }
        cinfo++;
    }

    free(info);

    if (!stopped && check) {
        luaL_exception_extended(L, LUA_THREAD_ERR_NON_EXISTENT, "or is not a Lua thread");
    }

    return 0;
}

static int lthread_list(lua_State *L) {
    char status[5];
    char type[7];
    uint8_t table = 0;
    uint8_t monitor = 0;
    uint8_t all = 0;
    task_info_t *info;
    task_info_t *cinfo;

    // Check if user wants result as a table, or wants result
    // on the console
    if (lua_gettop(L) > 0) {
        luaL_checktype(L, 1, LUA_TBOOLEAN);
        if (lua_toboolean(L, 1)) {
            table = 1;
        }
    }

    // Check if user wants to monitor threads at regular intervals
    if (lua_gettop(L) > 1) {
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        if (lua_toboolean(L, 2)) {
            monitor = 1;
        }
    }

    // Check if user wants to list all threads
    if (lua_gettop(L) > 2) {
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        if (lua_toboolean(L, 3)) {
            all = 1;
        }
    }

    if (table && monitor) {
        return luaL_exception(L, LUA_THREAD_ERR_CANNOT_MONITOR_AS_TABLE);
    }

    if (monitor) {
        console_clear();
        console_hide_cursor();
    }

    monitor_loop:
    info = GetTaskInfo();
    if (!info) {
        luaL_exception(L, LUA_THREAD_ERR_GET_TASKLIST);
    }

    if (monitor) {
        console_gotoxy(0,0);
        printf("Monitoring threads every 0.5 seconds\n\n");
    }

    if (!table) {
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    #ifdef CONFIG_HEAP_TASK_TRACKING
        printf("-------------------------------------------------------------------------------------------------------------------------\n");
        printf("           |        |                  |        |        CPU        |            STACK               |        HEAP       \n");
        printf("THID       | TYPE   | NAME             | STATUS | CORE   PRIO     %% |   SIZE     FREE     USED       |  BLOCKS    TOTAL  \n");
        printf("-------------------------------------------------------------------------------------------------------------------------\n");
    #else
        printf("-----------------------------------------------------------------------------------------------------\n");
        printf("           |        |                  |        |        CPU        |            STACK               \n");
        printf("THID       | TYPE   | NAME             | STATUS | CORE   PRIO     %% |   SIZE     FREE     USED       \n");
        printf("-----------------------------------------------------------------------------------------------------\n");
    #endif
#else
    #ifdef CONFIG_HEAP_TASK_TRACKING
        printf("-------------------------------------------------------------------------------------------------------------------\n");
        printf("           |        |                  |        |      |      |            STACK               |        HEAP       \n");
        printf("THID       | TYPE   | NAME             | STATUS | CORE | PRIO |   SIZE     FREE     USED       |  BLOCKS    TOTAL  \n");
        printf("-------------------------------------------------------------------------------------------------------------------\n");
    #else
        printf("-----------------------------------------------------------------------------------------------\n");
        printf("           |        |                  |        |      |      |            STACK               \n");
        printf("THID       | TYPE   | NAME             | STATUS | CORE | PRIO |   SIZE     FREE     USED       \n");
        printf("-----------------------------------------------------------------------------------------------\n");
    #endif
#endif
    } else {
        lua_newtable(L);
    }

    // For each Lua thread ...
    int table_row = 0;

    cinfo = info;
    while (cinfo->task_type != TASK_DELIMITER) {
        if ((cinfo->task_type != TASK_LUA) && (!all)) {
            cinfo++;
            continue;
        }
        if (cinfo->task_type == TASK_DELETED) {
            cinfo++;
            continue;
        }

        // Get status
        switch (cinfo->status) {
            case StatusRunning: strcpy(status,"run"); break;
            case StatusSuspended: strcpy(status,"susp"); break;
            default:
            strcpy(status,"");
        }

        switch (cinfo->task_type) {
            case TASK_HEAP: strcpy(type,"heap"); break;
            case TASK_PTHREAD: strcpy(type,"thread"); break;
            case TASK_LUA: strcpy(type,"lua"); break;
            default: strcpy(type,"task"); break;
        }

        if (!table) {
            printf(
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    #ifdef CONFIG_HEAP_TASK_TRACKING
                    "%10d   %-6s   %-16s   %-6s   % 4d   % 4d   % 3d   % 6d   % 6d   % 6d (% 3d%%)   % 6d  % 7d  \n",
    #else
                    "%10d   %-6s   %-16s   %-6s   % 4d   % 4d   % 3d   % 6d   % 6d   % 6d (% 3d%%)   \n",
    #endif
#else
    #ifdef CONFIG_HEAP_TASK_TRACKING
                    "%10d   %-6s   %-16s   %-6s   % 4d   % 4d   % 6d   % 6d   % 6d (% 3d%%)   % 6d  % 7d  \n",
    #else
                    "%10d   %-6s   %-16s   %-6s   % 4d   % 4d   % 6d   % 6d   % 6d (% 3d%%)   \n",
    #endif
#endif
                    cinfo->thid,
                    type,
                    cinfo->name,
                    status,
                    cinfo->core,
                    cinfo->prio,
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
                    cinfo->cpu_usage,
#endif
                    cinfo->stack_size,
                    cinfo->free_stack,
                    cinfo->stack_size - cinfo->free_stack,
                    cinfo->stack_size==0 ? 0 : (int)(100 * ((float)(cinfo->stack_size - cinfo->free_stack) / (float)cinfo->stack_size))
#ifdef CONFIG_HEAP_TASK_TRACKING
                    ,cinfo->heap_count
                    ,cinfo->heap_size
#endif
            );
        } else {

            lua_pushnumber(L, ++table_row); //row index
            lua_newtable(L);

            lua_pushinteger(L, cinfo->thid);
            lua_setfield (L, -2, "thid");

            lua_pushstring(L, type);
            lua_setfield (L, -2, "type");

            lua_pushstring(L, cinfo->name);
            lua_setfield (L, -2, "name");

            lua_pushstring(L, status);
            lua_setfield (L, -2, "status");

            lua_pushinteger(L, cinfo->core);
            lua_setfield (L, -2, "core");

            lua_pushinteger(L, cinfo->prio);
            lua_setfield (L, -2, "prio");

#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
            lua_pushinteger(L, cinfo->cpu_usage);
            lua_setfield (L, -2, "usage");
#endif

            lua_pushinteger(L, cinfo->stack_size);
            lua_setfield (L, -2, "stack_size");

            lua_pushinteger(L, cinfo->free_stack);
            lua_setfield (L, -2, "free_stack");

            lua_pushinteger(L, cinfo->stack_size - cinfo->free_stack);
            lua_setfield (L, -2, "used_stack");

#ifdef CONFIG_HEAP_TASK_TRACKING

            lua_pushinteger(L, cinfo->heap_count);
            lua_setfield (L, -2, "heap_blocks");

            lua_pushinteger(L, cinfo->heap_size);
            lua_setfield (L, -2, "heap_total");
#endif

            lua_settable( L, -3 );
        }

        cinfo++;
    }

    free(info);

    if (monitor) {
        printf("\n\nPress q for exit");

        char press;

        int flags = fcntl(fileno(stdin), F_GETFL, 0);
        fcntl(fileno(stdin), F_SETFL, flags | O_NONBLOCK);

        if (read(fileno(stdin), &press, 1) == 1) {
            if ((press == 'q') || (press == 'Q')) {
                fcntl(fileno(stdin), F_SETFL, flags);
                console_show_cursor();
                printf("\r\n");

                return table;
            }
        }

        fcntl(fileno(stdin), F_SETFL, flags);

        usleep(500 * 1000);
        goto monitor_loop;
    }

    return table;
}

static int new_thread(lua_State* L, int run) {
    struct lthread *thread;
    pthread_attr_t attr;
    struct sched_param sched;
    int res;
    pthread_t id;
    int retries;

    // Get stack size, priotity and cpu affinity
    int stack = luaL_optinteger(L, 2, CONFIG_LUA_RTOS_LUA_THREAD_STACK_SIZE);
    int priority = luaL_optinteger(L, 3, CONFIG_LUA_RTOS_LUA_THREAD_PRIORITY);
    int affinity = luaL_optinteger(L, 4, CONFIG_LUA_RTOS_LUA_THREAD_CPU);
    const char *name = luaL_optstring(L, 5, "lua_thread");

    // Sanity checks
    if (stack < PTHREAD_STACK_MIN) {
        return luaL_exception(L, LUA_THREAD_ERR_INVALID_STACK_SIZE);
    }

    if ((priority < ESP_TASK_PRIO_MIN + 3) || (priority > ESP_TASK_PRIO_MAX)) {
        return luaL_exception(L, LUA_THREAD_ERR_INVALID_PRIORITY);
    }

    if ((affinity < 0) || (affinity > 1)) {
        return luaL_exception(L, LUA_THREAD_ERR_INVALID_CPU_AFFINITY);
    }

    // Allocate space for lthread info
    thread = (struct lthread *)malloc(sizeof(struct lthread));
    if (!thread) {
        return luaL_exception(L, LUA_THREAD_ERR_NOT_ENOUGH_MEMORY);
    }

    // Check for argument is a function, and store it's reference
    luaL_checktype(L, 1, LUA_TFUNCTION);
    thread->function_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Create a new state, move function to it and store thread reference
    thread->PL = L;
    thread->L = lua_newthread(L);
    thread->thread_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_rawgeti(L, LUA_REGISTRYINDEX, thread->function_ref);

    // Ensure that we have only the thread function in the lua stack prior to
    // move it to the lua thread stack
    lua_settop(L, 1);

    lua_xmove(L, thread->L, 1);

    // Init thread attributes
    pthread_attr_init(&attr);

    // Set stack size
    pthread_attr_setstacksize(&attr, stack);

    // Set priority
    sched.sched_priority = priority;
    pthread_attr_setschedparam(&attr, &sched);

    // Set CPU
    cpu_set_t cpu_set = CPU_INITIALIZER;

    CPU_SET(affinity, &cpu_set);

    pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpu_set);
    pthread_attr_setinitialstate_np(&attr, PTHREAD_INITIAL_STATE_SUSPEND);
    pthread_attr_setinitfunc_np(&attr, lthread_init);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    retries = 0;

    retry:
    res = pthread_create(&id, &attr, lthread_start_task, thread);
    if (res) {
        if ((res == ENOMEM) && (retries < 4)) {
            luaC_checkGC(L); /* stack grow uses memory */
            luaD_checkstack(L, LUA_MINSTACK); /* ensure minimum stack size */

            retries++;
            goto retry;
        }

        return luaL_exception_extended(L, LUA_THREAD_ERR_CANNOT_START, strerror(res));
    }

    pthread_setname_np(id, name);

    lua_pushinteger(L, id);

    if (run) {
        _pthread_resume(id);
    }

    return 1;
}

// Create a new thread and run it
static int lthread_start(lua_State* L) {
    return new_thread(L, 1);
}

// Create a new thread in suspended mode
static int lthread_create(lua_State* L) {
    return new_thread(L, 0);
}

static int lthread_self(lua_State* L) {
    lua_pushinteger(L, pthread_self());

    return 1;
}

static int lthread_create_mutex(lua_State* L) {
    // Init mutex
    pthread_mutexattr_t attr;

    int type = luaL_optinteger(L, 1, PTHREAD_MUTEX_RECURSIVE);

    mutex_userdata *mtx = (mutex_userdata *)lua_newuserdata(L, sizeof(mutex_userdata));

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, type);

    mtx->mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_init(&mtx->mtx, &attr);

    luaL_getmetatable(L, "thread.mutex");
    lua_setmetatable(L, -2);

    return 1;
}

static int lthread_lock(lua_State* L) {
    mutex_userdata *mtx = NULL;

    mtx = (mutex_userdata *)luaL_checkudata(L, 1, "thread.mutex");
    luaL_argcheck(L, mtx, 1, "mutex expected");

    pthread_mutex_lock(&mtx->mtx);

    return 0;
}

static int lthread_unlock(lua_State* L) {
    mutex_userdata *mtx = NULL;

    mtx = (mutex_userdata *)luaL_checkudata(L, 1, "thread.mutex");
    luaL_argcheck(L, mtx, 1, "mutex expected");

    pthread_mutex_unlock(&mtx->mtx);

    return 0;
}

static int lthread_trylock(lua_State* L) {
    mutex_userdata *mtx = NULL;

    mtx = (mutex_userdata *)luaL_checkudata(L, 1, "thread.mutex");
    luaL_argcheck(L, mtx, 1, "mutex expected");

    int res = pthread_mutex_trylock(&mtx->mtx);
    if (res == EBUSY) {
        lua_pushboolean(L, 0);
    } else {
        lua_pushboolean(L, 1);
    }

    return 1;
}

static int lthread_sleep(lua_State* L) {
    int seconds;

    // Check argument (seconds)
    seconds = luaL_checkinteger(L, 1);
    sleep(seconds);

    return 0;
}

static int lthread_sleepms(lua_State* L) {
    int milliseconds;

    // Check argument (seconds)
    milliseconds = luaL_checkinteger(L, 1);
    usleep(milliseconds * 1000);

    return 0;
}

static int lthread_sleepus(lua_State* L) {
    int useconds;

    // Check argument (seconds)
    useconds = luaL_checkinteger(L, 1);
    usleep(useconds);

    return 0;
}

// Suspend all threads, or a specific thread
static int lthread_suspend(lua_State* L) {
    return lthread_suspend_pthreads(L, luaL_optinteger(L, 1, -1));
}

// Resume all threads, or a specific thread
static int lthread_resume(lua_State* L) {
    return lthread_resume_pthreads(L, luaL_optinteger(L, 1, -1));
}

// Stop all threads, or a specific thread
static int lthread_stop(lua_State* L) {
    return lthread_stop_pthreads(L, luaL_optinteger(L, 1, -1));
}

static int lthread_status(lua_State* L) {
    // Get pthread
    struct pthread *pthread = _pthread_get(luaL_checkinteger(L, 1));

    if (pthread) {
        // Get Lua RTOS specific TCB parts for current task
        lua_rtos_tcb_t *lua_rtos_tcb;

        if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(pthread->task, THREAD_LOCAL_STORAGE_POINTER_ID))) {
            switch (lua_rtos_tcb->status) {
                case StatusRunning: lua_pushstring(L,"running"); break;
                case StatusSuspended: lua_pushstring(L,"suspended"); break;
            }
            return 1;
        }
    }

    return 0;
}

#include "modules.h"

static const LUA_REG_TYPE thread[] = {
    { LSTRKEY( "status"      ),            LFUNCVAL( lthread_status         ) },
    { LSTRKEY( "create"      ),            LFUNCVAL( lthread_create         ) },
    { LSTRKEY( "self"        ),            LFUNCVAL( lthread_self           ) },
    { LSTRKEY( "createmutex" ),            LFUNCVAL( lthread_create_mutex   ) },
    { LSTRKEY( "start"       ),            LFUNCVAL( lthread_start          ) },
    { LSTRKEY( "suspend"     ),            LFUNCVAL( lthread_suspend        ) },
    { LSTRKEY( "resume"      ),            LFUNCVAL( lthread_resume         ) },
    { LSTRKEY( "stop"        ),            LFUNCVAL( lthread_stop           ) },
    { LSTRKEY( "list"        ),            LFUNCVAL( lthread_list           ) },
    { LSTRKEY( "sleep"       ),            LFUNCVAL( lthread_sleep          ) },
    { LSTRKEY( "sleepms"     ),            LFUNCVAL( lthread_sleepms        ) },
    { LSTRKEY( "sleepus"     ),            LFUNCVAL( lthread_sleepus        ) },
    { LSTRKEY( "usleep"      ),            LFUNCVAL( lthread_sleepus        ) },

    { LSTRKEY( "Lock"               ),     LINTVAL( PTHREAD_MUTEX_NORMAL    ) },
    { LSTRKEY( "RecursiveLock"      ),     LINTVAL( PTHREAD_MUTEX_RECURSIVE ) },

    DRIVER_REGISTER_LUA_ERRORS(thread)
    { LNILKEY, LNILVAL }
};

static const LUA_REG_TYPE mutex_map[] = {
    { LSTRKEY( "lock"        ),   LFUNCVAL( lthread_lock    ) },
    { LSTRKEY( "unlock"      ),   LFUNCVAL( lthread_unlock  ) },
    { LSTRKEY( "trylock"     ),   LFUNCVAL( lthread_trylock ) },
    { LSTRKEY( "__metatable" ),   LROVAL  ( mutex_map       ) },
    { LSTRKEY( "__index"     ),   LROVAL  ( mutex_map       ) },
    { LNILKEY, LNILVAL }
};

int luaopen_thread(lua_State* L) {
    luaL_newmetarotable(L,"thread.mutex", (void *)mutex_map);
    return 0;
} 
 
MODULE_REGISTER_ROM(THREAD, thread, thread, luaopen_thread, 1);

#endif
