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
 * Lua RTOS, GPS sensor (geolocation)
 *
 */

#include "luartos.h"

#if CONFIG_LUA_RTOS_LUA_USE_SENSOR
#if CONFIG_LUA_RTOS_USE_SENSOR_GPS

#include "gps.h"
#include "nmea0183.h"

#include <math.h>

#include <sys/driver.h>

#include <drivers/sensor.h>
#include <drivers/uart.h>

// Sensor specification and registration
static const sensor_t __attribute__((used,unused,section(".sensors"))) gps_sensor = {
	.id = "GPS",
	.interface = {
		{.type = UART_INTERFACE},
	},
	.data = {
		{.id = "lon", .type = SENSOR_DATA_DOUBLE},
		{.id = "lat" , .type = SENSOR_DATA_DOUBLE},
		{.id = "sats", .type = SENSOR_DATA_INT},
		{.id = "valid", .type = SENSOR_DATA_INT},
		{.id = "height", .type = SENSOR_DATA_DOUBLE},
	},
	.setup = gps_setup,
	.acquire = gps_acquire
};

static void gps(void *args) {
	int uart = (int)args;

	char sentence[MAX_NMA_SIZE];

	for(;;) {
		uart_reads(uart, sentence, 1, portMAX_DELAY);
		nmea_parse(sentence);
	}
}

/*
 * Operation functions
 */
driver_error_t *gps_setup(sensor_instance_t *unit) {
	xTaskCreatePinnedToCore(gps, "gps", configMINIMAL_STACK_SIZE, (void *)((int)unit->setup[0].uart.id), 21, NULL, 0);

	return NULL;
}

driver_error_t *gps_acquire(sensor_instance_t *unit, sensor_value_t *values) {
	values[0].doubled.value  = nmea_lon();
	values[1].doubled.value  = nmea_lat();
	values[2].integerd.value = nmea_sats();
	values[3].integerd.value = nmea_new_pos();
	values[4].doubled.value = nmea_height();
	return NULL;
}

#endif
#endif
