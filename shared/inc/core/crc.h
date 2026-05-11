#ifndef INCL_CRC_H
#define INCL_CRC_H
#include "common-defines.h"

uint8_t crc8(uint8_t *data, uint32_t length);
uint32_t crc32(const uint8_t* data, const uint32_t length);

#endif