#include "crc.h"

void init_crc16_ccitt(struct crc16_ccitt* s)
{
    uint16_t polynomial = 0x8408;
    for (int i = 0; i < 256; ++i)
    {
        uint16_t crc = i;
        for (int j = 8; j > 0; j--)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ polynomial;
            else
                crc >>= 1;
        }
        s->lut[i] = crc;
    }
}

void compute_crc(struct crc16_ccitt* s, char* in, int in_len, char out[2])
{
    uint16_t crc = 0x6363;
    for (int i = 0; i < in_len; ++i)
        crc = (crc >> 8) ^ s->lut[(uint8_t)(crc ^ in[i])];

    out[0] = crc & 0xFF;
    out[1] = (crc >> 8) & 0xFF;
}
