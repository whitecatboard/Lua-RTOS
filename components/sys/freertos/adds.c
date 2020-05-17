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
 * Lua RTOS, FreeRTOS additions
 *
 */

#include "luartos.h"

#include "lua.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/adds.h"

#include "esp_attr.h"
#include "esp_heap_task_info.h"

#include <stdint.h>
#include <string.h>
#include <inttypes.h>

// Reference to lua_thread, which is created in app_main
extern pthread_t lua_thread;

// Global state
static lua_State *gL = NULL;

static int compare(const void *a, const void *b) {
    if (((task_info_t *)a)->task_type < ((task_info_t *)b)->task_type) {
        return 1;
    } else if (((task_info_t*)a)->task_type > ((task_info_t *)b)->task_type) {
        return -1;
    } else {
        if (((task_info_t *)a)->core < ((task_info_t *)b)->core) {
            return -1;
        } else if (((task_info_t *)a)->core > ((task_info_t *)b)->core) {
            return 1;
        } else {
            return strcmp(((task_info_t *)a)->name, ((task_info_t *)b)->name);
        }
    }
}

void uxSetThreadId(UBaseType_t id) {
    lua_rtos_tcb_t *lua_rtos_tcb;

    // Get Lua RTOS specific TCB parts for current task
    if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(xTaskGetCurrentTaskHandle(), THREAD_LOCAL_STORAGE_POINTER_ID))) {
        // Store thread id into Lua RTOS specific TCB parts
        lua_rtos_tcb->threadid = id;
    }
}

UBaseType_t IRAM_ATTR uxGetThreadId() {
    lua_rtos_tcb_t *lua_rtos_tcb;
    int threadid = 0;

    // Get Lua RTOS specific TCB parts for current task
    if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(xTaskGetCurrentTaskHandle(), THREAD_LOCAL_STORAGE_POINTER_ID))) {
        // Get current thread od from Lua RTOS specific TCB parts
        threadid = lua_rtos_tcb->threadid;
    }

    return threadid;
}

void uxSetThreadStatus(TaskHandle_t h, pthread_status_t status) {
    lua_rtos_tcb_t *lua_rtos_tcb;

    // Get Lua RTOS specific TCB parts for current task
    if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(xTaskGetCurrentTaskHandle(), THREAD_LOCAL_STORAGE_POINTER_ID))) {
        // Store thread id into Lua RTOS specific TCB parts
        lua_rtos_tcb->status = status;
    }
}

void uxSetLThread(lthread_t *lthread) {
    lua_rtos_tcb_t *lua_rtos_tcb;

    // Get Lua RTOS specific TCB parts for current task
    if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(xTaskGetCurrentTaskHandle(), THREAD_LOCAL_STORAGE_POINTER_ID))) {
        lua_rtos_tcb->lthread = lthread;
    }
}

void uxSetLuaState(lua_State* L) {
    lua_rtos_tcb_t *lua_rtos_tcb;

    // Get Lua RTOS specific TCB parts for current task
    if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(xTaskGetCurrentTaskHandle(), THREAD_LOCAL_STORAGE_POINTER_ID))) {
        // Store current lua state into Lua RTOS specific TCB parts
        if (!lua_rtos_tcb->lthread) {
            lua_rtos_tcb->lthread = calloc(1, sizeof(lthread_t));
            assert(lua_rtos_tcb->lthread);
        }
        lua_rtos_tcb->lthread->L = L;
    }

    // If this is the lua thread, store state in global state
    if ((uxGetThreadId() == lua_thread) && (gL == NULL)) {
        gL = L;
    }
}

lua_State* pvGetLuaState() {
    lua_rtos_tcb_t *lua_rtos_tcb;
    lua_State *L = NULL;

    // Get Lua RTOS specific TCB parts for current task
    if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(NULL, THREAD_LOCAL_STORAGE_POINTER_ID))) {

        // Get current lua state from Lua RTOS specific TCB parts
        if (lua_rtos_tcb->lthread) {
            L = lua_rtos_tcb->lthread->L;
        }
    }

    if (L == NULL) {
        L = gL;
    }

    return L;
}

lthread_t *pvGetLThread() {
    lua_rtos_tcb_t *lua_rtos_tcb;
    lthread_t *lthread = NULL;

    // Get Lua RTOS specific TCB parts for current task
    if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(NULL, THREAD_LOCAL_STORAGE_POINTER_ID))) {
        lthread = lua_rtos_tcb->lthread;
    }

    return lthread;
}

