/*
** $Id: loslib.c,v 1.65 2016/07/18 17:58:58 roberto Exp $
** Standard Operating System library
** See Copyright Notice in lua.h
*/

#define loslib_c
#define LUA_LIB

#include "lprefix.h"


#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

/*
** {==================================================================
** List of valid conversion specifiers for the 'strftime' function;
** options are grouped by length; group of length 2 start with '||'.
** ===================================================================
*/
#if !defined(LUA_STRFTIMEOPTIONS)	/* { */

/* options for ANSI C 89 (only 1-char options) */
#define L_STRFTIMEC89		"aAbBcdHIjmMpSUwWxXyYZ%"

/* options for ISO C 99 and POSIX */
#define L_STRFTIMEC99 "aAbBcCdDeFgGhHIjmMnprRStTuUVwWxXyYzZ%" \
    "||" "EcECExEXEyEY" "OdOeOHOIOmOMOSOuOUOVOwOWOy"  /* two-char options */

/* options for Windows */
#define L_STRFTIMEWIN "aAbBcdHIjmMpSUwWxXyYzZ%" \
    "||" "#c#x#d#H#I#j#m#M#S#U#w#W#y#Y"  /* two-char options */

#if defined(LUA_USE_WINDOWS)
#define LUA_STRFTIMEOPTIONS	L_STRFTIMEWIN
#elif defined(LUA_USE_C89)
#define LUA_STRFTIMEOPTIONS	L_STRFTIMEC89
#else  /* C99 specification */
#define LUA_STRFTIMEOPTIONS	L_STRFTIMEC99
#endif

#endif					/* } */
/* }================================================================== */


/*
** {==================================================================
** Configuration for time-related stuff
** ===================================================================
*/

#if !defined(l_time_t)		/* { */
/*
** type to represent time_t in Lua
*/
#define l_timet			lua_Integer
#define l_pushtime(L,t)		lua_pushinteger(L,(lua_Integer)(t))

static time_t l_checktime (lua_State *L, int arg) {
  lua_Integer t = luaL_checkinteger(L, arg);
  luaL_argcheck(L, (time_t)t == t, arg, "time out-of-bounds");
  return (time_t)t;
}

#endif				/* } */


#if !defined(l_gmtime)		/* { */
/*
** By default, Lua uses gmtime/localtime, except when POSIX is available,
** where it uses gmtime_r/localtime_r
*/

#if defined(LUA_USE_POSIX)	/* { */

#define l_gmtime(t,r)		gmtime_r(t,r)
#define l_localtime(t,r)	localtime_r(t,r)

#else				/* }{ */

/* ISO C definitions */
#define l_gmtime(t,r)		((void)(r)->tm_sec, gmtime(t))
#define l_localtime(t,r)  	((void)(r)->tm_sec, localtime(t))

#endif				/* } */

#endif				/* } */

/* }================================================================== */


/*
** {==================================================================
** Configuration for 'tmpnam':
** By default, Lua uses tmpnam except when POSIX is available, where
** it uses mkstemp.
** ===================================================================
*/
#if !defined(lua_tmpnam)	/* { */

#if defined(LUA_USE_POSIX)	/* { */

#include <unistd.h>

#define LUA_TMPNAMBUFSIZE	32

#if !defined(LUA_TMPNAMTEMPLATE)
#define LUA_TMPNAMTEMPLATE	"/tmp/lua_XXXXXX"
#endif

#define lua_tmpnam(b,e) { \
        strcpy(b, LUA_TMPNAMTEMPLATE); \
        e = mkstemp(b); \
        if (e != -1) close(e); \
        e = (e == -1); }

#else				/* }{ */

/* ISO C definitions */
#define LUA_TMPNAMBUFSIZE	L_tmpnam
#define lua_tmpnam(b,e)		{ e = (tmpnam(b) == NULL); }

#endif				/* } */

#endif				/* } */
/* }================================================================== */




static int os_execute (lua_State *L) {
// LUA RTOS BEGIN
#if 0
  const char *cmd = luaL_optstring(L, 1, NULL);
  int stat = system(cmd);
  if (cmd != NULL)
    return luaL_execresult(L, stat);
  else {
    lua_pushboolean(L, stat);  /* true if there is a shell */
    return 1;
  }
#else
  return 0;
#endif
// LUA RTOS END
}


