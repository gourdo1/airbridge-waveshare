#pragma once
#include <stdint.h>
#include <stddef.h>

uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t crc = 0xFFFF);
uint8_t crc8_ccitt(const uint8_t *data, size_t len, uint8_t crc = 0x00);

uint32_t crc32_ieee_initial();
uint32_t crc32_ieee_update(uint32_t crc, const uint8_t *data, size_t len);
uint32_t crc32_ieee_finish(uint32_t crc);
uint32_t crc32_ieee(const uint8_t *data, size_t len);
