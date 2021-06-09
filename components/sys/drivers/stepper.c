/*
 *          /\       /\
 *         /  \_____/  \
 *        /_____________\
 *        W H I T E C A T
 *
 * Copyright (C) 2015 - 2020, IBEROXARXA SERVICIOS INTEGRALES, S.L.
 * Copyright (C) 2015 - 2020, Jaume Olivé Petrus (jolive@whitecatboard.org)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "luartos.h"

#if CONFIG_LUA_RTOS_LUA_USE_STEPPER

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include <sys/driver.h>
#include <sys/syslog.h>
#include <sys/mutex.h>
#include <sys/status.h>

#include <drivers/stepper.h>
#include <drivers/gpio.h>

#include <motion/motion.h>

typedef struct {
    uint8_t stepper;
    float units;
} stepper_oder_t;

// Register driver and messages
static void stepper_init();

DRIVER_REGISTER_BEGIN(STEPPER,stepper,0,stepper_init,NULL);
    DRIVER_REGISTER_ERROR(STEPPER, stepper, NotEnoughtMemory, "not enough memory", STEPPER_ERR_NOT_ENOUGH_MEMORY);
    DRIVER_REGISTER_ERROR(STEPPER, stepper, InvalidUnit, "invalid unit", STEPPER_ERR_INVALID_UNIT);
    DRIVER_REGISTER_ERROR(STEPPER, stepper, NoMoreUnits, "no more units available", STEPPER_ERR_NO_MORE_UNITS);
    DRIVER_REGISTER_ERROR(STEPPER, stepper, UnitNotSetup, "unit is not setup", STEPPER_ERR_UNIT_NOT_SETUP);
    DRIVER_REGISTER_ERROR(STEPPER, stepper, InvalidPin, "invalid pin", STEPPER_ERR_INVALID_PIN);
    DRIVER_REGISTER_ERROR(STEPPER, stepper, InvalidDirection, "invalid direction", STEPPER_ERR_INVALID_DIRECTION);
    DRIVER_REGISTER_ERROR(STEPPER, stepper, InvalidAcceleration, "invalid acceleration", STEPPER_ERR_INVALID_ACCELERATION);
DRIVER_REGISTER_END(STEPPER,stepper,0,stepper_init,NULL);

static stepper_t stepper[NSTEP];

// A queue to receive acceleration requests
QueueHandle_t acceleration_queue = NULL;

// Acceleration profile task handle
static TaskHandle_t acceleration_profile_task_h = NULL;

// RMT ISR handle
rmt_isr_handle_t isr_h = NULL;

// Steppers who are currently started (1 = started, 0 = not started),
static uint32_t start_mask = 0;
static uint8_t start_num = 0;

static struct mtx stepper_mutex;

static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

static EventGroupHandle_t move_event_group = NULL;
static EventGroupHandle_t stop_event_group = NULL;

/*
 * Helper functions
 */
static int _cmp(const void *o1, const void *o2) {
    stepper_oder_t *ord1 = (stepper_oder_t *)o1;
    stepper_oder_t *ord2 = (stepper_oder_t *)o2;

    if (ord1->units < ord2->units) {
        return 1;
    } else if (ord1->units > ord2->units) {
        return -1;
    }

    return 0;
}

static void stepper_init() {
    mtx_init(&stepper_mutex, NULL, NULL, 0);
    memset(stepper,0,sizeof(stepper_t) * NSTEP);
}

static void IRAM_ATTR step_feedback(void *arg) {
	stepper_t *pstepper = (stepper_t *)arg;

    if  (pstepper->dir){
        pstepper->pos++;
    } else {
        pstepper->pos--;
    }
}

