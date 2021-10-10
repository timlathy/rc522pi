#pragma once

#include <stdint.h>

// CRC-16-CCITT (from Wikipedia): polynomial is 0x8408
// ISO14443A CRC: seed is 0x6363
// Tested against https://hub.zhovner.com/tools/nfc/

struct crc16_ccitt
{
    uint16_t lut[256];
};

void init_crc16_ccitt(struct crc16_ccitt* s);

void compute_crc(struct crc16_ccitt* s, char* in, int in_len, char out[2]);
