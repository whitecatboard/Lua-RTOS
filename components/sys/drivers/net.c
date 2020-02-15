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
 * Lua RTOS, network manager
 *
 */

#include "sdkconfig.h"

#if CONFIG_LUA_RTOS_LUA_USE_NET

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_eth.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include <esp_task_wdt.h>

#if CONFIG_LUA_RTOS_LUA_USE_MDNS
#include <mdns.h>
#endif

#include "nvs.h"
#include "nvs_flash.h"

#include "openssl/ssl.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <sys/status.h>
#include <sys/delay.h>

#include <drivers/net.h>
#include <drivers/net_http.h>
#include <drivers/wifi.h>
#include <drivers/eth.h>

#include <lwip/ping.h>

// Register drivers and errors
DRIVER_REGISTER_BEGIN(NET,net,0,NULL,NULL);
    DRIVER_REGISTER_ERROR(NET, net, NotAvailable, "network is not available", NET_ERR_NOT_AVAILABLE);
    DRIVER_REGISTER_ERROR(NET, net, InvalidIpAddr, "invalid IP adddress", NET_ERR_INVALID_IP);
    DRIVER_REGISTER_ERROR(NET, net, NoMoreCallbacksAvailable, "no more callbacks available", NET_ERR_NO_MORE_CALLBACKS);
    DRIVER_REGISTER_ERROR(NET, net, CallbackNotFound, "callback not found", NET_ERR_NO_CALLBACK_NOT_FOUND);
    DRIVER_REGISTER_ERROR(NET, net, NameCannotBeResolved, "name cannot be resolved", NET_ERR_NAME_CANNOT_BE_RESOLVED);
    DRIVER_REGISTER_ERROR(NET, net, CannotCreateSocket, "cannot create socket", NET_ERR_NAME_CANNOT_CREATE_SOCKET);
    DRIVER_REGISTER_ERROR(NET, net, CannotSetupSocket, "cannot setup socket", NET_ERR_NAME_CANNOT_SETUP_SOCKET);
    DRIVER_REGISTER_ERROR(NET, net, CannotConnect, "cannot connect to server", NET_ERR_NAME_CANNOT_CONNECT);
    DRIVER_REGISTER_ERROR(NET, net, CannotCreateSSL, "cannot create SSL", NET_ERR_CANNOT_CREATE_SSL);
    DRIVER_REGISTER_ERROR(NET, net, CannotConnectSSL, "cannot connect SSL", NET_ERR_CANNOT_CONNECT_SSL);
    DRIVER_REGISTER_ERROR(NET, net, NotEnoughtMemory, "not enough memory", NET_ERR_NOT_ENOUGH_MEMORY);
    DRIVER_REGISTER_ERROR(NET, net, InvalidResponse, "invalid response", NET_ERR_INVALID_RESPONSE);
    DRIVER_REGISTER_ERROR(NET, net, InvalidContent, "invalid content", NET_ERR_INVALID_CONTENT);
    DRIVER_REGISTER_ERROR(NET, net, NoOTA, "OTA partition not found", NET_ERR_NO_OTA);
    DRIVER_REGISTER_ERROR(NET, net, OTANotEnabled, "OTA is not enabled in this build", NET_ERR_OTA_NOT_ENABLED);
DRIVER_REGISTER_END(NET,net,0,NULL,NULL);

// FreeRTOS events used by driver
EventGroupHandle_t netEvent;

// Retries for connect
static uint8_t connect_retries = 0;

// Event callbacks
static net_event_register_callback_t callback[MAX_NET_EVENT_CALLBACKS] = {0};

/*
 * Helper functions
 */