static int os_remove (lua_State *L) {
  // LUA RTOS BEGIN
#if 0
  const char *filename = luaL_checkstring(L, 1);
  return luaL_fileresult(L, remove(filename) == 0, filename);
#else
  struct stat statbuf;
  char *filename = (char*) luaL_checkstring(L, 1);

  if (stat(filename, &statbuf) == 0) {
		if (S_ISDIR(statbuf.st_mode)) {
			return luaL_fileresult(L, rmdir(filename) == 0, filename);
		} else {
			return luaL_fileresult(L, remove(filename) == 0, filename);
		}
	}

	const char* result = strstr(filename, "*");
	if (NULL != result) {
		const char *path = filename;
		char cpath[PATH_MAX];
		DIR *dir = NULL;
		struct dirent *ent;
		int found = 0;
		int rc = 0;

		//search back to the last dir name
		filename = (char*)path + strlen(path) - 1;
		while (filename > path && *filename!=0 && *filename!='/') {
			filename--;
		}

		//string given is not a valid path
		//so try to find a matching file
		if (*filename == '/') {
			*filename = 0; //will cut off the filename from the path
			filename++;
			if(strlen(path)==0) {
				path = "/";
			}
		}
		//try to find a matching file
		//in the current folder
		if (filename==path) {
			filename = (char*)path;
			if (!getcwd(cpath, PATH_MAX)) {
				return luaL_fileresult(L, 0, filename);
			}
			path = cpath;
		}

		// Open directory
		if (!(dir = opendir(path))) {
			return luaL_fileresult(L, 0, path);
		}
		// Read entries
		while ((ent = readdir(dir)) != NULL) {
			if (0==fnmatch(filename, ent->d_name, 0)) { //our implementation above does support only a subset
				found++;
				if (stat(ent->d_name, &statbuf) == 0) {
					rc = (S_ISDIR(statbuf.st_mode) ? rmdir(ent->d_name) : remove(ent->d_name));
					if ( 0 != rc ) {
						closedir(dir);
						return luaL_fileresult(L, rc, ent->d_name);
					}
				}
			}
		}
		closedir(dir);

		if (found>0)
			return 0;
	}

  return luaL_fileresult(L, 0, filename);
#endif
  // LUA RTOS END
}


static int os_rename (lua_State *L) {
  const char *fromname = luaL_checkstring(L, 1);
  const char *toname = luaL_checkstring(L, 2);
  return luaL_fileresult(L, rename(fromname, toname) == 0, NULL);
}


static int os_tmpname (lua_State *L) {
  char buff[LUA_TMPNAMBUFSIZE];
  int err;
  lua_tmpnam(buff, err);
  if (err)
    return luaL_error(L, "unable to generate a unique filename");
  lua_pushstring(L, buff);
  return 1;
}


static int os_getenv (lua_State *L) {
  const char *env;

  // LUA RTOS BEGIN
  env = luaL_checkstring(L, 1);
  if (!strcmp(env, "PATH")) {
	  lua_pushstring(L, "/");
  } else {
	  lua_pushnil(L);
  }

  //lua_pushstring(L, getenv(luaL_checkstring(L, 1)));  /* if NULL push nil */
  // LUA RTOS END
  return 1;
}


static int os_clock (lua_State *L) {
  lua_pushnumber(L, ((lua_Number)clock())/(lua_Number)CLOCKS_PER_SEC);
  return 1;
}


/*
** {======================================================
** Time/Date operations
** { year=%Y, month=%m, day=%d, hour=%H, min=%M, sec=%S,
**   wday=%w+1, yday=%j, isdst=? }
** =======================================================
*/

static void setfield (lua_State *L, const char *key, int value) {
  lua_pushinteger(L, value);
  lua_setfield(L, -2, key);
}

static void setboolfield (lua_State *L, const char *key, int value) {
  if (value < 0)  /* undefined? */
    return;  /* does not set field */
  lua_pushboolean(L, value);
  lua_setfield(L, -2, key);
}


