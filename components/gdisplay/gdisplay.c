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
 * Lua RTOS, graphic display
 *
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LUA_USE_GDISPLAY

#include <stdint.h>

#include <gdisplay/gdisplay.h>

#include <drivers/gpio.h>
#include <drivers/power_bus.h>

#include <pthread.h>

static uint8_t init = 0;

static int cursorx, cursory;
static uint8_t force_fixed = 0;
static uint8_t transparent = 0;
static uint16_t rotation = 0;
static uint8_t orientation = 0;
static uint8_t wrap = 0;
static uint8_t type = 0;
static int offset = 0;

static float arc_angle_max = DEFAULT_ARC_ANGLE_MAX;
static float angle_offset = DEFAULT_ANGLE_OFFSET;

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

/*
 * Frame buffer
 */
static uint8_t *buffer = NULL;     // Frame buffer, if used
static int bbx1, bbx2, bby1, bby2; // Frame buffer bounding box to update
static uint8_t nested = 0;		   // Display is only updated when nested == 0

// Supported display devices
static const gdisplay_t displaydevs[] = {
	{CHIPSET_PCD8544,        pcd8544_init},
	{CHIPSET_ST7735,         st7735_init },
	{CHIPSET_ST7735B,        st7735_init },
	{CHIPSET_ST7735G,        st7735_init },
	{CHIPSET_ST7735G_144,    st7735_init },
	{CHIPSET_ST7735_096 ,    st7735_init },
	{CHIPSET_ILI9341,        ili9341_init},
	{CHIPSET_ILI9341_BGR,    ili9341_init},
	{CHIPSET_SSD1306_128_32, ssd1306_init},
	{CHIPSET_SSD1306_128_64, ssd1306_init},
	{CHIPSET_SSD1306_96_16,  ssd1306_init},
	{NULL}
};

// Display window
typedef struct {
	uint16_t        x1;
	uint16_t        y1;
	uint16_t        x2;
	uint16_t        y2;
} dispWin_t;

static dispWin_t dispWin = {
  .x1 = 0,
  .y1 = 0,
  .x2 = 0,
  .y2 = 0,
};

// Register driver and messages
DRIVER_REGISTER_BEGIN(GDISPLAY,gdisplay,0,NULL,NULL);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, InvalidChipset, "invalid chipset", GDISPLAY_ERR_INVALID_CHIPSET);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, InvalidColor, "invalid color", GDISPLAY_ERR_INVALID_COLOR);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, NotSetup, "is not setup", GDISPLAY_ERR_IS_NOT_SETUP);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, ColorRequired, "color required", GDISPLAY_ERR_COLOR_REQUIRED);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, PointRequired, "point required", GDISPLAY_ERR_POINT_REQUIRED);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, NotEnoughtMemory, "not enough memory", GDISPLAY_ERR_NOT_ENOUGH_MEMORY);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, InvalidRadius, "radius must be > 0", GDISPLAY_ERR_INVALID_RADIUS);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, InvalidFont, "invalid font", GDISPLAY_ERR_INVALID_FONT);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, ImageError, "image error", GDISPLAY_ERR_IMAGE);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, OutOfScreen, "out of screen", GDISPLAY_ERR_OUT_OFF_SCREEN);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, InvalidOrientation, "invalid orientation", GDISPLAY_ERR_INVALID_ORIENTATION);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, MissingFileName, "missing file name", GDISPLAY_ERR_MISSING_FILE_NAME);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, ProcessingError, "image processing error", GDISPLAY_ERR_IMG_PROCESSING_ERROR);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, BooleanRequired, "boolean required", GDISPLAY_ERR_BOOLEAN_REQUIRED);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, TouchNotSupported, "touch pad not supported in this display", GDISPLAY_ERR_TOUCH_NOT_SUPPORTED);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, CannotSetup, "can't setup", GDISPLAY_ERR_TOUCH_NOT_SUPPORTED);
	DRIVER_REGISTER_ERROR(GDISPLAY, gdisplay, QREncodeError, "qrcode encode error", GDISPLAY_ERR_QR_ENCODING_ERROR);
DRIVER_REGISTER_END(GDISPLAY,gdisplay,0,NULL,NULL);

/*
 * Helper functions
 */
