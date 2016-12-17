/*
 * Lua RTOS, UART driver
 *
 * Copyright (C) 2015 - 2016
 * IBEROXARXA SERVICIOS INTEGRALES, S.L. & CSS IBÉRICA, S.L.
 * 
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 * 
 * All rights reserved.  
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

/*
 * ESPRSSIF MIT License
 *
 * Copyright (c) 2015 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on ESPRESSIF SYSTEMS ESP8266 only, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "luartos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/xtensa_api.h"

#include "esp_types.h"
#include "esp_err.h"
#include "esp_intr.h"
#include "esp_attr.h"
#include "soc/soc.h"
#include "soc/uart_reg.h"
#include "soc/io_mux_reg.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>

#include <sys/status.h>
#include <sys/driver.h>
#include <sys/syslog.h>
#include <sys/delay.h>

#include <pthread/pthread.h>
#include <drivers/uart.h>
#include <drivers/gpio.h>
#include <drivers/cpu.h>

const char *uart_errors[] = {
	"",
	"can't setup",
};

// Flags for determine some UART states
#define UART_FLAG_INIT		(1 << 1)
#define UART_FLAG_IRQ_INIT	(1 << 2)

#define ETS_UART_INUM  5
#define UART_INTR_SOURCE(u) ((u==0)?ETS_UART0_INTR_SOURCE:( (u==1)?ETS_UART1_INTR_SOURCE:((u==2)?ETS_UART2_INTR_SOURCE:0)))

// UART names
static const char *names[] = {
	"uart0",
	"uart1",
	"uart2",
};

// UART array
struct uart {
    uint8_t          flags;
    QueueHandle_t    q;         // RX queue
    uint16_t         qs;        // Queue size
    uint32_t         brg;       // Baud rate
    pthread_mutex_t  mtx;		// Mutex
};

static struct uart uart[NUART] = {
    {
        0, NULL, 0, 115200, PTHREAD_MUTEX_INITIALIZER
    },
    {
        0, NULL, 0, 115200, PTHREAD_MUTEX_INITIALIZER
    },
    {
        0, NULL, 0, 115200, PTHREAD_MUTEX_INITIALIZER
    },
};

/*
 * Helper functions
 */

// Configure the UART comm parameters
static void uart_comm_param_config(int8_t unit, UartBautRate brg, UartBitsNum4Char data, UartParityMode parity, UartStopBitsNum stop) {
	wait_tx_empty(unit);

	uart_div_modify(unit, (APB_CLK_FREQ << 4) / brg);

    WRITE_PERI_REG(UART_CONF0_REG(unit),
                   ((parity == NONE_BITS) ? 0x0 : (UART_PARITY_EN | parity))
                   | (stop << UART_STOP_BIT_NUM_S)
                   | (data << UART_BIT_NUM_S
                   | UART_TICK_REF_ALWAYS_ON_M));
}

// Configure the UART pins
static void uart_pin_config(int8_t unit, uint8_t rx, uint8_t tx) {
	wait_tx_empty(unit);

	switch (unit) {
		case 0:
			// Enable UTX0
			PIN_PULLUP_DIS(PERIPHS_IO_MUX_U0TXD_U);
			PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD_U0TXD);

			// Enable U0RX
	        PIN_PULLUP_EN(PERIPHS_IO_MUX_U0RXD_U);
	        PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_U0RXD_U0RXD);

			break;

		case 1:
			// Enable U1TX
			PIN_PULLUP_DIS(PERIPHS_IO_MUX_SD_DATA3_U);
			PIN_FUNC_SELECT(PERIPHS_IO_MUX_SD_DATA3_U, FUNC_SD_DATA3_U1TXD);

			// Enable U1RX
	        PIN_PULLUP_EN(PERIPHS_IO_MUX_SD_DATA2_U);
	        PIN_FUNC_SELECT(PERIPHS_IO_MUX_SD_DATA2_U, FUNC_SD_DATA2_U1RXD);

			break;

		case 2:
			// Enable U2TX
			PIN_PULLUP_DIS(PERIPHS_IO_MUX_GPIO17_U);
			PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO17_U, FUNC_GPIO17_U2TXD);

			// Enable U2RX
	        PIN_PULLUP_EN(PERIPHS_IO_MUX_GPIO16_U);
	        PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO16_U, FUNC_GPIO16_U2RXD);

			break;
	}}

