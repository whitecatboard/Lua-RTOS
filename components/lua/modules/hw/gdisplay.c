/* Lua-RTOS-ESP32 TFT module
 *
 *  Author: LoBo (loboris@gmail.com, loboris.github)
 *
 *  Module supporting SPI TFT displays based on ILI9341 & ST7735 controllers
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LUA_USE_GDISPLAY

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "time.h"
#include "tjpgd.h"
#include <math.h>
#include "sys/status.h"
#include "sys/syslog.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "error.h"

#include <drivers/spi.h>

#include <drivers/gdisplay.h>
#include <gdisplay/gdisplay.h>

#define GDISPLAY_CURR_BACKGROUND 0
#define GDISPLAY_CURR_FOREGROUND 1
#define GDISPLAY_CURR_STROKE     2
#define GDISPLAY_CURR_FILL       3
#define GDISPLAY_CUSTOM          4

// Default colors
static uint32_t static_background;
static uint32_t static_foreground;
static uint32_t static_stroke;
static uint32_t static_fill;

static uint8_t lgdisplay_get_point(lua_State* L, uint8_t n, int *x, int *y) {
	if ((lua_gettop(L) < n)) {
		luaL_exception(L, GDISPLAY_ERR_POINT_REQUIRED);
	}

	if (lua_istable(L, n)) {
		// Argument is coordinate {x, y}
		uint8_t i;
		double c[3];

		for (i = 0; i < 2; i++) {
			lua_rawgeti(L, n, i + 1);
			c[i] = (int) luaL_checknumber(L, -1);
			lua_pop(L, 1);
		}

		*x = (int)c[0];
		*y = (int)c[1];

		return n + 1;
	} else {
		if ((lua_gettop(L) < n + 1)) {
			luaL_exception(L, GDISPLAY_ERR_POINT_REQUIRED);
		}

		if (!lua_isnumber(L, n)) {
			luaL_exception_extended(L, GDISPLAY_ERR_POINT_REQUIRED, "point coordinate must be a number (x)");
		}

		if (!lua_isnumber(L, n + 1)) {
			luaL_exception_extended(L, GDISPLAY_ERR_POINT_REQUIRED, "point coordinate must be a number (y)");
		}

		*x =  (int) luaL_checknumber(L, n);
		*y =  (int) luaL_checknumber(L, n + 1);

		return n + 2;
	}
}

static uint32_t lgdisplay_get_color(lua_State* L, uint8_t n, uint8_t mandatory, uint8_t which, uint32_t custom) {
	driver_error_t *error;
	uint32_t color;

	// If color is mandatory, but not present raise an exception
	if (mandatory && (lua_gettop(L) < n)) {
		luaL_exception(L, GDISPLAY_ERR_COLOR_REQUIRED);
	}

	// If not color, and not custom, return current default color
	if ((lua_gettop(L) < n) && (which != GDISPLAY_CUSTOM)) {
		switch (which) {
			case GDISPLAY_CURR_BACKGROUND: return static_background;
			case GDISPLAY_CURR_FOREGROUND: return static_foreground;
			case GDISPLAY_CURR_STROKE:     return static_stroke;
			case GDISPLAY_CURR_FILL:       return static_fill;
		}
	}

	// If not color, and custom, return custom color
	if ((lua_gettop(L) < n) && (which == GDISPLAY_CUSTOM)) {
		return custom;
	}

	// There is a color argument, process
	if (lua_istable(L, n)) {
		// Argument is table {r, g, b}
		uint8_t i;
		uint8_t cl[3];

		for (i = 0; i < 3; i++) {
			lua_rawgeti(L, n, i + 1);
			cl[i] = (int) luaL_checkinteger(L, -1);
			lua_pop(L, 1);
		}

		// Convert color
		error = gdisplay_rgb_to_color(cl[0], cl[1], cl[2], &color);
		if (error) {
			return luaL_driver_error(L, error);
		}
	} else {
		int r, g, b;
		color = luaL_checkinteger(L, n);

		error = gdisplay_color_to_rgb(color, &r, &g, &b);
		if (error) {
			return luaL_driver_error(L, error);
		}
	}

	return color;
}

static uint8_t lgdisplay_getbool(lua_State *L, uint8_t n) {
	int b = 0;

	if ((lua_gettop(L) < n)) {
		luaL_exception(L, GDISPLAY_ERR_BOOLEAN_REQUIRED);
	}

	if (lua_isboolean(L, n)) {
		if (lua_toboolean(L, 1)) {
			b = 1;
		}
	} else if (lua_isinteger(L, n)) {
		b = (uint8_t)luaL_checkinteger(L, 1);
		if ((b != 0) && (b != 1)) {
			luaL_exception(L, GDISPLAY_ERR_BOOLEAN_REQUIRED);
		}
	}

	return (uint8_t)b;
}

//==================================
static int lgdisplay_gettype(lua_State *L) {
	driver_error_t *error;
	int8_t type;

	error = gdisplay_type(&type);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger( L, type);
	return 1;
}

//=========================================
static int lgdisplay_set_brightness(lua_State *L) {
	return 0;
}

//======================================
static int lgdisplay_setorient( lua_State* L ) {
	gdisplay_set_orientation(luaL_checkinteger( L, 1 ));
	return 0;
}

//==================================
static int lgdisplay_clear( lua_State* L ) {
	driver_error_t *error;

	error = gdisplay_clear(lgdisplay_get_color(L, 1, 0, 0, GDISPLAY_BLACK));
	if (error) {
		return luaL_driver_error(L, error);
	}

	static_stroke = GDISPLAY_WHITE;
	static_foreground = GDISPLAY_WHITE;;
	static_background = GDISPLAY_BLACK;

	return 0;
}

static int lgdisplay_foreground(lua_State* L) {
	static_foreground = lgdisplay_get_color(L, 1, 1, 0, 0);
	return 0;
}

static int lgdisplay_background(lua_State* L) {
	static_background = lgdisplay_get_color(L, 1, 1, 0, 0);
	return 0;
}

static int lgdisplay_stroke(lua_State* L) {
	static_stroke = lgdisplay_get_color(L, 1, 1, 0, 0);
	return 0;
}

static int lgdisplay_setcolor(lua_State* L) {
	static_foreground = lgdisplay_get_color(L, 1, 1, 0, 0);
	static_stroke = lgdisplay_get_color(L, 1, 1, 0, 0);
	return 0;
}


//===================================
static int lgdisplay_invert( lua_State* L ) {
	driver_error_t *error;
	uint8_t invert = 0;

	luaL_checktype(L, 1, LUA_TBOOLEAN);
	if (lua_toboolean(L, 1)) {
		invert = 1;
	}

	error = gdisplay_invert(invert);
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//====================================
static int lgdisplay_setwrap( lua_State* L ) {
	gdisplay_set_wrap(lgdisplay_getbool(L, 1));
	return 0;
}

//======================================
static int lgdisplay_settransp( lua_State* L ) {
	gdisplay_set_transparency(lgdisplay_getbool(L, 1));
	return 0;
}

//===================================
static int lgdisplay_setrot( lua_State* L ) {
	gdisplay_set_rotation(luaL_checkinteger( L, 1 ));
	return 0;
}

//===================================
static int lgdisplay_setfixed( lua_State* L ) {
	gdisplay_set_force_fixed(lgdisplay_getbool(L,1));

	return 0;
}



//====================================
static int lgdisplay_setfont( lua_State* L ) {
	driver_error_t *error;
	uint8_t fnt = DEFAULT_FONT;
	size_t fnlen = 0;
	const char* fname = NULL;

	if (lua_type(L, 1) == LUA_TNUMBER) {
		fnt = luaL_checkinteger(L, 1);
	} else if (lua_type(L, 1) == LUA_TSTRING) {
		fnt = USER_FONT;
		fname = lua_tolstring(L, -1, &fnlen);
	}

	error = gdisplay_set_font(fnt, fname);
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

static int lgdisplay_lock( lua_State* L ) {
	driver_error_t *error;

	error = gdisplay_lock();
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

static int lgdisplay_unlock( lua_State* L ) {
	driver_error_t *error;

	error = gdisplay_unlock();
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}


//========================================
static int lgdisplay_getfontsize( lua_State* L ) {
	driver_error_t *error;
	int w, h;

	error = gdisplay_get_font_size(&w,&h);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger(L, w);
	lua_pushinteger(L, h);

	return 2;
}

//==========================================
static int lgdisplay_getscreensize( lua_State* L ) {
	driver_error_t *error;
	int width;
	int height;

	error = gdisplay_width(&width);
	if (error) {
		return luaL_driver_error(L, error);
	}

	error = gdisplay_height(&height);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger( L, width);
	lua_pushinteger( L, height);

	return 2;
}

//==========================================
static int lgdisplay_getfontheight( lua_State* L ) {
	driver_error_t *error;
	int w, h;

	error = gdisplay_get_font_size(&w,&h);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger(L, h);

	return 1;
}

static int lgdisplay_getfontwidtht( lua_State* L ) {
	driver_error_t *error;
	int w, h;

	error = gdisplay_get_font_size(&w,&h);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger(L, w);

	return 1;
}

//===============================
static int lgdisplay_on( lua_State* L ) {
	driver_error_t *error;

	error = gdisplay_on();
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//================================
static int lgdisplay_off( lua_State* L ) {
	driver_error_t *error;

	error = gdisplay_off();
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//=======================================
static int lgdisplay_setclipwin( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x0, y0, x1, y1;

	n = lgdisplay_get_point(L, n, &x0, &y0);
	n = lgdisplay_get_point(L, n, &x1, &y1);

	error = gdisplay_set_clip_window(x0, y0, x1, y1);
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//=========================================
static int lgdisplay_resetclipwin( lua_State* L ) {
	driver_error_t *error;

	error = gdisplay_reset_clip_window();
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

static int lgdisplay_getclipwin( lua_State* L ) {
	driver_error_t *error;
	int x0, y0, x1, y1;

	error = gdisplay_get_clip_window(&x0, &y0, &x1, &y1);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger(L, x0);
	lua_pushinteger(L, y0);
	lua_pushinteger(L, x1);
	lua_pushinteger(L, y1);

	return 4;
}

//=====================================
static int lgdisplay_HSBtoRGB( lua_State* L ) {
	driver_error_t *error;
	uint32_t color;

	float hue = luaL_checknumber(L, 1);
	float sat = luaL_checknumber(L, 2);
	float bri = luaL_checknumber(L, 3);

	error = gdisplay_hsb(hue, sat, bri, &color);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger(L, color);

	return 1;
}

static int lgdisplay_rgb_to_color(lua_State* L) {
	driver_error_t *error;
	int r, g, b;
	uint32_t color;

	r = luaL_checkinteger(L, 1);
	g = luaL_checkinteger(L, 2);
	b = luaL_checkinteger(L, 3);

	error = gdisplay_rgb_to_color(r, g, b, &color);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger(L, color);

	return 1;
}
//=====================================
static int lgdisplay_putpixel( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x, y;

	n = lgdisplay_get_point(L, n, &x, &y);

	error = gdisplay_set_pixel(x, y, lgdisplay_get_color(L, n, 0, GDISPLAY_CURR_STROKE, 0));
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//=====================================
static int lgdisplay_getpixel( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x, y;
	uint32_t color;

	n = lgdisplay_get_point(L, n, &x, &y);

	error = gdisplay_get_pixel(x, y, &color);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger(L, color);

	return 1;
}

//======================================
static int lgdisplay_getwindow( lua_State* L )
{
#if 0
	_check(L);
	if (checkParam(4, L)) return 0;

	int out_type = 0;
	luaL_Buffer b;
	char hbuf[8];

	if ((lua_gettop(L) > 4) && (lua_isstring(L, 4))) {
		const char* sarg;
		size_t sarglen;
		sarg = luaL_checklstring(L, 5, &sarglen);
		if (sarglen == 2) {
			if (strstr(sarg, "*h") != NULL) out_type = 1;
			else if (strstr(sarg, "*t") != NULL) out_type = 2;
		}
	}

	int16_t x = luaL_checkinteger( L, 1 );
	int16_t y = luaL_checkinteger( L, 2 );
	int w = luaL_checkinteger( L, 3 );
	int h = luaL_checkinteger( L, 3 );
	uint8_t f = 0;
	if (w < 1) w = 1;
	if (w > _width) w = _width;
	if (h < 1) h = 1;
	if (h > _height) h = _height;
	if ((x + w) > _width) w = _width - x;
	if ((y + h) > _height) h = _height - h;

	int len = w*h;
	if ((y < 0) || (y > (_height-1))) f= 1;
	else if ((x < 0) || (x > (_width-1))) f= 1;
	else if (len > (TFT_LINEBUF_MAX_SIZE)) f = 1;
	if (f) {
		return luaL_error( L, "wrong coordinates or size > %d", TFT_LINEBUF_MAX_SIZE );
	}

	int err = read_data(x, y, x+w, y+h, len, (uint8_t *)tft_line);
	if (err < 0) {
		return luaL_error( L, "Error reading display data (%d)", err );
	}

	if (out_type < 2) luaL_buffinit(L, &b);
	else lua_newtable(L);

	if (out_type == 0) {
		luaL_addlstring(&b, (const char *)tft_line, len);
	}
	else {
		for (int i = 0; i < len; i++) {
			if (out_type == 1) {
				sprintf(hbuf, "%04x;", tft_line[i]);
				luaL_addstring(&b, hbuf);
			}
			else {
				lua_pushinteger( L, tft_line[i]);
				lua_rawseti(L, -2, i+1);
			}
		}
	}

	if (out_type < 2) luaL_pushresult(&b);
#endif
	return 1;
}

//=====================================
static int lgdisplay_drawline( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x0, y0;
	int x1, y1;

	// Get start / end point
	n = lgdisplay_get_point(L, n, &x0, &y0);
	n = lgdisplay_get_point(L, n, &x1, &y1);

	// Display
	error = gdisplay_line(x0, y0, x1, y1, lgdisplay_get_color(L, n, 0, GDISPLAY_CURR_STROKE, 0));
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//============================================
static int lgdisplay_drawlineByAngle( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x0, y0;

	// Get start point
	n = lgdisplay_get_point(L, n, &x0, &y0);

	// Get len / angle / start
	double length = luaL_checknumber(L, n++);
	double angle = luaL_checknumber(L, n++);

	uint32_t color = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0);
	double start = luaL_optnumber(L, n++, 0);

	// Display
	error = gdisplay_line_by_angle(x0, y0, (int)length, (int)angle, (int)start, 0, color);
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//=================================
static int lgdisplay_rect( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x, y;

	// Get start point
	n = lgdisplay_get_point(L, n, &x, &y);

	// Get width and height
	double wd = luaL_checknumber( L, n++ );
	double hd = luaL_checknumber( L, n++ );

	int w = (int)wd;
	int h = (int)hd;

	// Display
	if (lua_gettop(L) > n) {
		uint32_t stroke = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0);
		uint32_t fill = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_FILL, 0);

		error = gdisplay_rect_fill(x, y, w, h, stroke, fill);
	} else {
		error = gdisplay_rect(x, y, w, h,
				lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0)
		);
	}

	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//static void rounded_Square(int cx, int cy, int h, int w, float radius, uint16_t color, uint8_t fill)
//======================================
static int lgdisplay_roundrect( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x, y;

	// Get start point
	n = lgdisplay_get_point(L, n, &x, &y);

	// Get width / height
	int w = luaL_checknumber( L, n++ );
	int h = luaL_checknumber( L, n++ );

	// Get radius
	float r = luaL_checknumber( L, n++ );

	// Display
	if (lua_gettop(L) > n) {
		uint32_t stroke = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0);
		uint32_t fill = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_FILL, 0);

		error = gdisplay_round_rect_fill(x, y, w, h, r, stroke, fill);
	} else {
		error = gdisplay_round_rect(x, y, w, h, r,
				lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0)
		);
	}

	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//=================================
static int lgdisplay_circle( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x, y;

	// Get start point
	n = lgdisplay_get_point(L, n, &x, &y);

	// Get radius
	double rd = luaL_checknumber( L, n++ );

	int r = (int)rd;

	// Display
	if (lua_gettop(L) > n) {
		uint32_t stroke = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0);
		uint32_t fill = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_FILL, 0);

		error = gdisplay_circle_fill(x, y, r, stroke, fill);
	} else {
		error = gdisplay_circle(x, y, r,
				lgdisplay_get_color(L, n, 0, GDISPLAY_CURR_STROKE, 0)
		);
	}

	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//====================================
static int lgdisplay_ellipse( lua_State* L ) {
	driver_error_t *error;
	uint8_t opt = 15;
	int x, y;
	int n = 1;

	// Get start point
	n = lgdisplay_get_point(L, n, &x, &y);

	// Get radius
	int rx = luaL_checkinteger( L, n++ );
	int ry = luaL_checkinteger( L, n++ );

	// Options
	if (lua_gettop(L) == n + 3) opt = luaL_checkinteger( L, n + 3 );

	if (lua_gettop(L) > n) {
		uint32_t stroke = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0);
		uint32_t fill = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_FILL, 0);

		error = gdisplay_ellipse_fill(x, y, rx, ry, stroke, fill, opt);
	} else {
		error = gdisplay_ellipse(x, y, rx, ry, lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0), opt);
	}

	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//================================
static int lgdisplay_arc( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int cx, cy;

	n = lgdisplay_get_point(L, n, &cx, &cy);

	uint16_t r = luaL_checkinteger( L, n++ );
	uint16_t th = luaL_checkinteger( L, n++ );

	float start = luaL_checknumber( L, n++ );
	float end = luaL_checknumber( L, n++ );

	if (lua_gettop(L) > n) {
		uint32_t stroke = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0);
		uint32_t fill = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_FILL, 0);

		error = gdisplay_arc_fill(cx, cy, r, th, start, end, stroke, fill);
	} else {
		error = gdisplay_arc(cx, cy, r, th, start, end,
				lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0)
		);
	}

	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//static void drawPolygon(int cx, int cy, int sides, int diameter, uint16_t color, bool fill, float deg)
//=================================
static int lgdisplay_poly( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int cx, cy;

	n = lgdisplay_get_point(L, n, &cx, &cy);

	uint16_t sides = luaL_checkinteger( L, n++ );
	uint16_t r = luaL_checkinteger( L, n++ );

	float rot = luaL_checknumber( L, n++ );

	if (lua_gettop(L) > n) {
		uint32_t stroke = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0);
		uint32_t fill = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_FILL, 0);

		error = gdisplay_polygon_fill(cx, cy, sides, r, rot, stroke, fill);
	} else {
		error = gdisplay_polygon(cx, cy, sides, r, rot,
				lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0)
		);
	}

	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//static void drawStar(int cx, int cy, int diameter, uint16_t color, bool fill, float factor)
//=================================
static int lgdisplay_star( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int cx, cy;

	n = lgdisplay_get_point(L, n, &cx, &cy);
	int r = luaL_checknumber( L, n++ );
	int fact = luaL_checknumber( L, n++ );

	if (lua_gettop(L) > n) {
		uint32_t stroke = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0);
		uint32_t fill = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_FILL, 0);

		error = gdisplay_star_fill(cx, cy, r, fact, stroke, fill);
	} else {
		error = gdisplay_star(cx, cy, r, fact,
				lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0)
		);
	}

	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//=====================================
static int lgdisplay_triangle( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x0, y0, x1, y1, x2, y2;

	// Get points
	n = lgdisplay_get_point(L, n, &x0, &y0);
	n = lgdisplay_get_point(L, n, &x1, &y1);
	n = lgdisplay_get_point(L, n, &x2, &y2);

	if (lua_gettop(L) > n) {
		uint32_t stroke = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0);
		uint32_t fill = lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_FILL, 0);

		error = gdisplay_triangle_fill(x0, y0, x1, y1, x2, y2, stroke, fill);
	} else {
		error = gdisplay_triangle(x0, y0, x1, y1, x2, y2,
				lgdisplay_get_color(L, n++, 0, GDISPLAY_CURR_STROKE, 0)
		);
	}

	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

//=====================================
static int lgdisplay_writepos( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x, y;
	int px, py, pw, ph;
	const char* buf;
	size_t len;

	n = lgdisplay_get_point(L, n, &x, &y);

	luaL_checktype( L, n, LUA_TSTRING );
	buf = lua_tolstring( L, n, &len );

	error = gdisplay_string_pos(x, y, buf, &px, &py, &pw, &ph);
	if (error) {
		return luaL_driver_error(L, error);
	}

	if ((px == -1) && (py == -1)) {
		lua_pushnil(L);
		return 1;
	} else {
		lua_pushinteger(L, px);
		lua_pushinteger(L, py);
		lua_pushinteger(L, pw);
		lua_pushinteger(L, ph);

		return 4;
	}
}

//tft.write(x,y,string|intnum|{floatnum,dec},...)
//==================================
static int lgdisplay_write( lua_State* L ) {
	driver_error_t *error;
	const char* buf;
	char tmps[16];
	size_t len;
	uint8_t numdec = 0;
	float fnum;

	int n = 1;
	int x, y;

	// Get send point
	n = lgdisplay_get_point(L, n, &x, &y);

	// Get foreground/background colors
	int foreground = lgdisplay_get_color(L, n + 1, 0, GDISPLAY_CURR_FOREGROUND, 0);
	int background = lgdisplay_get_color(L, n + 2, 0, GDISPLAY_CURR_BACKGROUND, 0);

	// Get foreground/background colors
	if (lua_type(L, n) == LUA_TNUMBER) { // write integer number
		len = lua_tointeger(L, n);
		sprintf(tmps, "%d", len);
		error = gdisplay_print(x, y, &tmps[0], foreground, background);
		if (error) {
			return luaL_driver_error(L, error);
		}
	} else if (lua_type(L, n) == LUA_TTABLE) {
		if (lua_rawlen(L, n) == 2) {
			lua_rawgeti(L, n, 1);
			fnum = luaL_checknumber(L, -1);
			lua_pop(L, 1);
			lua_rawgeti(L, n, 2);
			numdec = (int) luaL_checkinteger(L, -1);
			lua_pop(L, 1);
			sprintf(tmps, "%.*f", numdec, fnum);
			error = gdisplay_print(x, y, &tmps[0], foreground, background);
			if (error) {
				return luaL_driver_error(L, error);
			}
		}
	} else if (lua_type(L, n) == LUA_TSTRING) { // write string
		luaL_checktype(L, n, LUA_TSTRING);
		buf = lua_tolstring(L, n, &len);
		error = gdisplay_print(x, y, (char*) buf, foreground, background);
		if (error) {
			return luaL_driver_error(L, error);
		}
	}

	return 0;
}

static int lgdisplay_image( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int x, y;
	size_t len;
	int max_scale;

	// Get point / maxscale / filename
	n = lgdisplay_get_point(L, n, &x, &y);

	// Get file name
	const char *fname = luaL_checklstring( L, n++, &len );

	// Check for image type
	image_type type;

	error = gdisplay_image_type(fname, &type);
	if (error) {
		return luaL_driver_error(L, error);
	}

	switch (type) {
		case BMPImage:
			error = gdisplay_image_bmp(x, y, fname);
			if (error) {
				return luaL_driver_error(L, error);
			}
			break;
		case JPGImage:
			max_scale = luaL_optinteger( L, n, 0);

			error = gdisplay_image_jpg(x, y, max_scale, fname);
			if (error) {
				return luaL_driver_error(L, error);
			}
			break;
		case UNKNOWImage:
			return luaL_exception_extended(L, GDISPLAY_ERR_IMAGE, "unknown image format");
			break;
	}

	return 0;
}

static int lgdisplay_qrcode( lua_State* L ) {
	driver_error_t *error;

	int n = 1;
	int sx, sy;
	size_t len;
	const char *buf = 0;

	// Get point / maxscale / filename
	n = lgdisplay_get_point(L, n, &sx, &sy);

	// Get file name
	int text_to_encode = n;
	if (!lua_istable(L, text_to_encode)) {
		luaL_checktype( L, n, LUA_TSTRING );
		buf = lua_tolstring( L, n++, &len );
	}
	else n++;

	uint8_t errCorLvl = luaL_optinteger( L, n++, qrcodegen_Ecc_LOW);
	uint8_t multi = luaL_optinteger( L, n++, 1);

	// Make and print the QR Code symbol
	uint8_t *qrcode = NULL;
	qrcode = malloc(qrcodegen_BUFFER_LEN_MAX);
	if (!qrcode) {
		luaL_exception_extended(L, GDISPLAY_ERR_NOT_ENOUGH_MEMORY, "error allocating qrcode image buffer");
	}
	uint8_t *tempBuffer = NULL;
	tempBuffer = malloc(qrcodegen_BUFFER_LEN_MAX);
	if (!qrcodegen_BUFFER_LEN_MAX) {
		free(qrcode);
		luaL_exception_extended(L, GDISPLAY_ERR_NOT_ENOUGH_MEMORY, "error allocating qrcode temp buffer");
	}

	bool ok = false;
	if (lua_istable(L, text_to_encode)) {
		#define MAX_SEGMENTS 10
		struct qrcodegen_Segment text_segments[MAX_SEGMENTS];
		int num_segments = 0;

		// Push another reference to the master table on top of the stack (so we know
		// where it is, and this function can work for negative, positive and
		// pseudo indices
		lua_pushvalue(L, text_to_encode);
		// stack now contains: -1 => table
		lua_pushnil(L);
		// stack now contains: -1 => nil; -2 => table
		while (lua_next(L, -2))
		{
			// stack now contains: -1 => value; -2 => key; -3 => table
			// copy the key so that lua_tostring does not modify the original
			lua_pushvalue(L, -2);
			// stack now contains: -1 => key; -2 => value; -3 => key; -4 => table
			// note: this "parent" array has no "string" key
			// note: this "parent" array value is a table

			int mode = 0;
			char *text = 0;

			// Push another reference to the child table on top of the stack (so we know
			// where it is, and this function can work for negative, positive and
			// pseudo indices
			lua_pushvalue(L, -2);
			// stack now contains: -1 => table
			lua_pushnil(L);
			// stack now contains: -1 => nil; -2 => table
			while (lua_next(L, -2))
			{
				// stack now contains: -1 => value; -2 => key; -3 => table
				// copy the key so that lua_tostring does not modify the original
				lua_pushvalue(L, -2);
				// stack now contains: -1 => key; -2 => value; -3 => key; -4 => table
				const char *key = lua_tostring(L, -1);
				const char *value = lua_tostring(L, -2);

				if (!strcasecmp(key,"mode"))  { mode = lua_tointeger(L, -2); }
				if (!strcasecmp(key,"text"))  { if(text) free(text); text = strdup(value); }
				if (!strcasecmp(key,"val"))   { if(text) free(text); text = strdup(value); }
				if (!strcasecmp(key,"value")) { if(text) free(text); text = strdup(value); }

				// pop value + copy of key, leaving original key
				lua_pop(L, 2);
				// stack now contains: -1 => key; -2 => table
			}
			// stack now contains: -1 => table (when lua_next returns 0 it pops the key
			// but does not push anything.)
			// Pop table
			lua_pop(L, 1);
			// Stack is now the same as it was on entry to this function

			if (text && num_segments < MAX_SEGMENTS) {
				syslog(LOG_DEBUG, "gdisplay: encoding segment type %i value '%s' ...\n", mode, text);
				uint8_t *segBuf = malloc(qrcodegen_calcSegmentBufferSize((enum qrcodegen_Mode)mode, strlen(text)) * sizeof(uint8_t));
				switch(mode) {
					case qrcodegen_Mode_NUMERIC:
						text_segments[num_segments] = qrcodegen_makeNumeric(text, segBuf);
						break;
					case qrcodegen_Mode_ALPHANUMERIC:
						text_segments[num_segments] = qrcodegen_makeAlphanumeric(text, segBuf);
						break;
					case qrcodegen_Mode_BYTE:
						text_segments[num_segments] = qrcodegen_makeBytes((const uint8_t *)text, strlen(text), segBuf);
						break;
					case qrcodegen_Mode_ECI:
						text_segments[num_segments] = qrcodegen_makeEci(atol(text), segBuf);
						break;
					default:
						syslog(LOG_WARNING, "gdisplay: invalid mode %i, ignoring segment '%s'\n", mode, text);
				}
				num_segments++;

				free(text);
				text = 0;
			} else if (text) {
				syslog(LOG_WARNING, "gdisplay: maximum number of segments reached, ignoring segment '%s' with mode %i\n", text, mode);
			} else {
				syslog(LOG_WARNING, "gdisplay: no content found, ignoring mode %i\n", mode);
			}

			// pop value + copy of key, leaving original key
			lua_pop(L, 2);
			// stack now contains: -1 => key; -2 => table
		}
		// stack now contains: -1 => table (when lua_next returns 0 it pops the key
		// but does not push anything.)
		// Pop table
		lua_pop(L, 1);
		// Stack is now the same as it was on entry to this function


		if (num_segments>0) {
			syslog(LOG_DEBUG, "gdisplay: found %i segments, now encoding ...\n", num_segments);
			ok = qrcodegen_encodeSegmentsAdvanced((const struct qrcodegen_Segment *)&text_segments, num_segments, errCorLvl,
				qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true, tempBuffer, qrcode);
		}

		for (int i=0; i<num_segments; i++) {
			free(text_segments[i].data);
		}
	}
	else {
		ok = qrcodegen_encodeText(buf, tempBuffer, qrcode, errCorLvl,
			qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
	}

	if (ok) {
		int foreground = lgdisplay_get_color(L, n + 1, 0, GDISPLAY_CURR_FOREGROUND, 0);
		int background = lgdisplay_get_color(L, n + 2, 0, GDISPLAY_CURR_BACKGROUND, 0);

		int size = qrcodegen_getSize(qrcode);
		syslog(LOG_DEBUG, "gdisplay: qrcode size is %i\n", size);
		if (multi>1) {
			syslog(LOG_DEBUG, "gdisplay: qrcode height/width is %i\n", size*multi);
		}
		int border = 0;
		error = gdisplay_lock();
		if (error) {
			free(qrcode);
			free(tempBuffer);
			return luaL_driver_error(L, error);
		}
		for (int y = -border; y < size + border; y++) {
			for (int x = -border; x < size + border; x++) {
				if (multi<2) {
					error = gdisplay_set_pixel(sx+x, sy+y, qrcodegen_getModule(qrcode, x, y) ? foreground:background);
					if (error) {
						free(qrcode);
						free(tempBuffer);
						return luaL_driver_error(L, error);
					}
				}
				else {
					//multi-size qr-code
					for(int mx = 0; mx < multi; mx++) {
						for(int my = 0; my < multi; my++) {
							error = gdisplay_set_pixel(sx+(x*multi)+mx, sy+(y*multi)+my, qrcodegen_getModule(qrcode, x, y) ? foreground:background);
							if (error) {
								free(qrcode);
								free(tempBuffer);
								return luaL_driver_error(L, error);
							}
						}
					}
				}
			}
		}
		error = gdisplay_unlock();
		if (error) {
			free(qrcode);
			free(tempBuffer);
			return luaL_driver_error(L, error);
		}
	}
	else {
		free(qrcode);
		free(tempBuffer);
		return luaL_driver_error(L, driver_error(GDISPLAY_DRIVER, GDISPLAY_ERR_QR_ENCODING_ERROR, NULL));
	}

	free(qrcode);
	free(tempBuffer);
	return 0;
}

//--------------------------------------------
static int lgdisplay_set_angleOffset(lua_State *L) {
	float angle = luaL_checknumber(L, 1);

	gdisplay_set_angle_offset(angle);

	return 0;
}

static int lgdisplay_get_angleOffset(lua_State *L) {
	lua_pushnumber(L, gdisplay_get_angle_offset());

	return 1;
}

//=====================================
static int lgdisplay_read_touch(lua_State *L) {
	driver_error_t *error;

	int x, y, z;

	error = gdisplay_touch_get(&x, &y, &z);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger(L, z);
	lua_pushinteger(L, x);
	lua_pushinteger(L, y);

	return 3;
}

static int lgdisplay_read_raw_touch(lua_State *L) {
	driver_error_t *error;

	int x, y, z;

	error = gdisplay_touch_get_raw(&x, &y, &z);
	if (error) {
		return luaL_driver_error(L, error);
	}

	lua_pushinteger(L, z);
	lua_pushinteger(L, x);
	lua_pushinteger(L, y);

	return 3;
}

//==================================
static int lgdisplay_touch_set_cal(lua_State *L) {
	driver_error_t *error;

	int x = luaL_checkinteger(L, 1);
	int y = luaL_checkinteger(L, 2);

	error = gdisplay_touch_set_cal(x, y);
	if (error) {
		return luaL_driver_error(L, error);
	}

	return 0;
}

static int lgdisplay_init( lua_State* L ) {
	driver_error_t *error;
	uint8_t buffered = 0;
	uint8_t address = 0;

	uint8_t chipset = luaL_checkinteger( L, 1);
	uint8_t orient = luaL_optinteger( L, 2, LANDSCAPE);

	const gdisplay_t *display = gdisplay_get(chipset);

	if (lua_gettop(L) >= 3) {
		luaL_checktype(L, 3, LUA_TBOOLEAN);
		if (lua_toboolean(L, 3)) {
			buffered = 1;
		}
	}

	if (
			(display->chipset == CHIPSET_SSD1306_128_32) ||
			(display->chipset == CHIPSET_SSD1306_128_64) ||
			(display->chipset == CHIPSET_SSD1306_96_16)
	) {
		if (lua_gettop(L) == 4) {
			address = luaL_checkinteger(L, 4);
		}
	}

	error = gdisplay_init(chipset, orient, buffered, address);
	if (error) {
		return luaL_driver_error(L, error);
	}

	// Set the default stroke, foreground and background colors
	static_stroke = GDISPLAY_WHITE;
	static_foreground = GDISPLAY_WHITE;;
	static_background = GDISPLAY_BLACK;

	return 0;
}

// =============================================================================

#include "modules.h"

static const LUA_REG_TYPE gdisplay_map[] = {
	{ LSTRKEY( "init" ),           LFUNCVAL( lgdisplay_init ) },
	{ LSTRKEY( "attach" ),         LFUNCVAL( lgdisplay_init ) },
	{ LSTRKEY( "setfont" ),        LFUNCVAL( lgdisplay_setfont )},
	{ LSTRKEY( "lock" ),           LFUNCVAL( lgdisplay_lock)},
	{ LSTRKEY( "unlock" ),         LFUNCVAL( lgdisplay_unlock)},
	{ LSTRKEY( "rgb" ),            LFUNCVAL( lgdisplay_rgb_to_color )},
	{ LSTRKEY( "clear" ),          LFUNCVAL( lgdisplay_clear )},
	{ LSTRKEY( "setforeground" ),  LFUNCVAL( lgdisplay_foreground )},
	{ LSTRKEY( "setbackground" ),  LFUNCVAL( lgdisplay_background )},
	{ LSTRKEY( "setstroke" ),      LFUNCVAL( lgdisplay_stroke )},
	{ LSTRKEY( "on" ),             LFUNCVAL( lgdisplay_on )},
	{ LSTRKEY( "off" ),            LFUNCVAL( lgdisplay_off )},
#if 0
	{ LSTRKEY( "compilefont" ),    LFUNCVAL( compile_font_file )},
#endif
	{ LSTRKEY( "getscreensize" ),  LFUNCVAL( lgdisplay_getscreensize )},
	{ LSTRKEY( "getfontsize" ),    LFUNCVAL( lgdisplay_getfontsize )},
	{ LSTRKEY( "getfontheight" ),  LFUNCVAL( lgdisplay_getfontheight )},
	{ LSTRKEY( "getfontwidtht" ),  LFUNCVAL( lgdisplay_getfontwidtht )},
	{ LSTRKEY( "gettype" ),        LFUNCVAL( lgdisplay_gettype )},
	{ LSTRKEY( "setrot" ),         LFUNCVAL( lgdisplay_setrot )},
	{ LSTRKEY( "setorient" ),      LFUNCVAL( lgdisplay_setorient )},
	{ LSTRKEY( "setcolor" ),       LFUNCVAL( lgdisplay_setcolor )},
	{ LSTRKEY( "settransp" ),      LFUNCVAL( lgdisplay_settransp )},
	{ LSTRKEY( "setfixed" ),       LFUNCVAL( lgdisplay_setfixed )},
	{ LSTRKEY( "setwrap" ),        LFUNCVAL( lgdisplay_setwrap )},
	{ LSTRKEY( "setangleoffset" ), LFUNCVAL( lgdisplay_set_angleOffset )},
	{ LSTRKEY( "getangleoffset" ), LFUNCVAL( lgdisplay_get_angleOffset )},
	{ LSTRKEY( "setclipwin" ),     LFUNCVAL( lgdisplay_setclipwin )},
	{ LSTRKEY( "resetclipwin" ),   LFUNCVAL( lgdisplay_resetclipwin )},
	{ LSTRKEY( "getclipwin" ),     LFUNCVAL( lgdisplay_getclipwin )},
	{ LSTRKEY( "invert" ),         LFUNCVAL( lgdisplay_invert )},
	{ LSTRKEY( "putpixel" ),       LFUNCVAL( lgdisplay_putpixel )},
	{ LSTRKEY( "setpixel" ),       LFUNCVAL( lgdisplay_putpixel )},
	{ LSTRKEY( "getpixel" ),       LFUNCVAL( lgdisplay_getpixel )},
	{ LSTRKEY( "getline" ),        LFUNCVAL( lgdisplay_getwindow )},
	{ LSTRKEY( "line" ),           LFUNCVAL( lgdisplay_drawline )},
	{ LSTRKEY( "rect" ),           LFUNCVAL( lgdisplay_rect )},
	{ LSTRKEY( "linebyangle" ),    LFUNCVAL( lgdisplay_drawlineByAngle )},
	{ LSTRKEY( "roundrect" ),      LFUNCVAL( lgdisplay_roundrect )},
	{ LSTRKEY( "circle" ),         LFUNCVAL( lgdisplay_circle )},
	{ LSTRKEY( "ellipse" ),        LFUNCVAL( lgdisplay_ellipse )},
	{ LSTRKEY( "arc" ),            LFUNCVAL( lgdisplay_arc )},
	{ LSTRKEY( "poly" ),           LFUNCVAL( lgdisplay_poly )},
	{ LSTRKEY( "star" ),           LFUNCVAL( lgdisplay_star )},
	{ LSTRKEY( "triangle" ),       LFUNCVAL( lgdisplay_triangle )},
	{ LSTRKEY( "write" ),          LFUNCVAL( lgdisplay_write )},
	{ LSTRKEY( "stringpos" ),      LFUNCVAL( lgdisplay_writepos )},
	{ LSTRKEY( "image" ),          LFUNCVAL( lgdisplay_image )},
	{ LSTRKEY( "qrcode" ),         LFUNCVAL( lgdisplay_qrcode )},
	{ LSTRKEY( "hsb2rgb" ),        LFUNCVAL( lgdisplay_HSBtoRGB )},
	{ LSTRKEY( "setbrightness" ),  LFUNCVAL( lgdisplay_set_brightness )},
	{ LSTRKEY( "gettouch" ),       LFUNCVAL( lgdisplay_read_touch )},
	{ LSTRKEY( "getrawtouch" ),    LFUNCVAL( lgdisplay_read_raw_touch )},
	{ LSTRKEY( "setcal" ),         LFUNCVAL( lgdisplay_touch_set_cal )},
	// Constant definitions
	{ LSTRKEY( "PORTRAIT" ),       LINTVAL( PORTRAIT ) },
	{ LSTRKEY( "PORTRAIT_FLIP" ),  LINTVAL( PORTRAIT_FLIP ) },
	{ LSTRKEY( "LANDSCAPE" ),      LINTVAL( LANDSCAPE ) },
	{ LSTRKEY( "LANDSCAPE_FLIP" ), LINTVAL( LANDSCAPE_FLIP ) },
	{ LSTRKEY( "CENTER" ),         LINTVAL( CENTER ) },
	{ LSTRKEY( "RIGHT" ),          LINTVAL( RIGHT ) },
	{ LSTRKEY( "BOTTOM" ),         LINTVAL( BOTTOM ) },
	{ LSTRKEY( "LASTX" ),          LINTVAL( LASTX ) },
	{ LSTRKEY( "LASTY" ),          LINTVAL( LASTY ) },
	{ LSTRKEY( "BLACK" ),          LINTVAL( GDISPLAY_BLACK ) },
	{ LSTRKEY( "NAVY" ),           LINTVAL( GDISPLAY_NAVY ) },
	{ LSTRKEY( "DARKGREEN" ),      LINTVAL( GDISPLAY_DARKGREEN ) },
	{ LSTRKEY( "DARKCYAN" ),       LINTVAL( GDISPLAY_DARKCYAN ) },
	{ LSTRKEY( "MAROON" ),         LINTVAL( GDISPLAY_MAROON ) },
	{ LSTRKEY( "PURPLE" ),         LINTVAL( GDISPLAY_PURPLE ) },
	{ LSTRKEY( "OLIVE" ),          LINTVAL( GDISPLAY_OLIVE ) },
	{ LSTRKEY( "LIGHTGREY" ),      LINTVAL( GDISPLAY_LIGHTGREY ) },
	{ LSTRKEY( "DARKGREY" ),       LINTVAL( GDISPLAY_DARKGREY ) },
	{ LSTRKEY( "BLUE" ),           LINTVAL( GDISPLAY_BLUE ) },
	{ LSTRKEY( "GREEN" ),          LINTVAL( GDISPLAY_GREEN ) },
	{ LSTRKEY( "CYAN" ),           LINTVAL( GDISPLAY_CYAN ) },
	{ LSTRKEY( "RED" ),            LINTVAL( GDISPLAY_RED ) },
	{ LSTRKEY( "MAGENTA" ),        LINTVAL( GDISPLAY_MAGENTA ) },
	{ LSTRKEY( "YELLOW" ),         LINTVAL( GDISPLAY_YELLOW ) },
	{ LSTRKEY( "WHITE" ),          LINTVAL( GDISPLAY_WHITE ) },
	{ LSTRKEY( "ORANGE" ),         LINTVAL( GDISPLAY_ORANGE ) },
	{ LSTRKEY( "GREENYELLOW" ),    LINTVAL( GDISPLAY_GREENYELLOW ) },
	{ LSTRKEY( "PINK" ),           LINTVAL( GDISPLAY_PINK ) },
	{ LSTRKEY( "FONT_DEFAULT" ),   LINTVAL( DEFAULT_FONT ) },
	{ LSTRKEY( "FONT_DEJAVU18" ),  LINTVAL( DEJAVU18_FONT ) },
	{ LSTRKEY( "FONT_DEJAVU24" ),  LINTVAL( DEJAVU24_FONT ) },
	{ LSTRKEY( "FONT_UBUNTU16" ),  LINTVAL( UBUNTU16_FONT ) },
	{ LSTRKEY( "FONT_COMIC24" ),   LINTVAL( COMIC24_FONT ) },
	{ LSTRKEY( "FONT_TOONEY32" ),  LINTVAL( TOONEY32_FONT ) },
	{ LSTRKEY( "FONT_MINYA24" ),   LINTVAL( MINYA24_FONT ) },
	{ LSTRKEY( "FONT_7SEG" ),      LINTVAL( FONT_7SEG ) },
	{ LSTRKEY( "FONT_LCD" ),       LINTVAL( LCD_FONT ) },

	{ LSTRKEY( "ST7735" ),         LINTVAL( CHIPSET_ST7735 ) },
	{ LSTRKEY( "ST7735B" ),        LINTVAL( CHIPSET_ST7735B ) },
	{ LSTRKEY( "ST7735G" ),        LINTVAL( CHIPSET_ST7735G ) },
	{ LSTRKEY( "ST7735_18" ),      LINTVAL( CHIPSET_ST7735_18 ) },
	{ LSTRKEY( "ST7735B_18" ),     LINTVAL( CHIPSET_ST7735B_18 ) },
	{ LSTRKEY( "ST7735G_18" ),     LINTVAL( CHIPSET_ST7735G_18 ) },
	{ LSTRKEY( "ST7735G_144" ),    LINTVAL( CHIPSET_ST7735G_144) },
	{ LSTRKEY( "ST7735_096" ),     LINTVAL( CHIPSET_ST7735_096 ) },

	{ LSTRKEY( "ILI9341" ),        LINTVAL( CHIPSET_ILI9341 ) },
	{ LSTRKEY( "ILI9341_BGR" ),    LINTVAL( CHIPSET_ILI9341_BGR ) },
	{ LSTRKEY( "PCD8544" ),        LINTVAL( CHIPSET_PCD8544 ) },

	{ LSTRKEY( "SSD1306_128_32" ), LINTVAL( CHIPSET_SSD1306_128_32 ) },
	{ LSTRKEY( "SSD1306_128_64" ), LINTVAL( CHIPSET_SSD1306_128_64 ) },
	{ LSTRKEY( "SSD1306_96_16" ),  LINTVAL( CHIPSET_SSD1306_96_16 ) },

	{ LSTRKEY( "ECC_LOW" ),        LINTVAL( qrcodegen_Ecc_LOW ) },
	{ LSTRKEY( "ECC_MEDIUM" ),     LINTVAL( qrcodegen_Ecc_MEDIUM ) },
	{ LSTRKEY( "ECC_QUARTILE" ),   LINTVAL( qrcodegen_Ecc_QUARTILE ) },
	{ LSTRKEY( "ECC_HIGH" ),       LINTVAL( qrcodegen_Ecc_HIGH ) },

	{ LSTRKEY( "MODE_NUMERIC" ),   LINTVAL( qrcodegen_Mode_NUMERIC ) },
	{ LSTRKEY( "MODE_ALPHANUM" ),  LINTVAL( qrcodegen_Mode_ALPHANUMERIC ) },
	{ LSTRKEY( "MODE_BYTE" ),      LINTVAL( qrcodegen_Mode_BYTE ) },
	{ LSTRKEY( "MODE_ECI" ),       LINTVAL( qrcodegen_Mode_ECI ) },

	DRIVER_REGISTER_LUA_ERRORS(gdisplay)

	{ LNILKEY, LNILVAL }
};

int luaopen_gdisplay(lua_State* L) {
	return 0;
}

MODULE_REGISTER_ROM(GDISPLAY, gdisplay, gdisplay_map, luaopen_gdisplay, 1);

#endif