/*
** Set all fields from structure 'tm' in the table on top of the stack
*/
static void setallfields (lua_State *L, struct tm *stm) {
  setfield(L, "sec", stm->tm_sec);
  setfield(L, "min", stm->tm_min);
  setfield(L, "hour", stm->tm_hour);
  setfield(L, "day", stm->tm_mday);
  setfield(L, "month", stm->tm_mon + 1);
  setfield(L, "year", stm->tm_year + 1900);
  setfield(L, "wday", stm->tm_wday + 1);
  setfield(L, "yday", stm->tm_yday + 1);
  setboolfield(L, "isdst", stm->tm_isdst);
}


static int getboolfield (lua_State *L, const char *key) {
  int res;
  res = (lua_getfield(L, -1, key) == LUA_TNIL) ? -1 : lua_toboolean(L, -1);
  lua_pop(L, 1);
  return res;
}


/* maximum value for date fields (to avoid arithmetic overflows with 'int') */
#if !defined(L_MAXDATEFIELD)
#define L_MAXDATEFIELD	(INT_MAX / 2)
#endif

static int getfield (lua_State *L, const char *key, int d, int delta) {
  int isnum;
  int t = lua_getfield(L, -1, key);  /* get field and its type */
  lua_Integer res = lua_tointegerx(L, -1, &isnum);
  if (!isnum) {  /* field is not an integer? */
    if (t != LUA_TNIL)  /* some other value? */
      return luaL_error(L, "field '%s' is not an integer", key);
    else if (d < 0)  /* absent field; no default? */
      return luaL_error(L, "field '%s' missing in date table", key);
    res = d;
  }
  else {
    if (!(-L_MAXDATEFIELD <= res && res <= L_MAXDATEFIELD))
      return luaL_error(L, "field '%s' is out-of-bound", key);
    res -= delta;
  }
  lua_pop(L, 1);
  return (int)res;
}


static const char *checkoption (lua_State *L, const char *conv,
                                ptrdiff_t convlen, char *buff) {
  const char *option = LUA_STRFTIMEOPTIONS;
  int oplen = 1;  /* length of options being checked */
  for (; *option != '\0' && oplen <= convlen; option += oplen) {
    if (*option == '|')  /* next block? */
      oplen++;  /* will check options with next length (+1) */
    else if (memcmp(conv, option, oplen) == 0) {  /* match? */
      memcpy(buff, conv, oplen);  /* copy valid option to buffer */
      buff[oplen] = '\0';
      return conv + oplen;  /* return next item */
    }
  }
  luaL_argerror(L, 1,
    lua_pushfstring(L, "invalid conversion specifier '%%%s'", conv));
  return conv;  /* to avoid warnings */
}


/* maximum size for an individual 'strftime' item */
#define SIZETIMEFMT	250


static int os_date (lua_State *L) {
  size_t slen;
  const char *s = luaL_optlstring(L, 1, "%c", &slen);
  time_t t = luaL_opt(L, l_checktime, 2, time(NULL));
  const char *se = s + slen;  /* 's' end */
  struct tm tmr, *stm;
  if (*s == '!') {  /* UTC? */
    stm = l_gmtime(&t, &tmr);
    s++;  /* skip '!' */
  }
  else
    stm = l_localtime(&t, &tmr);
  if (stm == NULL)  /* invalid date? */
    luaL_error(L, "time result cannot be represented in this installation");
  if (strcmp(s, "*t") == 0) {
    lua_createtable(L, 0, 9);  /* 9 = number of fields */
    setallfields(L, stm);
  }
  else {
    char cc[4];  /* buffer for individual conversion specifiers */
    luaL_Buffer b;
    cc[0] = '%';
    luaL_buffinit(L, &b);
    while (s < se) {
      if (*s != '%')  /* not a conversion specifier? */
        luaL_addchar(&b, *s++);
      else {
        size_t reslen;
        char *buff = luaL_prepbuffsize(&b, SIZETIMEFMT);
        s++;  /* skip '%' */
        s = checkoption(L, s, se - s, cc + 1);  /* copy specifier to 'cc' */
        reslen = strftime(buff, SIZETIMEFMT, cc, stm);
        luaL_addsize(&b, reslen);
      }
    }
    luaL_pushresult(&b);
  }
  return 1;
}