// Determine if byte must be queued
static int IRAM_ATTR queue_byte(int8_t unit, uint8_t byte, int *signal) {
	*signal = 0;

    if (unit == CONSOLE_UART) {
        if (byte == 0x04) {
            if (!status_get(STATUS_LUA_RUNNING)) {
            	uart_lock(CONSOLE_UART);
                uart_writes(CONSOLE_UART, "Lua RTOS-booting-ESP32\r\n");
            	uart_unlock(CONSOLE_UART);
            } else {
            	uart_lock(CONSOLE_UART);
                uart_writes(CONSOLE_UART, "Lua RTOS-running-ESP32\r\n");
            	uart_unlock(CONSOLE_UART);
            }

			status_set(STATUS_LUA_ABORT_BOOT_SCRIPTS);

            return 0;
        } else if (byte == 0x03) {
        	if (status_get(STATUS_LUA_RUNNING)) {
				*signal = SIGINT;
				if (_pthread_has_signal(*signal)) {
					return 0;
				}

				return 1;
        	} else {
        		return 0;
        	}
        }
    }
	
	if (status_get(STATUS_LUA_RUNNING)) {
		return 1;
	} else {
		return 0;
	}
}

/*
 * Operation functions
 */

void IRAM_ATTR uart_lock(int unit) {
	pthread_mutex_lock(&uart[unit].mtx);
}

void IRAM_ATTR uart_unlock(int unit) {
	pthread_mutex_unlock(&uart[unit].mtx);
}

void uart_pins(int8_t unit, uint8_t *rx, uint8_t *tx) {
	switch (unit) {
		case 0:
			if (rx) *rx = GPIO3;
			if (tx) *tx = GPIO1;

			break;

		case 1:
			if (rx) *rx = GPIO9;
			if (tx) *tx = GPIO10;

			break;

		case 2:
			if (rx) *rx = GPIO16;
			if (tx) *tx = GPIO17;

			break;
	}
}

void  IRAM_ATTR uart_rx_intr_handler(void *para) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint32_t uart_intr_status = 0;
	uint8_t byte;
	int signal = 0;
	int unit = 0;

	for(;unit < NUART;unit++) {
		if (!(uart[unit].flags & UART_FLAG_INIT)) continue;

		uart_intr_status = READ_PERI_REG(UART_INT_ST_REG(unit)) ;

	    while (uart_intr_status != 0x0) {
	        if (UART_FRM_ERR_INT_ST == (uart_intr_status & UART_FRM_ERR_INT_ST)) {
	            WRITE_PERI_REG(UART_INT_CLR_REG(unit), UART_FRM_ERR_INT_CLR);
	        } else if (UART_RXFIFO_FULL_INT_ST == (uart_intr_status & UART_RXFIFO_FULL_INT_ST)) {
	            WRITE_PERI_REG(UART_INT_CLR_REG(unit), UART_RXFIFO_FULL_INT_CLR);

	            while ((READ_PERI_REG(UART_STATUS_REG(unit)) >> UART_RXFIFO_CNT_S)&UART_RXFIFO_CNT) {
					byte = READ_PERI_REG(UART_FIFO_REG(unit)) & 0xFF;
					if (queue_byte(unit, byte, &signal)) {
			            // Put byte to UART queue
			            xQueueSendFromISR(uart[unit].q, &byte, &xHigherPriorityTaskWoken);
					} else {
						if (signal) {
							_pthread_queue_signal(signal);
						}
					}
	            }
	        } else if (UART_RXFIFO_TOUT_INT_ST == (uart_intr_status & UART_RXFIFO_TOUT_INT_ST)) {
	            WRITE_PERI_REG(UART_INT_CLR_REG(unit), UART_RXFIFO_TOUT_INT_CLR);

	            while ((READ_PERI_REG(UART_STATUS_REG(unit)) >> UART_RXFIFO_CNT_S)&UART_RXFIFO_CNT) {
					byte = READ_PERI_REG(UART_FIFO_REG(unit)) & 0xFF;
					if (queue_byte(unit, byte, &signal)) {
			            // Put byte to UART queue
			            xQueueSendFromISR(uart[unit].q, &byte, &xHigherPriorityTaskWoken);
					} else {
						if (signal) {
							_pthread_queue_signal(signal);
						}
					}
	            }
	        }

	        uart_intr_status = READ_PERI_REG(UART_INT_ST_REG(unit)) ;
	    }
	}

	portEND_SWITCHING_ISR(xHigherPriorityTaskWoken);
}

