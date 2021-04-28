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
 * Lua RTOS, Lua os library additions
 *
 */

#include "lauxlib.h"
#include "luartos.h"
#include "error.h"
#include "linenoise.h"

#include <freertos/FreeRTOS.h>

#include "esp_partition.h"
#include "esp_ota_ops.h"

#include <limits.h>

#include <crypt.h>
#include <sys/dirent.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vfs/vfs.h>
#include <sys/driver.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/syslog.h>
#include <sys/status.h>
#include <sys/console.h>
#include <drivers/spi.h>
#include <drivers/i2c.h>
#include <drivers/cpu.h>
#include <sys/mount.h>

#include <drivers/uart.h>
#include <drivers/net.h>

extern uint32_t boot_count;
extern uint8_t flash_unique_id[8];
extern const driver_t drivers[];
extern void linenoiseHistoryClear();

char lua_syslog_level = 0xff;
FILE *lua_stdout_file = NULL;

void luaC_fullgc (lua_State *L, int isemergency) ;
int edit_main(int argc, char *argv[]);
extern int os_get_random(unsigned char *buf, size_t len);
int settimeofday(const struct timeval *, const struct timezone *);

static int os_stdout(lua_State *L) {
    int total = lua_gettop(L);
    const char *path = NULL;

    if (total == 1) {
        path = luaL_checkstring(L, 1);
    }

    if (path) {
        if (lua_stdout_file) {
            fclose(lua_stdout_file);
        }

        lua_stdout_file = fopen(path,"a+");
    } else {
        if (lua_stdout_file) {
            fclose(lua_stdout_file);
            lua_stdout_file = NULL;
        }
    }
    return 0;
}

static int os_shell(lua_State *L) { 
    if (lua_gettop(L) == 1) {
        luaL_checktype(L, 1, LUA_TBOOLEAN);

        if (lua_toboolean( L, 1 )) {
            status_set(STATUS_LUA_SHELL, 0x00000000);
        } else {
            status_set(0x00000000, STATUS_LUA_SHELL);
        }

        return 0;
    } else {
        lua_pushboolean(L, status_get(STATUS_LUA_SHELL));
        return 1;
    }
}

static int os_edit (lua_State *L) {
    const char *path = luaL_checkstring(L, 1);

    struct stat st;

    if (stat(path, &st) == 0) {
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            errno = EISDIR;
            return luaL_fileresult(L, 0, path);
        }
    }

    // Create file if does not exists
    bool bCreated = false;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        bCreated = true;
        fp = fopen(path, "a");
        if (!fp) {
            return luaL_fileresult(L, 0, path);
        }
    }
    fclose(fp);
  
    char* lua_argv[] = {(char *)"edit", (char *)path, NULL};
    if (1 == edit_main(2, lua_argv) && bCreated) {
        unlink(path);
    }
    console_clear();
    return 0;
}

static int os_sleep(lua_State *L) {
    unsigned int secs = luaL_checkinteger(L, 1);

    //luaL_deprecated(L, "os.sleep", "cpu.sleep");

    cpu_sleep(  secs  );
    return 0;
}

static int os_reset_reason(lua_State *L) {
    //luaL_deprecated(L, "os.resetreason", "cpu.resetreason");

    lua_pushinteger(L, cpu_reset_reason());
    return 1;
}

static int os_loglevel(lua_State *L) {
    if (lua_gettop(L) > 0) {
        int total = lua_gettop(L);
        int mask = 0;
        int flag = 0;
        int i;

        for(i=1;i<=total;i++) {
            flag = luaL_checkinteger(L, i);
            if (((flag < 0) || (flag > 7)) && (flag != 0xff)) {
                return luaL_error(L, "invalid flag");
            }

            if (flag == 0xff) {
                mask |= LOG_UPTO(LOG_DEBUG);
            } else {
                mask |= LOG_UPTO(flag);
            }
        }

        setlogmask(mask);

        return 0;
    } else {
        int mask = getlogmask();
        lua_pushinteger(L, mask);
        return 1;
    }
}