static esp_err_t event_handler(void *ctx, system_event_t *event) {
    EventBits_t bits = 0;

    switch (event->event_id) {
        case SYSTEM_EVENT_WIFI_READY: // SP32 WiFi ready
            break;

        case SYSTEM_EVENT_SCAN_DONE: // ESP32 finish scanning AP
            bits |= evWIFI_SCAN_END;
            break;

        // STA events
        case SYSTEM_EVENT_STA_START: // ESP32 station start
            status_set(0x00000000, STATUS_WIFI_CONNECTED | STATUS_WIFI_HAS_IP);
            esp_wifi_connect();
            break;

        case SYSTEM_EVENT_STA_STOP: // ESP32 station stop
            status_set(0x00000000, STATUS_WIFI_CONNECTED | STATUS_WIFI_HAS_IP);
            break;

        case SYSTEM_EVENT_STA_CONNECTED: // ESP32 station connected to AP
            status_set(STATUS_WIFI_CONNECTED, 0x00000000);
            tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_STA);
            break;

        case SYSTEM_EVENT_STA_DISCONNECTED: // ESP32 station disconnected from AP */
            if (status_get(STATUS_WIFI_SYNC) && (connect_retries > WIFI_CONNECT_RETRIES)) {
                bits |= evWIFI_CANT_CONNECT;
                status_set(0x00000000, STATUS_WIFI_CONNECTED);
                connect_retries = 0;
            } else {
                status_set(0x00000000, STATUS_WIFI_CONNECTED);
                if (status_get(STATUS_WIFI_STARTED)) {
                    if (status_get(STATUS_WIFI_SYNC)) {
                        connect_retries++;
                    }
                    delay(200);
                    esp_wifi_connect();
                }
            }
            break;

        case SYSTEM_EVENT_STA_GOT_IP: // ESP32 station got IP from connected AP
            status_set(STATUS_WIFI_HAS_IP, 0x00000000);
            bits |= evWIFI_CONNECTED;
            break;

        case SYSTEM_EVENT_STA_LOST_IP: // ESP32 station lost IP and the IP is reset to 0
            status_set(0x00000000, STATUS_WIFI_HAS_IP);
            break;

        case SYSTEM_EVENT_STA_AUTHMODE_CHANGE: // The auth mode of AP connected by ESP32 station changed */
            break;

        // STA WPS events
        case SYSTEM_EVENT_STA_WPS_ER_SUCCESS: // ESP32 station wps succeeds in enrollee mode */
            wifi_wps_disable();
            esp_wifi_connect();
            break;

        case SYSTEM_EVENT_STA_WPS_ER_FAILED: // ESP32 station wps fails in enrollee mode
            wifi_wps_reconnect();
            break;

        case SYSTEM_EVENT_STA_WPS_ER_TIMEOUT:       /**< ESP32 station wps timeout in enrollee mode */
            wifi_wps_reconnect();
            break;

        case SYSTEM_EVENT_STA_WPS_ER_PIN:           /**< ESP32 station wps pin code in enrollee mode */
            wifi_wps_pin(event->event_info.sta_er_pin.pin_code);
            break;

        case SYSTEM_EVENT_AP_START:                 /**< ESP32 soft-AP start */
            tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_AP);
            break;

        case SYSTEM_EVENT_AP_STOP:                  /**< ESP32 soft-AP stop */
            status_set(0x00000000, STATUS_WIFI_CONNECTED | STATUS_WIFI_INITED);
            break;

        case SYSTEM_EVENT_AP_STACONNECTED:          /**< a station connected to ESP32 soft-AP */
            break;

        case SYSTEM_EVENT_AP_STADISCONNECTED:       /**< a station disconnected from ESP32 soft-AP */
            break;

        case SYSTEM_EVENT_AP_PROBEREQRECVED:        /**< Receive probe request packet in soft-AP interface */
            break;

        case SYSTEM_EVENT_AP_STA_GOT_IP6:           /**< ESP32 station or ap interface v6IP addr is preferred */
            bits |= evWIFI_CONNECTED;
            break;

