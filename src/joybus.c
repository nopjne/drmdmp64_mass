
/**
 * SPX-License-Identifier: BSD-2-Clause 
 * Copyright (c) 2023 - NopJne
 * 
 * N64CartInterface
 * Enables cartridge reading (0x1000'0000)
 * FlashRam/SRAM support     (0x0800'0000)
 * SI EEPROM support
 */

//Command Description   Console Devices  Tx Bytes Rx Bytes
//0x04    Read EEPROM   N64 Cartridge    2	      8
//0x05    Write EEPROM  N64 Cartridge    10	      1

#include <stdlib.h>
#include "pico/stdlib.h"

#include "pico/platform.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "generated/joybus.pio.h"
#include "joybus.h"

uint32_t ReadCount = 0;
uint32_t gEepromSize = 0;

void __time_critical_func(convertToPio)(const uint8_t* command, const int len, uint32_t* result, int* resultLen) {
    if (len == 0) {
        *resultLen = 0;
        return;
    }
    *resultLen = len/2 + 1;
    int i;
    for (i = 0; i < *resultLen; i++) {
        result[i] = 0;
    }
    for (i = 0; i < len; i++) {
        for (int j = 0; j < 8; j++) {
            result[i / 2] += (uint32_t)(1 << (2 * (8 * (i % 2) + j) + 1));
            result[i / 2] += (uint32_t)((!!(command[i] & (0x80u >> j))) << (2 * (8 * (i % 2) + j)));
        }
    }
    // End bit
    result[len / 2] += 3 << (2 * (8 * (len % 2)));
}

PIO pio = pio0;
PIO pio_1 = pio1;
void __time_critical_func(InitEepromClock)(uint clockpin)
{
    gpio_init(clockpin);
    gpio_set_dir(clockpin, GPIO_OUT);

    pio_gpio_init(pio_1, clockpin);

    uint offset_1 = pio_add_program(pio_1, &joybus_program);
    pio_sm_config config1 = joybus_program_get_default_config(offset_1);
    //sm_config_set_out_pins(&config1, clockpin, 1);
    sm_config_set_set_pins(&config1, clockpin, 1);
    sm_config_set_clkdiv(&config1, 5);
    //sm_config_set_out_shift(&config1, true, false, 32);
    //sm_config_set_in_shift(&config1, false, true, 8);
    
    pio_sm_init(pio_1, 1, offset_1 + joybus_offset_clockgen, &config1);
    pio_sm_set_enabled(pio_1, 1, true);
}

uint32_t GetInputWithTimeout(void)
{
    uint32_t lastWriteTime = time_us_32();
    while (1) {
        if(pio_sm_is_rx_fifo_empty(pio, 0)) {
            uint32_t now = time_us_32();
            uint32_t diff = now - lastWriteTime;

            // Send the eeprom data if it's been ?Seconds since the last eeprom write
            // Reset the lastWriteTime to 0 and don't sent the data unless we get another write
            if (lastWriteTime != 0 && diff > 100000) {
                lastWriteTime = 0;
                break;
            }
        } else {
            return pio_sm_get(pio, 0);
        }
    }

    return 0xFFFFFFFF;
}