uint32_t uxGetSignaled(TaskHandle_t h) {
    lua_rtos_tcb_t *lua_rtos_tcb;
    int signaled = 0;

    // Get Lua RTOS specific TCB parts for current task
    if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(h, THREAD_LOCAL_STORAGE_POINTER_ID))) {
        // Get current signeled mask from Lua RTOS specific TCB parts
        signaled = lua_rtos_tcb->signaled;
    }

    return signaled;
}

void IRAM_ATTR uxSetSignaled(TaskHandle_t h, int s) {
    lua_rtos_tcb_t *lua_rtos_tcb;

    // Get Lua RTOS specific TCB parts for current task
    if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(h, THREAD_LOCAL_STORAGE_POINTER_ID))) {
        // Store current signaled mask into Lua RTOS specific TCB parts
        lua_rtos_tcb->signaled = s;
    }
}

uint8_t ucGetCoreID(TaskHandle_t h) {
    tskTCB_t *task = (tskTCB_t *)h;
    return task->xCoreID;
}

int uxGetStack(TaskHandle_t h) {
    tskTCB_t *task = (tskTCB_t *)h;
    return task->pxEndOfStack - task->pxStack + 4;
}

task_info_t *GetTaskInfo() {
    tskTCB_t *ctask;
    task_info_t *info;
    lua_rtos_tcb_t *lua_rtos_tcb;
    TaskStatus_t *status_array;
    UBaseType_t task_num = 0;
    UBaseType_t heap_num = 0;
    UBaseType_t start_task_num = 0;
    uint32_t total_runtime = 0;

    //Allocate status_array
    start_task_num = uxTaskGetNumberOfTasks();

    status_array = (TaskStatus_t *)calloc(start_task_num, sizeof(TaskStatus_t));
    if (!status_array) {
        return NULL;
    }

#ifndef CONFIG_FREERTOS_USE_TRACE_FACILITY
#warning Please enable CONFIG_FREERTOS_USE_TRACE_FACILITY to support thread.list
    free(status_array);
    return NULL;
#else
    task_num = uxTaskGetSystemState(status_array, (start_task_num), &total_runtime);
#endif

#ifdef CONFIG_HEAP_TASK_TRACKING
#ifdef CONFIG_HEAP_POISONING_DISABLED
#warning Please enable any CONFIG_HEAP_POISONING to support CONFIG_HEAP_TASK_TRACKING
#endif
#define CONFIG_HEAP_TASK_TRACKING_MAX 20
    heap_task_totals_t totals[CONFIG_HEAP_TASK_TRACKING_MAX];       ///< Array of structs to collect task totals
    memset(totals, 0, sizeof(heap_task_totals_t)*CONFIG_HEAP_TASK_TRACKING_MAX);

    size_t num_totals = 0;
    heap_task_info_params_t params;
    memset(&params, 0, sizeof(heap_task_info_params_t));
    params.totals = totals;
    params.max_totals = CONFIG_HEAP_TASK_TRACKING_MAX;
    params.num_totals = &num_totals;
    size_t num_blocks = heap_caps_get_per_task_info(&params);
    (void)num_blocks;

    for(int cnt=0; cnt<num_totals; cnt++) {
        int found = 0;
        for(int i = 0; i <task_num; i++) {
            if (params.totals[cnt].task == status_array[i].xHandle) {
                found = 1;
            }
        }
        if (!found) {
            heap_num++;
        }
    }
    // at this point we got "heap_num" heaps that are not found in the task status array
    // the meaning of this is that there are tasks that have been ended and that still got
    // some heap allocated.
    // this *can* point out that there's some memory leak.
    // but there's also situtations where it's not a bug. like the intentionally unclosed 
    // server socket in the telnet or http server.
#endif

    // For percentage calculations.
    total_runtime /= 100UL;

    info = (task_info_t *)calloc(task_num + heap_num + 1, sizeof(task_info_t));
    if (!info) {
        free(status_array);
        return NULL;
    }
    info[task_num+heap_num].task_type = TASK_DELIMITER;


    for(int i = 0; i <task_num; i++){
        if (status_array[i].eCurrentState == eDeleted) {
            info[i].task_type = TASK_DELETED;
            continue;
        }

        // Get the task TCB
        ctask = (tskTCB_t *)status_array[i].xHandle;

        // Set the task type
        info[i].task_type = TASK_FREERTOS;
        info[i].thid = 0;
        info[i].status = 0;

        // Get Lua RTOS specific TCB parts for current task
        if ((lua_rtos_tcb = pvTaskGetThreadLocalStoragePointer(status_array[i].xHandle, THREAD_LOCAL_STORAGE_POINTER_ID))) {
            // Task has Lua RTOS specific TCB parts
            if (lua_rtos_tcb->lthread) {
                info[i].task_type = TASK_LUA;
            } else {
                info[i].task_type = TASK_PTHREAD;
            }

            info[i].thid = lua_rtos_tcb->threadid;
            info[i].lthread = lua_rtos_tcb->lthread;
            info[i].status = lua_rtos_tcb->status;
        }

        // Populate info item
        info[i].prio = status_array[i].uxCurrentPriority;
        info[i].core = ctask->xCoreID;

        // Some system tasks shows 255!!
        if (info[i].core > 1) {
            info[i].core = 0;
        }

        info[i].free_stack = uxTaskGetStackHighWaterMark(status_array[i].xHandle);
        info[i].stack_size = ctask->pxEndOfStack - ctask->pxStack + 4;
        memcpy(info[i].name, status_array[i].pcTaskName, configMAX_TASK_NAME_LEN);

        info[i].cpu_usage = 0;
#ifdef CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        if( total_runtime > 0 ) {
            // only gives valid values if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is defined
            info[i].cpu_usage = status_array[i].ulRunTimeCounter / total_runtime;
        }
#endif

        info[i].heap_count = 0;
        info[i].heap_size = 0;
#ifdef CONFIG_HEAP_TASK_TRACKING
        for(int cnt=0; cnt<num_totals; cnt++) {
            if (params.totals[cnt].task == status_array[i].xHandle) {
                for(int caps=0; caps<NUM_HEAP_TASK_CAPS; caps++) {
                    info[i].heap_count += params.totals[cnt].count[caps];
                    info[i].heap_size  += params.totals[cnt].size[caps];
                }
                break;
            }
        }
#endif
    }

    free(status_array);