static void IRAM_ATTR rmt_isr(void *arg) {
    // Get ISR status
    uint32_t intr_st = RMT.int_st.val;

    // Need to make a context switch at the end of the ISR?
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Current RMT channel inspected
    uint8_t channel;

    for(channel = 0;channel < NSTEP;channel++) {
        if (intr_st & BIT(channel * 3)) {
            // TX for channel (RMT has ended transmission)

        	// Stop RMT
            RMTMEM.chan[channel].data32[0].val = 0;
            RMT.conf_ch[channel].conf1.tx_start = 0;
            RMT.conf_ch[channel].conf1.mem_rd_rst = 1;
            RMT.conf_ch[channel].conf1.mem_rd_rst = 0;

            // Clear interrupt flag
            RMT.int_clr.val = BIT(channel * 3);

            // Stepper movement finish
            start_mask &= ~(1 << channel);

            // One stepper is stopped now
            if (start_num > 0) {
            	start_num--;
            }

            xEventGroupSetBitsFromISR(stop_event_group, 1 << channel, &xHigherPriorityTaskWoken);
            xEventGroupSetBitsFromISR(move_event_group, 1 << channel, &xHigherPriorityTaskWoken);
        }

        if (intr_st & BIT(24 + channel)) {
            // TX_THR for channel

            // Clear interrupt flag
            RMT.int_clr.val = BIT(24 + channel);

            // Consume a half of a RMT block from the pre-computed RMT data
            uint32_t i;

            i = 0;
            while ((i < (STEPPER_RMT_HALF_BUFF_SIZE)) && (stepper[channel].rmt_data_tail != stepper[channel].rmt_data_head)) {
            	if (stepper[channel].rmt_data_tail != stepper[channel].rmt_data_head) {
                    RMTMEM.chan[channel].data32[stepper[channel].rmt_offset + i].val = stepper[channel].rmt_data[stepper[channel].rmt_data_tail];
                    stepper[channel].rmt_data_tail = ((stepper[channel].rmt_data_tail + 1) % (STEPPER_RMT_DATA_SIZE));
            	}
                i++;
            }

            stepper[channel].rmt_offset = ((stepper[channel].rmt_offset + (STEPPER_RMT_HALF_BUFF_SIZE)) % STEPPER_RMT_BUFF_SIZE);

            // If data, inform to acceleration profile task that RMT has consumed a half of RMT block data
            if (i > 0) {
                uint32_t dummy = (1 << channel);
                xQueueSendFromISR(acceleration_queue, &dummy, &xHigherPriorityTaskWoken);
            }
        }
    }

    // Context switch if required
    if(xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR acceleration_profile_task(void *args) {
    stepper_t *pstepper;     // Current stepper in cycle
    uint8_t stepper_num;     // Current stepper number in cycle
    uint32_t cycle_for_mask; // Mask with the steppers that require to perform an acceleration cycle
    uint32_t cycle_mask;     // A copy of cycle_for_mask
    rmt_item32_t rmt_item;	 // RMT item for current step

    while (true) {
        // Wait for cycle
        xQueueReceive(acceleration_queue, &cycle_for_mask, portMAX_DELAY);

        // Run a new cycle with the involved steppers
        pstepper = stepper;
        stepper_num = 0;

        // For all the required steppers
        cycle_mask = cycle_for_mask;

        while (cycle_mask) {
            if (cycle_mask & 0x01) {
                // Prepare next RMT buffer
                uint32_t next = ((pstepper->rmt_data_head + 1) % (STEPPER_RMT_DATA_SIZE));

                if ((pstepper->steps == 0) && (next != pstepper->rmt_data_tail)) {
                    // RMT end condition
                    pstepper->rmt_data[pstepper->rmt_data_head] = 0;

                    // Advance
                    pstepper->rmt_data_head = next;
                    continue;
                }

                next = ((pstepper->rmt_data_head + 1) % (STEPPER_RMT_DATA_SIZE));

                while ((pstepper->steps > 0) && (next != pstepper->rmt_data_tail)) {
                    // In each RMT entry we have 1 bit level, and 15 bits for period, so in each item we can represent
                    // STEPPER_RMT_MAX_DURATION tick-period.
                    uint32_t rmt_ticks = 0;
                    uint8_t first;

                    if (pstepper->rmt_ticks_remain == 0) {
                        // Compute RMT ticks for next step
                    	first = 1;

                        pstepper->rmt_ticks = floor((motion_next(&pstepper->motion) * 1000000000.0) / (float)STEPPER_RMT_NANOS_PER_TICK);
                        rmt_ticks = pstepper->rmt_ticks;
                    } else {
                    	first = 0;
                        rmt_ticks = pstepper->rmt_ticks_remain;
                    }

                    while (rmt_ticks > 0) {
                        if (first) {
                            // Step signal -> H for 1 usec
                            rmt_item.level0 = 1;
                            rmt_item.duration0 = STEPPER_PULSE_TICKS;

                            // Step signal -> L for current step period - 1 usec
                            rmt_item.level1 = 0;

                            if (rmt_ticks - STEPPER_PULSE_TICKS < STEPPER_RMT_MAX_DURATION) {
                                rmt_item.duration1 = rmt_ticks - STEPPER_PULSE_TICKS;
                                rmt_ticks = 0;
                                pstepper->rmt_ticks_remain = 0;
                            } else {
                                rmt_item.duration1 = STEPPER_RMT_MAX_DURATION;
                                rmt_ticks -= STEPPER_RMT_MAX_DURATION;
                            }

                            // Write to RMT buffer
                            pstepper->rmt_data[pstepper->rmt_data_head] = rmt_item.val;

                            // Advance
                            pstepper->rmt_data_head = next;
                            next = ((pstepper->rmt_data_head + 1) % (STEPPER_RMT_DATA_SIZE));
                        } else {
                            // We need more RMT items

                            // Enough space in buffer?
                            if (next != pstepper->rmt_data_tail) {
                                // Enough space

                                // Now we can use level0 and level1
                                if (rmt_ticks < (STEPPER_RMT_MAX_DURATION << 1)) {
                                    // Enough space for this time using level0 and level1
                                    rmt_item.level0 = 0;
                                    rmt_item.duration0 = (rmt_ticks >> 1);
                                    rmt_ticks -= rmt_item.duration0;

                                    rmt_item.level1 = 0;
                                    rmt_item.duration1 = rmt_ticks;

                                    rmt_ticks = 0;
                                    pstepper->rmt_ticks_remain = 0;
                                } else {
                                    // Not enough space
                                    rmt_item.level0 = 0;
                                    rmt_item.duration0 = STEPPER_RMT_MAX_DURATION;
                                    rmt_ticks -= STEPPER_RMT_MAX_DURATION;

                                    rmt_item.level1 = 0;
                                    rmt_item.duration1 = STEPPER_RMT_MAX_DURATION;
                                    rmt_ticks -= STEPPER_RMT_MAX_DURATION;
                                }

                                // Write to RMT buffer
                                pstepper->rmt_data[pstepper->rmt_data_head] = rmt_item.val;

                                // Advance
                                pstepper->rmt_data_head = next;
                                next = ((pstepper->rmt_data_head + 1) % (STEPPER_RMT_DATA_SIZE));
                            } else {
                                // No space in buffer, we need to wait for next iteration
                                pstepper->rmt_ticks_remain = rmt_ticks;
                                break;
                            }
                        }

                        first = 0;
                    }

                    if (pstepper->rmt_ticks_remain > 0) {
                        continue;
                    }

                    // Decrement steps
                    pstepper->steps--;
                    if (pstepper->steps == 0) {
                        // RMT end
                        // Enough space in buffer?
                        if (next != pstepper->rmt_data_tail) {
                            // RMT end condition
                            pstepper->rmt_data[pstepper->rmt_data_head] = 0;

                            // Advance
                            pstepper->rmt_data_head = next;
                        }

                        // Not enough space in buffer, wait for next cycle
                    }
                }
            }

            // Next stepper
            cycle_mask = cycle_mask >> 1;
            pstepper++;
            stepper_num++;
        }

        // Start, RMT, if required
        cycle_mask = cycle_for_mask;

        pstepper = stepper;
        stepper_num = 0;

        while (cycle_mask) {
            if (cycle_mask & 0x01) {
                if (pstepper->rmt_start && !pstepper->rmt_started) {
                    int idx;

                    for(idx = 0;idx < STEPPER_RMT_BUFF_SIZE; idx++) {
                        RMTMEM.chan[stepper_num].data32[idx].val = pstepper->rmt_data[pstepper->rmt_data_tail];
                        pstepper->rmt_data_tail = ((pstepper->rmt_data_tail + 1) % (STEPPER_RMT_DATA_SIZE));
                    }

                    RMT.conf_ch[stepper_num].conf1.tx_start = 1;

                    pstepper->rmt_started = 1;
                }
            }

            cycle_mask = cycle_mask >> 1;
            pstepper++;
            stepper_num++;
        }
    }
}

/*
 * Operation functions
 */
driver_error_t *stepper_setup(uint8_t step_pin, uint8_t dir_pin, float min_spd, float max_spd, float max_acc, float stpu, uint8_t *unit) {
    driver_error_t *error;
#if CONFIG_LUA_RTOS_USE_HARDWARE_LOCKS
    driver_unit_lock_error_t *lock_error = NULL;
#endif
    int i;

    // Sanity checks
    if (step_pin > 31 ) {
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_INVALID_PIN, "must be between 0 and 31");
    }

    mtx_lock(&stepper_mutex);

    // Find a free unit
    for(i = 0; i < NSTEP; i++) {
        if (!stepper[i].setup) {
            *unit = i;
            break;
        }
    }

    if (i > NSTEP) {
        // No free unit
        mtx_unlock(&stepper_mutex);
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_NO_MORE_UNITS, NULL);
    }

    // Create RMT data circular buffer
    stepper[*unit].rmt_data = calloc(STEPPER_RMT_DATA_SIZE, sizeof(uint32_t));
    if (stepper[*unit].rmt_data == NULL) {
        mtx_unlock(&stepper_mutex);
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_NOT_ENOUGH_MEMORY, NULL);
    }

    stepper[*unit].rmt_data_head = 0;
    stepper[*unit].rmt_data_tail = 0;

    // Create acceleration task if not created yet
    if (acceleration_profile_task_h == NULL) {
        acceleration_queue = xQueueCreate(100, sizeof(uint32_t));
        if (acceleration_queue == NULL) {
            mtx_unlock(&stepper_mutex);
            return driver_error(STEPPER_DRIVER, STEPPER_ERR_NOT_ENOUGH_MEMORY, NULL);
        }

        if (xTaskCreatePinnedToCore(acceleration_profile_task, "stepper_accel", 2048, NULL, configMAX_PRIORITIES - 1, &acceleration_profile_task_h, 1) != pdTRUE) {
            mtx_unlock(&stepper_mutex);
            return driver_error(STEPPER_DRIVER, STEPPER_ERR_NOT_ENOUGH_MEMORY, NULL);
        }
    }

