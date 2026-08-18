#ifndef KHB_PAGE_H
#define KHB_PAGE_H

#include <stdint.h>
#include <stddef.h>
#include <khabibdb.h>

#define KHB_PAGE_SIZE 4096
#define KHB_MAGIC "KHABIBDB"
#define KHB_MAGIC_LEN 8
#define KHB_FORMAT_VERSION 1u

#define KHB_CHECKSUM_SKIP 4u

enum {
    PAGE_TYPE_FREE           = 0,
    PAGE_TYPE_FILE_HEADER    = 1,
    PAGE_TYPE_CATALOG        = 2,
    PAGE_TYPE_BTREE_LEAF     = 3,
    PAGE_TYPE_BTREE_INTERNAL = 4,
    PAGE_TYPE_HEAP           = 5
};

#pragma pack(push, 1)

typedef struct {
    uint32_t checksum;
    uint32_t page_id;
    uint8_t page_type;
    uint8_t reserved[3];
} page_header_t;

typedef struct {
    page_header_t hdr;
    char magic[KHB_MAGIC_LEN];
    uint32_t page_size;
    uint32_t format_version;
    uint32_t catalog_root;
    uint32_t freelist_head;
    uint32_t page_count;
} file_header_t;

#pragma pack(pop)

_Static_assert(sizeof(page_header_t) == 12, "page_header_t must be 12 bytes");
_Static_assert(sizeof(file_header_t) == 40, "file_header_t must be 40 bytes");
_Static_assert(sizeof(file_header_t) <= KHB_PAGE_SIZE, "file header must fit inside page 0");

/*
 * All of these operate on a caller-supplied buffer of exactly KHB_PAGE_SIZE
 * bytes. Never pass a bare struct — page_init zeroes the full page.
 */
void       page_init(void *page, uint32_t page_id, uint8_t type);
void       page_finalize(void *page);
khb_status page_verify(const void *page, uint32_t expect_id);
uint8_t    page_type_of(const void *page);

void       file_header_init(void *page);
khb_status file_header_validate(const void *page);

#endif /* KHB_PAGE_H */
