/**
 * SPX-License-Identifier: BSD-2-Clause 
 * Copyright (c) 2023 - NopJne
 * 
 * N64CartInterface
 * Enables cartridge reading (0x1000'0000)
 * FlashRam/SRAM support     (0x0800'0000)
 * SI EEPROM support
 */

#pragma once

#include "joybus.h"

#define N64_EEPROM_DAT (16)
#define N64_EEPROM_CLK (17)
#define N64_WRITE      (18)
#define N64_READ       (19)
#define N64_CIC_DCLK   (20)
#define N64_CIC_DIO    (21)
#define N64_COLD_RESET (22)
#define N64_ALEL       (27)
#define N64_ALEH       (28)

enum CIC_TYPES {
    CIC_TYPE_PAL = 0,
    CIC_TYPE_NTSC = 1,
    CIC_TYPE_INVALID = 0xFF,
};

void cartio_init(void);
void set_address(uint32_t address);
uint16_t read16();
void write32(uint32_t value);
void write16(uint16_t value);
void FlashRamWrite512B(uint32_t address, unsigned char *buffer, bool flip);

extern uint32_t gRomSize;
extern uint32_t readarr[32768];
extern uint32_t gFramPresent;
extern uint32_t gSRAMPresent;
extern uint32_t gCICType;
extern uint16_t gGameTitle[0x16];
extern uint16_t gGameCode[6];
extern uint32_t gChecksum;
extern const char* gCICName;

inline uint16_t flip16(uint16_t value)
{
    return (uint16_t)((uint16_t)(value) << 8) | (((uint16_t)value) >> 8);
}