// Lock resources needed by the UART
driver_error_t *uart_lock_resources(int unit, void *resources) {
	uart_resources_t tmp_uart_resources;

	if (!resources) {
		resources = &tmp_uart_resources;
	}

	uart_resources_t *uart_resources = (uart_resources_t *)resources;
    driver_unit_lock_error_t *lock_error = NULL;

    uart_pins(unit, &uart_resources->rx, &uart_resources->tx);

    // Lock this pins
    if ((lock_error = driver_lock(UART_DRIVER, unit, GPIO_DRIVER, uart_resources->rx))) {
    	// Revoked lock on pin
    	return driver_lock_error(UART_DRIVER, lock_error);
    }

    if ((lock_error = driver_lock(UART_DRIVER, unit, GPIO_DRIVER, uart_resources->tx))) {
    	// Revoked lock on pin
    	return driver_lock_error(UART_DRIVER, lock_error);
    }

    return NULL;
}

// Init UART. Interrupts are not enabled.
driver_error_t *uart_init(int8_t unit, uint32_t brg, uint8_t databits, uint8_t parity, uint8_t stop_bits, uint32_t qs) {
	// Sanity checks
	if ((unit > CPU_LAST_UART) || (unit < CPU_FIRST_UART)) {
		return driver_setup_error(UART_DRIVER, UART_ERR_CANT_INIT, "invalid unit");
	}

	// Get data bits, and sanity checks
    UartBitsNum4Char esp_databits = EIGHT_BITS;
    switch (databits) {
    	case 5: esp_databits = FIVE_BITS; break;
    	case 6: esp_databits = SIX_BITS; break;
    	case 7: esp_databits = SEVEN_BITS; break;
    	case 8: esp_databits = EIGHT_BITS; break;
    	default:
    		return driver_setup_error(UART_DRIVER, UART_ERR_CANT_INIT, "invalid data bits");
    }

    // Get parity, and sanity checks
    UartParityMode esp_parity = NONE_BITS;
    switch (parity) {
    	case 0: esp_parity = NONE_BITS;break;
    	case 1: esp_parity = EVEN_BITS;break;
    	case 2: esp_parity = ODD_BITS;break;
    	default:
    		return driver_setup_error(UART_DRIVER, UART_ERR_CANT_INIT, "invalid parity");
    }

    // Get stop bits, and sanity checks
    UartStopBitsNum esp_stop_bits = ONE_STOP_BIT;
    switch (stop_bits) {
    	case 0: esp_stop_bits = ONE_HALF_STOP_BIT; break;
    	case 1: esp_stop_bits = ONE_STOP_BIT; break;
    	case 2: esp_stop_bits = TWO_STOP_BIT; break;
    	default:
    		return driver_setup_error(UART_DRIVER, UART_ERR_CANT_INIT, "invalid stop bits");
    }

    // Lock resources
    driver_error_t *error;
    uart_resources_t resources;

    if ((error = uart_lock_resources(unit, &resources))) {
		return error;
	}

	// There are not errors, continue with init ...

    // If requestes queue size is greater than current size, delete queue and create
	// a new one
    if (qs > uart[unit].qs) {
		if (uart[unit].q) {
			vQueueDelete(uart[unit].q);
		}

		uart[unit].q  = xQueueCreate(qs, sizeof(uint8_t));
		if (!uart[unit].q) {
    		return driver_setup_error(UART_DRIVER, UART_ERR_CANT_INIT, "not enough memory");
		}
	}

    // Init mutex, if needed
    if (uart[unit].mtx == PTHREAD_MUTEX_INITIALIZER) {
        pthread_mutexattr_t attr;

        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

        pthread_mutex_init(&uart[unit].mtx, &attr);
    }

    uart_pin_config(unit, resources.rx, resources.tx);
	uart_comm_param_config(unit, brg, esp_databits, esp_parity, esp_stop_bits);

    uart[unit].brg = brg; 
    uart[unit].qs  = qs; 

    uart[unit].flags |= UART_FLAG_INIT;

    syslog(LOG_INFO, "%s: at pins rx=%s%d/tx=%s%d",names[unit],
            gpio_portname(resources.rx), gpio_name(resources.rx),
            gpio_portname(resources.tx), gpio_name(resources.tx));

    syslog(LOG_INFO, "%s: speed %d bauds", names[unit],brg);

    return NULL;
}

