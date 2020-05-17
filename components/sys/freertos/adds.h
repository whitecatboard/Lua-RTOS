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

#ifndef _FREERTOS_ADDS_H
#define _FREERTOS_ADDS_H

#include "lua.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <pthread.h>

/* Value that can be assigned to the eNotifyState member of the TCB. */
typedef enum
{
    eNotWaitingNotification = 0,
    eWaitingNotification,
    eNotified
} eNotifyValue_t;

typedef struct
{
    volatile StackType_t    *pxTopOfStack;    /*< Points to the location of the last item placed on the tasks stack.  THIS MUST BE THE FIRST MEMBER OF THE TCB STRUCT. */

    #if ( portUSING_MPU_WRAPPERS == 1 )
        xMPU_SETTINGS    xMPUSettings;        /*< The MPU settings are defined as part of the port layer.  THIS MUST BE THE SECOND MEMBER OF THE TCB STRUCT. */
    #endif

    ListItem_t            xGenericListItem;    /*< The list that the state list item of a task is reference from denotes the state of that task (Ready, Blocked, Suspended ). */
    ListItem_t            xEventListItem;        /*< Used to reference a task from an event list. */
    UBaseType_t            uxPriority;            /*< The priority of the task.  0 is the lowest priority. */
    StackType_t            *pxStack;            /*< Points to the start of the stack. */
    char                pcTaskName[ configMAX_TASK_NAME_LEN ];/*< Descriptive name given to the task when created.  Facilitates debugging only. */ /*lint !e971 Unqualified char types are allowed for strings and single characters only. */
    BaseType_t            xCoreID;            /*< Core this task is pinned to */
                                            /* If this moves around (other than pcTaskName size changes), please change the define in xtensa_vectors.S as well. */
    #if ( portSTACK_GROWTH > 0 || configENABLE_TASK_SNAPSHOT == 1 )
        StackType_t        *pxEndOfStack;        /*< Points to the end of the stack on architectures where the stack grows up from low memory. */
    #endif

    #if ( portCRITICAL_NESTING_IN_TCB == 1 )
        UBaseType_t     uxCriticalNesting;     /*< Holds the critical section nesting depth for ports that do not maintain their own count in the port layer. */
        uint32_t        uxOldInterruptState; /*< Interrupt state before the outer taskEnterCritical was called */
    #endif

    #if ( configUSE_TRACE_FACILITY == 1 )
        UBaseType_t        uxTCBNumber;        /*< Stores a number that increments each time a TCB is created.  It allows debuggers to determine when a task has been deleted and then recreated. */
        UBaseType_t      uxTaskNumber;        /*< Stores a number specifically for use by third party trace code. */
    #endif

    #if ( configUSE_MUTEXES == 1 )
        UBaseType_t     uxBasePriority;        /*< The priority last assigned to the task - used by the priority inheritance mechanism. */
        UBaseType_t     uxMutexesHeld;
    #endif

    #if ( configUSE_APPLICATION_TASK_TAG == 1 )
        TaskHookFunction_t pxTaskTag;
    #endif

    #if( configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0 )
        void *pvThreadLocalStoragePointers[ configNUM_THREAD_LOCAL_STORAGE_POINTERS ];
    #if ( configTHREAD_LOCAL_STORAGE_DELETE_CALLBACKS )
        TlsDeleteCallbackFunction_t pvThreadLocalStoragePointersDelCallback[ configNUM_THREAD_LOCAL_STORAGE_POINTERS ];
    #endif
    #endif

    #if ( configGENERATE_RUN_TIME_STATS == 1 )
        uint32_t        ulRunTimeCounter;    /*< Stores the amount of time the task has spent in the Running state. */
    #endif

    #if ( configUSE_NEWLIB_REENTRANT == 1 )
        /* Allocate a Newlib reent structure that is specific to this task.
        Note Newlib support has been included by popular demand, but is not
        used by the FreeRTOS maintainers themselves.  FreeRTOS is not
        responsible for resulting newlib operation.  User must be familiar with
        newlib and must provide system-wide implementations of the necessary
        stubs. Be warned that (at the time of writing) the current newlib design
        implements a system-wide malloc() that must be provided with locks. */
        struct     _reent xNewLib_reent;
    #endif

    #if ( configUSE_TASK_NOTIFICATIONS == 1 )
        volatile uint32_t ulNotifiedValue;
        volatile eNotifyValue_t eNotifyState;
    #endif

    /* See the comments above the definition of
    tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE. */
    #if( tskSTATIC_AND_DYNAMIC_ALLOCATION_POSSIBLE != 0 )
        uint8_t    ucStaticallyAllocated;         /*< Set to pdTRUE if the task is a statically allocated to ensure no attempt is made to free the memory. */
    #endif

} tskTCB_t;

typedef enum {
    StatusRunning = 1,
    StatusSuspended = 2
} pthread_status_t;

typedef struct lthread {
    lua_State *PL; // Parent thread
    lua_State *L;  // Thread state
    int function_ref;
    int thread_ref;
    int status;
} lthread_t;

typedef enum {
    TASK_HEAP,
    TASK_FREERTOS,
    TASK_PTHREAD,
    TASK_LUA,
    TASK_DELETED,
    TASK_DELIMITER
} TASK_TYPE_t;

typedef struct {
    TASK_TYPE_t task_type;
    char name[configMAX_TASK_NAME_LEN];
    uint8_t core;
    uint8_t prio;
    size_t stack_size;
    size_t free_stack;
    size_t heap_count;
    size_t heap_size;
    uint32_t cpu_usage;
    int thid;
    lthread_t *lthread;
    pthread_status_t status;
} task_info_t;

typedef struct {
    int32_t    threadid;
     uint32_t   signaled;
     pthread_status_t status;
     struct lthread *lthread;
} lua_rtos_tcb_t;

// This macro is not present in all FreeRTOS ports. In Lua RTOS is used in some places
// in the source code shared by all the supported platforms. ESP32 don't define this macro,
// so we define it for reuse code beetween platforms.
#define portEND_SWITCHING_ISR(xSwitchRequired) \
if (xSwitchRequired) {      \
    _frxt_setup_switch(); \
}

UBaseType_t uxGetTaskId();
UBaseType_t uxGetThreadId();
void uxSetThreadStatus(TaskHandle_t h, pthread_status_t status);
void uxSetThreadId(UBaseType_t id);
void uxSetLThread(lthread_t *lthread);
lthread_t *pvGetLThread();
void uxSetSignaled(TaskHandle_t h, int s);
uint32_t uxGetSignaled(TaskHandle_t h);
TaskHandle_t xGetCurrentTask();
uint8_t ucGetCoreID(TaskHandle_t h);
int uxGetStack(TaskHandle_t h);
task_info_t *GetTaskInfo();
void uxSetLuaState(lua_State* L);
lua_State* pvGetLuaState();

#endif