static int more(const char *path, int stop) {
    FILE *fp;
    int rows = 0;
    int cols = 0;
    int c;
    char ch;
    int lines;
    struct stat st;

    if (stat(path, &st) < 0) {
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            errno = EISDIR;
            return -1;
        }
    }

    fp = fopen(path,"r");
    if (!fp) {
        return -1;
    }

    if (stop) {
        console_size(&rows, &cols);
        console_clear();

        rows--;
        lines = 0;
    }

    while((c = fgetc(fp)) != EOF) {
        if ((c == '\n') && (stop)) {
            lines++;
            if (lines == rows) {
                console_statusline(path,": any key for next page, q for exit");
                ch = getchar();
                if ((ch == 'q') || (ch == 'Q')) {
                    console_clearstatusline();
                    break;
                }

                lines = 0;
                console_clear();
            }
        }

        printf("%c",c);
    }

    fclose(fp);

    return 0;
}

static int os_cat(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int res;

    if ((res = more(path, 0)) < 0) {
        return luaL_fileresult(L, 0, path);
    }

    return 0;
}

static int os_more(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    int res;

    if ((res = more(path, 1)) < 0) {
        return luaL_fileresult(L, 0, path);
    }

    return 0;
}

static int os_dmesg(lua_State *L) {
    int res;

    char fname[PATH_MAX + 1];

    if (!mount_messages_file(fname, sizeof(fname))) {
        return luaL_error(L, "logging disabled or not available");
    }

    if ((res = more(fname, 1)) < 0) {
        return luaL_fileresult(L, 0, fname);
    }

    return 0;
}

static int os_cd (lua_State *L) {
    const char *path = luaL_optstring(L, 1, "/");

    if (chdir(path) < 0) {
        return luaL_fileresult(L, 0, path);
    }

    return 0;
}

static int os_pwd (lua_State *L) {
    char path[PATH_MAX + 1];

    if (getcwd(path, PATH_MAX)) {
        lua_pushstring(L, path);
        return 1;
    } else {
        return luaL_fileresult(L, 0, path);
    }
}

static int os_mkdir (lua_State *L) {
    const char *path = luaL_optstring(L, 1, NULL);
    char cpath[PATH_MAX + 1];

    // If path is not present get the current directory as path
    if (!path) {
        if (!getcwd(cpath, PATH_MAX)) {
            return luaL_fileresult(L, 0, cpath);
        }

        path = cpath;
    }

    return luaL_fileresult(L, mkdir(path, 0) == 0, path);
}

inline int fnmatch(/*const*/ char *pattern, const char *string, int flags) {
    int i=0;
    int si=strlen(string)-1;
    int pi=strlen(pattern)-1;

    (void) flags;

    // the following does support just one asterisk
    while (string[i]!=0 && pattern[i]!=0 && pattern[i]!='*' && string[i]==pattern[i]) {
        i++;
    }
    if (pattern[i]=='*' || (string[i]==0 && pattern[i]==0))
    {
        if (pattern[i+1]!=0) {
            //need to check strlen(pattern)-i chars from the back...
            while (string[si]!=0 && pattern[pi]!=0 && string[si]==pattern[pi] && pi>i) {
                si--; pi--;
            }
            if (pi==i) {
                return 0;
            }
        }
        else {
            return 0;
        }
    }

    // the following does support just exactly two asterisk:
    // one at the very start plus one at the very end
    pi=strlen(pattern)-1;
    if (pattern[0]=='*' && pattern[pi]=='*') {
        // yes we could copy the pattern each time instead of this hack...
        pattern[pi]=0;
        pattern++;
        const char* result = strstr(string, pattern);
        pattern--;
        pattern[pi]='*';
        if (NULL!=result) {
            return 0;
        }
    }

    return 1;
}

