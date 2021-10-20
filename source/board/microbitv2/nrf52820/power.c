/**
 * @file    power.c
 * @brief
 *
 * DAPLink Interface Firmware
 * Copyright 2021 Micro:bit Educational Foundation
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "nrf.h"

#include "gpio.h"
#include "gpio_extra.h"
#include "main_interface.h"
#include "pwr_mon.h"
#include "IO_Config.h"
#include "DAP_config.h"
#include "uart.h"
#include "i2c.h"
#include "rl_usb.h"

#include "power.h"


static void power_before(bool systemoff);
static void power_after();
static void power_systemoff();
static void power_wfi();

void POWER_CLOCK_IRQHandler(void);
void GPIOTE_IRQHandler(void);


extern volatile uint8_t wake_from_reset;
extern volatile uint8_t wake_from_usb;
extern volatile bool usb_pc_connected;
extern main_usb_connect_t usb_state;
extern power_source_t power_source;


uint8_t  power_in_wfi;
uint8_t  power_gpiote_enabled;
uint32_t power_gpiote_intenset;


void power_init()
{
    power_in_wfi = 0;

    gpio_cfg_input(GPIO_REG(RESET_BUTTON), GPIO_IDX(RESET_BUTTON), RESET_BUTTON_PULL);

    // Enable NRF_POWER interrupt for USB removed/detected
    NRF_POWER->INTENSET = POWER_INTENSET_USBREMOVED_Msk | POWER_INTENSET_USBDETECTED_Msk;
    NVIC_EnableIRQ(POWER_CLOCK_IRQn);
}

void power_down()
{
    power_systemoff();
}

void power_sleep()
{
    power_wfi();
}


static void power_before(bool systemoff)
{
    // TODO - wait for USB transmit to computer to finish?
    /* Wait for debug console output finished. */
    // while (!(LPUART_STAT_TC_MASK & UART->STAT))
    // {
    // }

    uart_uninitialize();

    // TODO - I think maybe uart_uninitialize does this in USART_Uninitialize
    /* Disable pins to lower current leakage */
    // PORT_SetPinMux(UART_PORT, PIN_UART_RX_BIT, kPORT_PinDisabledOrAnalog);
    // PORT_SetPinMux(UART_PORT, PIN_UART_TX_BIT, kPORT_PinDisabledOrAnalog);

    gpio_disable_hid_led();
    
    /* Disable I/O pin SWCLK, SWDIO */
    PORT_OFF();

    if (systemoff) {
        i2c_deinitialize(); // disables I2C SCL & SDA
    }

    // Store NRF_GPIOTE state
    power_gpiote_enabled = NVIC_GetEnableIRQ(GPIOTE_IRQn);
    power_gpiote_intenset = NRF_GPIOTE->INTENSET;
    NRF_GPIOTE->INTENCLR = NRF_GPIOTE->INTENSET;

    // Enable IRQ from RESET_BUTTON 
    gpio_cfg(GPIO_REG(RESET_BUTTON),
            GPIO_IDX(RESET_BUTTON),
            NRF_GPIO_PIN_DIR_INPUT,
            NRF_GPIO_PIN_INPUT_CONNECT,
            RESET_BUTTON_PULL,
            NRF_GPIO_PIN_S0S1,
            RESET_BUTTON_PULL == NRF_GPIO_PIN_PULLUP ? GPIO_PIN_CNF_SENSE_Low : GPIO_PIN_CNF_SENSE_High);

    NRF_GPIOTE->INTENSET = power_gpiote_intenset | ( GPIOTE_INTENSET_PORT_Set << GPIOTE_INTENSET_PORT_Pos);
    NVIC_EnableIRQ(GPIOTE_IRQn);

    wake_from_usb = 0;
    wake_from_reset = 0;
}

static void power_after()
{
    // Restore GPIOTE state
    if (!power_gpiote_enabled) {
        NVIC_DisableIRQ(GPIOTE_IRQn);
    }
    NRF_GPIOTE->INTENSET = power_gpiote_intenset;

    // Disable RESET_BUTTON edge events
    gpio_cfg_input(GPIO_REG(RESET_BUTTON), GPIO_IDX(RESET_BUTTON), RESET_BUTTON_PULL);

    /* Configure I/O pin SWCLK, SWDIO */
    PORT_SWD_SETUP();
    
    // TODO - done by uart_initialize?
    // /* re-configure pinmux of disabled pins */
    // PORT_SetPinMux(UART_PORT, PIN_UART_RX_BIT, (port_mux_t)PIN_UART_RX_MUX_ALT);
    // PORT_SetPinMux(UART_PORT, PIN_UART_TX_BIT, (port_mux_t)PIN_UART_TX_MUX_ALT);

    uart_initialize();
    i2c_deinitialize();
    i2c_initialize();
}


static void power_systemoff()
{
    power_before(true /*systemoff*/);
    NRF_POWER->SYSTEMOFF = 1;
    //never get here
}

static void power_wfi()
{
    power_before(false /*systemoff*/);
    power_in_wfi = 1;
    __WFI();
    power_in_wfi = 0;
    power_after(false /*systemoff*/);
}


// TODO - It feels wrong to call pwr_mon_get_power_source() and the USB functions in the IRQ handler
void POWER_CLOCK_IRQHandler(void)
{
    if (NRF_POWER->EVENTS_USBDETECTED) {
        NRF_POWER->EVENTS_USBDETECTED = 0;
        power_source = pwr_mon_get_power_source();
        if (power_in_wfi) {
            wake_from_usb = 1;
        }
        usb_pc_connected = true;
    }

    if (NRF_POWER->EVENTS_USBREMOVED) {
        NRF_POWER->EVENTS_USBREMOVED = 0;
        power_source = pwr_mon_get_power_source();
        if (power_in_wfi) {
            wake_from_usb = 1;
        }
        /* Reset USB on cable detach (VBUS falling edge) */
        USBD_Reset();
        usbd_reset_core();
        usb_pc_connected = false;
        usb_state = USB_DISCONNECTED;
    }
}

void GPIOTE_IRQHandler(void)
{
    if (NRF_GPIOTE->EVENTS_PORT) {
        NRF_GPIOTE->EVENTS_PORT = 0;
        if (power_in_wfi && (GPIO_REG(RESET_BUTTON)->LATCH & (1 << GPIO_IDX(RESET_BUTTON)))) {
            wake_from_reset = 1;
        }
    }
}
