/*******************************************************************************
 * Copyright (c) 2015, http://www.jbox.dk/
 * All rights reserved. Released under the BSD license.
 * httpsrv.c 1.0 01/01/2016 (Simple Http Server)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 ******************************************************************************/

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_USE_HTTP_SERVER

#include "preprocessor.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include <sys/panic.h>
#include <sys/delay.h>

#include "esp_wifi_types.h"
#include "esp_log.h"
#include <pthread.h>
#include <esp_wifi.h>

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/syslog.h>
#include <sys/path.h>
#include <sys/socket.h>
#include <netdb.h>
#include <linux/in6.h>

#include <openssl/ssl.h>
#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/esp_debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/certs.h"

#define SERVER_ID      "lua-rtos-http-server/1.0"
#define PROTOCOL       "HTTP/1.1"
#define RFC1123FMT     "%a, %d %b %Y %H:%M:%S GMT"
#define HTTP_BUFF_SIZE 1024
#define CAPTIVE_SERVER_NAME	"config-esp32-settings"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lgc.h"
#include "sys.h"
#include <drivers/net.h>
#include <drivers/spi_eth.h>


char *strcasestr(const char *haystack, const char *needle);
driver_error_t *wifi_check_error(esp_err_t error);

extern int captivedns_start(lua_State* L);
extern void captivedns_stop();
extern int captivedns_running();

static lua_callback_t* http_callback = NULL;

static wifi_mode_t wifi_mode = WIFI_MODE_STA;
static u8_t http_refcount = 0;
static u8_t volatile http_shutdown = 0;
static u8_t volatile script_wants_reboot = 0;
static u8_t http_captiverun = 0;
static u32_t ap_ip4addr = 0;
static char  ap_ip4addr_str[IP4ADDR_STRLEN_MAX];
static int socket_server_normal = 0;
static int socket_server_secure = 0;

typedef struct {
	int port;
	int *server; //socket
	const int secure;
	char *certificate;
	char *private_key;
} http_server_config;

#define HTTP_Normal_initializer { CONFIG_LUA_RTOS_HTTP_SERVER_PORT, &socket_server_normal, 0, NULL, NULL }
#define HTTP_Secure_initializer { CONFIG_LUA_RTOS_HTTP_SERVER_PORT_SSL, &socket_server_secure, 1, NULL, NULL } //cert and privkey need to be supplied from lua

typedef struct {
	http_server_config *config;
	int socket;
	SSL *ssl;
	uint8_t headers_sent;
	struct sockaddr_storage *client;
	socklen_t client_len;
	char *path;
	char *method;
	char *data;
	char *chunk_buffer;
	char *printf_buffer;
} http_request_handle;

#define HTTP_Request_Normal_initializer { config, client, NULL, 0, &client_addr, client_addr_len, NULL, NULL, NULL, NULL, NULL };
#define HTTP_Request_Secure_initializer { config, client, ssl,  0, &client_addr, client_addr_len, NULL, NULL, NULL, NULL, NULL };

static http_server_config http_normal = HTTP_Normal_initializer;
static http_server_config http_secure = HTTP_Secure_initializer;

int is_luasp(char *name) {
	char *ext = strrchr(name, '.');
	if (!ext) return 0;

	if (strcmp(ext, ".luasp")  == 0) return 1;
	return 0;
}

char *get_mime_type(char *name) {

	char *ext = strrchr(name, '.');
	if (!ext) return NULL;
	if (strcmp(ext, ".html") == 0) return "text/html";
	if (strcmp(ext, ".htm")  == 0) return "text/html";
	if (strcmp(ext, ".txt")  == 0) return "text/html";
	if (strcmp(ext, ".jpg")  == 0) return "image/jpeg";
	if (strcmp(ext, ".jpeg") == 0) return "image/jpeg";
	if (strcmp(ext, ".gif")  == 0) return "image/gif";
	if (strcmp(ext, ".png")  == 0) return "image/png";
	if (strcmp(ext, ".css")  == 0) return "text/css";
	if (strcmp(ext, ".au")   == 0) return "audio/basic";
	if (strcmp(ext, ".wav")  == 0) return "audio/wav";
	if (strcmp(ext, ".avi")  == 0) return "video/x-msvideo";
	if (strcmp(ext, ".mpeg") == 0) return "video/mpeg";
	if (strcmp(ext, ".mpg")  == 0) return "video/mpeg";
	if (strcmp(ext, ".mp3")  == 0) return "audio/mpeg";
	if (strcmp(ext, ".svg")  == 0) return "image/svg+xml";
	if (strcmp(ext, ".pdf")  == 0) return "application/pdf";
	return NULL;
}

static int request_write(http_request_handle *request, char *buffer, int length) {
	return (request->config->secure) ? SSL_write(request->ssl, buffer, length) : send(request->socket, buffer, length, MSG_DONTWAIT);
}

#define BUFFER_SIZE_INITIAL 256
#define BUFFER_SIZE_MAX 2048
static int do_printf(http_request_handle *request, const char *fmt, ...) {
	int ret = 0;
	va_list args;

	if (request->printf_buffer == NULL)
		request->printf_buffer = (char *)malloc(BUFFER_SIZE_INITIAL);

	if (request->printf_buffer) {
		*(request->printf_buffer) = '\0';
		va_start(args, fmt);
		int length = vsnprintf(request->printf_buffer, BUFFER_SIZE_INITIAL, fmt, args);
		va_end(args);
		if (length>=0 && length<BUFFER_SIZE_INITIAL) {
			if(length) { //don't try to transfer "nothing"
				ret = request_write(request, request->printf_buffer, length);
			}
		}
		else {
			// retry with a bigger buffer...
			char *buffer = (char *)malloc(BUFFER_SIZE_MAX);
			if (buffer) {
				*buffer = '\0';
				va_start(args, fmt);
				length = vsnprintf(buffer, BUFFER_SIZE_MAX, fmt, args);
				va_end(args);
				if(length) { //don't try to transfer "nothing"
					ret = request_write(request, buffer, length);
				}
				free(buffer);
			}
		}

	}
	return ret;
}

