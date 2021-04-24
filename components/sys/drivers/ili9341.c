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
 * Lua RTOS, ILI9341 driver
 *
 */

/*
 * This driver is inspired and takes code from the following projects:
 *
 * Boris Lovošević, tft driver for Lua RTOS:
 *
 * https://github.com/loboris/Lua-RTOS-ESP32-lobo/tree/master/components/lua_rtos/Lua/modules/screen
 *
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LUA_USE_GDISPLAY

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#include <sys/driver.h>
#include <sys/syslog.h>

#include <gdisplay/gdisplay.h>

#include <drivers/gpio.h>
#include <drivers/spi.h>
#include <drivers/gdisplay.h>

#include <drivers/ili9341.h>
#include <drivers/st7735.h>

static int touch_spi = -1;
static int tp_calx = 0;
static int tp_caly = 0;

// Current chipset
static uint8_t chipset;

static const uint8_t ILI9341_init[] = {
  23,                   					        // 23 commands in list
  ILI9341_SWRESET, DELAY,   						//  1: Software reset, no args, w/delay
  200,												//     50 ms delay
  ILI9341_POWERA, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
  ILI9341_POWERB, 3, 0x00, 0XC1, 0X30,
  0xEF, 3, 0x03, 0x80, 0x02,
  ILI9341_DTCA, 3, 0x85, 0x00, 0x78,
  ILI9341_DTCB, 2, 0x00, 0x00,
  ILI9341_POWER_SEQ, 4, 0x64, 0x03, 0X12, 0X81,
  ILI9341_PRC, 1, 0x20,
  ILI9341_PWCTR1, 1,  								//Power control
  0x23,               								//VRH[5:0]
  ILI9341_PWCTR2, 1,   								//Power control
  0x10,                 							//SAP[2:0];BT[3:0]
  ILI9341_VMCTR1, 2,    							//VCM control
  0x3e,                 							//Contrast
  0x28,
  ILI9341_VMCTR2, 1,  								//VCM control2
  0x86,
  ST7735_MADCTL, 1,    								// Memory Access Control
  0x48,
  ILI9341_PIXFMT, 1,
  0x55,
  ILI9341_FRMCTR1, 2,
  0x00,
  0x18,
  ILI9341_DFUNCTR, 3,   							// Display Function Control
  0x08,
  0x82,
  0x27,
  ST7735_PTLAR, 4, 0x00, 0x00, 0x01, 0x3F,
  ILI9341_3GAMMA_EN, 1,								// 3Gamma Function Disable
  0x00, // 0x02
  ILI9341_GAMMASET, 1, 								//Gamma curve selected
  0x01,
  ILI9341_GMCTRP1, 15,   							//Positive Gamma Correction
  0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
  0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
  ILI9341_GMCTRN1, 15,   							//Negative Gamma Correction
  0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
  0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
  ILI9341_SLPOUT, DELAY, 							//  Sleep out
  120,			 									//  120 ms delay
  ST7735_DISPON, 0,
};

/*
 * Helper functions
 */
static uint16_t ili9341_tp_command(uint8_t type) {
	uint8_t rxbuf[3];

 	spi_ll_select(touch_spi);
 	spi_ll_transfer(touch_spi, type, &rxbuf[0]);
 	spi_ll_transfer(touch_spi, 0x55, &rxbuf[1]);
 	spi_ll_transfer(touch_spi, 0x55, &rxbuf[2]);
	spi_ll_deselect(touch_spi);

    return (((uint16_t)(rxbuf[1] << 8) | (uint16_t)(rxbuf[2])) >> 4);
}

static int ili9341_tp_read(uint8_t type, int samples) {
	int n, result, val = 0;
	uint32_t i = 0;
	uint32_t vbuf[18];
	uint32_t minval, maxval, dif;

    if (samples < 3) samples = 1;
    if (samples > 18) samples = 18;

    // one dummy read
    result = ili9341_tp_command(type);

    // read data
	while (i < 10) {
    	minval = 5000;
    	maxval = 0;
		// get values
		for (n=0;n<samples;n++) {
		    result = ili9341_tp_command(type);
			if (result < 0) break;

			vbuf[n] = result;
			if (result < minval) minval = result;
			if (result > maxval) maxval = result;
		}
		if (result < 0) break;
		dif = maxval - minval;
		if (dif < 40) break;
		i++;
    }
	if (result < 0) return -1;

	if (samples > 2) {
		// remove one min value
		for (n = 0; n < samples; n++) {
			if (vbuf[n] == minval) {
				vbuf[n] = 5000;
				break;
			}
		}
		// remove one max value
		for (n = 0; n < samples; n++) {
			if (vbuf[n] == maxval) {
				vbuf[n] = 5000;
				break;
			}
		}
		for (n = 0; n < samples; n++) {
			if (vbuf[n] < 5000) val += vbuf[n];
		}
		val /= (samples-2);
	}
	else val = vbuf[0];

    return val;
}