static void gdisplay_update() {
	if (buffer) {
		gdisplay_ll_update(bbx1, bby1, bbx2, bby2, (uint8_t *)buffer);
	} else {
		gdisplay_ll_update(bbx1, bby1, bbx2, bby2, NULL);
	}

	bbx1 = 5000;
	bbx2 = -1;
	bby1 = 5000;
	bby2 = -1;
}


/*
 * Operation functions
 */

void gdisplay_set_bitmap_pixel(int x, int y, uint32_t color, uint8_t *buff, int buffw, int buffh) {
	gdisplay_ll_set_pixel(x, y, color, buff, buffw, buffh);
}

uint32_t gdisplay_get_bitmap_pixel(int x, int y, uint8_t *buff, int buffw, int buffh) {
	return gdisplay_ll_get_pixel(x, y, buff, buffw, buffh);
}

void gdisplay_set_bitmap(int x, int y, uint8_t *buff, int buffw, int buffh) {
	// Update bound box
	if (x < bbx1) bbx1 = x;
	if (x + buffw - 1 > bbx2) bbx2 = x + buffw - 1;
	if (y < bby1) bby1 = y;
	if (y + buffh - 1 > bby2) bby2 = y + buffh - 1;

	gdisplay_ll_set_bitmap(x, y, buffer?buffer:NULL, buff, buffw, buffh);
}

void gdisplay_get_bitmap(int x, int y, uint8_t *buff, int buffw, int buffh) {
	gdisplay_ll_get_bitmap(x, y, buffer?buffer:NULL, buff, buffw, buffh);
}

driver_error_t *gdisplay_set_pixel(int x, int y, uint32_t color) {
	// Avoid pixels out of the display window
	if ((x < dispWin.x1) || (y < dispWin.y1) || (x > dispWin.x2) || (y > dispWin.y2)) {
		return NULL;
	}

	// Update bound box, only if frame buffer is used
	if (buffer) {
		if (x < bbx1) bbx1 = x;
		if (x > bbx2) bbx2 = x;
		if (y < bby1) bby1 = y;
		if (y > bby2) bby2 = y;
	}

	gdisplay_begin();
	gdisplay_ll_set_pixel(x, y, color, buffer?buffer:NULL, -1, -1);
	gdisplay_end();

	return NULL;
}

driver_error_t *gdisplay_get_pixel(int x, int y, uint32_t *color) {
	// Avoid pixels out of the display window
	if ((x < dispWin.x1) || (y < dispWin.y1) || (x > dispWin.x2) || (y > dispWin.y2)) {
		*color = 0;
		return NULL;
	}

	*color = gdisplay_ll_get_pixel(x, y, buffer?buffer:NULL, -1, -1);

	return NULL;
}

const gdisplay_t *gdisplay_get(uint8_t chipset) {
	const gdisplay_t *cdisplay;
	int i = 0;

	cdisplay = &displaydevs[0];
	while (cdisplay->chipset) {
		if (cdisplay->chipset == chipset) {
			return cdisplay;
		}

		cdisplay++;
		i++;
	}

	return NULL;
}

// Init display
driver_error_t *gdisplay_init(uint8_t chipset, uint8_t orient, uint8_t buffered, uint8_t address) {
	driver_error_t *error;
	gdisplay_t *display;

	// Sanity checks
	if ((orient != PORTRAIT) && (orient != PORTRAIT_FLIP) && (orient != LANDSCAPE) && (orient != LANDSCAPE_FLIP)) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_INVALID_ORIENTATION, NULL);
	}

	if (init) {
		// Flush all
		if (nested > 0) {
			gdisplay_update();
		}

		nested = 0;

		gdisplay_set_orientation(orient);

		return NULL;
	}

#if CONFIG_LUA_RTOS_GDISPLAY_BACKLIGHT >= 0
	gpio_pin_output(CONFIG_LUA_RTOS_GDISPLAY_BACKLIGHT);
	gpio_pin_clr(CONFIG_LUA_RTOS_GDISPLAY_BACKLIGHT);
#endif

	// Get display data
	display = (gdisplay_t *)gdisplay_get(chipset);
	if (!display) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_INVALID_CHIPSET, NULL);
	}

	cursorx = 0;
	cursory = 0;

#if ((CONFIG_LUA_RTOS_POWER_BUS_PIN >= 0) && (CONFIG_LUA_RTOS_GDISPLAY_CONNECTED_TO_POWER_BUS || CONFIG_LUA_RTOS_GDISPLAY_I2C_CONNECTED_TO_POWER_BUS))
    pwbus_on();
