/**
 * SPX-License-Identifier: BSD-2-Clause 
 * Copyright (c) 2023 - NopJne
 * 
 * N64CartInterface
 * Enables cartridge reading (0x1000'0000)
 * FlashRam/SRAM support     (0x0800'0000)
 * SI EEPROM support
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "n64cartinterface.h"
#include "joybus.h"

#define LATCH_DELAY_US 1
#define LATCH_DELAY_NS (110 / 14)
#define READ_LOW_DELAY_NS (133 / 4) // 133 = 1us 1us / 5 = ~300ns

#define CART_ADDRESS_START (0x10000000)
#define SRAM_ADDRESS_START (0x08000000)
uint32_t readarr[32768];

#define CRC_NUS_5101 0x587BD543 // ??
#define CRC_NUS_6101 0x9AF30466 //0x6170A4A1
#define CRC_NUS_7102 0x009E9EA3 // ??
#define CRC_NUS_6102 0x6D089C64 //0x90BB6CB5
#define CRC_NUS_6103 0x211BA9FB //0x0B050EE0
#define CRC_NUS_6105 0x520D9ABB //0x98BC2C86
#define CRC_NUS_6106 0x266C376C //0xACC8580A
#define CRC_NUS_8303 0x0E018159 // ??
#define CRC_iQue_1 0xCD19FEF1
#define CRC_iQue_2 0xB98CED9A
#define CRC_iQue_3 0xE71C2766
#define CRC_NUS_7101 0x12706049

uint32_t address_pin_mask = 0;
static char gpio_is_output = 0;
uint32_t gRomSize = 64 * 1024 * 1024;
uint32_t gFramPresent = 0;
uint32_t gSRAMPresent = 1;
uint32_t gCICType = 0xFF;
uint16_t gGameTitle[0x16];
uint16_t gGameCode[6];
uint32_t gChecksum;
const char* gCICName;

void set_ad_input() {
    for(uint32_t i = 0; i < 16; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_IN);
    }

    gpio_is_output = 0;
}

void set_ad_output() {
    for(uint32_t i = 0; i < 16; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_OUT);
    }

    gpio_is_output = 1;
}

uint32_t CrcTable[256];
bool TableBuilt = false;
uint32_t si_crc32(const uint8_t *data, size_t size) {
    unsigned n, k;
    uint32_t c;

    // No need to recompute the table on every invocation.
    if (TableBuilt == false) {
        for (n = 0; n < 256; n++) {
            c = (uint32_t) n;
            for (k = 0; k < 8; k++) {
                if (c & 1) {
                    c = 0xEDB88320L ^ (c >> 1);
                    
                } else {
                    c = c >> 1;
                }

                CrcTable[n] = c;
            }
        }

        TableBuilt = true;
    }

    c = 0L ^ 0xFFFFFFFF;
    for (n = 0; n < size; n++) {
        c = CrcTable[(c ^ data[n]) & 0xFF] ^ (c >> 8);
    }

  return c ^ 0xFFFFFFFF;
}

void cartio_init()
{
    // Setup the LED pin
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, true);

    // Roms do not read until COLD_RESET is pulled hi, atleast on the NUS3 carts with battery backed SRAM.
    gpio_init(N64_COLD_RESET);
    gpio_set_dir(N64_COLD_RESET, true);
    gpio_put(N64_COLD_RESET, false); 

    // Indicate the initialization process through the onboard led.
    volatile int t = 0;
    while(t < 5) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_ms(100);
        t++;
    }

    gpio_put(PICO_DEFAULT_LED_PIN, true);
    sleep_ms(50);

    // Setup data/address lines.
    for (uint32_t i = 0; i < 16; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_OUT);
        gpio_set_pulls(i, false, false);
        gpio_set_function(i, GPIO_FUNC_SIO);
    }

    gpio_init(N64_ALEH);
    gpio_set_dir(N64_ALEH, true);
    gpio_put(N64_ALEH, true);
    gpio_set_pulls(N64_ALEH, true, false);

    gpio_init(N64_ALEL);
    gpio_set_dir(N64_ALEL, true);
    gpio_put(N64_ALEL, false);
    gpio_set_pulls(N64_ALEL, true, false);

    gpio_init(N64_READ);
    gpio_set_dir(N64_READ, true);
    gpio_put(N64_READ, true);
    gpio_set_pulls(N64_READ, true, false);

    gpio_init(N64_WRITE);
    gpio_set_dir(N64_WRITE, true);
    gpio_put(N64_WRITE, true);
    gpio_set_pulls(N64_WRITE, true, false);

    // Create the AD gpio mask
    address_pin_mask = 0xFFFF;
    set_ad_output();

    // Eeprom init.
    InitEepromClock(N64_EEPROM_CLK);

    sleep_ms(300);
    gpio_put(N64_COLD_RESET, true);
    sleep_ms(100);

    gpio_init(N64_CIC_DCLK);
    gpio_set_dir(N64_CIC_DCLK, true);
    gpio_put(N64_CIC_DCLK, true);
    gpio_set_pulls(N64_CIC_DCLK, true, false);

    gpio_init(N64_CIC_DIO);
    gpio_set_dir(N64_CIC_DIO, false);
    gpio_set_pulls(N64_CIC_DIO, true, false);

    // Read start address, assert that the retured value is something valid.
    set_address(CART_ADDRESS_START);
    uint32_t read = (((uint32_t)read16()) << 16) | (read16());
    assert(read == 0x80371240);
    while(read != 0x80371240) {
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(100);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
        sleep_ms(100);
    }

    for (uint32_t x = 4; x < 64; x += 4) {
        // Check if the data at 16MB is a repeat of start.
        set_address(CART_ADDRESS_START + (x * 1024 * 1024));
        uint32_t readcheck = (((uint32_t)read16()) << 16) | (read16());
        // Check if the data at x is a repeat of start.
        if (read == readcheck) {
            gRomSize = x * 1024 * 1024;
            break;
        }

        // Check for an open bus. This means no hw is responding to the set address.
        bool IsOpenBus = true;
        for (uint32_t y = 0; y < 256; y += 1) {
            set_address(CART_ADDRESS_START + (x * 1024 * 1024) + (y * 2));
            uint32_t OpenBusValue = (uint16_t)(x * 1024 * 1024) + (y * 2);
            OpenBusValue = OpenBusValue | (OpenBusValue << 16);
            readcheck = (((uint32_t)read16()) << 16) | (read16());
            if (readcheck != OpenBusValue) {
                IsOpenBus = false;
                break;
            }
        }

        if (IsOpenBus != false) {
            gRomSize = x * 1024 * 1024;
            break;
        }
    }

    // Check for FRAM presence. This write is okay on every cart
    // because it will always be outside of the 32K SRAM space and the 512 write space of an FRAM chip.
    // For banked SRAM the 32K are split to address spaces above 0x1'0000, so the write is safe too.
    set_address(SRAM_ADDRESS_START + 0x10000);
    write32(0xE1000000);

    set_address(SRAM_ADDRESS_START);
    readarr[0] = (((uint32_t)read16()) << 16) | (read16());
    readarr[1] = (((uint32_t)read16()) << 16) | (read16());
    uint8_t FlashType = (readarr[1] & 0xFF);
    if ((readarr[0] == 0x11118001) && 
        (    (FlashType == 0x1E)
          || (FlashType == 0x1D)
          || (FlashType == 0xF1)
          || (FlashType == 0x8E)
          || (FlashType == 0x84)
        )
        ) {

        set_address(SRAM_ADDRESS_START + 0x10000);
        write32(0xF0000000);
        gFramPresent = true;
    }


    // Check for an open bus. This means no hw is responding to the set address.
    bool IsOpenBus = true;
    for (uint32_t i = 0; i < 256; i += 1) {
        set_address(SRAM_ADDRESS_START + (i * 2));
        uint32_t OpenBusValue = (uint16_t)(SRAM_ADDRESS_START + (i * 2));
        OpenBusValue = OpenBusValue | (OpenBusValue << 16);
        uint32_t readcheck = (((uint32_t)read16()) << 16) | (read16());
        if (readcheck != OpenBusValue) {
            IsOpenBus = false;
            break;
        }
    }

    if (IsOpenBus != false) {
        gSRAMPresent = false;
        set_address(SRAM_ADDRESS_START);
        for (uint i = 0; i < (sizeof(readarr) / 4); i += 1) {
            readarr[i] = (((uint32_t)read16()) << 16) | (read16());
        }
    }

    // EEPROM init.
    InitEeprom(N64_EEPROM_DAT);
    uint8_t Buffer[512];
    ReadEepromData(0, Buffer);

    // Do cart test and get cart data. Start with the CIC hello protocol.
    uint8_t CICHello = 0;
    for (uint32_t x = 0; x < 4; x += 1) {
        gpio_put(N64_CIC_DCLK, false);
        sleep_us(10);
        CICHello |= (uint8_t)(((gpio_get(N64_CIC_DIO) == false) ? 0 : 1) << (3 - x));
        sleep_us(16);
        gpio_put(N64_CIC_DCLK, true);
        sleep_us(20);
    }

    if (CICHello == 0x5) {
        gCICType = CIC_TYPE_PAL;
    } else if (CICHello == 0x1) {
        gCICType = CIC_TYPE_NTSC;
    } else {
        gCICType = CIC_TYPE_INVALID;
    }

    // Read the 0x1000 bytes to determine Rom name, Cart Id, Region and CIC hash.
    set_address(CART_ADDRESS_START + 0x20);
    for (uint i = 0; i < (sizeof(gGameTitle) / 2); i += 1) {
        gGameTitle[i] = flip16(read16());
    }

    set_address(CART_ADDRESS_START + 0x3A);
    for (uint i = 0; i < (sizeof(gGameCode) / 2); i += 1) {
        gGameCode[i] = read16();
    }

    uint16_t buffer[0xFC0 / 2];
    for (uint i = 0; i < (0xFC0 / 2); i += 1) {
        set_address(CART_ADDRESS_START + 0x40 + (i * 2));
        buffer[i] = read16();
    }

    uint32_t crc = si_crc32((uint8_t*)buffer, sizeof(buffer));
    switch (crc) {
    case CRC_NUS_6101:
        gCICName = "6101";
    break;
    case CRC_iQue_1:
        gCICName = "iQue 1";
    break;
    case CRC_iQue_2:
        gCICName = "iQue 2";
    break;
    case CRC_iQue_3:
        gCICName = "iQue 3";
    break;

    case CRC_NUS_6102:
        gCICName = "6102";
    break;

    case CRC_NUS_6103:
        gCICName = "6103";
    break;

    case CRC_NUS_6105:
        gCICName = "6105";
    break;

    case CRC_NUS_6106:
        gCICName = "6105";
    break;

    case CRC_NUS_8303:
        gCICName = "8303";
    break;

    case CRC_NUS_7101:
        gCICName = "7101";
    break;

    default:
        gCICName = "Unknown";
    }

}

void set_address(uint32_t address) {
    if (gpio_is_output == 0) {
        set_ad_output();
    }

    gpio_put(N64_READ, true);
    gpio_put(N64_ALEH, true);

    // translate upper 16 bits to gpio 
    uint16_t high16 = (uint16_t)(address >> 16);
    gpio_put_masked(address_pin_mask, high16);

    gpio_put(N64_ALEL, true);
    // Leave the high 16 bits on the line for at least this long
    busy_wait_at_least_cycles(LATCH_DELAY_NS); 
    
    // Set aleH low to send the lower 16 bits
    gpio_put(N64_ALEH, false);
    
    // now the lower 16 bits 
    uint16_t low16 = (uint16_t)(address & 0xFFFF);
    gpio_put_masked(address_pin_mask, low16);

    // Leave the low 16 bits on the line for at least this long
    busy_wait_at_least_cycles(LATCH_DELAY_NS);

    // set aleL low to tell the cart we are done sending the address
    gpio_put(N64_ALEL, false);
}

uint16_t read16() {
    if (gpio_is_output != 0) {
        set_ad_input();
    }

    gpio_put(N64_READ, false);
    busy_wait_at_least_cycles(READ_LOW_DELAY_NS);

    // Read the AD bus.
    gpio_put(N64_READ, true);
    uint16_t high16 = (uint16_t)gpio_get_all();
    return high16;
}

void write32(uint32_t value)
{
    write16((uint16_t)(value >> 16));
    busy_wait_at_least_cycles(READ_LOW_DELAY_NS);
    write16((uint16_t)(value & 0xFFFF));
}

void write16(uint16_t value)
{
    // After a set address the gpio mode is assumed to be output and in case of a write, it should not need to be set again.
    assert(gpio_is_output == 1);

    gpio_put_masked(address_pin_mask, value);
    gpio_put(N64_WRITE, false);
    busy_wait_at_least_cycles(READ_LOW_DELAY_NS);
    gpio_put(N64_WRITE, true);
}

void FlashRamWrite512B(uint32_t address, unsigned char *buffer, bool flip)
{
    for (uint8_t x = 0; x < 4; x += 1) {
        uint32_t offset = address + (x * 128);
        set_address(0x08000000 + 0x10000);
        write32(0x4B000000 | offset);
        set_address(0x08000000 + 0x10000);
        write32(0x78000000);
        set_address(0x08000000 + 0x10000);
        write32(0xB4000000);
        set_address(0x08000000);
        for (uint8_t i = 0; i < 128; i += 2) {
            uint32_t temp[3];
            temp[0] = buffer[i + (x * 128)];
            temp[1] = buffer[i + (x * 128) + 1];
            temp[2] = temp[0] | temp[1] << 8;
            
            if (flip != false) {
                temp[3] = flip16((uint16_t)temp[3]);
            }

            write16((uint16_t)temp[3]);
        }

        set_address(0x08000000 + 0x10000);
        write32(0xA5000000 | offset);
    }
}