// Enable UART interrupts
driver_error_t *uart_setup_interrupts(int8_t unit) {
	if ((unit > CPU_LAST_UART) || (unit < CPU_FIRST_UART)) {
		return driver_setup_error(UART_DRIVER, UART_ERR_CANT_INIT, "invalid unit");
	}

	if (uart[unit].flags & UART_FLAG_IRQ_INIT) {
        return NULL;
    }

    uint32_t reg_val = 0;
	uint32_t mask = UART_RXFIFO_TOUT_INT_ENA | UART_FRM_ERR_INT_ENA | UART_RXFIFO_FULL_INT_ENA;
		
	ESP_INTR_DISABLE(ETS_UART_INUM);

	// Update CONF1 register
    reg_val = READ_PERI_REG(UART_CONF1_REG(unit)) & ~((UART_RX_FLOW_THRHD << UART_RX_FLOW_THRHD_S) | UART_RX_FLOW_EN) ;

    reg_val |= ((mask & UART_RXFIFO_TOUT_INT_ENA) ?
                (((2 & UART_RX_TOUT_THRHD) << UART_RX_TOUT_THRHD_S) | UART_RX_TOUT_EN) : 0);

    reg_val |= ((mask & UART_RXFIFO_FULL_INT_ENA) ?
                ((10 & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S) : 0);

    reg_val |= ((mask & UART_TXFIFO_EMPTY_INT_ENA) ?
                ((20 & UART_TXFIFO_EMPTY_THRHD) << UART_TXFIFO_EMPTY_THRHD_S) : 0);

    WRITE_PERI_REG(UART_CONF1_REG(unit), reg_val);

	// Update INT_ENA register
    WRITE_PERI_REG(UART_INT_ENA_REG(unit), mask);
	
	intr_matrix_set(xPortGetCoreID(), UART_INTR_SOURCE(unit), ETS_UART_INUM);
	xt_set_interrupt_handler(ETS_UART_INUM, uart_rx_intr_handler, NULL);
	ESP_INTR_ENABLE(ETS_UART_INUM);
	
	syslog(LOG_INFO, "%s: interrupts enabled",names[unit]);

	return NULL;
}

// Writes a byte to the UART
void IRAM_ATTR uart_write(int8_t unit, char byte) {
    while (((READ_PERI_REG(UART_STATUS_REG(unit)) & (UART_TXFIFO_CNT << UART_TXFIFO_CNT_S)) >> UART_TXFIFO_CNT_S & UART_TXFIFO_CNT) >= 126);
    WRITE_PERI_REG(UART_FIFO_REG(unit), byte);
}

// Writes a null-terminated string to the UART
void IRAM_ATTR uart_writes(int8_t unit, char *s) {
    while (*s) {
	    while (((READ_PERI_REG(UART_STATUS_REG(unit)) & (UART_TXFIFO_CNT << UART_TXFIFO_CNT_S)) >> UART_TXFIFO_CNT_S & UART_TXFIFO_CNT) >= 126);
	    WRITE_PERI_REG(UART_FIFO_REG(unit) , *s++);
   }
}

// Reads a byte from uart
uint8_t IRAM_ATTR uart_read(int8_t unit, char *c, uint32_t timeout) {
    if (timeout != portMAX_DELAY) {
        timeout = timeout / portTICK_PERIOD_MS;
    }
	
    if (xQueueReceive(uart[unit].q, c, (TickType_t)(timeout)) == pdTRUE) {
        return 1;
    } else {
        return 0;
    }
}

// Consume all received bytes, and do not nothing with them
void uart_consume(int8_t unit) {
    char tmp;
    
    while(uart_read(unit,&tmp,1));
} 

// Reads a string from the UART, ended by the CR + LF character
uint8_t uart_reads(int8_t unit, char *buff, uint8_t crlf, uint32_t timeout) {
    char c;

    for (;;) {
        if (uart_read(unit, &c, timeout)) {
            if (c == '\0') {
                return 1;
            } else if (c == '\n') {
                *buff = 0;
                return 1;
            } else {
                if ((c == '\r') && !crlf) {
                    *buff = 0;
                    return 1;
                } else {
                    if (c != '\r') {
                        *buff++ = c;
                    }
                }
            }
        } else {
            return 0;
        }
    }

    return 0;
}

// Read from the UART and waits for a response
static uint8_t _uart_wait_response(int8_t unit, char *command, uint8_t echo, char *ret, uint8_t substring, uint32_t timeout, int nargs, va_list pargs) {
    int ok = 1;

    va_list args;
    
    char buffer[80];
    char *arg;

    // Test if we receive an echo of the command sended
    if ((command != NULL) && (echo)) {
        if (uart_reads(unit,buffer, 1, timeout)) {
            ok = (strcmp(buffer, command) == 0);
        } else {
            ok = 0;
        }
    }

    if (ok && nargs > 0) {
        ok = 0;

        // Read until we received expected response
        while (!ok) {
            if (uart_reads(unit,buffer, 1, timeout)) {
                args = pargs;

                int i;
                for (i = 0; i < nargs; i++) {
                    arg = va_arg(args, char *);
                    if (!substring) {
                        ok = ((strcmp(buffer, arg) == 0) || (strcmp(buffer, "ERROR") == 0));
                    } else {
                        ok = ((strstr(buffer, arg) != 0) || (strcmp(buffer, "ERROR") == 0));
                    }

                    if (ok) {
                        // If we expected for a return, copy
                        if (ret != NULL) {
                            strcpy(ret, buffer);
                        }

                        break;
                    }
                }

                if (strcmp(buffer, "ERROR") == 0) {
                    ok = 0;

                    break;
                }
            } else {
                ok = 0;
                break;
            }
        }
    }

    return ok;
}

// Read from the UART and waits for a response
uint8_t uart_wait_response(int8_t unit, char *command, uint8_t echo, char *ret, uint8_t substring, uint32_t timeout, int nargs, ...) {
    va_list pargs;

    va_start(pargs, nargs);

    uint8_t ok = _uart_wait_response(unit, command, echo, ret, substring, timeout, nargs, pargs);

    va_end(pargs);

    return ok;
}

// Sends a command to a device connected to the UART and waits for a response
uint8_t uart_send_command(int8_t unit, char *command, uint8_t echo, uint8_t crlf, char *ret, uint8_t substring, uint32_t timeout, int nargs, ...) {
    uint8_t ok = 0;

    uart_writes(unit,command);
    if (crlf) {
        uart_writes(unit,"\r\n");
    }

    va_list pargs;
    va_start(pargs, nargs);


    ok = _uart_wait_response(unit, command, echo, ret, substring, timeout, nargs, pargs);

    va_end(pargs);

    return ok;
}

// Gets the UART name
const char *uart_name(int8_t unit) {
    return names[unit - 1];
}

// Gets the UART queue
QueueHandle_t *uart_get_queue(int8_t unit) {
    return  uart[unit].q;
}

int uart_get_br(int unit) {
//    int divisor;
//    unit--;

//    reg = uart[unit].regs;
//    divisor = reg->brg;

//    return ((double)PBCLK2_HZ / (double)(16 * (divisor + 1)));
	return 0;
}

int uart_is_setup(int unit) {
    return ((uart[unit].flags & UART_FLAG_INIT) && (uart[unit].flags & UART_FLAG_IRQ_INIT));
}

void uart_stop(int unit) {
	int cunit = 0;

	for(cunit = 0;cunit < NUART; cunit++) {
		if ((unit == -1) || (cunit == unit)) {
		    WRITE_PERI_REG(UART_CONF0_REG(unit), 0);
		}
	}
}