static char *do_gets(char *s, int size, http_request_handle *request) {

	int socket = request->config->secure ? SSL_get_fd(request->ssl) : request->socket;

	fd_set set;
	FD_ZERO(&set); /* clear the set */
	FD_SET(socket, &set); /* add our file descriptor to the set */
	struct timeval timeout = {0L, 100L}; //wait up to 100ms

	int rc;
	char *c = s;
	while (c < (s + size - 1)) {

		if (request->config->secure) {

			//only do this once for secure (or if nothing pending)
			if (c == s && 0 == SSL_pending(request->ssl)) {
				// select supports setting a timeout
				// so *only* if select tells us data
				// is ready we read the data using recv
				rc = select(socket+1, &set, NULL, NULL, &timeout);
				if (rc == -1) {
					syslog(LOG_DEBUG, "http: select failed\r");
					return NULL;
				}
				else if (rc == 0 && !FD_ISSET(socket, &set)) {
					//no data received or connection is closed
					syslog(LOG_DEBUG, "http: no data received\r");
					break;
				}
			}

			//see http://www.past5.com/tutorials/2014/02/21/openssl-and-select/
			int ssl_error;
			rc = SSL_read(request->ssl, c, 1);

			//check SSL errors
			switch(ssl_error = SSL_get_error(request->ssl,rc)) {
				case SSL_ERROR_NONE:
					//all good - we do our stuff with the newly read character below
					break;
				case SSL_ERROR_ZERO_RETURN:	 	//connection closed by client, clean up
				case SSL_ERROR_WANT_READ:			//the operation did not complete, block the read
				case SSL_ERROR_WANT_WRITE:		//the operation did not complete
				case SSL_ERROR_SYSCALL:				//some I/O error occured (could be caused by false start in Chrome for instance), disconnect the client and clean up
				default:											//some other error, clean up
					return NULL;
					break;
			}
		}
		else
			rc = recv(socket, c, 1, MSG_WAITALL);

		if (rc>0) {
			//syslog(LOG_DEBUG, "http: got %c\r", *c);
			if (*c == '\n') {
				c++;
				break;
			}
			c++;
		}
		else if (rc==0) {
			//no data received or connection is closed
			syslog(LOG_DEBUG, "http: no data received or connection is closed\r");
			break;
		}
		else {
			syslog(LOG_DEBUG, "http: discarding half-received data\r");
			return NULL; //discard half-received data
		}
	}
	*c = 0;
	return (c == s ? 0 : s);
}

void send_headers(http_request_handle *request, int status, char *title, char *extra, char *mime, int length) {
	do_printf(request, "%s %d %s\r\n", PROTOCOL, status, title);
	do_printf(request, "Server: %s\r\n", SERVER_ID);
	if (extra) do_printf(request, "%s\r\n", extra);
	if (mime) do_printf(request, "Content-Type: %s\r\n", mime);

	if (length >= 0) {
		do_printf(request, "Content-Length: %d\r\n", length);
	} else {
		do_printf(request, "Transfer-Encoding: chunked\r\n");
	}

	do_printf(request, "Connection: close\r\n");

	do_printf(request, "Cache-Control: no-cache, no-store, must-revalidate\r\n");
	do_printf(request, "no-cache\r\n");
	do_printf(request, "0\r\n");

	do_printf(request, "\r\n");
}

#define HTTP_STATUS_LEN     3
#define HTTP_ERROR_LINE_1   "<HTML><HEAD><TITLE>%d %s</TITLE></HEAD>\r\n"
#define HTTP_ERROR_LINE_2   "<BODY><H4>%d %s</H4>\r\n"
#define HTTP_ERROR_LINE_3   "%s\r\n"
#define HTTP_ERROR_LINE_4   "</BODY></HTML>\r\n"
#define HTTP_ERROR_VARS_LEN (2 * 5)

void send_error(http_request_handle *request, int status, char *title, char *extra, char *text) {
	int len = strlen(title) * 2 +
			  HTTP_STATUS_LEN * 2 +
			  strlen(text) +
			  strlen(HTTP_ERROR_LINE_1) +
			  strlen(HTTP_ERROR_LINE_2) +
			  strlen(HTTP_ERROR_LINE_3) +
			  strlen(HTTP_ERROR_LINE_4) -
			  HTTP_ERROR_VARS_LEN;

	send_headers(request, status, title, extra, "text/html", len);
	do_printf(request, HTTP_ERROR_LINE_1, status, title);
	do_printf(request, HTTP_ERROR_LINE_2, status, title);
	do_printf(request, HTTP_ERROR_LINE_3, text);
	do_printf(request, HTTP_ERROR_LINE_4);

	// after send_error the request will usually end
	// so make sure the buffer used in do_printf is deleted here
	// as the calling function might not really notice it's usage
	if (request->printf_buffer) {
		free(request->printf_buffer);
		request->printf_buffer = NULL;
	}
}

#define CHUNK_SIZE_INITIAL (BUFFER_SIZE_INITIAL-2)
#define CHUNK_SIZE_MAX  (BUFFER_SIZE_MAX-2)
static void chunk(http_request_handle *request, const char *fmt, ...) {
	va_list args;

	if (request->chunk_buffer == NULL)
		request->chunk_buffer = (char *)malloc(CHUNK_SIZE_INITIAL);

	if (request->chunk_buffer) {
		*(request->chunk_buffer) = '\0';
		va_start(args, fmt);
		int length = vsnprintf(request->chunk_buffer, CHUNK_SIZE_INITIAL, fmt, args);
		va_end(args);
		if (length>=0 && length<CHUNK_SIZE_INITIAL) {
			if(length) { //a length of zero would end our whole transfer
				do_printf(request, "%x\r\n", length);
				do_printf(request, "%s\r\n", request->chunk_buffer);
			}
		}
		else {
			// retry with a bigger buffer...
			char *buffer = (char *)malloc(CHUNK_SIZE_MAX);
			if (buffer) {
				*buffer = '\0';
				va_start(args, fmt);
				length = vsnprintf(buffer, CHUNK_SIZE_MAX, fmt, args);
				va_end(args);
				if(length) { //a length of zero would end our whole transfer
					do_printf(request, "%x\r\n", length);
					do_printf(request, "%s\r\n", buffer);
				}
				free(buffer);
			}
		}
	}
}

int http_status(lua_State* L) {

	int code = luaL_optinteger( L, 1, 200 );
	const char *title = luaL_optstring( L, 2, "OK" );
	const char *extra_headers = luaL_optstring( L, 3, NULL );
	const char *content_type = luaL_optstring( L, 4, "text/html" );
	int content_length = luaL_optinteger( L, 5, -1 );

	lua_getglobal(L, "http_internal_handle");
	if (!lua_islightuserdata(L, -1)) {
		return luaL_error(L, "this function may only be called inside a lua script served by httpsrv");
	}
	http_request_handle *request = (http_request_handle*)lua_touserdata(L, -1);

	if (!request) {
		return luaL_error(L, "this function may only be called inside a lua script served by httpsrv");
	}

	if (!request->headers_sent) {
		send_headers(request, code, (char *)title, (char *)extra_headers, (char *)content_type, content_length);
		if (!request->config->secure) fsync(request->socket);
		request->headers_sent = 1;

		lua_pop(L, lua_gettop(L));
		lua_pushinteger(L, 1);
	}
	else {
		lua_pop(L, lua_gettop(L));
		lua_pushinteger(L, 0);
	}

	return 1;
}