/*
 * Operation functions
 */
driver_error_t *ili9341_init(uint8_t chip, uint8_t orientation, uint8_t address) {
	driver_error_t *error;
	gdisplay_caps_t *caps = gdisplay_ll_get_caps();

	caps->addr_window = st7735_addr_window;
	caps->on = st7735_on;
	caps->off = st7735_off;
	caps->invert = st7735_invert;
	caps->orientation = ili9341_set_orientation;
	caps->touch_get = ili9341_tp_get;
	caps->touch_cal = ili9341_tp_set_cal;
	caps->bytes_per_pixel = 2;
	caps->rdepth = 5;
	caps->gdepth = 6;
	caps->bdepth = 5;
	caps->phys_width  = ILI9341_HEIGHT;
	caps->phys_height = ILI9341_WIDTH;
	caps->interface = GDisplaySPIInterface;

	// Store chipset
	chipset = chip;

#if CONFIG_LUA_RTOS_GDISPLAY_TP_SPI > 0
	#if CONFIG_LUA_RTOS_GDISPLAY_TP_CS < 0
	#error "If touch pannel support is enabled CONFIG_LUA_RTOS_GDISPLAY_TP_CS must be >= 0."
	#endif
    // Init display SPI bus
	if (caps->device == -1) {
		if ((error = spi_setup(CONFIG_LUA_RTOS_GDISPLAY_SPI, 1, CONFIG_LUA_RTOS_GDISPLAY_CS, 0, 48000000, SPI_FLAG_WRITE | SPI_FLAG_READ | SPI_FLAG_NO_DMA, &caps->device))) {
			return error;
		}
	}
	// Init touch SPI
	if ((error = spi_setup(CONFIG_LUA_RTOS_GDISPLAY_TP_SPI, 1, CONFIG_LUA_RTOS_GDISPLAY_TP_CS, 0, 2500000, SPI_FLAG_WRITE | SPI_FLAG_READ | SPI_FLAG_NO_DMA, &touch_spi))) {
		return error;
	}

#else
    // Init display SPI bus
	if (caps->device == -1) {
		if ((error = spi_setup(CONFIG_LUA_RTOS_GDISPLAY_SPI, 1, CONFIG_LUA_RTOS_GDISPLAY_CS, 0, 48000000, SPI_FLAG_WRITE | SPI_FLAG_READ | SPI_FLAG_NO_DMA, &caps->device))) {
			return error;
		}
	}
#endif

#if CONFIG_LUA_RTOS_USE_HARDWARE_LOCKS
	driver_unit_lock_error_t *lock_error = NULL;
	if ((error = spi_lock_bus_resources(CONFIG_LUA_RTOS_GDISPLAY_SPI, DRIVER_ALL_FLAGS))) {
		return error;
	}

	if ((lock_error = driver_lock(GDISPLAY_DRIVER, 0, GPIO_DRIVER, CONFIG_LUA_RTOS_GDISPLAY_CMD, DRIVER_ALL_FLAGS, "gdisplay - ILI9341"))) {
		return driver_lock_error(GDISPLAY_DRIVER, lock_error);
	}
#endif

	// setup command pin
	gpio_pin_output(CONFIG_LUA_RTOS_GDISPLAY_CMD);

#if CONFIG_LUA_RTOS_GDISPLAY_RESET != -1
#if CONFIG_LUA_RTOS_USE_HARDWARE_LOCKS
	if ((lock_error = driver_lock(GDISPLAY_DRIVER, 0, GPIO_DRIVER, CONFIG_LUA_RTOS_GDISPLAY_RESET, DRIVER_ALL_FLAGS, "gdisplay - ILI9341"))) {
		return driver_lock_error(GDISPLAY_DRIVER, lock_error);
	}
#endif
	// setup reset pin
	gpio_pin_output(CONFIG_LUA_RTOS_GDISPLAY_RESET);
	gpio_ll_pin_clr(CONFIG_LUA_RTOS_GDISPLAY_RESET);
#endif

#if CONFIG_LUA_RTOS_GDISPLAY_TP_SPI > 0
#if CONFIG_LUA_RTOS_USE_HARDWARE_LOCKS
	if ((lock_error = driver_lock(GDISPLAY_DRIVER, 0, SPI_DRIVER, touch_spi, DRIVER_ALL_FLAGS, "gdisplay - ILI9341 TOUCH PANNEL"))) {
		return driver_lock_error(GDISPLAY_DRIVER, lock_error);
	}
#endif
#endif

	// Reset
#if CONFIG_LUA_RTOS_GDISPLAY_RESET == -1
	gdisplay_ll_command(ST7735_SWRESET);
	vTaskDelay(130 / portTICK_RATE_MS);
#else
	gpio_pin_set(CONFIG_LUA_RTOS_GDISPLAY_RESET);
	vTaskDelay(100 / portTICK_RATE_MS);
	gpio_pin_clr(CONFIG_LUA_RTOS_GDISPLAY_RESET);
	vTaskDelay(100 / portTICK_RATE_MS);
	gpio_pin_set(CONFIG_LUA_RTOS_GDISPLAY_RESET);
	vTaskDelay(200 / portTICK_RATE_MS);
#endif

	// Init display
    switch (chipset) {
		case CHIPSET_ILI9341:
			gdisplay_ll_command_list(ILI9341_init);
			break;
    }

    ili9341_set_orientation(orientation);

	// Allocate buffer
	if (!gdisplay_ll_allocate_buffer(ST7735_BUFFER)) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_NOT_ENOUGH_MEMORY, NULL);
	}

    // Clear screen (black)
    st7735_clear(GDISPLAY_BLACK);

    gdisplay_ll_command(ST7735_DISPON); // Display On

    return NULL;
}

