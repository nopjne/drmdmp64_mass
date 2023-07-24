/**
 * SPX-License-Identifier: BSD-2-Clause 
 * Copyright (c) 2023 - NopJne
 * 
 * Joybus
 * Provides SI joybus support for EEPROM interaction.
 */

#pragma once
void InitEeprom(uint dataPin);
void InitEepromClock(uint clockpin);
void ReadEepromData(uint32_t offset, uint8_t *buffer);
void WriteEepromData(uint32_t offset, uint8_t *buffer);

extern uint32_t gEepromSize;