#endif

	error = (display->init)(chipset, orient, address);
	if (error) {
		return error;
	}

	// Create frame buffer
	gdisplay_caps_t *caps = gdisplay_ll_get_caps();

	if (buffered || (caps->bytes_per_pixel == 0)) {
		if (caps->bytes_per_pixel > 0) {
			buffer = calloc(caps->width * caps->height, caps->bytes_per_pixel);
		} else {
			// This is a special case, for example for PCD8644 controller, which is
			// a monochrome controller (1 bit per pixel)
			buffer = calloc((caps->width * caps->height) / 8, 1);
		}
		if (!buffer) {
			return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_NOT_ENOUGH_MEMORY, NULL);
		}

		bbx1 = 5000;
		bbx2 = -1;
		bby1 = 5000;
		bby2 = -1;
	}

	orientation = orient;
	rotation = 0;
	wrap = 0;
	transparent = 1;
	force_fixed = 0;

	dispWin.x1 = 0;
	dispWin.y1 = 0;
	dispWin.x2 = caps->width - 1;
	dispWin.y2 = caps->height - 1;

	// Init mutex
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    pthread_mutex_init(&mtx, &attr);

    // Init complete
	init = 1;
	type = chipset;

	return NULL;
}

driver_error_t *gdisplay_width(int *awidth) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_caps_t *caps = gdisplay_ll_get_caps();
	*awidth = caps->width;

	return NULL;
}

driver_error_t *gdisplay_height(int *aheight) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_caps_t *caps = gdisplay_ll_get_caps();
	*aheight =  caps->height;

	return NULL;
}

// Code a color expressed in it's RGB to a display color
driver_error_t *gdisplay_rgb_to_color(int r, int g, int b, uint32_t *color) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_caps_t *caps = gdisplay_ll_get_caps();

	if (caps->bytes_per_pixel == 0) {
		if (((r != 255) || (g != 255) || (b != 255)) && ((r != 0) || (g != 0) || (b != 0))) {
			return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_INVALID_COLOR, "only 2 colors allowed");
		}

		if (r == 255) {
			*color = 1;
		} else {
			*color = 0;
		}

		return NULL;
	}

	if ((r < 0) || (r > 255)) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_INVALID_COLOR, "R component");
	}

	if ((g < 0) || (g > 255)) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_INVALID_COLOR, "G component");
	}

	if ((b < 0) || (b > 255)) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_INVALID_COLOR, "B component");
	}

	*color = (
        ((r >> (8 - caps->rdepth)) << (caps->gdepth + caps->bdepth)) |
        ((g >> (8 - caps->gdepth)) << (caps->bdepth)) |
        (b >> (8 - caps->bdepth))
    );

	return NULL;
}

driver_error_t *gdisplay_color_to_rgb(uint32_t color, int *r, int *g, int *b) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_caps_t *caps = gdisplay_ll_get_caps();
	if (caps->bytes_per_pixel == 0) {
		if ((color != GDISPLAY_BLACK) && (color != GDISPLAY_WHITE)) {
			return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_INVALID_COLOR, "only 2 colors allowed");
		}

		if (color == GDISPLAY_BLACK) {
			*r = 0;
			*g = 0;
			*b = 0;
		} else {
			*r = 255;
			*g = 255;
			*b = 255;
		}
	} else {
		*b = ((~(0xffffffff << caps->bdepth) & color) << (8 - caps->bdepth)) | (0xffffffff >> (32 - caps->bdepth));
		color >>= caps->bdepth;
		*g = ((~(0xffffffff << caps->gdepth) & color) << (8 - caps->gdepth)) | (0xffffffff >> (32 - caps->gdepth));
		color >>= caps->gdepth;
		*r = ((~(0xffffffff << caps->rdepth) & color) << (8 - caps->rdepth)) | (0xffffffff >> (32 - caps->rdepth));
	}

	return NULL;
}