static int os_ls (lua_State *L) {
    const char *path = luaL_optstring(L, 1, NULL);
    char *filename = NULL;
    DIR *dir = NULL;
    struct dirent *ent;
    char type;
    char size[9];
    char cpath[PATH_MAX];
    char tpath[PATH_MAX];
    char tbuffer[250];
    struct stat sb;
    struct tm *tm_info;

    // Check for a command like > ls foo*
    if (path) {
        dir = opendir(path);
        if (!dir) {
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
                path = NULL;
            }
        }
        else {
            closedir(dir);
        }
    }

    // If path is not present (at all or any more) get the current directory as path
    if (!path) {
        if (!getcwd(cpath, PATH_MAX)) {
            return luaL_fileresult(L, 0, cpath);
        }

        path = cpath;
    }

    // Open directory
    dir = opendir(path);
    if (!dir) {
        return luaL_fileresult(L, 0, path);
    }

    // Read entries
    while ((ent = readdir(dir)) != NULL) {
        strcpy(tpath, path);
        strcat(tpath,"/");
        strcat(tpath,ent->d_name);
        tbuffer[0] = '\0';

        if (stat(tpath, &sb) == 0) {
            if (sb.st_atime > 0) {
                tm_info = localtime(&sb.st_atime);
                strftime(tbuffer, 250, "%c", tm_info);
            } else {
                strcpy(tbuffer, "                        ");
            }
        } else {
            strcpy(tbuffer, "                        ");
        }

        type = 'd';
        if (ent->d_type == DT_REG) {
            type = 'f';
            snprintf(size, sizeof(size),"%8d", ent->d_fsize);
        } else {
            strcpy(size, "       -");
        }

        if (filename==NULL || 0==strcmp(filename, ent->d_name) || 0==fnmatch(filename, ent->d_name, 0)) { //our implementation above does support only a subset
            printf("%c\t%s\t%s\t%s\n", type, size, tbuffer, ent->d_name);
        }
    }

    closedir(dir);

    return 0;
}
 
static int os_clear (lua_State *L) {
    console_clear();

    return 0;
}

static int os_version(lua_State *L) {
    lua_pushstring(L, "Lua RTOS");
    lua_pushstring(L, LUA_OS_VER);
    lua_pushinteger(L, BUILD_TIME);
    lua_pushstring(L, BUILD_COMMIT);

    return 4;
}

static int os_cpu(lua_State *L) {
    int revision;

    char model[18];
    char cpuInfo[26];

    //luaL_deprecated(L, "os.cpu", "cpu.model");

    cpu_model(model, sizeof(model));
    revision = cpu_revision();
    if (revision) {
        snprintf(cpuInfo, sizeof(cpuInfo), "%s rev A%d", model, cpu_revision());
    } else {
        snprintf(cpuInfo, sizeof(cpuInfo), "%s", model);
    }

    lua_pushstring(L, cpuInfo);

    return 1;
}

static int os_board(lua_State *L) {
    lua_pushstring(L, CONFIG_LUA_RTOS_BOARD_TYPE);
    lua_pushstring(L, CONFIG_LUA_RTOS_BOARD_SUBTYPE);
    lua_pushstring(L, "");
    return 3;
}

static int os_logcons(lua_State *L) {
    if (lua_gettop(L) == 1) {
        int mask = LOG_NDELAY;

        luaL_checktype(L, 1, LUA_TBOOLEAN);
        int cons = lua_toboolean( L, 1 );

        if (cons) {
            mask = mask | LOG_CONS;
        }

        closelog();
        openlog(mask , LOG_LOCAL1);

        return 0;
    } else {
        int mask = getlogstat();
        lua_pushboolean(L, mask & LOG_CONS);
        return 1;
    }
}

static int os_syslog(lua_State *L) {
    const char *message = luaL_checkstring(L, 1);
    int level = luaL_optinteger( L, 2, LOG_ERR );
    syslog(level, message);

    return 0;
}

#if CONFIG_LUA_RTOS_USE_RSYSLOG
static int os_setrsyslog(lua_State *L) {
    if (lua_gettop(L) == 1) {
        const char *host = luaL_checkstring(L, 1);
        const char *ret = syslog_setloghost(host);
        if (ret == NULL) return 0;
        lua_pushstring(L, ret);
        return 1;
    }
    const char *ret = syslog_getloghost();
    if (ret == NULL) return 0;
    lua_pushstring(L, ret);
    return 1;
}
#endif

#if CONFIG_LUA_RTOS_LUA_USE_MQTT
static int os_setmsyslog(lua_State *L) {
    if (lua_gettop(L) > 0) {
        if (lua_type(L, 1) == LUA_TNIL) {
          syslog_setlogmqtt(0, NULL);
          return 0;
        }
        const char *topic = luaL_optstring(L, 2, NULL); //order matters
        luaL_checktype(L, 1, LUA_TUSERDATA);
        lua_pushvalue(L, 1);
        int mqtt_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        const int ret = syslog_setlogmqtt(mqtt_ref, topic);
        if (ret) {
          lua_rawgeti(L, LUA_REGISTRYINDEX, ret);
          return 1;
        }
        return 0;
    }
    const int ret = syslog_getlogmqtt();
    if (ret) {
      lua_rawgeti(L, LUA_REGISTRYINDEX, ret);
      return 1;
    }
    return 0;
}
#endif