#if CONFIG_LUA_RTOS_ETH_HW_TYPE_RMII
        // ETH events
        case SYSTEM_EVENT_ETH_START: // ESP32 ethernet start
            status_set(STATUS_ETH_STARTED, STATUS_ETH_CONNECTED | STATUS_ETH_HAS_IP);
            break;
        case SYSTEM_EVENT_ETH_STOP: // ESP32 ethernet stop
            status_set(0x00000000, STATUS_ETH_STARTED | STATUS_ETH_CONNECTED | STATUS_ETH_HAS_IP);
            break;

        case SYSTEM_EVENT_ETH_CONNECTED: // ESP32 ethernet phy link up
            status_set(STATUS_ETH_CONNECTED, STATUS_ETH_HAS_IP);
            tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_ETH);
            break;

        case SYSTEM_EVENT_ETH_DISCONNECTED: // ESP32 ethernet phy link down
            status_set(0x00000000, STATUS_ETH_CONNECTED | STATUS_ETH_HAS_IP);
            bits |= evETH_CANT_CONNECT;
            break;

        case SYSTEM_EVENT_ETH_GOT_IP: // ESP32 ethernet got IP from connected AP
            status_set(STATUS_ETH_HAS_IP, 0x00000000);
            bits |= evETH_CONNECTED;
            break;
#endif

#if CONFIG_LUA_RTOS_ETH_HW_TYPE_SPI
        // ETH SPI events
        case SYSTEM_EVENT_SPI_ETH_START: // ESP32 spi ethernet start
            status_set(STATUS_SPI_ETH_STARTED, STATUS_SPI_ETH_CONNECTED | STATUS_SPI_ETH_HAS_IP);
            break;

        case SYSTEM_EVENT_SPI_ETH_STOP: // ESP32 spi ethernet stop
            status_set(0x00000000, STATUS_SPI_ETH_STARTED | STATUS_SPI_ETH_CONNECTED | STATUS_SPI_ETH_HAS_IP);
            break;

        case SYSTEM_EVENT_SPI_ETH_CONNECTED: // ESP32 spi ethernet phy link up
            status_set(STATUS_SPI_ETH_CONNECTED, STATUS_SPI_ETH_HAS_IP);
            tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_SPI_ETH);
            break;

        case SYSTEM_EVENT_SPI_ETH_DISCONNECTED: // ESP32 spi ethernet phy link down
            status_set(0x00000000, STATUS_SPI_ETH_CONNECTED | STATUS_SPI_ETH_HAS_IP);
            bits |= evETH_CANT_CONNECT;
            break;

        case SYSTEM_EVENT_SPI_ETH_GOT_IP: // ESP32 spi ethernet got IP from connected AP
            status_set(STATUS_SPI_ETH_HAS_IP, 0x00000000);
            bits |= evSPI_ETH_CONNECTED;
            break;
#endif
        default :
            break;
    }

#if CONFIG_LUA_RTOS_LUA_USE_MDNS
    mdns_handle_system_event(ctx, event);
#endif

    // Call to the registered callbacks
    for(int i=0; i < MAX_NET_EVENT_CALLBACKS; i++) {
        if (callback[i]) {
            callback[i](event);
        }
    }

    if (bits) {
        xEventGroupSetBits(netEvent, bits);
    }

    return ESP_OK;
}

/*
 * Operation functions
 */
driver_error_t *net_init() {
    if (!status_get(STATUS_TCPIP_INITED)) {
        connect_retries = 0;

        netEvent = xEventGroupCreate();

        tcpip_adapter_init();

        esp_event_loop_init(event_handler, NULL);

        status_set(STATUS_TCPIP_INITED, 0x00000000);
    }

    return NULL;
}

driver_error_t *net_check_connectivity() {
    if (!NETWORK_AVAILABLE()) {
        return driver_error(NET_DRIVER, NET_ERR_NOT_AVAILABLE,NULL);
    }

    return NULL;
}

driver_error_t *net_lookup(const char *name, int port, struct sockaddr_in *address) {
    int rc = 0;
    int lookup_retries = 0;

retry:
    if (!wait_for_network(20000)) {
        return driver_error(NET_DRIVER, NET_ERR_NOT_AVAILABLE,NULL);
    }

    sa_family_t family = AF_INET;
    struct addrinfo *result = NULL;
    struct addrinfo hints = {0, AF_INET, SOCK_STREAM, IPPROTO_TCP, 0, NULL, NULL, NULL};

    if ((rc = getaddrinfo(name, NULL, &hints, &result)) == 0) {
        struct addrinfo *res = result;
        while (res) {
            if (res->ai_family == AF_INET) {
                result = res;
                break;
            }
            res = res->ai_next;
        }

        if (result->ai_family == AF_INET) {
            address->sin_port = htons(port);
            address->sin_family = family = AF_INET;
            address->sin_addr = ((struct sockaddr_in*)(result->ai_addr))->sin_addr;
        }

        freeaddrinfo(result);
    } else {
        lookup_retries++;
        if (lookup_retries < 4) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            goto retry;
        }

        return driver_error(NET_DRIVER, NET_ERR_NAME_CANNOT_BE_RESOLVED,NULL);
    }

    return NULL;
}