#if CONFIG_LUA_RTOS_USE_HARDWARE_LOCKS
    // Lock the step pin
    if ((lock_error = driver_lock(STEPPER_DRIVER, *unit, GPIO_DRIVER, step_pin, DRIVER_ALL_FLAGS, "STEP"))) {
        // Revoked lock on pin
        mtx_unlock(&stepper_mutex);
        return driver_lock_error(STEPPER_DRIVER, lock_error);
    }

    // Lock the dir pin
    if ((lock_error = driver_lock(STEPPER_DRIVER, *unit, GPIO_DRIVER, dir_pin, DRIVER_ALL_FLAGS, "DIR"))) {
        // Revoked lock on pin
        mtx_unlock(&stepper_mutex);
        return driver_lock_error(STEPPER_DRIVER, lock_error);
    }
#endif

    // Configure stepper pins, as output, initial low
    if ((error = gpio_pin_output(step_pin))) {
        mtx_unlock(&stepper_mutex);
        return error;
    }

    if ((error = gpio_pin_output(dir_pin))) {
        mtx_unlock(&stepper_mutex);
        return error;
    }

    gpio_ll_pin_clr(step_pin);
    gpio_ll_pin_clr(dir_pin);

    stepper[*unit].step_pin = step_pin;
    stepper[*unit].dir_pin = dir_pin;
    stepper[*unit].steps_per_unit = stpu;
    stepper[*unit].units_per_step = 1.0 / stpu;
    stepper[*unit].min_spd = min_spd;
    stepper[*unit].max_spd = max_spd;
    stepper[*unit].mac_acc = max_acc;
    stepper[*unit].setup = 1;

    // Reset RMT
    if (isr_h == NULL) {
        periph_module_reset(PERIPH_RMT_MODULE);
    }

    // Configure RMT for this stepper
    periph_module_enable(PERIPH_RMT_MODULE);

    // Set divider
    RMT.conf_ch[*unit].conf0.div_cnt = 2; // 40 Mhz, 25 nsecs for tick

    // Visit data use memory not FIFO
    RMT.apb_conf.fifo_mask = RMT_DATA_MODE_MEM;

    // No continuous mode
    RMT.conf_ch[*unit].conf1.tx_conti_mode = 0;

    // Reset TX / RX memory index
    RMT.conf_ch[*unit].conf1.mem_rd_rst = 1;
    RMT.conf_ch[*unit].conf1.mem_rd_rst = 0;

    // Wraparound mode
    RMT.apb_conf.mem_tx_wrap_en = 1;
    RMT.tx_lim_ch[*unit].limit = STEPPER_RMT_HALF_BUFF_SIZE;

    // Memory set block number
    RMT.conf_ch[*unit].conf0.mem_size = 1;
    RMT.conf_ch[*unit].conf1.mem_owner = RMT_MEM_OWNER_TX;

    // Use APB clock (80Mhz)
    RMT.conf_ch[*unit].conf1.ref_always_on = RMT_BASECLK_APB;

    // Set idle level to L
    RMT.conf_ch[*unit].conf1.idle_out_en = 1;
    RMT.conf_ch[*unit].conf1.idle_out_lv = 0;

    // No carrier
    RMT.conf_ch[*unit].conf0.carrier_en = 0;
    RMT.conf_ch[*unit].conf0.carrier_out_lv = 0;
    RMT.carrier_duty_ch[*unit].high = 0;
    RMT.carrier_duty_ch[*unit].low = 0;

    // Set pin
    //
    // NOTE: pin is configured as input/output because we need feedback to count exactly
    // the number of steps.
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[stepper[*unit].step_pin], 2);
    gpio_set_direction(stepper[*unit].step_pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_matrix_out(stepper[*unit].step_pin, RMT_SIG_OUT0_IDX + *unit, 0, 0);

    // Enable TX interrupt
    RMT.int_ena.val |= BIT(*unit * 3);

    // Enable TX_THR interrupt
    RMT.int_ena.val |= BIT(24 + *unit);

    // Allocate ISR
    if (isr_h == NULL) {
    	stop_event_group = xEventGroupCreate();
    	if (stop_event_group == NULL) {
            mtx_unlock(&stepper_mutex);
            return driver_error(STEPPER_DRIVER, STEPPER_ERR_NOT_ENOUGH_MEMORY, NULL);
    	}

    	move_event_group = xEventGroupCreate();
    	if (move_event_group == NULL) {
    		vEventGroupDelete(stop_event_group);
            mtx_unlock(&stepper_mutex);
            return driver_error(STEPPER_DRIVER, STEPPER_ERR_NOT_ENOUGH_MEMORY, NULL);
    	}

    	esp_intr_alloc(ETS_RMT_INTR_SOURCE, ESP_INTR_FLAG_IRAM, rmt_isr, NULL, &isr_h);
    }

    // Configure interrupts
    if (!status_get(STATUS_ISR_SERVICE_INSTALLED)) {
        gpio_install_isr_service(0);

        status_set(STATUS_ISR_SERVICE_INSTALLED, 0x00000000);
    }

    // Attach ISR on step_in to get feedback
    gpio_set_intr_type(stepper[*unit].step_pin, GPIO_INTR_POSEDGE);
    gpio_isr_handler_add(stepper[*unit].step_pin, step_feedback, &stepper[*unit]);

    mtx_unlock(&stepper_mutex);

    syslog(LOG_INFO,"stepper%d, at pins step=%s%d, dir=%s%d", *unit,
           gpio_portname(step_pin),
           gpio_name(step_pin),
           gpio_portname(dir_pin),
           gpio_name(dir_pin));

    return NULL;
}