pio_sm_config config;
void __time_critical_func(InitEeprom)(uint dataPin)
{
    gpio_init(dataPin);
    gpio_set_dir(dataPin, GPIO_IN);
    gpio_pull_up(dataPin);

    sleep_us(100); // Stabilize voltages

    pio_gpio_init(pio, dataPin);

    uint offset = pio_add_program(pio, &joybus_program);
    config = joybus_program_get_default_config(offset);
    sm_config_set_in_pins(&config, dataPin);
    sm_config_set_out_pins(&config, dataPin, 1);
    sm_config_set_set_pins(&config, dataPin, 1);
    sm_config_set_clkdiv(&config, 5);
    sm_config_set_out_shift(&config, true, false, 32);
    sm_config_set_in_shift(&config, false, true, 8);

    pio_sm_init(pio, 0, offset, &config);
    pio_sm_set_enabled(pio, 0, true);

    // Send the info command
    {
        uint8_t probeResponse[1] = {0x00};
        uint32_t result[8];
        int resultLen;
        convertToPio(probeResponse, 1, result, &resultLen);
        sleep_us(6); // 3.75us into the bit before end bit => 6.25 to wait if the end-bit is 5us long

        pio_sm_set_enabled(pio, 0, false);
        pio_sm_init(pio, 0, offset + joybus_offset_outmode, &config);
        pio_sm_set_enabled(pio, 0, true);

        for (int i = 0; i < resultLen; i++) pio_sm_put_blocking(pio, 0, result[i]);
    }

    // Check response
    uint32_t buffer[3];
    buffer[0] = GetInputWithTimeout();
    if (buffer[0] == 0) {
        buffer[1] = pio_sm_get_blocking(pio, 0);
        buffer[2] = pio_sm_get_blocking(pio, 0);

        // Determine the size of the EEPROM.
        if (buffer[1] == 0x80) {
            // 4K Eeprom.
            ReadCount = 64;
            gEepromSize = 512;
        } else if (buffer[1] == 0xC0) {
            // 16K Eeprom.
            ReadCount = 256;
            gEepromSize = 512 * 4;
        } else {
            // Unknown SI eeprom type.
            ReadCount = 0;
            gEepromSize = 0;
        }
    }
}

void __time_critical_func(ReadEepromData)(uint32_t offset, uint8_t *buffer) 
{
    if (gEepromSize == 0) {
        return;
    }

    // Read the eeprom.
    for (uint32_t ReadIndex = 0; ReadIndex < 64; ReadIndex += 1) {
        // Construct the read command.
        uint8_t probeResponse[] = {0x04, (uint8_t)(ReadIndex + offset)};
        uint32_t result[8];
        int resultLen;
        convertToPio(probeResponse, 1, result, &resultLen);
        sleep_us(6); // 3.75us into the bit before end bit => 6.25 to wait if the end-bit is 5us long

        // Send the read command
        pio_sm_set_enabled(pio, 0, false);
        pio_sm_init(pio, 0, offset + joybus_offset_outmode, &config);
        pio_sm_set_enabled(pio, 0, true);

        for (int i = 0; i < resultLen; i++) pio_sm_put_blocking(pio, 0, result[i]);

        // Read the incoming data from the cart.
        for (int i = 0; i < resultLen; i += 1) {
            buffer[(uint)i + (uint)ReadIndex] = (uint8_t)pio_sm_get_blocking(pio, 0);
        }
    }
}

void __time_critical_func(WriteEepromData)(uint32_t offset, uint8_t *buffer)
{
    // Write the eeprom.
    for (uint32_t ReadIndex = 0; ReadIndex < 64; ReadIndex += 1) {
        // Construct the write command.
        uint8_t probeResponse[10] = {0x05, (uint8_t)(ReadIndex + offset)};
        for (uint i = 0; i < 8; i += 1) {
            probeResponse[i + 2] = buffer[i + ReadIndex];
        }

        uint32_t result[9];
        int resultLen;
        convertToPio(probeResponse, 1, result, &resultLen);
        sleep_us(6); // 3.75us into the bit before end bit => 6.25 to wait if the end-bit is 5us long

        // Send the read command
        pio_sm_set_enabled(pio, 0, false);
        pio_sm_init(pio, 0, offset + joybus_offset_outmode, &config);
        pio_sm_set_enabled(pio, 0, true);

        for (int i = 0; i < resultLen; i++) pio_sm_put_blocking(pio, 0, result[i]);

        // Read the incoming data from the cart.
        uint8_t response[2];
        for (int i = 0; i < 2; i += 1) {
            response[i] = (uint8_t)pio_sm_get_blocking(pio, 0);
        }

        if (response[1] != 0) {
            sleep_ms(1);
        }
    }
}