driver_error_t *net_event_register_callback(net_event_register_callback_t func) {
    int i = 0;

    for(i=0; i < MAX_NET_EVENT_CALLBACKS; i++) {
        if (callback[i] == 0) {
            callback[i] = func;
            return NULL;
        }
    }

    return driver_error(NET_DRIVER, NET_ERR_NO_MORE_CALLBACKS,NULL);
}

driver_error_t *net_event_unregister_callback(net_event_register_callback_t func) {
    int i = 0;

    for(i=0; i < MAX_NET_EVENT_CALLBACKS; i++) {
        if (callback[i] == func) {
            callback[i] = NULL;
            return NULL;
        }
    }

    return driver_error(NET_DRIVER, NET_ERR_NO_MORE_CALLBACKS,NULL);
}

driver_error_t *net_ping(const char *name, int count, int interval, int size, int timeout) {
    driver_error_t *error;

    if ((error = net_check_connectivity())) return error;

    if ((error = ping(name, count, interval, size, timeout))) return error;

    return NULL;
}

driver_error_t *net_reconnect() {
    return NULL;
}

int network_started() {
    return (status_get(STATUS_WIFI_STARTED) | status_get(STATUS_SPI_ETH_STARTED) | status_get(STATUS_ETH_STARTED));
}

int wait_for_network_init(uint32_t timeout) {
    while ((timeout > 0) && (!status_get(STATUS_TCPIP_INITED))) {
        vTaskDelay(1/portTICK_PERIOD_MS);
        timeout--;
    }

    return status_get(STATUS_TCPIP_INITED);
}

int wait_for_network(uint32_t timeout) {
    TickType_t ticks_to_wait;

    if (timeout == 0) {
        ticks_to_wait = portMAX_DELAY;
    } else {
        ticks_to_wait = timeout / portTICK_PERIOD_MS;

    }

    uint32_t elapsed = 0;
    while ((!status_get(STATUS_TCPIP_INITED)) && (elapsed < timeout)) {
        delay(1);
        elapsed++;
    }

    if (!status_get(STATUS_TCPIP_INITED)) {
        return 0;
    }

    if (!NETWORK_AVAILABLE()) {
        EventBits_t uxBits = xEventGroupWaitBits(
                netEvent,
                evWIFI_CONNECTED | evWIFI_CANT_CONNECT |
                evSPI_ETH_CONNECTED | evSPI_ETH_CANT_CONNECT |
                evETH_CONNECTED | evETH_CANT_CONNECT,
                pdTRUE, pdFALSE, ticks_to_wait
        );

        if (uxBits & (evWIFI_CONNECTED | evSPI_ETH_CONNECTED | evETH_CONNECTED)) {
            return 1;
        }

        if (uxBits & (evWIFI_CANT_CONNECT | evSPI_ETH_CANT_CONNECT | evETH_CANT_CONNECT)) {
            return 0;
        }
    }

    return 1;
}