int http_print(lua_State* L) {

	lua_getglobal(L, "http_internal_handle");
	if (!lua_islightuserdata(L, -1)) {
		return luaL_error(L, "this function may only be called inside a lua script served by httpsrv");
	}
	http_request_handle *request = (http_request_handle*)lua_touserdata(L, -1);

	if (!request) {
		return luaL_error(L, "this function may only be called inside a lua script served by httpsrv");
	}

	if (!request->headers_sent) {
			send_headers(request, 200, "OK", NULL, "text/html", -1);
			if (!request->config->secure) fsync(request->socket);
			request->headers_sent = 1;
	}

	int nargs = lua_gettop(L);
	for (int i=1; i <= nargs; i++) {
		if (lua_isstring(L, i)) {
			chunk(request, "%s", lua_tostring(L, i));
		}
		else {
			/* non-strings handling not reqired */
		}
	}

	lua_pop(L, nargs);
	return 0;
}

int http_reboot(lua_State* L) {

	lua_getglobal(L, "http_internal_handle");
	if (!lua_islightuserdata(L, -1)) {
		return luaL_error(L, "this function may only be called inside a lua script served by httpsrv");
	}
	http_request_handle *request = (http_request_handle*)lua_touserdata(L, -1);

	if (!request) {
		return luaL_error(L, "this function may only be called inside a lua script served by httpsrv");
	}

	script_wants_reboot = 1;

	return 0;
}

#define LUA_INTERPRETER_ERROR_LENGTH 256
int __garbage_collector();
static int http_execute_lua (lua_State *L) {
		if (!lua_islightuserdata(L, 2)) {
			syslog(LOG_ERR, "http: FATAL ERROR, got wrong param...");
			return 0;
		}
		http_request_handle *request = (http_request_handle*)lua_touserdata(L, 2);
		const char *path = luaL_checkstring( L, 3);

		struct stat statbuf;
		stat(path, &statbuf);

		char ppath[PATH_MAX + 1];
		strcpy(ppath, path);

		if (strlen(ppath) < PATH_MAX) {
			strcat(ppath, "p");

			// Store .lua file modified time
			time_t src_mtime = statbuf.st_mtime;

			// Get .luap file modified time
			if (stat(ppath, &statbuf) == 0) {
				if (src_mtime > statbuf.st_mtime) {
					http_preprocess_lua_page(path,ppath);
				}
			} else {
				http_preprocess_lua_page(path,ppath);
			}

			if (S_ISDIR(statbuf.st_mode)) {
				send_error(request, 500, "Internal Server Error", NULL, "Folder found where a precompiled file was expected.");
			}
			else if (!S_ISREG(statbuf.st_mode)) {
				send_error(request, 500, "Internal Server Error", NULL, "Special file found where a regular precompiled file was expected.");
			}
			else {
				if (heap_caps_get_free_size(MALLOC_CAP_DEFAULT) < statbuf.st_size*3) {
					//free heap might be too low to load the file, so call GC before trying to load
					luaC_fullgc(L, 0);
					lua_unlock(L);
					vTaskDelay(1 / portTICK_PERIOD_MS);
					lua_lock(L);
					luaC_fullgc(L, 0);
					lua_unlock(L);
					vTaskDelay(1 / portTICK_PERIOD_MS);
				}

				if (heap_caps_get_free_size(MALLOC_CAP_DEFAULT) < statbuf.st_size*3) {
					//free heap might still be too low to load the file, so call the emergency GC before trying to load
					__garbage_collector();
				}

				lua_lock(L);
				int ret = luaL_loadfile(L, ppath);
				lua_unlock(L);

				if (LUA_OK != ret) {
					char* error = (char *)malloc(LUA_INTERPRETER_ERROR_LENGTH+1);
					if (error) {
						*error = '\0';
						snprintf(error, LUA_INTERPRETER_ERROR_LENGTH, "FATAL ERROR: %s", lua_tostring(L, -1));
						send_error(request, 500, "Internal Server Error", NULL, error);
						free(error);
					}
					else {
						send_error(request, 500, "Internal Server Error", NULL, "FATAL ERROR occurred");
					}
				}
				else {

					//as we execute a lua script let's prepare the remote ip address
					// -> first clean a possible IP4 addr from its leading '::ffff:'
					if (request->client->ss_family==AF_INET6) {
							struct sockaddr_in6* sa6=(struct sockaddr_in6*)request->client;
							if (IN6_IS_ADDR_V4MAPPED(&sa6->sin6_addr)) {
									struct sockaddr_in sa4;
									memset(&sa4,0,sizeof(sa4));
									sa4.sin_family=AF_INET;
									sa4.sin_port=sa6->sin6_port;
									memcpy(&sa4.sin_addr.s_addr,sa6->sin6_addr.s6_addr+12,4);
									memcpy(request->client,&sa4,sizeof(sa4));
									request->client_len=sizeof(sa4);
							}
					}

					// -> now convert the address to human-readable form
					char *buffer = (char *)malloc(INET6_ADDRSTRLEN+1);
					if (buffer) {
						int err=getnameinfo((struct sockaddr*)request->client,request->client_len,buffer,INET6_ADDRSTRLEN,0,0,NI_NUMERICHOST);
						if (err!=0) {
								snprintf(buffer,INET6_ADDRSTRLEN,"invalid address");
						}
					}

					lua_lock(L);
					lua_pushstring(L, (strcasecmp(request->method, "GET") == 0) ? "GET":"POST");
					lua_setglobal(L, "http_method");
					lua_pushstring(L, request->path);
					lua_setglobal(L, "http_uri");
					lua_pushstring(L, (request->data && *request->data) ? request->data:"");
					lua_setglobal(L, "http_request");
					lua_pushinteger(L, request->config->port);
					lua_setglobal(L, "http_port");
					lua_pushinteger(L, request->config->secure);
					lua_setglobal(L, "http_secure");
					lua_pushstring(L, buffer ? buffer:"");
					lua_setglobal(L, "http_remote_addr");
					lua_pushinteger(L, request->client->ss_family==AF_INET6 ? ((struct sockaddr_in6*)request->client)->sin6_port : ((struct sockaddr_in*)request->client)->sin_port);
					lua_setglobal(L, "http_remote_port");
					lua_pushstring(L, path);
					lua_setglobal(L, "http_script_name");
					lua_pushlightuserdata(L, (void*)request);
					lua_setglobal(L, "http_internal_handle");
					lua_unlock(L);

					int rc = lua_pcall(L, 0, 0, 0);
					if (LUA_OK != rc) {
						if (LUA_ERRRUN != rc) {
							syslog(LOG_ERR, "http: couldn't execute lua script, error %i\n", rc);
						}
						else {
							char* error = (char *)malloc(LUA_INTERPRETER_ERROR_LENGTH+1);
							if (error) {
								*error = '\0';
								snprintf(error, LUA_INTERPRETER_ERROR_LENGTH, "FATAL ERROR: %s", lua_tostring(L, -1));
								send_error(request, 500, "Internal Server Error", NULL, error);
								syslog(LOG_ERR, "http: couldn't execute lua script, %s\n", error);
								free(error);
							}
							else {
								send_error(request, 500, "Internal Server Error", NULL, "FATAL ERROR occurred");
							}
						}
					}

					lua_lock(L);
					lua_pushnil(L);
					lua_setglobal(L, "http_method");
					lua_pushnil(L);
					lua_setglobal(L, "http_uri");
					lua_pushnil(L);
					lua_setglobal(L, "http_request");
					lua_pushnil(L);
					lua_setglobal(L, "http_port");
					lua_pushnil(L);
					lua_setglobal(L, "http_secure");
					lua_pushnil(L);
					lua_setglobal(L, "http_remote_addr");
					lua_pushnil(L);
					lua_setglobal(L, "http_remote_port");
					lua_pushnil(L);
					lua_setglobal(L, "http_script_name");
					lua_pushnil(L);
					lua_setglobal(L, "http_internal_handle");
					lua_unlock(L);

					free(buffer);

					if (!request->config->secure) fsync(request->socket);
					do_printf(request, "0\r\n\r\n");
				}

				//free the heap again by calling GC
				lua_lock(L);
				luaC_fullgc(L, 0);
				lua_unlock(L);
				vTaskDelay(1 / portTICK_PERIOD_MS);

			}
		}
		else {
			send_error(request, 500, "Internal Server Error", NULL, "Path too long.");
		}

	return 0;
}