static int os_stats(lua_State *L) {
    const char *stat = luaL_optstring(L, 1, NULL);

    // Do a garbage collection
    lua_lock(L);
    luaC_fullgc(L, 0);
    lua_unlock(L);

    if (stat && strcmp(stat,"mem") == 0) {
        lua_pushinteger(L, xPortGetFreeHeapSize());
        return 1;
    } else {
        printf("Free mem: %d\n",xPortGetFreeHeapSize());
        printf("Free mem min: %d\n",xPortGetMinimumEverFreeHeapSize());
    }

    return 0;
}

int os_format(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);

    const struct mount_pt *mountp;

    if ((mountp = mount_get_mount_point_for_path(path))) {
        if (mountp->format) {
            char response = ' ';

            // Confirmation
            while ((response != 'y') && (response != 'Y') && (response != 'n') && (response != 'N')) {
                printf("\r");
                console_erase_l();
                printf("All data in %s will be erased. Continue? [y/n]: ", path);
                response = fgetc(stdin);
            }

            printf("\n");

            if ((response == 'y') || (response == 'Y')) {
                printf("Formatting %s as %s file system ...\r\n", path, mountp->fs);

                mountp->format(path);
            } else {
                printf("Formatting canceled\r\n");
            }

            return 0;
        }
    }

    printf("%s can't be formatted\r\n", path);

    return 0;
}

int os_df(lua_State *L) {
    const char *path = luaL_optstring(L, 1, "/");
    const struct mount_pt *mountp;

    if ((mountp = mount_get_mount_point_for_path(path))) {
        if (mountp->fsstat) {
            uint32_t total, used;
            if (0 == mountp->fsstat(path, &total, &used)) {
                lua_pushinteger(L, total-used); // free
                lua_pushinteger(L, total);      // total
                return 2;
            }
        }
    }

    printf("can't get free space of %s\r\n", path);
    return 0;
}

static int os_lua_running(lua_State *L) { 
    lua_pushboolean(L, status_get(STATUS_LUA_RUNNING));

    return 1;
}

static int os_lua_interpreter(lua_State *L) { 
    lua_pushboolean(L, status_get(STATUS_LUA_INTERPRETER));

    return 1;
}

static int os_history(lua_State *L) { 
    if (lua_gettop(L) == 1) {
        luaL_checktype(L, 1, LUA_TBOOLEAN);

        if (lua_toboolean( L, 1 )) {
            status_set(STATUS_LUA_HISTORY, 0x00000000);
        } else {
            status_set(0x00000000, STATUS_LUA_HISTORY);
            linenoiseHistoryClear();
        }

        return 0;
    } else {
        lua_pushboolean(L, status_get(STATUS_LUA_HISTORY));
        return 1;
    }
}

