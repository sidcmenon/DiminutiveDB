#include "crc32.h"
#include "stats.h"

#include <stdbool.h>
#include <string.h>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define KHB_CRC32_HW 1
#endif

static uint32_t crc_table[256];
static bool     table_ready;

static void build_table(void)
{
    uint32_t i;
    int      k;

    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    table_ready = true;
}

uint32_t khb_crc32_table(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t       c;
    size_t         i;

    if (!table_ready)
        build_table();

    c = 0xFFFFFFFFu;
    for (i = 0; i < len; i++)
        c = crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);

    return c ^ 0xFFFFFFFFu;
}

#ifdef KHB_CRC32_HW
/*
 * The ARMv8 crc32* instructions use the IEEE 802.3 polynomial, which is the
 * same one khb_crc32_table uses -- so this produces bit-identical results and
 * the on-disk format is unchanged.
 *
 * The crc32c* instructions are Castagnoli and produce DIFFERENT values. Never
 * substitute them: every existing database file would fail verification.
 */
static uint32_t khb_crc32_hw(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t       c = 0xFFFFFFFFu;

    while (len >= 8) {
        uint64_t chunk;

        memcpy(&chunk, p, sizeof chunk);
        c    = __crc32d(c, chunk);
        p   += 8;
        len -= 8;
    }

    while (len-- > 0)
        c = __crc32b(c, *p++);

    return c ^ 0xFFFFFFFFu;
}
#endif

uint32_t khb_crc32(const void *data, size_t len)
{
    khb_stat.crc_calls++;
    khb_stat.crc_bytes += (uint64_t)len;

#ifdef KHB_CRC32_HW
    return khb_crc32_hw(data, len);
#else
    return khb_crc32_table(data, len);
#endif
}

const char *khb_crc32_impl(void)
{
#ifdef KHB_CRC32_HW
    return "hardware";
#else
    return "table";
#endif
}