void send_file(http_request_handle *request, char *path, struct stat *statbuf) {

	FILE *file = fopen(path, "r");
	if (!file) {
		send_error(request, 403, "Forbidden", NULL, "Access denied.");
	} else if (is_luasp(path)) {
		fclose(file);

		lua_State *L = luaS_callback_state(http_callback);
		lua_pushcfunction(L, &http_execute_lua);  /* to call 'http_execute_lua' in protected mode */
		lua_pushlightuserdata(L, (void*)request);
		lua_pushstring(L, path);
		int rc = luaS_callback_call(http_callback, 3);
		if (LUA_OK != rc) {
			if (LUA_ERRRUN != rc) {
				syslog(LOG_ERR, "http: couldn't execute http_callback, error %i\n", rc);
			}
			else {
				char* error = (char *)malloc(LUA_INTERPRETER_ERROR_LENGTH+1);
				if (error) {
					*error = '\0';
					snprintf(error, LUA_INTERPRETER_ERROR_LENGTH, "FATAL ERROR: %s", lua_tostring(L, -2));
					send_error(request, 500, "Internal Server Error", NULL, error);
					syslog(LOG_ERR, "http: couldn't execute http_callback, %s\n", error);
					free(error);
				}
				else {
					send_error(request, 500, "Internal Server Error", NULL, "FATAL ERROR occurred");
				}
			}
		}
		//NOTE: no need to "clean up" the stack here!
	} else {
		char *data = calloc(1, HTTP_BUFF_SIZE);
		if (data) {
			int length = S_ISREG(statbuf->st_mode) ? statbuf->st_size : -1;
			send_headers(request, 200, "OK", NULL, get_mime_type(path), length);
			int read = 0;
			while ((read = fread(data, 1, sizeof (data), file)) > 0) request_write(request, data, read);
			free(data);
		}
		fclose(file);
	}
}

//newpath must have a size of HTTP_BUFF_SIZE
//rootpath must not be relative, should be '/' or any valid file system path
//reqpath may be relative
//addpath must not be relative and should not start with '/', or should be NULL
int filepath_merge(char *newpath, const char *rootpath, const char *reqpath, const char *addpath) {
	char *pos = newpath;
	int seglen;

	//newpath has a size of HTTP_BUFF_SIZE
	memset(newpath, 0, HTTP_BUFF_SIZE);

	// treat null as an empty path.
	if (!reqpath)
		reqpath = "";

	// copy the root path
	seglen = strlen(rootpath);
	if(seglen==0) {
	}
	else if(seglen==1) {
		// no need to memcpy
		*pos = *rootpath;
		pos++;
	}
	else {
		memcpy(newpath, rootpath, seglen);
		pos += seglen;
	}

	// make sure the root path ends with a slash
	if(*(pos-1) != '/') {
		*pos = '/';
		pos++;
	}
	*pos = 0;

	// remove leading slashes from the request path
	while (*reqpath=='/') {
		reqpath++;
	}

	int rootlen = pos - newpath;
	int pathlen = rootlen;

	// add the request path segment by segment
	while (*reqpath) {
		// finding the closing '/'
		const char *next = reqpath;
		while (*next && (*next != '/')) {
			++next;
		}
		seglen = next - reqpath;

		if (seglen == 0 || (seglen == 1 && reqpath[0] == '.')) {
			// noop segment (/ or ./) => skip
		}
		else if (seglen == 2 && reqpath[0] == '.' && reqpath[1] == '.') {
			// backpath (../)

			// try to crop the prior segment
			do {
				--pathlen;
			} while (pathlen && newpath[pathlen - 1] != '/');

			// now test if we are above root path length and
			// get back if necessary
			if (pathlen < rootlen) {
				pathlen = rootlen;
			}
		}
		else {
			// an actual segment, append to dest path
			if (*next) {
				seglen++;
			}
			memcpy(newpath + pathlen, reqpath, seglen);
			pathlen += seglen;
		}

		// skip over trailing slash => next segment
		if (*next) {
			++next;
		}

		reqpath = next;
	}
	pos = newpath + pathlen;

	// add the additional path
	if (addpath) {

		// check if the addpath is only a file extension:
		if (*addpath == '.') {
			// make sure the current path does NOT end with a slash
			if(*(pos-1) == '/') {
				pos--;
			}
		}
		else {
			// make sure the current path ends with a slash
			if(*(pos-1) != '/') {
				*pos = '/';
				pos++;
			}

			// remove leading slashes from the request path
			while (*addpath=='/') {
				addpath++;
			}
		}

		seglen = strlen(addpath);
		if(pos - newpath + seglen < HTTP_BUFF_SIZE)
			memcpy(pos, addpath, seglen);

		pos += seglen;
	}
	*pos = 0;

	return 1;
}