driver_error_t *stepper_move(uint8_t unit, float units, float initial_spd, float target_spd, float acc, float jerk) {
    // Sanity checks
    if (unit > NSTEP) {
        // Invalid unit
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_INVALID_UNIT, NULL);
    }

    mtx_lock(&stepper_mutex);

    if (!stepper[unit].setup) {
        // Unit not setup
        mtx_unlock(&stepper_mutex);
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_UNIT_NOT_SETUP, NULL);
    }

    stepper_t *pstepper = &stepper[unit];

    // Calculate direction
    uint8_t dir = (units >= 0.0);
    pstepper->dir = dir;

    // Prepare motion
    motion_constraints_t constraints;

    constraints.accleration_profile = MotionSCurve;

    constraints.s_curve.v0 = initial_spd;
    constraints.s_curve.v = target_spd;
    constraints.s_curve.a = acc;
    constraints.s_curve.j = jerk;
    constraints.s_curve.s = fabs(units);
    constraints.s_curve.t = 0;
    constraints.s_curve.steps_per_unit = pstepper->steps_per_unit;
    constraints.s_curve.units_per_step = pstepper->units_per_step;

    motion_prepare(&constraints, &pstepper->motion);

    stepper[unit].steps = floor(fabs(units) * pstepper->steps_per_unit);
    stepper[unit].units = fabs(units);

    stepper[unit].rmt_ticks_remain = 0;
    stepper[unit].rmt_data_head = 0;
    stepper[unit].rmt_data_tail = 0;
    stepper[unit].rmt_offset = 0;
    stepper[unit].rmt_start = 1;
    stepper[unit].rmt_started = 0;

    mtx_unlock(&stepper_mutex);

    return NULL;
}

