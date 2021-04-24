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
 * Lua RTOS,  http lua page preprocessor
 *
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_USE_HTTP_SERVER

#include <stdio.h>

#include <sys/syslog.h>

// from httpsrv.c
#define PRINTF_BUFFER_SIZE_INITIAL 256
#define PRINT_MAXLINE 200

int http_preprocess_lua_page(const char *ipath, const char *opath) {
    FILE *ifp; // Input file
    FILE *ofp; // Output file

    int c;
    int lc = 0;
    char lua = 0; // kind of text we are currently reading
    // 0: verbatim text;
    // 1: lua tag found, we still need to decide if there is a '=' to follow
    // 2: statment lua code (eg: <?lua ... ?> )
    // 3: expression lua code ( eg: <?lua 1+2 ?> )
    char buff[6];
    // avoid line longer than PRINTF_BUFFER_SIZE_INITIAL, as they will trigger a memory allocation in do_print()
    // they may even silently fail if they are longer than BUFFER_SIZE_MAX
    int line_len = 0; // length of the current verbatim line

    const char *bt = "<?lua";
    const char *et = "?>";

    const char *cbt = bt;
    const char *cet = et;
    char *cbuff = buff;

    // Open input file
    ifp = fopen(ipath,"r");
    if (!ifp) {
        return -1;
    }

    // Open output file
    ofp = fopen(opath,"w+");
    if (!ofp) {
        fclose(ifp);

        return -1;
    }

    *cbuff = '\0';

    fprintf(ofp, "do\n");
    fprintf(ofp, "local print = http_internal_handle and net.service.http.print_chunk or print\n"); // can be run in console
    fprintf(ofp, "local _w = http_internal_handle and net.service.http.print_chunk or io.write\n");
    while((c = fgetc(ifp)) != EOF) {

        if (!lua && (c == *cbt)) { // in opening lua mark
            cbt++;
            if (!*cbt) { // end of mark reached, switch to lua
                lua = 1;
                cbuff = buff;  // drop buffer content
                if (line_len > 0) {
                    fprintf(ofp, "]]\n");
                }
                line_len = 0;
            } else {
                *cbuff++ = c;
            }

            *cbuff = '\0';
        } else if (lua == 1 && c == '=') { // shortand lua tag
              lua = 3;
              fprintf(ofp, "_w(");
        } else if (lua && (c == *cet)) {  // in closing lua mark
            cet++;
            if (!*cet) { // end of mark, switch to text
                cbuff = buff;  // drop buffer content
                if (lua == 3) {
                    fprintf(ofp, ")\n");  // end of lua expression
                } else {
                    fprintf(ofp, "\n");  // end of user lua statement
                }
                lua = 0;
            } else {
                *cbuff++ = c;
            }

            *cbuff = '\0';
        } else { // not in mark
		    if (lua == 1) {
                lua = 2; // normal lua code
            }
            cbt = bt;
            cet = et;

            // output buffered mark
            cbuff = buff; // reset mark buffer
            if (*cbuff) {
                if (!lua && line_len == 0) {
                    fprintf(ofp, "_w[[\n");
                }
                fprintf(ofp, "%s", cbuff); // we can output buffer in one go, as it is only a part of the mark like "<?l" and
                // cannot contains special characters like '\n' or ']'
                line_len++; // slightly under estimated, but we don't care about the real length
                *cbuff = '\0';
            }

            if (!lua) {
                if (c == ']' && c == lc) {// escape long string end marker
                    fprintf(ofp, "]\n_w']'\n");
                    line_len = 0;
                } else if (c == '[' && c == lc) { // escape long string marker
                    fprintf(ofp, "]]\n");
                    line_len = 0;
                } else if (c == '\n') {
                    if (line_len > PRINT_MAXLINE) {
                        fprintf(ofp, "\n]]\n");
                        line_len = 0;
                    }
                } else if (c == '\r') {
                    lc = c; // remember last char
                    continue;
                }
                if (0 == line_len) {
                    fprintf(ofp, "_w[[\n");
                }
                line_len++;
            }
            fprintf(ofp, "%c", c);
        }
        lc = c; // remember last char
    }

    if (line_len > 0) {
        fprintf(ofp, "]]\n");
    }

    fprintf(ofp, "end");

    fclose(ifp);
    fclose(ofp);

    return 0;
}

#endif