static void list_dir(http_request_handle *request, char *pathbuf, struct stat *statbuf, int len) {
	DIR *dir;
	struct dirent *de;

	send_headers(request, 200, "OK", NULL, "text/html", -1);
	chunk(request, "<HTML><HEAD><TITLE>Index of %s</TITLE></HEAD><BODY>", request->path);
	chunk(request, "<H4>Index of %s</H4>", request->path);

	chunk(request, "<TABLE>");
	chunk(request, "<TR>");
	chunk(request, "<TH style=\"width: 250;text-align: left;\">Name</TH><TH style=\"width: 100px;text-align: right;\">Size</TH>");
	chunk(request, "</TR>");

	if (len > 1) {
		chunk(request, "<TR>");
		chunk(request, "<TD><A HREF=\"..\">..</A></TD><TD></TD>");
		chunk(request, "</TR>");
	}

	filepath_merge(pathbuf, CONFIG_LUA_RTOS_HTTP_SERVER_DOCUMENT_ROOT, request->path, NULL); //restore folder pathbuf
	dir = opendir(pathbuf);
	while ((de = readdir(dir)) != NULL) {
		filepath_merge(pathbuf, CONFIG_LUA_RTOS_HTTP_SERVER_DOCUMENT_ROOT, request->path, de->d_name);
		stat(pathbuf, statbuf);

		chunk(request, "<TR>");
		chunk(request, "<TD>");
		chunk(request, "<A HREF=\"%s%s\">", de->d_name, S_ISDIR(statbuf->st_mode) ? "/" : "");
		chunk(request, "%s%s", de->d_name, S_ISDIR(statbuf->st_mode) ? "/</A>" : "</A> ");
		chunk(request, "</TD>");
		chunk(request, "<TD style=\"text-align: right;\">");
		if (!S_ISDIR(statbuf->st_mode)) {
			chunk(request, "%d", (int)statbuf->st_size);
		}
		chunk(request, "</TD>");
		chunk(request, "</TR>");
	}
	closedir(dir);


	chunk(request, "</TABLE>");

	chunk(request, "</BODY></HTML>");

	do_printf(request, "0\r\n\r\n");
}

/* we need this as the lwip implementation of
 * getsockname(request->socket, (struct sockaddr*)&addr, &addr_len)
 * returns an ip-address of 0 */
static bool remote_matches_ap_subnet(http_request_handle *request) {

	// -> first clean a possible IP4 addr from its leading '::ffff:'
	if (request->client->ss_family==AF_INET6) {
			struct sockaddr_in6* sa6=(struct sockaddr_in6*)request->client;
			if (IN6_IS_ADDR_V4MAPPED(&sa6->sin6_addr)) {
					struct sockaddr_in sa4;
					memset(&sa4,0,sizeof(sa4));
					sa4.sin_family=AF_INET;
					sa4.sin_port=sa6->sin6_port;
					memcpy(&sa4.sin_addr.s_addr,sa6->sin6_addr.s6_addr+12,4);
					memcpy(request->client,&sa4,sizeof(sa4));
					request->client_len=sizeof(sa4);
			}
	}

	if (request->client->ss_family==AF_INET) {
		u32_t addr_client = ((struct sockaddr_in*)request->client)->sin_addr.s_addr & IP_CLASSA_HOST; //use IP_CLASSA_HOST instead of IP_CLASSC_NET as the IP address is "reversed"
		return (ap_ip4addr == addr_client);
	}
	else if (request->client->ss_family==AF_INET6) {
		//FIXME find out how to properly compare remote IPv6 with local AP's IPv6
		//FIXME if they match on the right "class" then return true here
	}

	return false;
}