driver_error_t *stepper_get_distance(uint8_t unit, float* units) {
     // Sanity checks
    if (unit > NSTEP) {
        // Invalid unit
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_INVALID_UNIT, NULL);
    }

    mtx_lock(&stepper_mutex);

    if (!stepper[unit].setup) {
        // Unit not setup
        mtx_unlock(&stepper_mutex);
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_UNIT_NOT_SETUP, NULL);
    }

    stepper_t *pstepper = &stepper[unit];
    *units = stepper[unit].steps * pstepper->units_per_step;

    mtx_unlock(&stepper_mutex);
    return NULL;
}

driver_error_t *stepper_set_position(uint8_t unit, float units) {
     // Sanity checks
    if (unit > NSTEP) {
        // Invalid unit
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_INVALID_UNIT, NULL);
    }

    mtx_lock(&stepper_mutex);

    if (!stepper[unit].setup) {
        // Unit not setup
        mtx_unlock(&stepper_mutex);
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_UNIT_NOT_SETUP, NULL);
    }

    stepper_t *pstepper = &stepper[unit];
    pstepper->pos = units * pstepper->steps_per_unit;

    mtx_unlock(&stepper_mutex);
    return NULL;
}

driver_error_t *stepper_get_position(uint8_t unit, float* units) {
     // Sanity checks
    if (unit > NSTEP) {
        // Invalid unit
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_INVALID_UNIT, NULL);
    }

    mtx_lock(&stepper_mutex);

    if (!stepper[unit].setup) {
        // Unit not setup
        mtx_unlock(&stepper_mutex);
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_UNIT_NOT_SETUP, NULL);
    }

    stepper_t *pstepper = &stepper[unit];
    *units = pstepper->pos * pstepper->units_per_step;

    mtx_unlock(&stepper_mutex);
    return NULL;
}