static int os_cp(lua_State *L) {
    const char *src = luaL_optstring(L, 1, NULL);
    const char *dst = luaL_optstring(L, 2, NULL);

    // Check that source and destination are not the same file
    char *nsrc = mount_normalize_path(src);
    if (!nsrc) {
        errno = ENOMEM;
        return luaL_fileresult(L, 0, dst);
    }

    char *ndst = mount_normalize_path(dst);
    if (!ndst) {
        errno = ENOMEM;
        free(nsrc);
        return luaL_fileresult(L, 0, dst);
    }

    // If src and dst file are the same, do noting and exit
    if (strcmp(nsrc, ndst) == 0) {
        free(nsrc);
        free(ndst);

        lua_pushboolean(L, 1);
        return 1;
    }

    free(nsrc);
    free(ndst);

    // Copy file
    FILE* fsrc = fopen(src,"r");
    if (!fsrc) {
        return luaL_fileresult(L, 0, src);
    }

    FILE *fdst = fopen(dst,"w");
    if (!fdst) {
        fclose(fsrc);
        return luaL_fileresult(L, 0, dst);
    }

    char c = fgetc(fsrc);
    while (!feof(fsrc)) {
        fputc(c, fdst);
        c = fgetc(fsrc);
    }

    int res1 = fclose(fsrc);
    int res2 = fclose(fdst);

    if (res1 != 0) {
        return luaL_fileresult(L, 0, src);
    }

    if (res2 != 0) {
        return luaL_fileresult(L, 0, dst);
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int os_run (lua_State *L) {
    const char *argCode = luaL_optstring(L, 1, "");
    unsigned int i;
    int done;
    int status;
    int from_uart = 0;

    char *code = NULL;
    int code_size = 0;

    char *cchunk;
    char chunk_size;

    if (*argCode) {
        code = (char *)argCode;
        lua_pop(L, 1);


        goto skip;
    }

    from_uart = 1;

    lua_pop(L, 1);

    // Lock tty, avoid other threads to write to console
    uart_ll_lock(CONSOLE_UART);
    uart_ll_set_raw(1);

    // Clear received buffer
    uart_consume(CONSOLE_UART);

    // Send 'C' for start
    uart_write(CONSOLE_UART, 'C');
    uart_write(CONSOLE_UART, '\n');

    done = 0;

    for(;;) {
        // Wait for chunk size
        if (!uart_read(CONSOLE_UART, &chunk_size, 2000)) {
            break;
        }

        // More chunks?
        if (chunk_size == 0) {
            done = 1;
            break;
        }

        code = realloc(code, code_size + chunk_size + 1);
        if (!code) {
            uart_ll_set_raw(0);
            uart_ll_unlock(CONSOLE_UART);
            return luaL_error(L, "not enough memory");
        }

        // Read chunk
        cchunk = code + code_size;
        for(i=0; i < chunk_size; i++) {
            if (!uart_read(CONSOLE_UART, cchunk++, 2000)) {
                break;
            }
        }

        *cchunk = 0x00;

        code_size = code_size + chunk_size;

        // Send 'C' for start
        uart_write(CONSOLE_UART, 'C');
        uart_write(CONSOLE_UART, '\n');
    }

    if (!done) {
        // Unlock tty, permit other threads to write to console
        uart_ll_set_raw(0);
        uart_ll_unlock(CONSOLE_UART);

        free(code);

        return luaL_error(L, "timeout");
    }

    uart_ll_set_raw(0);
    uart_ll_unlock(CONSOLE_UART);

skip:
    // Call load
    lua_getglobal(L, "load");
    lua_pushstring(L, (const char *)code);

    status = lua_pcall(L, 1, 2, 0);
    if (status != LUA_OK) {
        if (code && from_uart) free(code);
        return luaL_error(L, lua_tostring(L, -1));
    }

    // Call to function
    status = lua_pcall(L, 1, 0, 0);
    if (status != LUA_OK) {
        if (code && from_uart) free(code);
        return luaL_error(L, lua_tostring(L, -1));
    }


    if (code && from_uart) free(code);

    return 0;
}

static int os_bootcount(lua_State *L) {
    lua_pushinteger(L, boot_count);

    return 1;
}

static int os_flash_unique_id(lua_State *L) {
    #if CONFIG_LUA_RTOS_READ_FLASH_UNIQUE_ID
    char buffer[17];

    snprintf(buffer, sizeof(buffer),
            "%02x%02x%02x%02x%02x%02x%02x%02x",
            flash_unique_id[0], flash_unique_id[1],
            flash_unique_id[2], flash_unique_id[3],
            flash_unique_id[4], flash_unique_id[5],
            flash_unique_id[6], flash_unique_id[7]
    );

    lua_pushstring(L, buffer);
    #else
    lua_pushnil(L);
    #endif
    return 1;
}

static int os_exists(lua_State *L) {
    const char *fname;
    struct stat sb;
    size_t len;

    fname = luaL_checklstring( L, 1, &len );
    if (stat(fname, &sb) != 0) {
        lua_pushboolean(L, false);
    }
    else lua_pushboolean(L, true);

    return 1;
}

#if CONFIG_LUA_RTOS_USE_HARDWARE_LOCKS
static int os_locks(lua_State *L) {
    const driver_t *cdriver = drivers;
    int lock = 0;
    int first = 0;
    int has_locks = 0;
    int unit = 0;
    while (cdriver->name) {
        if (*cdriver->lock) {
            first = 1;
            has_locks = 0;

            for(lock = 0; lock < cdriver->locks; lock++) {
                unit = lock;
                if ((*cdriver->lock)[lock].owner) {
                    has_locks = 1;

                    if (first){
                        printf("%s\r\n", cdriver->name);
                    }

                    first = 0;

                    if (strcmp(cdriver->name, "spi") == 0) {
                        unit = ((lock / SPI_BUS_DEVICES) << 8) | (lock % SPI_BUS_DEVICES);
                    } else if (strcmp(cdriver->name, "i2c") == 0) {
                        unit = ((lock / I2C_BUS_DEVICES) << 8) | (lock % I2C_BUS_DEVICES);
                    } else {
                        unit = lock;
                    }

                    char *oname = driver_target_name(cdriver, unit, NULL);
                    char *tname = driver_target_name((*cdriver->lock)[lock].owner, (*cdriver->lock)[lock].unit, (*cdriver->lock)[lock].tag);

                    printf("  %s locked by %s\r\n", oname, tname);

                    free(oname);
                    free(tname);
                }
            }

            if (has_locks) {
                printf("\r\n");
            }
        }
        cdriver++;
    }

    return 0;
}
#endif

static int os_factory_reset(lua_State *L) {
    // Find ota data partition
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);

    if (partition) {
        esp_partition_erase_range(partition, partition->address, partition->size);

        esp_restart();
    } else {
        return luaL_error(L, "OTA partition not found, reset to factory settings is not possible");
    }

    return 0;
}

static int os_partitions(lua_State *L) {
    char address[9];
    char size[9];
    esp_partition_iterator_t it;
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* running = esp_ota_get_running_partition();

    printf("TYPE:SUB\t ADDRESS\t  LENGTH\tENC\tLABEL\n");

    it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);

        snprintf(address, sizeof(address),"%8d", p->address);
        snprintf(size, sizeof(size),"%8d", p->size);
        printf("0x%02x:0x%02x\t%s\t%s\t%c\t%s%s%s\n", p->type, p->subtype, address, size, p->encrypted ? 'Y':'N', p->label, p==boot ? " < boot":"", p==running ? " < running":"");
    }
    esp_partition_iterator_release(it);

    it = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);

        snprintf(address, sizeof(address),"%8d", p->address);
        snprintf(size, sizeof(size),"%8d", p->size);
        printf("0x%02x:0x%02x\t%s\t%s\t%c\t%s\n", p->type, p->subtype, address, size, p->encrypted ? 'Y':'N', p->label);
    }
    esp_partition_iterator_release(it);

    return 0;
}