static int process(http_request_handle *request) {
	char *reqbuf;
	char *databuf = NULL;
	char *host = NULL;
	char *protocol;
	struct stat statbuf;
	char *pathbuf;
	int len;

	// Allocate space for buffers
	reqbuf = calloc(1, HTTP_BUFF_SIZE);
	if (!reqbuf) {
		send_error(request, 500, "Internal Server Error", NULL, "Error allocating memory.");
		return 0;
	}

	pathbuf = calloc(1, HTTP_BUFF_SIZE);
	if (!pathbuf) {
		send_error(request, 500, "Internal Server Error", NULL, "Error allocating memory.");
		free(reqbuf);
		return 0;
	}

	if (!do_gets(reqbuf, HTTP_BUFF_SIZE, request) || 0 == strlen(reqbuf) ) {
		send_error(request, 400, "Bad Request", NULL, "Got empty request buffer.");
		free(reqbuf);
		free(pathbuf);
		return 0;
	}

	char *save_ptr = NULL;
	request->method = strtok_r(reqbuf, " ", &save_ptr);
	request->path = strtok_r(NULL, " ", &save_ptr);
	protocol = strtok_r(NULL, "\r", &save_ptr);

	if(!request->path) {
		len = strlen(request->method)-1;
		while(len>0 && (request->method[len]=='\r' || request->method[len]=='\n')) {
			request->method[len] = 0;
			len--;
		}
		request->path = "/";
	}

	//in case the protocol wasn't given we need to fix the path
	if(!protocol) {
		len = strlen(request->path)-1;
		while(len>=0 && (request->path[len]=='\r' || request->path[len]=='\n')) {
			request->path[len] = 0;
			len--;
		}
		if(!strlen(request->path)) request->path = "/";
	}

	if(strcasecmp(request->method, "GET") == 0 && request->path) {
		request->data = strchr(request->path, '?');
		if (request->data) {
			*request->data = 0; //cut off the path
			request->data++; //point to start of params
		}
	}

	//only in AP mode we redirect arbitrary host names to our own host name
	if (captivedns_running() && (wifi_mode == WIFI_MODE_AP || wifi_mode == WIFI_MODE_APSTA) && remote_matches_ap_subnet(request)) {

		//find the Host: header and check if it matches our IP or captive server name
		while (do_gets(pathbuf, HTTP_BUFF_SIZE, request) && strlen(pathbuf)>0 ) {

			//quick check if the first char matches, only then do strcasestr
			if(pathbuf[0]=='h' || pathbuf[0]=='H') {
				host = strcasestr(pathbuf, "Host:");

				//check if the line begins with "Host:"
				if (host==(char *)pathbuf) {
					save_ptr = NULL;
					host = strtok_r(host, ":", &save_ptr); //Host:
					host = strtok_r(NULL, "\r", &save_ptr); //the actual host
					while(*host==' ') host++; //skip any spaces after the colon

					if (0 == strcasecmp(CAPTIVE_SERVER_NAME, host) ||
							0 == strcasecmp(ap_ip4addr_str, host)) {
						break; //done parsing headers
					}
					else {
						//redirect
						snprintf(pathbuf, HTTP_BUFF_SIZE, "Location: http://%s/", CAPTIVE_SERVER_NAME);
						send_headers(request, 302, "Found", pathbuf, NULL, 0);
						free(reqbuf);
						free(pathbuf);
						request->path = NULL;
						request->data = NULL;
						if (request->printf_buffer) {
							free(request->printf_buffer);
							request->printf_buffer = NULL;
						}
						return 0;
					}
				}
			}

		} // while
	} // AP mode

	if (!request->method || !request->path) {
		free(reqbuf);
		free(pathbuf);
		request->path = NULL;
		request->data = NULL;
		return -1; //protocol may be omitted
	}

	if(strcasecmp(request->method, "POST") == 0) {
		char *skip;
		int contentlength = HTTP_BUFF_SIZE;
		//skip headers to the actual request data
		while (do_gets(pathbuf, HTTP_BUFF_SIZE, request) && strlen(pathbuf)>0 ) {
			if (strlen(pathbuf)<3) {
				skip = pathbuf;
				while (*skip=='\r' || *skip=='\n') skip++;
				if (strlen(skip)==0) {
					break;
				}
			}
			else {
				//look for the content-length header to avoid
				//a timeout later when reading the actual request data

				//quick check if the first char matches, only then do strcasestr
				if(pathbuf[0]=='c' || pathbuf[0]=='C') {
					char *contentlen = strcasestr(pathbuf, "Content-Length:");

					//check if the line begins with "Content-Length:"
					if (contentlen==(char *)pathbuf) {
						save_ptr = NULL;
						contentlen = strtok_r(contentlen, ":", &save_ptr); //Content-Length:
						contentlen = strtok_r(NULL, "\r", &save_ptr); //the actual content length
						while(*contentlen==' ') contentlen++; //skip any spaces after the colon
						contentlength = atoi(contentlen)+1;
					}
				}
			}
		} // while headers
		//while empty lines
		while (do_gets(pathbuf, contentlength, request) && strlen(pathbuf)>0 ) {
			if (pathbuf && strlen(pathbuf)>0) {
				skip = pathbuf;
				while (*skip=='\r' || *skip=='\n') skip++;
				if (strlen(skip)>0) {
					break;
				}
			}
		} // while empty lines
		//preserve the data
		if (pathbuf && strlen(pathbuf)>0 ) {
			databuf = calloc(1, strlen(pathbuf)+1);
			if (!databuf) {
				send_error(request, 500, "Internal Server Error", NULL, "Error allocating POST data memory.");
				free(reqbuf);
				free(pathbuf);
				request->path = NULL;
				request->data = NULL;
				return 0;
			}
			strcpy(databuf, pathbuf);
			request->data = databuf;
		} // while
	}

	syslog(LOG_DEBUG, "http: %s %s %s\r", request->method, request->path, protocol ? protocol:"");

	if (!request->config->secure) shutdown(request->socket, SHUT_RD);

	if (strcasecmp(request->method, "GET") != 0 && strcasecmp(request->method, "POST") != 0) {
		syslog(LOG_DEBUG, "http: %s not supported\r", request->method);
		send_error(request, 501, "Not supported", NULL, "Method is not supported.");
	}
	else {
		bool found = false;

		//look for a file or folder with the exact name, .lua extension or .html extension
		if (!found && filepath_merge(pathbuf, CONFIG_LUA_RTOS_HTTP_SERVER_DOCUMENT_ROOT, request->path, NULL)    && stat(pathbuf, &statbuf) == 0) found = true;
		if (!found && filepath_merge(pathbuf, CONFIG_LUA_RTOS_HTTP_SERVER_DOCUMENT_ROOT, request->path, ".lua")  && stat(pathbuf, &statbuf) == 0) found = true;
		if (!found && filepath_merge(pathbuf, CONFIG_LUA_RTOS_HTTP_SERVER_DOCUMENT_ROOT, request->path, ".html") && stat(pathbuf, &statbuf) == 0) found = true;

		if (!found) {
			send_error(request, 404, "Not Found", NULL, "File not found.");
			syslog(LOG_DEBUG, "http: %s Not found\r", request->path);
		}
		else if (S_ISREG(statbuf.st_mode)) {
			//send the found file
			send_file(request, pathbuf, &statbuf);
		}
		else if (S_ISDIR(statbuf.st_mode)) {

			len = strlen(request->path);
			if (len == 0 || request->path[len - 1] != '/') {
				//send a redirect
				snprintf(pathbuf, HTTP_BUFF_SIZE, "Location: %s/", request->path);
				send_error(request, 302, "Found", pathbuf, "Directories must end with a slash.");
			} else {
				//try to find index.lua or index.html
				found = false;

				//look for a file named index.lua or index.html
				if (!found && filepath_merge(pathbuf, CONFIG_LUA_RTOS_HTTP_SERVER_DOCUMENT_ROOT, request->path, "index.lua")  && stat(pathbuf, &statbuf) == 0) found = true;
				if (!found && filepath_merge(pathbuf, CONFIG_LUA_RTOS_HTTP_SERVER_DOCUMENT_ROOT, request->path, "index.html") && stat(pathbuf, &statbuf) == 0) found = true;

				if (!found || !S_ISREG(statbuf.st_mode)) {
					//generate a dirlisting
					list_dir(request, pathbuf, &statbuf, len);
				}
				else {
					send_file(request, pathbuf, &statbuf);
				}
			}

		} // S_ISDIR
	}

	free(reqbuf);
	free(pathbuf);
	if (databuf) free(databuf);
	request->path = NULL;
	request->data = NULL;

	if (request->chunk_buffer) {
		free(request->chunk_buffer);
		request->chunk_buffer = NULL;
	}
	if (request->printf_buffer) {
		free(request->printf_buffer);
		request->printf_buffer = NULL;
	}

	return 0;
}