driver_error_t *net_ota(const char *server, const char *project, int verify, int reboot) {
#if CONFIG_LUA_RTOS_USE_OTA
    driver_error_t *error;
    net_http_client_t client = HTTP_CLIENT_INITIALIZER;
    net_http_response_t response;
    esp_ota_handle_t update_handle = 0 ;
    uint8_t buffer[1024];
    esp_err_t err;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (!update_partition) {
        return driver_error(NET_DRIVER, NET_ERR_NO_OTA,NULL);    }

    if ((error = net_check_connectivity())) {
        return error;
    }

    printf("Connecting to https://%s ...\r\n", server ? server : CONFIG_LUA_RTOS_OTA_SERVER_NAME);
    if ((error = net_http_create_client(server ? server : CONFIG_LUA_RTOS_OTA_SERVER_NAME, "443", &client))) {
        return error;
    }

    printf("Current firmware commit is %s\r\n", BUILD_COMMIT);

    if (NULL != project) {
        snprintf((char *)buffer, sizeof(buffer), "/?firmware=%s&commit=%s&project=%s", CONFIG_LUA_RTOS_FIRMWARE, BUILD_COMMIT, project);
    }
    else {
        snprintf((char *)buffer, sizeof(buffer), "/?firmware=%s&commit=%s", CONFIG_LUA_RTOS_FIRMWARE, BUILD_COMMIT);
    }

    if ((error = net_http_get(&client, (const char *)buffer, "application/octet-stream", &response))) {
        net_http_destroy_client(&client);
        return error;
    }

    if ((response.code == 200) && (response.size > 0)) {
        printf(
            "Running from %s at offset 0x%08x\r\n",
             running->label, running->address
        );

        printf(
            "Writing to   %s at offset 0x%08x - capacity is %.2f MB\r\n",
            update_partition->label, update_partition->address,
            (((float)update_partition->size)/(1024.0*1024.0))
        );

        float total = (float)response.size;

        esp_task_wdt_reset();

        printf("Starting OTA update, downloading partition image with %.2f MB\r\n", (total/(1024.0*1024.0)) );
        err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
        if (err != ESP_OK) {
            printf("OTA update failed, error %d\r\n", err);
            net_http_destroy_client(&client);
            return NULL;
        }

        uint32_t address = update_partition->address;

        while (response.size > 0){
            if ((error = net_http_read_response(&response, buffer, sizeof(buffer)))) {
                net_http_destroy_client(&client);
                return error;
            }

            err = esp_ota_write(update_handle, buffer, response.len);
            if (err != ESP_OK) {
                printf("\nError while writing chunk to 0x%08x, error %d\r\n", address, err);
                net_http_destroy_client(&client);
                return NULL;
            } else {
                int pct = (int)(((float)(address-update_partition->address))/total*100);
                if (0 == pct%10) esp_task_wdt_reset();
                printf("\rWriting chunk at 0x%08x... (%i %%)", address, pct);
            }

            address = address + response.len;
        }

        printf("\rWriting chunk at 0x%08x... (%i %%)\r\n", address, 100 );

        esp_task_wdt_reset();

        if (esp_ota_end(update_handle) != ESP_OK) {
            printf("OTA transfer complete, update failed\r\n");
            net_http_destroy_client(&client);
            return NULL;
        }
    } else if (response.code == 470) {
        printf("Missing or bad arguments\r\n");
        net_http_destroy_client(&client);
        return NULL;
    } else if (response.code == 471) {
        printf("No new firmware available\r\n");
        net_http_destroy_client(&client);
        return NULL;
    } else {
        printf("Unexpected error, response code %i, size %i\r\n", response.code, response.size);
        net_http_destroy_client(&client);
        return NULL;
    }

    if ((error = net_http_destroy_client(&client))) {
        return error;
    }

    if (0 != verify) {
        esp_image_metadata_t data = { 0 };
        const esp_partition_pos_t partition_pos  = {
            .offset = update_partition->address,
            .size = update_partition->size,
        };
        err = esp_image_verify(ESP_IMAGE_VERIFY, &partition_pos, &data);
        if (err != ESP_OK) {
            printf("OTA update finished but the written image is not valid, error %d\r\n", err);
            return NULL;
        }
    }

    printf("OTA update finished\r\n");

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        printf("Changing the boot partition failed, error %d\r\n", err);
        return NULL;
    } else {
        printf("Successfully changed boot partition\r\n");
    }

    if (response.code == 200) {
        if (0 != reboot) {
            printf("Restarting ...\r\n");
            esp_restart();
        }
    }

    return NULL;
#else
    return driver_error(NET_DRIVER, NET_ERR_OTA_NOT_ENABLED,NULL);
#endif
}

#endif