static int os_passwd(lua_State *L) {
    char buffer[LUA_MAXINPUT];
    char pass[65];
    char re_pass[65];
    char hash_pass[120];
    char *password;

    // Check that /etc exists, and create it if not exists
    struct stat sb;

    if (stat("/etc", &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        mkdir("/etc", 0755);
    }

    // If system password is set read the password hash
    // from /etc/passwd
    hash_pass[0] = 0x00;

    FILE *fp = fopen("/etc/passwd", "r");
    if (fp) {
        fgets(hash_pass, sizeof(hash_pass), fp);
        fclose(fp);
    } else {
        fp = fopen("/etc/passwd", "a+");
        fclose(fp);
    }

    // If system password is set, answer the password to the
    // user to check before change it
    if (strlen(hash_pass) > 0) {
        // Get password
        linenoisePassword(buffer, "Old Password: ", true);
        if (strlen(buffer) > 64) {
            printf("try again, password too long\r\n");
            goto exit;
        }
        if (strlen(buffer) == 0) {
            goto exit;
        }

        password = crypt(buffer, hash_pass);
        if (!password || (strcmp(password, hash_pass) != 0)) {
            printf("try again\r\n");
            goto exit;
        }
    }

    // Change password

    // Get password
    linenoisePassword(buffer, "New Password: ", true);
    if (strlen(buffer) > 64) {
        printf("try again, password too long\r\n");
        goto exit;
    }
    if (strlen(buffer) == 0) {
        goto exit;
    }
    strcpy(pass, buffer);

    // Retype password
    linenoisePassword(buffer, "Retype New Password: ", true);
    if (strlen(buffer) > 64) {
        printf("try again, password too long\r\n");
        goto exit;
    }
    if (strlen(buffer) == 0) {
        goto exit;
    }
    strcpy(re_pass, buffer);

    // Check
    if (strcmp(pass, re_pass) != 0) {
        printf("try again\r\n");
        goto exit;
    }

    // Generate the salt
    char salt[] = "$1$........";
    const char *const seedchars = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char random;
    int i;

    for (i = 0; i < 8; i++) {
        os_get_random(&random, sizeof(random));
        salt[3+i] = seedchars[random % strlen(seedchars)];
    }

    password = crypt(pass, salt);

    // Store password in file
    fp = fopen("/etc/passwd", "wb");
    if (fp) {
        fputs(password, fp);
        fclose(fp);
    } else {
        printf("can't write to /etc/passwd (%s)\r\n", strerror(errno));
        goto exit;
    }

    return 0;

 exit:
     fclose(fp);
     return 0;
}

static int os_uptime(lua_State *L) {
    uint8_t table = 0;

    // Check if user wants result as a table, or wants result
    // on the console
    if (lua_gettop(L) == 1) {
        luaL_checktype(L, 1, LUA_TBOOLEAN);
        if (lua_toboolean(L, 1)) {
            table = 1;
        }
    }

    // Get time since last boot (no RTC)
    struct tms tms;

    times(&tms);
    uint32_t uptime = tms.tms_stime / 1000;

    int updays = uptime / 86400;
    int uphours = (uptime - (updays * 86400)) / 3600;
    int upmins = (uptime - (updays * 86400) - (uphours * 3600)) / 60;
    int upsecs = (uptime - (updays * 86400) - (uphours * 3600) - (upmins *60));

    // Get current time
    time_t timer;
    char buffer[26];
    struct tm* tm_info;

    time(&timer);
    tm_info = localtime(&timer);

    strftime(buffer, 26, "%H:%M", tm_info);

    if (!table) {
        // Print
        if (updays == 1) {
            printf("%s up 1 day %2d:%02d:%02d\r\n", buffer, uphours, upmins, upsecs);
        } else if (updays > 1) {
            printf("%s up %d day %2d:%02d:%02d\r\n", buffer, updays, uphours, upmins, upsecs);
        } else {
            printf("%s up %2d:%02d:%02d\r\n", buffer, uphours, upmins, upsecs);
        }
    } else {
        lua_createtable(L, 0, 0);

        lua_pushstring (L, "current");
        lua_pushstring(L, buffer);
        lua_settable(L, -3);

        lua_pushstring (L, "days");
        lua_pushinteger(L, updays);
        lua_settable(L, -3);

        lua_pushstring (L, "hours");
        lua_pushinteger(L, uphours);
        lua_settable(L, -3);

        lua_pushstring (L, "mins");
        lua_pushinteger(L, upmins);
        lua_settable(L, -3);

        lua_pushstring (L, "secs");
        lua_pushinteger(L, upsecs);
        lua_settable(L,-3);
    }

    return table;
}

static int os_time (lua_State *L);
static int os_settime(lua_State *L) {
    time_t t = (time_t)-1;

    // Check if user provides input as a table
    if (1 == lua_gettop(L) && lua_istable(L, 1)) {
        if (1 == os_time(L)) {
            t = luaL_checkinteger(L, -1);
        }
    }
    else {
        struct tm ts;
        ts.tm_hour  = luaL_checkinteger(L, 1);
        ts.tm_min   = luaL_checkinteger(L, 2);
        ts.tm_sec   = luaL_checkinteger(L, 3);
        ts.tm_mon   = luaL_checkinteger(L, 4) - 1;
        ts.tm_mday  = luaL_checkinteger(L, 5);
        ts.tm_year  = luaL_checkinteger(L, 6) - 1900;
        ts.tm_isdst = luaL_optinteger(L, 7, 0);
        t = mktime(&ts);
    }

    if (t == (time_t)-1) {
        luaL_error(L, "parameters given cannot be set as system time");
    }

    struct timeval tv;
    tv.tv_sec = t;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);

    lua_pushinteger(L, (lua_Integer)(t));
    return 1;
}

#if defined(MALLOC_DEBUG_H)
static int os_rmstat(lua_State *L) {
	RM_STAT;
	return 0;
}
#endif