static int os_time (lua_State *L) {
  time_t t;
  if (lua_isnoneornil(L, 1))  /* called without args? */
    t = time(NULL);  /* get current time */
  else {
    struct tm ts;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);  /* make sure table is at the top */
    ts.tm_sec = getfield(L, "sec", 0, 0);
    ts.tm_min = getfield(L, "min", 0, 0);
    ts.tm_hour = getfield(L, "hour", 12, 0);
    ts.tm_mday = getfield(L, "day", -1, 0);
    ts.tm_mon = getfield(L, "month", -1, 1);
    ts.tm_year = getfield(L, "year", -1, 1900);
    ts.tm_isdst = getboolfield(L, "isdst");
    t = mktime(&ts);
    setallfields(L, &ts);  /* update fields with normalized values */
  }
  if (t != (time_t)(l_timet)t || t == (time_t)(-1))
    luaL_error(L, "time result cannot be represented in this installation");
  l_pushtime(L, t);
  return 1;
}


static int os_difftime (lua_State *L) {
  time_t t1 = l_checktime(L, 1);
  time_t t2 = l_checktime(L, 2);
  lua_pushnumber(L, (lua_Number)difftime(t1, t2));
  return 1;
}

/* }====================================================== */


static int os_setlocale (lua_State *L) {
  static const int cat[] = {LC_ALL, LC_COLLATE, LC_CTYPE, LC_MONETARY,
                      LC_NUMERIC, LC_TIME};
  static const char *const catnames[] = {"all", "collate", "ctype", "monetary",
     "numeric", "time", NULL};
  const char *l = luaL_optstring(L, 1, NULL);
  int op = luaL_checkoption(L, 2, "all", catnames);
  lua_pushstring(L, setlocale(cat[op], l));
  return 1;
}

#if __XTENSA__
extern __NOINIT_ATTR uint32_t backtrace_count;
#endif
static int os_exit (lua_State *L) {
  int status;
  if (lua_isboolean(L, 1))
    status = (lua_toboolean(L, 1) ? EXIT_SUCCESS : EXIT_FAILURE);
  else
    status = (int)luaL_optinteger(L, 1, EXIT_SUCCESS);
  if (lua_toboolean(L, 2))
    lua_close(L);
#if __XTENSA__
  if (EXIT_SUCCESS==status) {
      backtrace_count = 0;
      esp_restart(); /* restart without panic'ing */
  }
#endif
  if (L) exit(status);  /* 'if' to avoid warnings for unreachable 'return' */
  return 0;
}


#include "modules.h"