static void http_net_callback(system_event_t *event){
	//syslog(LOG_DEBUG, "event: %d\r\n", event->event_id);

	switch (event->event_id) {
		case SYSTEM_EVENT_STA_START:                /**< ESP32 station start */
			//only if we have previously been in AP mode
			if (wifi_mode == WIFI_MODE_AP) {
				driver_error_t *error;
				if ((error = wifi_check_error(esp_wifi_get_mode(&wifi_mode)))) {
					free(error);
				}

				syslog(LOG_DEBUG, "http: switched to non-captive mode\n");
				http_captiverun = captivedns_running();
				if (http_captiverun) {
					syslog(LOG_DEBUG, "http: auto-stopping captive dns service\n");
					captivedns_stop();
				}
			}
			break;

		case SYSTEM_EVENT_AP_START:                 /**< ESP32 soft-AP start */
			//only if we have previously been in STA mode
			if (wifi_mode == WIFI_MODE_STA) {
				driver_error_t *error;
				if ((error = wifi_check_error(esp_wifi_get_mode(&wifi_mode)))) {
					free(error);
				}

				ap_ip4addr = 0;
				//get the IP address of the AP explicitly
				tcpip_adapter_ip_info_t esp_info;
				if ((error = wifi_check_error(tcpip_adapter_get_ip_info(ESP_IF_WIFI_AP, &esp_info)))) {
					free(error);
				}
				else {
					ap_ip4addr = esp_info.ip.addr & IP_CLASSA_HOST; //use IP_CLASSA_HOST instead of IP_CLASSC_NET as the IP address is "reversed"
				}
				strcpy(ap_ip4addr_str, inet_ntoa(esp_info.ip.addr));

				syslog(LOG_DEBUG, "http: switched to captive mode on %s\n", ap_ip4addr_str);
				if (http_captiverun) {
					syslog(LOG_DEBUG, "http: auto-restarting captive dns service\n");
					captivedns_start(luaS_callback_state(http_callback));
				}
			}
			break;

		default :
			break;
	}
}

static void mbedtls_zeroize( void *v, size_t n ) {
	volatile unsigned char *p = v; while( n-- ) *p++ = 0;
}

