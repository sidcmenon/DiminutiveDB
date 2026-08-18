#include "crc32.h"

#include <stdbool.h>

#include "stats.h"

static uint32_t khb_crc32_raw(const void *data, size_t len);

uint32_t khb_crc32(const void *data, size_t len)
{
    khb_stat.crc_calls++;
    khb_stat.crc_bytes += (uint64_t)len;
    return khb_crc32_raw(data, len);
}

static uint32_t crc_table[256];
static bool table_ready;

static void build_table(void){
    uint32_t i;
    int k;
    for (i = 0; i<256; i++){
        uint32_t c = i;
        for (k=0; k<8; k++){
            c = (c&1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
    table_ready = true;
}

static uint32_t khb_crc32_raw(const void *data, size_t len){
    const uint8_t *p = (const uint8_t *) data;
    uint32_t c;
    size_t i;
    if (!table_ready){
        build_table();
    }
    c = 0xFFFFFFFFu;
    for (i = 0; i < len; i++)
        c = crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