static const LUA_REG_TYPE syslib[] =
{
  { LSTRKEY( "date" ),        LFUNCVAL( os_date ) },
  { LSTRKEY( "difftime" ),    LFUNCVAL( os_difftime ) },
  { LSTRKEY( "clock" ),       LFUNCVAL( os_clock ) },
  { LSTRKEY( "remove" ),      LFUNCVAL( os_remove ) },
  { LSTRKEY( "rename" ),      LFUNCVAL( os_rename ) },
  { LSTRKEY( "time" ),        LFUNCVAL( os_time ) },
  { LSTRKEY( "settime" ),     LFUNCVAL( os_settime ) },
  { LSTRKEY( "tmpname" ),     LFUNCVAL( os_tmpname ) },
  { LSTRKEY( "exit" ),        LFUNCVAL( os_exit ) },
  { LSTRKEY( "execute" ),     LFUNCVAL( os_execute ) },
  { LSTRKEY( "setlocale" ),   LFUNCVAL( os_setlocale ) },
  { LSTRKEY( "getenv" ),      LFUNCVAL( os_getenv ) },
  { LSTRKEY( "factoryreset"), LFUNCVAL( os_factory_reset ) },
  { LSTRKEY( "partitions"),   LFUNCVAL( os_partitions ) },
  { LSTRKEY( "passwd"),       LFUNCVAL( os_passwd ) },
  { LSTRKEY( "uptime"),       LFUNCVAL( os_uptime ) },
#if CONFIG_LUA_RTOS_USE_HARDWARE_LOCKS
  { LSTRKEY( "locks" ),       LFUNCVAL( os_locks ) },
#endif
  { LSTRKEY( "exists" ),      LFUNCVAL( os_exists ) },
  { LSTRKEY( "stdout" ),      LFUNCVAL( os_stdout ) },
  { LSTRKEY( "clear" ),       LFUNCVAL( os_clear ) },
  { LSTRKEY( "cpu" ),         LFUNCVAL( os_cpu ) }, //deprecated
  { LSTRKEY( "board" ),       LFUNCVAL( os_board ) }, //deprecated
  { LSTRKEY( "sleep" ),       LFUNCVAL( os_sleep ) }, //deprecated
  { LSTRKEY( "version" ),     LFUNCVAL( os_version ) },
  { LSTRKEY( "ls" ),          LFUNCVAL( os_ls ) },
  { LSTRKEY( "cd" ),          LFUNCVAL( os_cd ) },
  { LSTRKEY( "pwd" ),         LFUNCVAL( os_pwd ) },
  { LSTRKEY( "mkdir" ),       LFUNCVAL( os_mkdir ) },
  { LSTRKEY( "logcons" ),     LFUNCVAL( os_logcons ) },
  { LSTRKEY( "loglevel" ),    LFUNCVAL( os_loglevel ) },
  { LSTRKEY( "syslog" ),      LFUNCVAL( os_syslog ) },
#if CONFIG_LUA_RTOS_USE_RSYSLOG
  { LSTRKEY( "rsyslog" ),     LFUNCVAL( os_setrsyslog ) },
#endif
#if CONFIG_LUA_RTOS_LUA_USE_MQTT
  { LSTRKEY( "msyslog" ),     LFUNCVAL( os_setmsyslog ) },
#endif
  { LSTRKEY( "stats" ),       LFUNCVAL( os_stats ) },
  { LSTRKEY( "format" ),      LFUNCVAL( os_format ) },
  { LSTRKEY( "history" ),     LFUNCVAL( os_history ) },
  { LSTRKEY( "shell" ),       LFUNCVAL( os_shell ) },
  { LSTRKEY( "cp" ),          LFUNCVAL( os_cp ) },
  { LSTRKEY( "cat" ),         LFUNCVAL( os_cat ) },
  { LSTRKEY( "more" ),        LFUNCVAL( os_more ) },
  { LSTRKEY( "dmesg" ),       LFUNCVAL( os_dmesg ) },
  { LSTRKEY( "run" ),         LFUNCVAL( os_run ) },
  { LSTRKEY( "luarunning" ),  LFUNCVAL( os_lua_running ) },
  { LSTRKEY( "luainterpreter" ), LFUNCVAL( os_lua_interpreter ) },
  { LSTRKEY( "resetreason" ), LFUNCVAL( os_reset_reason ) }, //deprecated
  { LSTRKEY( "bootcount" ),   LFUNCVAL( os_bootcount ) },
  { LSTRKEY( "flashEUI" ),    LFUNCVAL( os_flash_unique_id ) },
  { LSTRKEY( "edit" ),        LFUNCVAL( os_edit ) },
  { LSTRKEY( "df" ),          LFUNCVAL( os_df ) },

  { LSTRKEY( "LOG_INFO" ),    LINTVAL( LOG_INFO    ) },
  { LSTRKEY( "LOG_EMERG" ),   LINTVAL( LOG_EMERG   ) },
  { LSTRKEY( "LOG_ALERT" ),   LINTVAL( LOG_ALERT   ) },
  { LSTRKEY( "LOG_CRIT" ),    LINTVAL( LOG_CRIT    ) },
  { LSTRKEY( "LOG_ERR" ),     LINTVAL( LOG_ERR     ) },
  { LSTRKEY( "LOG_WARNING" ), LINTVAL( LOG_WARNING ) },
  { LSTRKEY( "LOG_NOTICE" ),  LINTVAL( LOG_NOTICE  ) },
  { LSTRKEY( "LOG_DEBUG" ),   LINTVAL( LOG_DEBUG   ) },
  { LSTRKEY( "LOG_ALL" ),     LINTVAL( 0b11111111  ) },

  { LNILKEY, LNILVAL }
};

/* }====================================================== */



LUAMOD_API int luaopen_os (lua_State *L) {
  return 0;
}

MODULE_REGISTER_ROM(OS, os, syslib, luaopen_os, 1);