extern __NOINIT_ATTR uint32_t backtrace_count;
static void *http_thread(void *arg) {
	http_server_config *config = (http_server_config*) arg;
	struct sockaddr_in6 sin;
	SSL_CTX *ctx = NULL;
	SSL *ssl = NULL;
	int rc = 0;

	net_init();
	if(0 == *config->server) {
		*config->server = socket(AF_INET6, SOCK_STREAM, 0);
		if(0 > *config->server) {
			syslog(LOG_ERR, "http: couldn't create server socket\n");
			return NULL;
		}

		u8_t one = 1;
		setsockopt(*config->server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

		memset(&sin, 0, sizeof(sin));
		sin.sin6_family = AF_INET6;
		memcpy(&sin.sin6_addr.un.u32_addr, &in6addr_any, sizeof(in6addr_any));
		sin.sin6_port   = htons(config->port);
		rc = bind(*config->server, (struct sockaddr *) &sin, sizeof (sin));
		if(0 != rc) {
			syslog(LOG_ERR, "http: couldn't bind to port %d\n", config->port);
			return NULL;
		}

		listen(*config->server, 5);
		LWIP_ASSERT("httpd_init: listen failed", *config->server >= 0);
		if(0 > *config->server) {
			syslog(LOG_ERR, "http: couldn't listen on port %d\n", config->port);
			return NULL;
		}

		// Set the timeout for accept
		struct timeval timeout = {1L, 0L}; /* check for shutdown every second */
		setsockopt(*config->server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	}

	if (config->secure) {
		ctx = SSL_CTX_new(TLS_server_method());
		if (!ctx) {
			syslog(LOG_ERR, "http: couldn't create SSL context\n");
			return NULL;
		}

		//load cert
		size_t certificate_bytes;
		unsigned char *certificate_buf;
		if( mbedtls_pk_load_file( config->certificate, &certificate_buf, &certificate_bytes ) != 0 ) {
			syslog(LOG_ERR, "http: couldn't load SSL certificate\n");
			SSL_CTX_free(ctx);
			ctx = NULL;
			return NULL;
		}
		if (!SSL_CTX_use_certificate_ASN1(ctx, certificate_bytes, certificate_buf)) {
			syslog(LOG_ERR, "http: couldn't set SSL certificate\n");
			mbedtls_zeroize( certificate_buf, certificate_bytes );
			//MUST NOT free certificate_buf !!!
			SSL_CTX_free(ctx);
			ctx = NULL;
			return NULL;
		}
		mbedtls_zeroize( certificate_buf, certificate_bytes );
		//MUST NOT free certificate_buf !!!

		//load privkey
		size_t private_key_bytes;
		unsigned char *private_key_buf;
		if( mbedtls_pk_load_file( config->private_key, &private_key_buf, &private_key_bytes ) != 0 ) {
			syslog(LOG_ERR, "http: couldn't load SSL certificate\n");
			SSL_CTX_free(ctx);
			ctx = NULL;
			return NULL;
		}
		if (!SSL_CTX_use_PrivateKey_ASN1(0, ctx, private_key_buf, private_key_bytes)) {
			syslog(LOG_ERR, "http: couldn't load SSL private key\n");
			mbedtls_zeroize( private_key_buf, private_key_bytes );
			//MUST NOT free private_key_buf !!!
			SSL_CTX_free(ctx);
			ctx = NULL;
			return NULL;
		}
		mbedtls_zeroize( private_key_buf, private_key_bytes );
		//MUST NOT free private_key_buf !!!
	}

	syslog(LOG_INFO, "http: server listening on port %d\n", config->port);

	int client;
	struct sockaddr_storage client_addr;
	socklen_t client_addr_len = sizeof(client_addr);

	http_refcount++;
	while (!http_shutdown) {

		// Wait for a request ...
		if ((client = accept(*config->server, (struct sockaddr *)&client_addr, &client_addr_len)) != -1) {

			// We wait for send all data before close socket's stream
			struct linger so_linger;
			so_linger.l_onoff  = 1;
			so_linger.l_linger = 2;
			setsockopt(client, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));

			// Set a timeout for send / receive
			struct timeval timeout = {60L, 0L}; /* 1 minute to send all data */
			setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
			setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

			if (config->secure) {
				ssl = SSL_new(ctx);
				if (!ssl) {
					syslog(LOG_ERR, "http: couldn't create SSL session\n");
					break; //exit the loop to shutdown the server
				}

				SSL_set_fd(ssl, client);

				if (!(rc = SSL_accept(ssl))) {
					int err_SSL_get_error = SSL_get_error(ssl, rc);
					if (err_SSL_get_error == SSL_ERROR_SYSCALL) {
						if (errno != EAGAIN && errno != ECONNRESET) {
							syslog(LOG_ERR, "http: couldn't accept SSL connection - RC %d errno %d %s\n", rc, errno, strerror(errno));
						}
					} else {
						syslog(LOG_ERR, "http: couldn't accept SSL connection - SSL_get_error() returned: %i\n", err_SSL_get_error);
					}
				} else {
					http_request_handle request = HTTP_Request_Secure_initializer;
					process(&request);
				}

				SSL_shutdown(ssl);
				SSL_free(ssl);
				ssl = NULL;
			}
			else
			{
				http_request_handle request = HTTP_Request_Normal_initializer;
				process(&request);
				shutdown(client, SHUT_RDWR);
			}

			close(client);
			client = -1;

			//make sure external systems can't occupy our whole cpu...
			vTaskDelay(1 / portTICK_PERIOD_MS);
		}

		if (script_wants_reboot) {
			delay(2); //probably required for data to be sent
			backtrace_count = 0;
			esp_restart(); /* restart without panic'ing */
		}
	}

	if (config->secure) {
		SSL_CTX_free(ctx);
		ctx = NULL;

		free(config->certificate);
		config->certificate = NULL;

		free(config->private_key);
		config->private_key = NULL;
	}

	syslog(LOG_INFO, "http: server shutting down on port %d\n", config->port);

	/* it's not ideal to keep the server_socket open as it is blocked
	   but now at least the httpsrv can be restarted from lua scripts.
	   if we'd properly close the socket we would need to bind() again
	   when restarting the httpsrv but bind() will succeed only after
	   several (~4) minutes: http://lwip.wikia.com/wiki/Netconn_bind
	   "Note that if you try to bind the same address and/or port you
	    might get an error (ERR_USE, address in use), even if you
	    delete the netconn. Only after some time (minutes) the
	    resources are completely cleared in the underlying stack due
	    to the need to follow the TCP specification and go through
	    the TCP timewait state."
	*/

	http_refcount--;

	if (0 == http_refcount) {
		//last one needs to unregister the net callback
		driver_error_t *error;
		if ((error = net_event_unregister_callback(http_net_callback))) {
			syslog(LOG_WARNING, "http: couldn't unregister net callback\n");
		}

		//last one needs to unregister the lua execution callback
		if (http_callback != NULL) {
			luaS_callback_destroy(http_callback);
			http_callback = NULL;
		}
	}

	return NULL;
}

int http_start(lua_State* L) {
	//wait until an ongoing shutdown has been finished
	while(http_refcount && http_shutdown) delay(10);

	if(!http_refcount) {
		pthread_attr_t attr;
		struct sched_param sched;
		pthread_t thread_normal;
		pthread_t thread_secure;
		int res;
		driver_error_t *error;

		// Create document root directory if not exist
		mkpath(CONFIG_LUA_RTOS_HTTP_SERVER_DOCUMENT_ROOT);

		// Prepare a callback to execute lua code
		if (http_callback != NULL) {
			luaS_callback_destroy(http_callback);
			http_callback = NULL;
		}
		lua_pushcfunction(L, &http_execute_lua);  /* to call 'http_execute_lua' in protected mode */
		http_callback = luaS_callback_create(L, -1);
		lua_pop(L, 1);

		ap_ip4addr = 0;
		strcpy(ap_ip4addr_str, "0.0.0.0");

		esp_log_level_set("wifi", ESP_LOG_NONE);
		if ((error = wifi_check_error(esp_wifi_get_mode(&wifi_mode)))) {
			esp_log_level_set("wifi", ESP_LOG_ERROR);
			free(error);
		} else {
			//get the IP address of the AP explicitly
			tcpip_adapter_ip_info_t esp_info;
			if ((error = wifi_check_error(tcpip_adapter_get_ip_info(ESP_IF_WIFI_AP, &esp_info)))) {
				esp_log_level_set("wifi", ESP_LOG_ERROR);
				free(error);
			}
			else {
				ap_ip4addr = esp_info.ip.addr & IP_CLASSA_HOST; //use IP_CLASSA_HOST instead of IP_CLASSC_NET as the IP address is "reversed"
			}
			strcpy(ap_ip4addr_str, inet_ntoa(esp_info.ip.addr));
		}

		esp_log_level_set("wifi", ESP_LOG_ERROR);

		if ((error = net_event_register_callback(http_net_callback))) {
			syslog(LOG_WARNING, "http: couldn't register net callback, please restart http service from lua after changing connectivity\n");
		}

		// Init thread attributes
		pthread_attr_init(&attr);

		// Set stack size
		pthread_attr_setstacksize(&attr, CONFIG_LUA_RTOS_HTTP_SERVER_STACK_SIZE);

		// Set priority
		sched.sched_priority = CONFIG_LUA_RTOS_HTTP_SERVER_TASK_PRIORITY;
		pthread_attr_setschedparam(&attr, &sched);

		// Set CPU
		cpu_set_t cpu_set = CPU_INITIALIZER;
		CPU_SET(CONFIG_LUA_RTOS_HTTP_SERVER_TASK_CPU, &cpu_set);

		pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpu_set);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

		// Create threads
		http_shutdown = 0;

		http_normal.port = luaL_optinteger( L, 1, CONFIG_LUA_RTOS_HTTP_SERVER_PORT );
		if (http_normal.port) {
			res = pthread_create(&thread_normal, &attr, http_thread, &http_normal);
			if (res) {
				return luaL_error(L, "couldn't start http_thread");
			}

			pthread_setname_np(thread_normal, "http");
		}

		http_secure.port = luaL_optinteger( L, 2, CONFIG_LUA_RTOS_HTTP_SERVER_PORT_SSL );
		const char *certificate = luaL_optstring( L, 3, NULL );
		const char *private_key = luaL_optstring( L, 4, NULL );
		http_secure.certificate = certificate ? strdup(certificate) : NULL;
		http_secure.private_key = private_key ? strdup(private_key) : NULL;

		if ( http_secure.port && http_secure.certificate && http_secure.private_key ) {
			res = pthread_create(&thread_secure, &attr, http_thread, &http_secure);
			if (res) {
				return luaL_error(L, "couldn't start secure http_thread");
			}

			pthread_setname_np(thread_secure, "https");
		}

		pthread_attr_destroy(&attr);
	}

	return 0;
}

void http_stop() {
	if(http_refcount) {
		http_shutdown++;
	}
}

int http_running() {
	return (http_refcount>0 ? 1 : 0);
}

#endif
