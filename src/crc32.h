#ifndef KHB_CRC32_H
#define KHB_CRC32_H

#include <stdint.h>
#include <stddef.h>

uint32_t khb_crc32(const void *data, size_t len);

uint32_t khb_crc32_table(const void *data, size_t len);

const char *khb_crc32_impl(void);

#endif /* KHB_CRC32_H */
