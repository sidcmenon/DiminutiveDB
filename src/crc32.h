#ifndef KHB_CRC32_H
#define KHB_CRC32_H

#include <stdint.h>
#include <stddef.h>

/*
 * Standard IEEE 802.3 CRC-32 (reflected, poly 0xEDB88320, init/final 0xFFFFFFFF).
 * Self-contained, to keep the project's zero-dependency property.
 *
 * Check value: khb_crc32("123456789", 9) == 0xCBF43926
 */
uint32_t khb_crc32(const void *data, size_t len);

#endif /* KHB_CRC32_H */