/**
 * Converts the components of a color, as specified by the HSB
 * model, to an equivalent set of values for the default RGB model.
 * The _sat and _brightnesscomponents
 * should be floating-point values between zero and one (numbers in the range 0.0-1.0)
 * The _hue component can be any floating-point number.  The floor of this number is
 * subtracted from it to create a fraction between 0 and 1.
 * This fractional number is then multiplied by 360 to produce the hue
 * angle in the HSB color model.
 * The integer that is returned by HSBtoRGB encodes the
 * value of a color in bits 0-15 of an integer value
*/
//-------------------------------------------------------------------
driver_error_t *gdisplay_hsb(float _hue, float _sat, float _brightness, uint32_t *color) {
	float red = 0.0;
	float green = 0.0;
	float blue = 0.0;

	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER,
				GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	if (_sat == 0.0) {
		red = _brightness;
		green = _brightness;
		blue = _brightness;
	} else {
		if (_hue == 360.0) {
			_hue = 0;
		}

		int slice = (int) (_hue / 60.0);
		float hue_frac = (_hue / 60.0) - slice;

		float aa = _brightness * (1.0 - _sat);
		float bb = _brightness * (1.0 - _sat * hue_frac);
		float cc = _brightness * (1.0 - _sat * (1.0 - hue_frac));

		switch (slice) {
		case 0:
			red = _brightness;
			green = cc;
			blue = aa;
			break;
		case 1:
			red = bb;
			green = _brightness;
			blue = aa;
			break;
		case 2:
			red = aa;
			green = _brightness;
			blue = cc;
			break;
		case 3:
			red = aa;
			green = bb;
			blue = _brightness;
			break;
		case 4:
			red = cc;
			green = aa;
			blue = _brightness;
			break;
		case 5:
			red = _brightness;
			green = aa;
			blue = bb;
			break;
		default:
			red = 0.0;
			green = 0.0;
			blue = 0.0;
			break;
		}
	}

	// Convert to RGB888
	if (red < 0.0) red = 0.0;
	if (green < 0.0) green = 0.0;
	if (blue < 0.0) blue = 0.0;

	uint8_t ired = (uint8_t) (red * 255.0);
	uint8_t igreen = (uint8_t) (green * 255.0);
	uint8_t iblue = (uint8_t) (blue * 255.0);

	// Convert color to display capabilities
	gdisplay_rgb_to_color(ired, igreen, iblue, color);

	return NULL;
}

driver_error_t *gdisplay_clear(uint32_t color) {
	gdisplay_caps_t *caps = gdisplay_ll_get_caps();

	int x, y;

	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_begin();
	for(y=0;y < caps->height;y++) {
		for(x=0;x < caps->width;x++) {
			gdisplay_set_pixel(x,y,color);
		}
	}
	gdisplay_end();

	cursorx = 0;
	cursory = 0;

	//rotation = 0;
	//wrap = 0;
	//transparent = 1;
	//force_fixed = 0;

	dispWin.x1 = 0;
	dispWin.y1 = 0;
	dispWin.x2 = caps->width - 1;
	dispWin.y2 = caps->height - 1;

	return NULL;
}

driver_error_t *gdisplay_on() {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_ll_on();

#if CONFIG_LUA_RTOS_GDISPLAY_BACKLIGHT >= 0
	gpio_pin_set(CONFIG_LUA_RTOS_GDISPLAY_BACKLIGHT);
#endif

	return NULL;
}

driver_error_t *gdisplay_off() {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_ll_off();

#if CONFIG_LUA_RTOS_GDISPLAY_BACKLIGHT >= 0
	gpio_pin_clr(CONFIG_LUA_RTOS_GDISPLAY_BACKLIGHT);
#endif

	return NULL;
}

uint8_t gdisplay_is_init() {
	return init;
}

void gdisplay_begin() {
	nested++;
}

void gdisplay_end() {
	nested--;
	if (nested == 0) {
		gdisplay_update();
	}
}

void gdisplay_set_transparency(uint8_t ntransparent) {
	transparent = ntransparent;
}

uint8_t gdisplay_get_transparency() {
	return transparent;
}

void gdisplay_set_wrap(uint8_t nwrap) {
	wrap = nwrap;
}

uint8_t gdisplay_get_wrap() {
	return wrap;
}

int gdisplay_get_offset() {
	return offset;
}

void gdisplay_set_offset(int noffset) {
	offset = noffset;
}