void ili9341_set_orientation(uint8_t m) {
	gdisplay_caps_t *caps = gdisplay_ll_get_caps();
	uint8_t orientation = m & 3; // can't be higher than 3
	uint8_t madctl = 0;

	caps->ystart = 0;
	caps->xstart = 0;

	switch (orientation) {
	  case LANDSCAPE:
		madctl = (ST7735_MADCTL_MX | ST7735_MADCTL_RGB);
		caps->width  = ILI9341_HEIGHT;
		caps->height = ILI9341_WIDTH;
		break;
	  case PORTRAIT:
		madctl = (ST7735_MADCTL_MV | ST7735_MADCTL_RGB);
		caps->width  = ILI9341_WIDTH;
		caps->height = ILI9341_HEIGHT;
		break;
	  case LANDSCAPE_FLIP:
		madctl = (ST7735_MADCTL_MY | ST7735_MADCTL_RGB);
		caps->width  = ILI9341_HEIGHT;
		caps->height = ILI9341_WIDTH;
		break;
	  case PORTRAIT_FLIP:
		madctl = (ST7735_MADCTL_MX | ST7735_MADCTL_MY | ST7735_MADCTL_MV | ST7735_MADCTL_RGB);
		caps->width  = ILI9341_WIDTH;
		caps->height = ILI9341_HEIGHT;
		break;
	}

	gdisplay_ll_command(ST7735_MADCTL);
	gdisplay_ll_data(&madctl, 1);
}

void ili9341_tp_set_cal(int calx, int caly) {
	tp_calx = calx;
	tp_caly = caly;
}

void ili9341_tp_get(int *x, int *y, int *z, uint8_t raw) {
	int result = -1;
	int tmp;

	*x = 0;
	*y = 0;
	*z = 0;

	if (raw) {
	    result = ili9341_tp_read(0xB0, 3);
		if (result > 50)  {
			// tp pressed
			*z = result;

			result = ili9341_tp_read(0xD0, 10);
			if (result >= 0) {
				*x = result;

				result = ili9341_tp_read(0x90, 10);
				if (result >= 0) *y = result;
			}
		}

		if (result <0)
			*z = result;
	} else {
	    result = ili9341_tp_read(0xB0, 3);
		if (result > 50)  {
			// tp pressed
			*z = result;

			result = ili9341_tp_read(0xD0, 10);
			if (result >= 0) {
				*x = result;

				result = ili9341_tp_read(0x90, 10);
				if (result >= 0) *y = result;
			}
		}

		if (result < 0) {
			*x = 0;
			*y = 0;
			*z = result;
			return;
		}

		int xleft   = (tp_calx >> 16) & 0x3FFF;
		int xright  = tp_calx & 0x3FFF;
		int ytop    = (tp_caly >> 16) & 0x3FFF;
		int ybottom = tp_caly & 0x3FFF;

		if (((xright - xleft) != 0) && ((ybottom - ytop) != 0)) {
			*x = ((*x - xleft) * 320) / (xright - xleft);
			*y = ((*y - ytop) * 240) / (ybottom - ytop);
		}
		else {
			*z = 0;
			*x = 0;
			*y = 0;
			return;
		}

		if (*x < 0) *x = 0;
		if (*x > 319) *x = 319;
		if (*y < 0) *y = 0;
		if (*y > 239) *y = 239;

		gdisplay_caps_t *caps = gdisplay_ll_get_caps();

		switch (caps->orient) {
			case PORTRAIT:
				tmp = *x;
				*x = 240 - *y - 1;
				*y = tmp;
				break;
			case PORTRAIT_FLIP:
				tmp = *x;
				*x = *y;
				*y = 320 - tmp - 1;
				break;
			case LANDSCAPE_FLIP:
				*x = 320 - *x - 1;
				*y = 240 - *y - 1;
				break;
		}
	}
}

#endif