#ifdef CONFIG_HEAP_TASK_TRACKING
    /*
    https://www.esp32.com/viewtopic.php?t=1831
    [...] Specifically, there is 328 KB of DRAM available on the chip (the rest is IRAM and RTC RAM) [...]
    */
    int missing_task = 0;
    for(int cnt=0; cnt<num_totals; cnt++) {

        uint8_t found = 0;
        for(int i = 0; i <task_num; i++) {
            if (params.totals[cnt].task == status_array[i].xHandle) {
                found = 1;
            }
        }

        if (!found) {
            int i = task_num+missing_task;
            //note that params.totals[cnt].task is pointing to *invalid* memory!
            memset(&info[i], 0, sizeof(task_info_t));

            info[i].task_type  = TASK_HEAP;
            info[i].heap_count = 0;
            info[i].heap_size  = 0;

            for(int caps=0; caps<NUM_HEAP_TASK_CAPS; caps++) {
                if (params.totals[cnt].count[caps]>0 || params.totals[cnt].size[caps]>0) {
                    info[i].heap_count += params.totals[cnt].count[caps];
                    info[i].heap_size  += params.totals[cnt].size[caps];
                }
            }

            if(!params.totals[cnt].task) {
                // this characterizes the system's NULL/root task which has a heap of ~ 4k
                strncpy(info[i].name, "root", configMAX_TASK_NAME_LEN);
            } else if (((0x3ffc0c00 & (uintptr_t)params.totals[cnt].task) == 0x3ffc0c00) && info[i].heap_count > 60) {
                // this characterizes the system's freertos
                strncpy(info[i].name, "freertos", configMAX_TASK_NAME_LEN);
            } else {
                // for the other "leftovers" we use the (previously valid) task address as name
                #define PRIxPTR_WIDTH ((int)(sizeof(uintptr_t)*2))
                snprintf(info[i].name, configMAX_TASK_NAME_LEN, "0x%0*" PRIxPTR "", PRIxPTR_WIDTH, (uintptr_t)params.totals[cnt].task);
            }

            missing_task++;
            if (missing_task>heap_num) {
                printf("WARNING: found more heap tasks than space was reserved for\n");
                break;
            }
        }
    }

    if (missing_task<heap_num) {
        printf("WARNING: found less heap tasks than space was reserved for\n");
    }
#endif

    qsort (info, task_num+heap_num, sizeof (task_info_t), compare);
    return info;
}