driver_error_t *gdisplay_set_orientation(uint16_t orient) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	if ((rotation != PORTRAIT) && (rotation != PORTRAIT_FLIP) && (rotation != LANDSCAPE) && (rotation != LANDSCAPE_FLIP)) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_INVALID_ORIENTATION, NULL);
	}

	gdisplay_caps_t *caps = gdisplay_ll_get_caps();
	if (caps->bytes_per_pixel == 0) {
		gdisplay_clear(GDISPLAY_WHITE);
	} else {
		gdisplay_clear(GDISPLAY_BLACK);
	}

	orientation = orient;
	gdisplay_ll_set_orientation(orient);

	dispWin.x1 = 0;
	dispWin.y1 = 0;
	dispWin.x2 = caps->width - 1;
	dispWin.y2 = caps->height - 1;

	return NULL;
}

driver_error_t *gdisplay_set_rotation(uint16_t newrot) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	rotation = newrot;

	return NULL;
}

void gdisplay_set_force_fixed(uint8_t fixed) {
	force_fixed = fixed;
}

uint8_t gdisplay_get_force_fixed() {
	return force_fixed;
}

uint16_t gdisplay_get_rotation() {
	return rotation;
}

void gdisplay_set_cursor(int x, int y) {
	cursorx = x;
	cursory = y;
}

void gdisplay_get_cursor(int *x, int *y) {
	*x = cursorx;
	*y = cursory;
}

float gdisplay_get_max_angle() {
	return arc_angle_max;
}

float gdisplay_get_angle_offset() {
	return angle_offset;
}

void gdisplay_set_angle_offset(float noffset) {
	if (noffset < -360.0) noffset = -360.0;
	if (noffset > 360.0) noffset = 360.0;

	angle_offset = noffset;
}

driver_error_t *gdisplay_set_clip_window(int x0, int y0, int x1, int y1) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	if (x0 < 0) {
		x0 = 0;
	}

	if (y0 < 0) {
		y0 = 0;
	}

	gdisplay_caps_t *caps = gdisplay_ll_get_caps();

	if (x1 > caps->width - 1) {
		x1 = caps->width - 1;
	}

	if (y1 > caps->height - 1) {
		y1 = caps->height - 1;
	}

	dispWin.x1 = x0;
	dispWin.y1 = y0;
	dispWin.x2 = x1;
	dispWin.y2 = y1;

	return NULL;
}

driver_error_t *gdisplay_get_clip_window(int *x0, int *y0, int *x1, int *y1) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	*x0 = dispWin.x1;
	*y0 = dispWin.y1;
	*x1 = dispWin.x2;
	*y1 = dispWin.y2;

	return NULL;
}

driver_error_t *gdisplay_reset_clip_window() {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_caps_t *caps = gdisplay_ll_get_caps();

	dispWin.x1 = 0;
	dispWin.y1 = 0;
	dispWin.x2 = caps->width - 1;
	dispWin.y2 = caps->height - 1;

	return NULL;
}

driver_error_t *gdisplay_invert(uint8_t on) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_ll_invert(on);

	return NULL;
}

driver_error_t *gdisplay_type(int8_t *dtype) {
	// Sanity checks
	if (!init) {
		*dtype = -1;
		return NULL;
	}

	*dtype = type;

	return NULL;
}

driver_error_t *gdisplay_lock() {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	pthread_mutex_lock(&mtx);
	gdisplay_begin();

	return NULL;
}

driver_error_t *gdisplay_unlock() {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_end();
	pthread_mutex_unlock(&mtx);

	return NULL;
}

driver_error_t *gdisplay_touch_get(int *x, int *y, int *z) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_caps_t *caps = gdisplay_ll_get_caps();

	if (caps->touch_get) {
		caps->touch_get(x, y, z, 0);
	} else {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_TOUCH_NOT_SUPPORTED, NULL);
	}

	return NULL;
}

driver_error_t *gdisplay_touch_get_raw(int *x, int *y, int *z) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_caps_t *caps = gdisplay_ll_get_caps();

	if (caps->touch_get) {
		caps->touch_get(x, y, z, 1);
	} else {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_TOUCH_NOT_SUPPORTED, NULL);
	}

	return NULL;
}

driver_error_t *gdisplay_touch_set_cal(int x, int y) {
	// Sanity checks
	if (!init) {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_IS_NOT_SETUP, "init display first");
	}

	gdisplay_caps_t *caps = gdisplay_ll_get_caps();

	if (caps->touch_cal) {
		caps->touch_cal(x, y);
	} else {
		return driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_TOUCH_NOT_SUPPORTED, NULL);
	}

	return NULL;
}

#endif