driver_error_t *stepper_is_running(uint8_t unit, uint32_t* running) {
  // Sanity checks
    if (unit > NSTEP) {
        // Invalid unit
        return driver_error(STEPPER_DRIVER, STEPPER_ERR_INVALID_UNIT, NULL);
    }

    portENTER_CRITICAL(&spinlock);
    *running = start_mask & (1<<unit) ;
    portEXIT_CRITICAL(&spinlock);
    return NULL;
}


void stepper_start(int mask, uint8_t async) {
    mtx_lock(&stepper_mutex);

    start_num = 0;

    // For each required stepper set its direction pin
    stepper_t *pstepper = stepper;
    int testMask = 0x01;

    while (testMask != (1 << (NSTEP - 1))) {
        if (mask & testMask) {
            if (pstepper->dir) {
                gpio_ll_pin_set(pstepper->dir_pin);
            } else {
                gpio_ll_pin_clr(pstepper->dir_pin);
            }
            start_num++;
        }

        testMask = testMask << 1;
        pstepper++;
    }

    if (start_num > 1) {
        // Now, order involved stepper by displacement units
        stepper_oder_t *stepper_order;
        stepper_oder_t *pstepper_order;
        uint8_t stepper_id;
        stepper_order = calloc(start_num, sizeof(stepper_oder_t));
        assert(stepper_order != NULL);
        pstepper_order = stepper_order;

        testMask = 0x01;
        pstepper = stepper;
        stepper_id = 0;

        while (testMask != (1 << (NSTEP - 1))) {
            if (mask & testMask) {
                pstepper_order->stepper = stepper_id;
                pstepper_order->units = pstepper->units;
                pstepper_order++;
            }

            testMask = testMask << 1;
            pstepper++;
            stepper_id++;
        }

        qsort(stepper_order, start_num, sizeof(stepper_oder_t), _cmp);

        // Get total time for first stepper, this time is a constraint for the
        // other steppers involved in motion with different units
        float t = stepper[stepper_order[0].stepper].motion.s_curve.bound.total_t;
        pstepper = &stepper[stepper_order[0].stepper];

    	motion_dumnp(&pstepper->motion);

    	uint8_t order_id;

        for(order_id = 1;order_id < start_num;order_id++) {
            pstepper = &stepper[stepper_order[order_id].stepper];

            if (pstepper->motion.s_curve.s != stepper[stepper_order[0].stepper].motion.s_curve.s) {
                motion_constraint_t(&pstepper->motion, t);
            }

        	motion_dumnp(&pstepper->motion);
        }
    } else {
        stepper_t *pstepper = stepper;
        int testMask = 0x01;

        while (testMask != (1 << (NSTEP - 1))) {
            if (mask & testMask) {
            	motion_dumnp(&pstepper->motion);
            	break;
            }

            testMask = testMask << 1;
            pstepper++;
        }
    }

    // Start required steppers
    portENTER_CRITICAL(&spinlock);
    start_mask |= mask;

    xEventGroupClearBits(stop_event_group, 0xff);
    xEventGroupClearBits(move_event_group, 0xff);

    portEXIT_CRITICAL(&spinlock);

    mtx_unlock(&stepper_mutex);

    // Start acceleration cycle for all the steppers
    xQueueSend(acceleration_queue, &start_mask, portMAX_DELAY);

    // Wait until movements done
    if (mask && !async) {
    	xEventGroupWaitBits(move_event_group, mask, pdTRUE, pdTRUE, portMAX_DELAY);
    }
}

void stepper_stop(int mask, uint8_t async) {
    // Stop required steppers
    portENTER_CRITICAL(&spinlock);

    int stop_mask = 0x00;
    int testMask = 0x01;
    uint8_t channel = 0;

    while (testMask != (1 << (NSTEP - 1))) {
        if (start_mask & testMask & mask) {
            RMTMEM.chan[channel].data32[0].val = 0;
            RMT.conf_ch[channel].conf1.tx_start = 0;
            RMT.conf_ch[channel].conf1.mem_rd_rst = 1;
            RMT.conf_ch[channel].conf1.mem_rd_rst = 0;
            stop_mask |= testMask;
        }

        testMask = testMask << 1;
        channel++;
    }

    portEXIT_CRITICAL(&spinlock);

    // Wait for stop
    if (stop_mask && ! async) {
    	xEventGroupWaitBits(stop_event_group, stop_mask, pdTRUE, pdTRUE, portMAX_DELAY);
    }
}
#endif
